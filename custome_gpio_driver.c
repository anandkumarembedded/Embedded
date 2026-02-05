#include <linux/module.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/uaccess.h>
#include <linux/wait.h>
#include <linux/poll.h>
#include <linux/jiffies.h>

#define DEV_NAME "gpio_ctrl"

/* ioctl commands */
#define GPIO_SET_PIN     _IOW('G', 1, int)
#define GPIO_SET_DIR     _IOW('G', 2, int)
#define GPIO_GET_VAL     _IOR('G', 3, int)
#define GPIO_RESET_CNT  _IO('G', 4)

#define DEBOUNCE_MS 200

static dev_t devno;
static struct cdev gpio_cdev;
static struct class *gpio_class;

static int current_gpio = -1;
static int irq_btn1, irq_btn2;
static atomic_t btn_count = ATOMIC_INIT(0);

static wait_queue_head_t wq;
static int event_pending;
static unsigned long last_jiffy;

/* ---------------- ISR ---------------- */
static irqreturn_t button_isr(int irq, void *dev_id)
{
    unsigned long now = jiffies;

    if (time_before(now, last_jiffy + msecs_to_jiffies(DEBOUNCE_MS)))
        return IRQ_HANDLED;

    last_jiffy = now;

    atomic_inc(&btn_count);
    event_pending = 1;
    wake_up_interruptible(&wq);

    return IRQ_HANDLED;
}

/* ---------------- File Ops ---------------- */

static int dev_open(struct inode *i, struct file *f)
{
    return 0;
}

static int dev_release(struct inode *i, struct file *f)
{
    return 0;
}

static ssize_t dev_read(struct file *f, char __user *buf,
                        size_t len, loff_t *off)
{
    int count;

    if (wait_event_interruptible(wq, event_pending))
        return -ERESTARTSYS;

    event_pending = 0;
    count = atomic_read(&btn_count);

    if (copy_to_user(buf, &count, sizeof(count)))
        return -EFAULT;

    return sizeof(count);
}

static ssize_t dev_write(struct file *f, const char __user *buf,
                         size_t len, loff_t *off)
{
    char k;

    if (current_gpio < 0)
        return -EINVAL;

    if (copy_from_user(&k, buf, 1))
        return -EFAULT;

    gpio_set_value(current_gpio, (k == '1'));
    return len;
}

static long dev_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    int val;

    switch (cmd) {
    case GPIO_SET_PIN:
        copy_from_user(&current_gpio, (int __user *)arg, sizeof(int));
        gpio_request(current_gpio, "user_gpio");
        return 0;

    case GPIO_SET_DIR:
        copy_from_user(&val, (int __user *)arg, sizeof(int));
        if (val)
            gpio_direction_output(current_gpio, 0);
        else
            gpio_direction_input(current_gpio);
        return 0;

    case GPIO_GET_VAL:
        val = gpio_get_value(current_gpio);
        copy_to_user((int __user *)arg, &val, sizeof(val));
        return 0;

    case GPIO_RESET_CNT:
        atomic_set(&btn_count, 0);
        return 0;

    default:
        return -EINVAL;
    }
}
static unsigned int dev_poll(struct file *f, poll_table *wait)
{
    poll_wait(f, &wq, wait);
    if (event_pending)
        return POLLIN | POLLRDNORM;
    return 0;
}

static struct file_operations fops = {
    .owner          = THIS_MODULE,
    .open           = dev_open,
    .release        = dev_release,
    .read           = dev_read,
    .write          = dev_write,
    .unlocked_ioctl = dev_ioctl,
    .poll           = dev_poll,
};

/* ---------------- Init / Exit ---------------- */

static int __init gpio_init(void)
{
    alloc_chrdev_region(&devno, 0, 1, DEV_NAME);
    cdev_init(&gpio_cdev, &fops);
    cdev_add(&gpio_cdev, devno, 1);

    gpio_class = class_create(DEV_NAME);
    device_create(gpio_class, NULL, devno, NULL, DEV_NAME);

    init_waitqueue_head(&wq);

    gpio_request(539, "BTN1");
    gpio_direction_input(539);
    irq_btn1 = gpio_to_irq(539);
    request_irq(irq_btn1, button_isr, IRQF_TRIGGER_FALLING,
                "btn1_irq", NULL);

    gpio_request(540, "BTN2");
    gpio_direction_input(540);
    irq_btn2 = gpio_to_irq(540);
    request_irq(irq_btn2, button_isr, IRQF_TRIGGER_FALLING,
                "btn2_irq", NULL);

    pr_info("GPIO Control Driver Loaded\n");
    return 0;
}

static void __exit gpio_exit(void)
{
    free_irq(irq_btn1, NULL);
    free_irq(irq_btn2, NULL);

    device_destroy(gpio_class, devno);
    class_destroy(gpio_class);
    cdev_del(&gpio_cdev);
    unregister_chrdev_region(devno, 1);

    pr_info("GPIO Control Driver Unloaded\n");
}

module_init(gpio_init);
module_exit(gpio_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Team");
MODULE_DESCRIPTION("GPIO Control Driver with IRQ, poll, ioctl");
