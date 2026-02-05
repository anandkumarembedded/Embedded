#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/delay.h>

#define DEV_NAME "rpi_loopback"

/* Kernel GPIO numbers on your Pi */
#define GPIO_OUT 529   /* BCM 17 */
#define GPIO_IN  539   /* BCM 27 */

static dev_t devno;
static struct cdev cdev;
static struct class *cls;

/* --- Helper: detect jumper --- */
static bool cable_connected(void)
{
	int saved = gpio_get_value(GPIO_OUT);

    gpio_set_value(GPIO_OUT, 1);
    msleep(5);
    int v = gpio_get_value(GPIO_IN);
    gpio_set_value(GPIO_OUT, saved);
    return v == 1;
}

/* --- File Ops --- */

static ssize_t loopback_read(struct file *f,
                             char __user *buf,
                             size_t len,
                             loff_t *off)
{
    char msg[64];
    bool connected = cable_connected();
    int out_val = gpio_get_value(GPIO_OUT);

    /* Raw machine-readable state */
    int n = snprintf(msg, sizeof(msg),
                     "%d %d\n", connected ? 1 : 0, out_val ? 1 : 0);

    *off = 0;
    return simple_read_from_buffer(buf, len, off, msg, n);
}

static ssize_t loopback_write(struct file *f,
                              const char __user *buf,
                              size_t len,
                              loff_t *off)
{
    char k;

    if (len < 1)
        return -EINVAL;

    if (copy_from_user(&k, buf, 1))
        return -EFAULT;

    if (!cable_connected()) {
        gpio_set_value(GPIO_OUT, 0);
        return -ENODEV;
    }

    if (k == '1')
        gpio_set_value(GPIO_OUT, 1);
    else if (k == '0')
        gpio_set_value(GPIO_OUT, 0);
    else
        return -EINVAL;

    return len;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .read  = loopback_read,
    .write = loopback_write,
};

/* --- Module Init / Exit --- */

static int __init loopback_init(void)
{
    int ret;

    ret = alloc_chrdev_region(&devno, 0, 1, DEV_NAME);
    if (ret)
        return ret;

    cdev_init(&cdev, &fops);
    cdev_add(&cdev, devno, 1);

    cls = class_create(DEV_NAME);
    device_create(cls, NULL, devno, NULL, DEV_NAME);

    gpio_request(GPIO_OUT, "loopback_out");
    gpio_direction_output(GPIO_OUT, 0);

    gpio_request(GPIO_IN, "loopback_in");
    gpio_direction_input(GPIO_IN);

    pr_info("rpi_loopback loaded\n");
    return 0;
}

static void __exit loopback_exit(void)
{
    gpio_set_value(GPIO_OUT, 0);
    gpio_free(GPIO_OUT);
    gpio_free(GPIO_IN);

    device_destroy(cls, devno);
    class_destroy(cls);
    cdev_del(&cdev);
    unregister_chrdev_region(devno, 1);

    pr_info("rpi_loopback removed\n");
}

module_init(loopback_init);
module_exit(loopback_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Team");
