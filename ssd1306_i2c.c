#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/delay.h>
#include "ssd1306_ioctl.h"
#define DRIVER_NAME "ssd1306"
#define DEVICE_NAME "oled"
#define OLED_WIDTH   128
#define OLED_HEIGHT   64
#define OLED_PAGES   (OLED_HEIGHT / 8)
#define FB_SIZE      (OLED_WIDTH * OLED_PAGES)
/* ---------------- GLOBALS ---------------- */
static struct i2c_client *oled_client;
static struct class *oled_class;
static struct cdev oled_cdev;
static dev_t devno;
static u8 *framebuffer;
/* ---------------- LOW LEVEL ---------------- */
static void oled_cmd(u8 cmd)
{
}
    u8 buf[2] = { 0x00, cmd };
    i2c_master_send(oled_client, buf, 2);
static void oled_data(u8 *data, size_t len)
{
    u8 *buf = kmalloc(len + 1, GFP_KERNEL);
    if (!buf)
        return;
    buf[0] = 0x40;
    memcpy(&buf[1], data, len);
    i2c_master_send(oled_client, buf, len + 1);
    kfree(buf);
}
/* ---------------- SSD1306 INIT ---------------- */
static void oled_init(void)
{
}
    msleep(100);
    oled_cmd(0xAE);              // Display OFF
    oled_cmd(0xD5); oled_cmd(0x80);
    oled_cmd(0xA8); oled_cmd(0x3F);
    oled_cmd(0xD3); oled_cmd(0x00);
    oled_cmd(0x40);
    oled_cmd(0x8D); oled_cmd(0x14);   // Charge pump
    oled_cmd(0x20); oled_cmd(0x00);   // Horizontal addressing
    oled_cmd(0xA1);
    oled_cmd(0xC8);
    oled_cmd(0xDA); oled_cmd(0x12);
    oled_cmd(0x81); oled_cmd(0xCF);
    oled_cmd(0xD9); oled_cmd(0xF1);
    oled_cmd(0xDB); oled_cmd(0x40);
    oled_cmd(0xA4);
    oled_cmd(0xA6);
    oled_cmd(0xAF);              // Display ON
/* ---------------- DISPLAY UPDATE ---------------- */
static void oled_update(void)
{
    int page;
    for (page = 0; page < OLED_PAGES; page++) {
        oled_cmd(0xB0 + page);   // Page
        oled_cmd(0x02);          // Column offset
        oled_cmd(0x10);
        oled_data(&framebuffer[page * OLED_WIDTH], OLED_WIDTH);
    }
}
/* ---------------- CHAR DEVICE ---------------- */
static ssize_t oled_write(struct file *f,
{
    if (len > FB_SIZE)
        len = FB_SIZE;
                          const char __user *buf,
                          size_t len,
                          loff_t *off)
    if (copy_from_user(framebuffer, buf, len))
        return -EFAULT;
    oled_update();
    return len;
}
static long oled_ioctl(struct file *f,
                       unsigned int cmd,
{
                       unsigned long arg)
    switch (cmd) {
    case OLED_CLEAR:
        memset(framebuffer, 0x00, FB_SIZE);  // clear RAM
        oled_update();                       // update OLED
        break;
    default:
        return -EINVAL;
    }
    return 0;
}
static const struct file_operations oled_fops = {
    .owner = THIS_MODULE,
    .write = oled_write,
    .unlocked_ioctl = oled_ioctl,
};
/* ---------------- I2C PROBE ---------------- */
static int ssd1306_probe(struct i2c_client *client)
{
    oled_client = client;
    framebuffer = kzalloc(FB_SIZE, GFP_KERNEL);
    if (!framebuffer)
        return -ENOMEM;
    alloc_chrdev_region(&devno, 0, 1, DEVICE_NAME);
    cdev_init(&oled_cdev, &oled_fops);
    cdev_add(&oled_cdev, devno, 1);
    oled_class = class_create(DEVICE_NAME);
    device_create(oled_class, NULL, devno, NULL, DEVICE_NAME);
    oled_init();
    memset(framebuffer, 0xFF, FB_SIZE);  // FORCE WHITE
    oled_update();
    pr_info("SSD1306 OLED I2C driver loaded\n");
    return 0;
}
static void ssd1306_remove(struct i2c_client *client)
{
    device_destroy(oled_class, devno);
    class_destroy(oled_class);
    cdev_del(&oled_cdev);
    unregister_chrdev_region(devno, 1);
    kfree(framebuffer);
    pr_info("SSD1306 OLED removed\n");
}
/* ---------------- DEVICE TREE ---------------- */
static const struct of_device_id ssd1306_of_match[] = {
    { .compatible = "ssd1306" },
    { }
};
MODULE_DEVICE_TABLE(of, ssd1306_of_match);
static struct i2c_driver ssd1306_driver = {
    .driver = {
        .name = DRIVER_NAME,
        .of_match_table = ssd1306_of_match,
    },
    .probe  = ssd1306_probe,
    .remove = ssd1306_remove,
};
module_i2c_driver(ssd1306_driver);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Embedded Linux");
MODULE_DESCRIPTION("SSD1306 I2C OLED Kernel Driver");
