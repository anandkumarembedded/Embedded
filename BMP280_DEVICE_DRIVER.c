// SPDX-License-Identifier: GPL-2.0
/*
 * BMP280 I2C Driver
 *
 * Features:
 * - I2C communication with BMP280 sensor
 * - Reads temperature and pressure
 * - Character device: /dev/bmpdr
 * - Periodic polling using workqueue
 * - Logs data to /var/log/bmpdr.log
 *
 * Author: Bisabathini Kireeti
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/mutex.h>
#include <linux/timekeeping.h>
#include <linux/file.h>

/* ---------- BMP280 Registers ---------- */
#define BMP280_CHIP_ID_REG  0xD0
#define BMP280_CHIP_ID      0x58
#define BMP280_CTRL_MEAS    0xF4
#define BMP280_CONFIG       0xF5
#define BMP280_PRESS_MSB    0xF7

#define DEVICE_NAME         "bmpdr"
#define BMP280_POLL_MS      1000
#define BMPDR_LOG_FILE      "/var/log/bmpdr.log"

/* ---------- Calibration Data ---------- */
struct bmp280_calib {
    u16 dig_T1;
    s16 dig_T2;
    s16 dig_T3;
    u16 dig_P1;
    s16 dig_P2;
    s16 dig_P3;
    s16 dig_P4;
    s16 dig_P5;
    s16 dig_P6;
    s16 dig_P7;
    s16 dig_P8;
    s16 dig_P9;
};

/* ---------- Driver Private Data ---------- */
struct bmpdr_data {
    struct i2c_client *client;      // I2C device
    struct bmp280_calib calib;      // Calibration values
    int32_t t_fine;                 // Used in compensation
    char buffer[64];                // Data shared to user space
    struct cdev cdev;               // Char device
    struct delayed_work dwork;      // Periodic polling
    struct mutex lock;              // Protects I2C access
};

static dev_t bmpdr_dev;
static struct class *bmpdr_class;

