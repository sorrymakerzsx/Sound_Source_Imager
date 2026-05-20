#include <asm/io.h>
#include <asm/uaccess.h>
#include <linux/cdev.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/errno.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/poll.h>
#include <linux/timer.h>

#define AF_KEY_NAME "af_key"
#define AF_KEY_CNT 1

enum af_key_status {
    AF_KEY_PRESS = 0,
    AF_KEY_RELEASE,
    AF_KEY_KEEP,
};

struct af_key_dev {
    // 字符设备号，包含主设备号和次设备号，用于注册 /dev/af_key。
    dev_t devid;
    // 内核字符设备对象，把 open/read/poll 等 file_operations 绑定到设备号上。
    struct cdev cdev;
    // 设备类，供 sysfs 和 device_create() 使用。
    struct class *class;
    // 具体创建设备节点后的 device 对象，对应用户态看到的 /dev/af_key。
    struct device *device;
    // 设备树节点指针，保存 /af_key 节点，后续从这里解析 GPIO 等属性。
    struct device_node *nd;
    // 按键对应的 GPIO 编号。
    int gpio;
    // 由 GPIO 转换得到的中断号，用于注册按键中断处理函数。
    int irq;
    // 软件消抖定时器；按键中断到来后不立即判稳，而是延后若干毫秒再读电平。
    struct timer_list timer;
    // 当前按键状态，取值为按下/松开/保持，用原子变量便于中断和进程上下文共享。
    atomic_t status;
    // 读等待队列；阻塞 read() / poll() 的用户进程会睡在这里，状态变化后被唤醒。
    wait_queue_head_t r_wait;
};

// 全局按键设备实例，驱动各流程都围绕这一份状态展开。
static struct af_key_dev g_af_key;

static irqreturn_t af_key_interrupt(int irq, void *dev_id)
{
    mod_timer(&g_af_key.timer, jiffies + msecs_to_jiffies(15));
    return IRQ_HANDLED;
}

static void af_key_timer_function(struct timer_list *arg)
{
    static int last_val = 1;
    const int current_val = gpio_get_value(g_af_key.gpio);

    if (current_val == 0 && last_val == 1) {
        atomic_set(&g_af_key.status, AF_KEY_PRESS);
        wake_up_interruptible(&g_af_key.r_wait);
    } else if (current_val == 1 && last_val == 0) {
        atomic_set(&g_af_key.status, AF_KEY_RELEASE);
        wake_up_interruptible(&g_af_key.r_wait);
    } else {
        atomic_set(&g_af_key.status, AF_KEY_KEEP);
    }

    last_val = current_val;
}

static int af_key_parse_dt(void)
{
    int ret;
    const char *status = NULL;
    const char *compatible = NULL;

    g_af_key.nd = of_find_node_by_path("/af_key");
    if (!g_af_key.nd) {
        pr_err("af_key: node /af_key not found\n");
        return -EINVAL;
    }

    ret = of_property_read_string(g_af_key.nd, "status", &status);
    if (ret < 0 || strcmp(status, "okay")) {
        pr_err("af_key: invalid status\n");
        return -EINVAL;
    }

    ret = of_property_read_string(g_af_key.nd, "compatible", &compatible);
    if (ret < 0 || strcmp(compatible, "zsx,af-key")) {
        pr_err("af_key: compatible mismatch\n");
        return -EINVAL;
    }

    g_af_key.gpio = of_get_named_gpio(g_af_key.nd, "key-gpio", 0);
    if (g_af_key.gpio < 0) {
        pr_err("af_key: failed to get key-gpio\n");
        return -EINVAL;
    }

    g_af_key.irq = gpio_to_irq(g_af_key.gpio);
    if (g_af_key.irq < 0) {
        pr_err("af_key: failed to map irq\n");
        return g_af_key.irq;
    }

    return 0;
}

static int af_key_gpio_init(void)
{
    int ret;
    unsigned long irq_flags;

    ret = gpio_request(g_af_key.gpio, AF_KEY_NAME);
    if (ret) {
        pr_err("af_key: gpio_request failed\n");
        return ret;
    }

    gpio_direction_input(g_af_key.gpio);

    irq_flags = irq_get_trigger_type(g_af_key.irq);
    if (irq_flags == IRQF_TRIGGER_NONE) {
        irq_flags = IRQF_TRIGGER_FALLING | IRQF_TRIGGER_RISING;
    }

    ret = request_irq(g_af_key.irq, af_key_interrupt, irq_flags, AF_KEY_NAME, NULL);
    if (ret) {
        gpio_free(g_af_key.gpio);
        return ret;
    }

    return 0;
}

