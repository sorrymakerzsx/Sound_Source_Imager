#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-common.h>

#define DW9763_NAME "dw9763"
#define RK_VIDIOC_VCM_TIMEINFO _IOR('V', BASE_VIDIOC_PRIVATE + 0, struct rk_cam_vcm_tim)
#define VCMDRV_MAX_LOG 64U
#define DW9763_MAX_REG 1023U

/*
 * 功能：
 *   RK 私有 ioctl 示例里用于返回马达移动耗时的结构体。
 * 参数：
 *   move_us: 输出给上层的马达移动耗时，单位微秒。
 *   reserved: 预留字段，方便后续扩展。
 * 返回值：
 *   这是数据结构体，本身没有返回值。
 */
struct rk_cam_vcm_tim {
    unsigned int move_us;
    unsigned int reserved;
};

/*
 * 功能：
 *   保存 DW9763 子设备驱动的运行时上下文，包括：
 *   1. V4L2 subdev/ctrl 句柄；
 *   2. I2C 客户端；
 *   3. VCM 位置映射参数；
 *   4. 当前镜头逻辑位置和最近一次动作时间。
 * 参数：
 *   这是设备上下文结构体，各成员由 probe/ctrl/ioctl 路径共同维护。
 * 返回值：
 *   这是数据结构体，本身没有返回值。
 */
struct dw9763_device {
    struct v4l2_subdev sd;
    struct v4l2_ctrl_handler ctrls;
    struct i2c_client *client;
    struct mutex lock;
    unsigned int start_current;
    unsigned int rated_current;
    unsigned int step_mode;
    unsigned int step;
    unsigned int current_lens_pos;
    unsigned int current_related_pos;
    unsigned int last_move_us;
};

/*
 * 功能：
 *   由 v4l2_subdev 指针反查出驱动私有的 dw9763_device。
 * 参数：
 *   sd: V4L2 子设备对象指针。
 * 返回值：
 *   对应的 dw9763_device 私有上下文指针。
 */
static inline struct dw9763_device *to_dw9763_vcm(struct v4l2_subdev *sd)
{
    return container_of(sd, struct dw9763_device, sd);
}

/*
 * 功能：
 *   向 DW9763 的指定寄存器写入指定长度的数据。
 * 参数：
 *   client: 当前 DW9763 的 I2C 客户端。
 *   reg: 目标寄存器地址。
 *   len: 要写入的数据字节数，最大 4 字节。
 *   val: 要写入的值。
 * 返回值：
 *   0 表示写寄存器成功；负数表示参数错误或 I2C 发送失败。
 */
static int dw9763_write_reg(struct i2c_client *client, u16 reg, u16 len, u32 val)
{
    u32 buf_i, val_i;
    u8 buf[6];
    u8 *val_p;
    __be32 val_be;

    if (len > 4)
        return -EINVAL;

    buf[0] = reg >> 8;
    buf[1] = reg & 0xff;
    val_be = cpu_to_be32(val);
    val_p = (u8 *)&val_be;
    buf_i = 2;
    val_i = 4 - len;
    while (val_i < 4)
        buf[buf_i++] = val_p[val_i++];

    return i2c_master_send(client, buf, len + 2) == len + 2 ? 0 : -EIO;
}

/*
 * 功能：
 *   从 DW9763 的指定寄存器读取数据。
 * 参数：
 *   client: 当前 DW9763 的 I2C 客户端。
 *   reg: 要读取的寄存器地址。
 *   len: 读取字节数，范围 1~4。
 *   val: 输出参数，成功时写入读取到的寄存器值。
 * 返回值：
 *   0 表示读取成功；负数表示参数错误或 I2C 传输失败。
 */
static int dw9763_read_reg(struct i2c_client *client, u16 reg, u16 len, u32 *val)
{
    struct i2c_msg msgs[2];
    u8 *data_be_p;
    __be32 data_be = 0;
    __be16 reg_addr_be = cpu_to_be16(reg);
    int ret;

    if (len > 4 || !len)
        return -EINVAL;

    data_be_p = (u8 *)&data_be;
    msgs[0].addr = client->addr;
    msgs[0].flags = 0;
    msgs[0].len = 2;
    msgs[0].buf = (u8 *)&reg_addr_be;
    msgs[1].addr = client->addr;
    msgs[1].flags = I2C_M_RD;
    msgs[1].len = len;
    msgs[1].buf = &data_be_p[4 - len];

    ret = i2c_transfer(client->adapter, msgs, ARRAY_SIZE(msgs));
    if (ret != ARRAY_SIZE(msgs))
        return -EIO;

    *val = be32_to_cpu(data_be);
    return 0;
}