/* ---------- Log to File ---------- */
static void bmpdr_write_log(struct bmpdr_data *d)
{
    struct file *fp;
    char logbuf[128];
    struct timespec64 ts;

    ktime_get_real_ts64(&ts);

    snprintf(logbuf, sizeof(logbuf),
             "[%lld] %s\n",
             (long long)ts.tv_sec, d->buffer);

    fp = filp_open(BMPDR_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (IS_ERR(fp))
        return;

    kernel_write(fp, logbuf, strlen(logbuf), &fp->f_pos);
    filp_close(fp, NULL);
}

/* ---------- Read Calibration ---------- */
static int bmpdr_read_calibration(struct i2c_client *client,
                                  struct bmp280_calib *c)
{
    u8 buf[24];
    int ret;

    ret = i2c_smbus_read_i2c_block_data(client, 0x88, 24, buf);
    if (ret < 0)
        return ret;

    c->dig_T1 = (buf[1] << 8) | buf[0];
    c->dig_T2 = (buf[3] << 8) | buf[2];
    c->dig_T3 = (buf[5] << 8) | buf[4];
    c->dig_P1 = (buf[7] << 8) | buf[6];
    c->dig_P2 = (buf[9] << 8) | buf[8];
    c->dig_P3 = (buf[11] << 8) | buf[10];
    c->dig_P4 = (buf[13] << 8) | buf[12];
    c->dig_P5 = (buf[15] << 8) | buf[14];
    c->dig_P6 = (buf[17] << 8) | buf[16];
    c->dig_P7 = (buf[19] << 8) | buf[18];
    c->dig_P8 = (buf[21] << 8) | buf[20];
    c->dig_P9 = (buf[23] << 8) | buf[22];

    return 0;
}

/* ---------- Temperature Compensation ---------- */
static int32_t bmpdr_comp_temp(struct bmpdr_data *d, int32_t adc_T)
{
    int32_t var1, var2;

    var1 = ((((adc_T >> 3) - ((int32_t)d->calib.dig_T1 << 1))) *
            d->calib.dig_T2) >> 11;

    var2 = (((((adc_T >> 4) - d->calib.dig_T1) *
              ((adc_T >> 4) - d->calib.dig_T1)) >> 12) *
            d->calib.dig_T3) >> 14;

    d->t_fine = var1 + var2;
    return (d->t_fine * 5 + 128) >> 8;
}

/* ---------- Pressure Compensation ---------- */
static uint32_t bmpdr_comp_press(struct bmpdr_data *d, int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = d->t_fine - 128000;
    var2 = var1 * var1 * d->calib.dig_P6;
    var2 += (var1 * d->calib.dig_P5) << 17;
    var2 += ((int64_t)d->calib.dig_P4) << 35;

    var1 = ((var1 * var1 * d->calib.dig_P3) >> 8) +
           ((var1 * d->calib.dig_P2) << 12);

    var1 = (((1LL << 47) + var1) * d->calib.dig_P1) >> 33;
    if (!var1)
        return 0;

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;

    var1 = (d->calib.dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = (d->calib.dig_P8 * p) >> 19;

    p = ((p + var1 + var2) >> 8) +
        ((int64_t)d->calib.dig_P7 << 4);

    return p;
}

/* ---------- Read Sensor ---------- */
static void bmpdr_read_sensor(struct bmpdr_data *d)
{
    u8 buf[6];
    int32_t adc_T, adc_P, temp;
    uint32_t press;

    mutex_lock(&d->lock);

    i2c_smbus_read_i2c_block_data(d->client, BMP280_PRESS_MSB, 6, buf);

    adc_P = (buf[0] << 12) | (buf[1] << 4) | (buf[2] >> 4);
    adc_T = (buf[3] << 12) | (buf[4] << 4) | (buf[5] >> 4);

    temp  = bmpdr_comp_temp(d, adc_T);
    press = bmpdr_comp_press(d, adc_P);

    snprintf(d->buffer, sizeof(d->buffer),
             "Temperature: %d.%02d C, Pressure: %d.%02d hPa",
             temp / 100, temp % 100,
             press / 25600, (press % 25600) / 256);

    bmpdr_write_log(d);
    mutex_unlock(&d->lock);
}

/* ---------- Workqueue ---------- */
static void bmpdr_work(struct work_struct *work)
{
    struct bmpdr_data *d =
        container_of(work, struct bmpdr_data, dwork.work);

    bmpdr_read_sensor(d);
    schedule_delayed_work(&d->dwork,
                          msecs_to_jiffies(BMP280_POLL_MS));
}

/* ---------- Char Device ---------- */
static ssize_t bmpdr_read(struct file *f, char __user *buf,
                          size_t cnt, loff_t *ppos)
{
    struct bmpdr_data *d = f->private_data;
    return simple_read_from_buffer(buf, cnt, ppos,
                                   d->buffer,
                                   strlen(d->buffer));
}

static int bmpdr_open(struct inode *inode, struct file *f)
{
    f->private_data =
        container_of(inode->i_cdev, struct bmpdr_data, cdev);
    return 0;
}

static const struct file_operations bmpdr_fops = {
    .owner = THIS_MODULE,
    .open  = bmpdr_open,
    .read  = bmpdr_read,
};

/* ---------- I2C Probe ---------- */
static int bmpdr_probe(struct i2c_client *client)
{
    struct bmpdr_data *d;

    d = devm_kzalloc(&client->dev, sizeof(*d), GFP_KERNEL);
    d->client = client;
    mutex_init(&d->lock);

    bmpdr_read_calibration(client, &d->calib);

    i2c_smbus_write_byte_data(client, BMP280_CTRL_MEAS, 0x27);
    i2c_smbus_write_byte_data(client, BMP280_CONFIG, 0xA0);

    alloc_chrdev_region(&bmpdr_dev, 0, 1, DEVICE_NAME);
    bmpdr_class = class_create(DEVICE_NAME);

    cdev_init(&d->cdev, &bmpdr_fops);
    cdev_add(&d->cdev, bmpdr_dev, 1);
    device_create(bmpdr_class, NULL, bmpdr_dev, NULL, DEVICE_NAME);

    INIT_DELAYED_WORK(&d->dwork, bmpdr_work);
    schedule_delayed_work(&d->dwork,
                          msecs_to_jiffies(BMP280_POLL_MS));

    i2c_set_clientdata(client, d);
    return 0;
}

static void bmpdr_remove(struct i2c_client *client)
{
    struct bmpdr_data *d = i2c_get_clientdata(client);

    cancel_delayed_work_sync(&d->dwork);
    device_destroy(bmpdr_class, bmpdr_dev);
    cdev_del(&d->cdev);
    class_destroy(bmpdr_class);
    unregister_chrdev_region(bmpdr_dev, 1);
}

/* ---------- Device Tree Match ---------- */
static const struct of_device_id bmpdr_of_match[] = {
    { .compatible = "bosch,bmp280" },
    { }
};
MODULE_DEVICE_TABLE(of, bmpdr_of_match);

/* ---------- I2C Driver ---------- */
static struct i2c_driver bmpdr_driver = {
    .driver = {
        .name = "bmpdr",
        .of_match_table = bmpdr_of_match,
    },
    .probe  = bmpdr_probe,
    .remove = bmpdr_remove,
};

module_i2c_driver(bmpdr_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Bisabathini Kireeti");
MODULE_DESCRIPTION("BMP280 I2C Driver with char device and logging");

/* =====================================================================
 * DEVICE TREE OVERLAY (REFERENCE ONLY - COMMENTED)
 * =====================================================================
 *
 * /dts-v1/;
 * /plugin/;
 *
 * / {
 *     compatible = "brcm,bcm2835";
 *
 *     fragment@0 {
 *         target = <&i2c1>;
 *         __overlay__ {
 *             status = "okay";
 *
 *             bmp280@76 {
 *                 compatible = "bosch,bmp280";
 *                 reg = <0x76>;
 *             };
 *         };
 *     };
 * };
 *
 * How it works:
 * - Enables I2C-1
 * - Registers BMP280 at address 0x76
 * - Matches with of_match_table
 * - Calls bmpdr_probe()
 * =====================================================================
 */