static int af_key_open(struct inode *inode, struct file *filp)
{
    return 0;
}

static ssize_t af_key_read(struct file *filp, char __user *buf, size_t cnt, loff_t *offt)
{
    int value;
    int ret;

    if (filp->f_flags & O_NONBLOCK) {
        if (atomic_read(&g_af_key.status) == AF_KEY_KEEP) {
            return -EAGAIN;
        }
    } else {
        ret = wait_event_interruptible(g_af_key.r_wait,
                                       atomic_read(&g_af_key.status) != AF_KEY_KEEP);
        if (ret) {
            return ret;
        }
    }

    value = atomic_read(&g_af_key.status);
    ret = copy_to_user(buf, &value, sizeof(value));
    atomic_set(&g_af_key.status, AF_KEY_KEEP);

    if (ret) {
        return -EFAULT;
    }

    return sizeof(value);
}

static unsigned int af_key_poll(struct file *filp, struct poll_table_struct *wait)
{
    unsigned int mask = 0;

    poll_wait(filp, &g_af_key.r_wait, wait);
    if (atomic_read(&g_af_key.status) != AF_KEY_KEEP) {
        mask = POLLIN | POLLRDNORM;
    }

    return mask;
}

static int af_key_release(struct inode *inode, struct file *filp)
{
    return 0;
}

static const struct file_operations af_key_fops = {
    // 模块引用计数拥有者，防止设备使用期间模块被卸载。
    .owner = THIS_MODULE,
    // 用户态 open("/dev/af_key") 时进入的回调。
    .open = af_key_open,
    // 用户态 read() 读取按键状态时进入的回调。
    .read = af_key_read,
    // 用户态 select/poll/epoll 监听可读事件时进入的回调。
    .poll = af_key_poll,
    // 用户态 close() 关闭设备时进入的回调。
    .release = af_key_release,
};

static int __init af_key_init(void)
{
    int ret;

    init_waitqueue_head(&g_af_key.r_wait);
    atomic_set(&g_af_key.status, AF_KEY_KEEP);
    timer_setup(&g_af_key.timer, af_key_timer_function, 0);

    ret = af_key_parse_dt();
    if (ret) {
        return ret;
    }

    ret = af_key_gpio_init();
    if (ret) {
        return ret;
    }

    ret = alloc_chrdev_region(&g_af_key.devid, 0, AF_KEY_CNT, AF_KEY_NAME);
    if (ret < 0) {
        goto free_gpio;
    }

    cdev_init(&g_af_key.cdev, &af_key_fops);
    ret = cdev_add(&g_af_key.cdev, g_af_key.devid, AF_KEY_CNT);
    if (ret < 0) {
        goto unregister_chrdev;
    }

    g_af_key.class = class_create(THIS_MODULE, AF_KEY_NAME);
    if (IS_ERR(g_af_key.class)) {
        ret = PTR_ERR(g_af_key.class);
        goto del_cdev;
    }

    g_af_key.device = device_create(g_af_key.class, NULL, g_af_key.devid, NULL, AF_KEY_NAME);
    if (IS_ERR(g_af_key.device)) {
        ret = PTR_ERR(g_af_key.device);
        goto destroy_class;
    }

    pr_info("af_key: /dev/%s ready\n", AF_KEY_NAME);
    return 0;

destroy_class:
    class_destroy(g_af_key.class);
del_cdev:
    cdev_del(&g_af_key.cdev);
unregister_chrdev:
    unregister_chrdev_region(g_af_key.devid, AF_KEY_CNT);
free_gpio:
    del_timer_sync(&g_af_key.timer);
    free_irq(g_af_key.irq, NULL);
    gpio_free(g_af_key.gpio);
    return ret;
}

static void __exit af_key_exit(void)
{
    device_destroy(g_af_key.class, g_af_key.devid);
    class_destroy(g_af_key.class);
    cdev_del(&g_af_key.cdev);
    unregister_chrdev_region(g_af_key.devid, AF_KEY_CNT);
    del_timer_sync(&g_af_key.timer);
    free_irq(g_af_key.irq, NULL);
    gpio_free(g_af_key.gpio);
}

module_init(af_key_init);
module_exit(af_key_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("zsx");
MODULE_DESCRIPTION("Reference non-blocking autofocus key driver");