/*
 * 功能：
 *   将上层给出的逻辑焦点位置 [0, 64] 映射成 DAC 寄存器值，并写入 DW9763，
 *   从而驱动镜头移动到新的焦点位置。
 * 参数：
 *   dev_vcm: 驱动私有上下文。
 *   dest_pos: 目标逻辑焦点位置，0 一般表示一端焦点，64 表示另一端焦点。
 * 返回值：
 *   0 表示位置设置成功；负数表示 I2C 忙检查或写寄存器失败。
 */
static int dw9763_set_pos(struct dw9763_device *dev_vcm, unsigned int dest_pos)
{
    int ret;
    unsigned int position;
    u32 is_busy;
    unsigned int i;

    if (dest_pos >= VCMDRV_MAX_LOG)
        position = dev_vcm->rated_current;
    else
        position = dev_vcm->start_current + dev_vcm->step * dest_pos;

    if (position > DW9763_MAX_REG)
        position = DW9763_MAX_REG;

    mutex_lock(&dev_vcm->lock);
    dev_vcm->current_lens_pos = position;
    dev_vcm->current_related_pos = dest_pos;

    for (i = 0; i < 100; i++) {
        ret = dw9763_read_reg(dev_vcm->client, 0x05, 1, &is_busy);
        if (!ret && !(is_busy & 0x01))
            break;
        usleep_range(100, 200);
    }

    ret = dw9763_write_reg(dev_vcm->client, 0x03, 2, dev_vcm->current_lens_pos);
    dev_vcm->last_move_us = 2000;
    mutex_unlock(&dev_vcm->lock);
    return ret;
}

/*
 * 功能：
 *   处理 V4L2_CID_FOCUS_ABSOLUTE 的读取请求，把当前缓存的逻辑焦点返回给上层。
 * 参数：
 *   ctrl: V4L2 控件对象。
 * 返回值：
 *   成功时返回当前逻辑焦点值；不支持的控件返回 -EINVAL。
 */
static int dw9763_get_ctrl(struct v4l2_ctrl *ctrl)
{
    struct dw9763_device *dev_vcm = container_of(ctrl->handler,
                                                 struct dw9763_device,
                                                 ctrls);

    if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
        return dev_vcm->current_related_pos;
    return -EINVAL;
}

/*
 * 功能：
 *   处理 V4L2_CID_FOCUS_ABSOLUTE 的设置请求，并把新焦点下发到芯片。
 * 参数：
 *   ctrl: V4L2 控件对象，ctrl->val 中携带目标逻辑焦点位置。
 * 返回值：
 *   0 表示设置成功；负数表示控件不支持或写硬件失败。
 */
static int dw9763_set_ctrl(struct v4l2_ctrl *ctrl)
{
    struct dw9763_device *dev_vcm = container_of(ctrl->handler,
                                                 struct dw9763_device,
                                                 ctrls);

    if (ctrl->id == V4L2_CID_FOCUS_ABSOLUTE)
        return dw9763_set_pos(dev_vcm, ctrl->val);
    return -EINVAL;
}

static const struct v4l2_ctrl_ops dw9763_ctrl_ops = {
    .g_volatile_ctrl = dw9763_get_ctrl,
    .s_ctrl = dw9763_set_ctrl,
};

/*
 * 功能：
 *   处理子设备核心 ioctl，这里示例性支持 RK_VIDIOC_VCM_TIMEINFO，
 *   用于把最近一次镜头移动耗时返回给上层 AF 算法。
 * 参数：
 *   sd: DW9763 对应的 V4L2 子设备对象。
 *   cmd: ioctl 命令号。
 *   arg: 命令参数缓冲区，这里约定为 struct rk_cam_vcm_tim*。
 * 返回值：
 *   0 表示命令已处理；-ENOIOCTLCMD 表示当前命令未支持。
 */
static long dw9763_ioctl(struct v4l2_subdev *sd, unsigned int cmd, void *arg)
{
    struct dw9763_device *dev_vcm = to_dw9763_vcm(sd);
    struct rk_cam_vcm_tim *timeinfo = arg;

    if (cmd == RK_VIDIOC_VCM_TIMEINFO && timeinfo) {
        timeinfo->move_us = dev_vcm->last_move_us;
        return 0;
    }
    return -ENOIOCTLCMD;
}

