#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/uaccess.h>

#define EEPROM_NAME "eeprom_ver"
#define EEPROM_CNT 1

struct eeprom_dev {
    /* 字符设备号，对应 /dev/eeprom_ver。 */
    dev_t devid;
    /* 字符设备对象，绑定 open/read/write 等文件操作。 */
    struct cdev cdev;
    /* 设备类，供 sysfs 和 device_create 使用。 */
    struct class *class;
    /* 创建设备节点后的 device 对象。 */
    struct device *device;
    /* 当前 EEPROM 所在的 I2C 设备句柄。 */
    struct i2c_client *client;
    /* 主设备号缓存，便于静态注册或复用。 */
    int major;
};

/* 全局 EEPROM 设备实例，驱动所有文件操作都依赖它。 */
static struct eeprom_dev eeprom_device;

static int eeprom_write_regs(struct eeprom_dev *dev, u8 reg, u8 *buf, int len)
{
    u8 packet[256];
    struct i2c_msg msg;
    struct i2c_client *client = dev->client;

    packet[0] = reg;
    memcpy(&packet[1], buf, len);

    msg.addr = client->addr;
    msg.flags = 0;
    msg.buf = packet;
    msg.len = len + 1;
    return i2c_transfer(client->adapter, &msg, 1) == 1 ? 0 : -EREMOTEIO;
}

static int eeprom_read_regs(struct eeprom_dev *dev, u8 reg, void *buf, int len)
{
    struct i2c_msg msg[2];
    struct i2c_client *client = dev->client;
    int ret;

    msg[0].addr = client->addr;
    msg[0].flags = 0;
    msg[0].buf = &reg;
    msg[0].len = 1;

    msg[1].addr = client->addr;
    msg[1].flags = I2C_M_RD;
    msg[1].buf = buf;
    msg[1].len = len;

    ret = i2c_transfer(client->adapter, msg, 2);
    return ret == 2 ? 0 : -EREMOTEIO;
}

static int eeprom_open(struct inode *inode, struct file *filp)
{
    filp->private_data = &eeprom_device;
    return 0;
}

static ssize_t eeprom_read(struct file *filp, char __user *buf, size_t cnt, loff_t *offt)
{
    u8 data_buf[32] = {0};
    struct eeprom_dev *dev = filp->private_data;
    int ret;

    if (cnt > sizeof(data_buf))
        cnt = sizeof(data_buf);

    ret = eeprom_read_regs(dev, 0x00, data_buf, cnt);
    if (ret < 0)
        return ret;

    if (copy_to_user(buf, data_buf, cnt))
        return -EFAULT;

    return cnt;
}

static ssize_t eeprom_write(struct file *filp, const char __user *buf, size_t cnt, loff_t *offt)
{
    u8 data_buf[32] = {0};
    struct eeprom_dev *dev = filp->private_data;
    int ret;

    if (cnt > sizeof(data_buf))
        cnt = sizeof(data_buf);

    if (copy_from_user(data_buf, buf, cnt))
        return -EFAULT;

    ret = eeprom_write_regs(dev, 0x00, data_buf, cnt);
    if (ret < 0)
        return ret;

    return cnt;
}

static const struct file_operations eeprom_fops = {
    .owner = THIS_MODULE,
    .open = eeprom_open,
    .read = eeprom_read,
    .write = eeprom_write,
};

static int eeprom_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
    int ret;

    if (eeprom_device.major) {
        eeprom_device.devid = MKDEV(eeprom_device.major, 0);
        ret = register_chrdev_region(eeprom_device.devid, EEPROM_CNT, EEPROM_NAME);
    } else {
        ret = alloc_chrdev_region(&eeprom_device.devid, 0, EEPROM_CNT, EEPROM_NAME);
        eeprom_device.major = MAJOR(eeprom_device.devid);
    }
    if (ret < 0)
        return ret;

    cdev_init(&eeprom_device.cdev, &eeprom_fops);
    eeprom_device.cdev.owner = THIS_MODULE;
    ret = cdev_add(&eeprom_device.cdev, eeprom_device.devid, EEPROM_CNT);
    if (ret < 0)
        goto fail_cdev;

    eeprom_device.class = class_create(THIS_MODULE, EEPROM_NAME);
    if (IS_ERR(eeprom_device.class)) {
        ret = PTR_ERR(eeprom_device.class);
        goto fail_class;
    }

    eeprom_device.device = device_create(eeprom_device.class, NULL,
                                         eeprom_device.devid, NULL, EEPROM_NAME);
    if (IS_ERR(eeprom_device.device)) {
        ret = PTR_ERR(eeprom_device.device);
        goto fail_device;
    }

    eeprom_device.client = client;
    dev_info(&client->dev, "EEPROM version driver ready: /dev/%s\n", EEPROM_NAME);
    return 0;

fail_device:
    class_destroy(eeprom_device.class);
fail_class:
    cdev_del(&eeprom_device.cdev);
fail_cdev:
    unregister_chrdev_region(eeprom_device.devid, EEPROM_CNT);
    return ret;
}

static int eeprom_remove(struct i2c_client *client)
{
    device_destroy(eeprom_device.class, eeprom_device.devid);
    class_destroy(eeprom_device.class);
    cdev_del(&eeprom_device.cdev);
    unregister_chrdev_region(eeprom_device.devid, EEPROM_CNT);
    return 0;
}

static const struct i2c_device_id eeprom_id[] = {
    {"zsx,eeprom_ver", 0},
    {}
};
MODULE_DEVICE_TABLE(i2c, eeprom_id);

static const struct of_device_id eeprom_of_match[] = {
    { .compatible = "zsx,eeprom_ver" },
    {}
};
MODULE_DEVICE_TABLE(of, eeprom_of_match);

static struct i2c_driver eeprom_driver = {
    .probe = eeprom_probe,
    .remove = eeprom_remove,
    .driver = {
        .owner = THIS_MODULE,
        .name = "eeprom_ver_drv",
        .of_match_table = eeprom_of_match,
    },
    .id_table = eeprom_id,
};
module_i2c_driver(eeprom_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zsx");
MODULE_DESCRIPTION("EEPROM version character device over I2C");