static const struct v4l2_subdev_core_ops dw9763_core_ops = {
    .ioctl = dw9763_ioctl,
};

static const struct v4l2_subdev_ops dw9763_ops = {
    .core = &dw9763_core_ops,
};

/*
 * 功能：
 *   I2C probe 入口。完成 DW9763 私有结构体分配、设备树参数读取、
 *   V4L2 subdev 初始化、focus 控件注册，以及异步子设备注册。
 * 参数：
 *   client: I2C 设备对象。
 *   id: I2C 设备 ID 表项。
 * 返回值：
 *   0 表示 probe 成功；负数表示内存申请、控件注册或 subdev 注册失败。
 */
static int dw9763_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    struct device *dev = &client->dev;
    struct dw9763_device *dw9763_dev;
    struct v4l2_subdev *sd;
    u32 val;
    int ret;

    dw9763_dev = devm_kzalloc(dev, sizeof(*dw9763_dev), GFP_KERNEL);
    if (!dw9763_dev)
        return -ENOMEM;

    dw9763_dev->client = client;
    mutex_init(&dw9763_dev->lock);
    dw9763_dev->start_current = device_property_read_u32(dev, "rockchip,vcm-start-current", &val) ? 0 : val;
    dw9763_dev->rated_current = device_property_read_u32(dev, "rockchip,vcm-rated-current", &val) ? 100 : val;
    dw9763_dev->step_mode = device_property_read_u32(dev, "rockchip,vcm-step-mode", &val) ? 4 : val;
    dw9763_dev->step = (dw9763_dev->rated_current > dw9763_dev->start_current) ?
                       (dw9763_dev->rated_current - dw9763_dev->start_current) / VCMDRV_MAX_LOG : 1;

    sd = &dw9763_dev->sd;
    v4l2_i2c_subdev_init(sd, client, &dw9763_ops);
    sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
    sd->entity.function = MEDIA_ENT_F_LENS;
    strlcpy(sd->name, DW9763_NAME, sizeof(sd->name));

    v4l2_ctrl_handler_init(&dw9763_dev->ctrls, 1);
    v4l2_ctrl_new_std(&dw9763_dev->ctrls, &dw9763_ctrl_ops,
                      V4L2_CID_FOCUS_ABSOLUTE, 0, VCMDRV_MAX_LOG, 1, 0);
    if (dw9763_dev->ctrls.error)
        return dw9763_dev->ctrls.error;

    sd->ctrl_handler = &dw9763_dev->ctrls;
    ret = v4l2_async_register_subdev(sd);
    if (ret < 0) {
        v4l2_ctrl_handler_free(&dw9763_dev->ctrls);
        return ret;
    }

    return 0;
}

/*
 * 功能：
 *   I2C remove 入口。注销 subdev、释放 ctrl handler，并销毁互斥锁。
 * 参数：
 *   client: I2C 设备对象。
 * 返回值：
 *   0 表示清理完成。
 */
static int dw9763_remove(struct i2c_client *client)
{
    struct v4l2_subdev *sd = i2c_get_clientdata(client);
    struct dw9763_device *dw9763_dev = to_dw9763_vcm(sd);

    v4l2_async_unregister_subdev(sd);
    v4l2_ctrl_handler_free(&dw9763_dev->ctrls);
    mutex_destroy(&dw9763_dev->lock);
    return 0;
}

static const struct i2c_device_id dw9763_id[] = {
    { DW9763_NAME, 0 },
    { }
};
MODULE_DEVICE_TABLE(i2c, dw9763_id);

static const struct of_device_id dw9763_of_match[] = {
    { .compatible = "dongwoon,dw9763" },
    { }
};
MODULE_DEVICE_TABLE(of, dw9763_of_match);

static struct i2c_driver dw9763_i2c_driver = {
    .driver = {
        .name = DW9763_NAME,
        .of_match_table = dw9763_of_match,
    },
    .probe = dw9763_probe,
    .remove = dw9763_remove,
    .id_table = dw9763_id,
};
module_i2c_driver(dw9763_i2c_driver);

MODULE_DESCRIPTION("DW9763 VCM Driver Reference");
MODULE_LICENSE("GPL");
