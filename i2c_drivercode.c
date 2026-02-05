#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>

/* Device */
#define DEV_NAME "i2c_sensor"

/* Registers */
#define REG_CHIP_ID     0xD0
#define REG_PRESS_MSB   0xF7
#define REG_TEMP_MSB    0xFA
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5

/* Calibration registers */
#define REG_CALIB_T1    0x88
#define REG_CALIB_T2    0x8A
#define REG_CALIB_T3    0x8C
#define REG_CALIB_P1    0x8E
#define REG_CALIB_P2    0x90
#define REG_CALIB_P3    0x92
#define REG_CALIB_P4    0x94
#define REG_CALIB_P5    0x96
#define REG_CALIB_P6    0x98
#define REG_CALIB_P7    0x9A
#define REG_CALIB_P8    0x9C
#define REG_CALIB_P9    0x9E

/* Chip IDs */
#define BMP280_CHIP_ID  0x58

/* ctrl_meas settings */
#define OSRS_T_x2   (2 << 5)
#define OSRS_P_x16  (5 << 2)
#define MODE_NORMAL 0x03

/* config settings */
#define T_SB_250MS  (3 << 5)
#define FILTER_4    (2 << 2)

/* Globals */
static dev_t dev_num;
static struct cdev bmp_cdev;
static struct class *bmp_class;
static struct i2c_client *bmp_client;

/* Calibration data */
static u16 dig_T1;
static s16 dig_T2, dig_T3;
static u16 dig_P1;
static s16 dig_P2, dig_P3, dig_P4, dig_P5;
static s16 dig_P6, dig_P7, dig_P8, dig_P9;
static s32 t_fine;

/* ---------- device permissions ---------- */
static char *bmp_devnode(const struct device *dev, umode_t *mode)
{
    if (mode)
        *mode = 0666;
    return NULL;
}

/* ---------- read calibration ---------- */
static int bmp280_read_calib(struct i2c_client *client)
{
    dig_T1 = i2c_smbus_read_word_data(client, REG_CALIB_T1);
    dig_T2 = i2c_smbus_read_word_data(client, REG_CALIB_T2);
    dig_T3 = i2c_smbus_read_word_data(client, REG_CALIB_T3);

    dig_P1 = i2c_smbus_read_word_data(client, REG_CALIB_P1);
    dig_P2 = i2c_smbus_read_word_data(client, REG_CALIB_P2);
    dig_P3 = i2c_smbus_read_word_data(client, REG_CALIB_P3);
    dig_P4 = i2c_smbus_read_word_data(client, REG_CALIB_P4);
    dig_P5 = i2c_smbus_read_word_data(client, REG_CALIB_P5);
    dig_P6 = i2c_smbus_read_word_data(client, REG_CALIB_P6);
    dig_P7 = i2c_smbus_read_word_data(client, REG_CALIB_P7);
    dig_P8 = i2c_smbus_read_word_data(client, REG_CALIB_P8);
    dig_P9 = i2c_smbus_read_word_data(client, REG_CALIB_P9);

    if (!dig_T1 || !dig_P1)
        return -EIO;

    return 0;
}
static void bmp280_configure(struct i2c_client *client)
{
    /* standby 250ms, IIR filter x4 */
    i2c_smbus_write_byte_data(client, REG_CONFIG,
                              T_SB_250MS | FILTER_4);

    /* temp x2, pressure x16, normal mode */
    i2c_smbus_write_byte_data(client, REG_CTRL_MEAS,
                              OSRS_T_x2 | OSRS_P_x16 | MODE_NORMAL);
}

/* ---------- temperature ---------- */
static int bmp280_read_temp(int *temp)
{
    s32 adc_T, var1, var2;
    int msb, lsb, xlsb;

    msb  = i2c_smbus_read_byte_data(bmp_client, REG_TEMP_MSB);
    lsb  = i2c_smbus_read_byte_data(bmp_client, REG_TEMP_MSB + 1);
    xlsb = i2c_smbus_read_byte_data(bmp_client, REG_TEMP_MSB + 2);

    adc_T = (msb << 12) | (lsb << 4) | (xlsb >> 4);

    var1 = ((((adc_T >> 3) - ((s32)dig_T1 << 1))) *
            ((s32)dig_T2)) >> 11;

    var2 = (((((adc_T >> 4) - ((s32)dig_T1)) *
              ((adc_T >> 4) - ((s32)dig_T1))) >> 12) *
            ((s32)dig_T3)) >> 14;

    t_fine = var1 + var2;
    *temp = (t_fine * 5 + 128) >> 8;
    *temp /= 100;

    return 0;
}

/* ---------- pressure ---------- */
static int bmp280_read_pressure(int *pressure)
{
    s32 adc_P;
    s64 var1, var2, p;
    int msb, lsb, xlsb;

    msb  = i2c_smbus_read_byte_data(bmp_client, REG_PRESS_MSB);
    lsb  = i2c_smbus_read_byte_data(bmp_client, REG_PRESS_MSB + 1);
    xlsb = i2c_smbus_read_byte_data(bmp_client, REG_PRESS_MSB + 2);

    adc_P = (msb << 12) | (lsb << 4) | (xlsb >> 4);

    var1 = ((s64)t_fine) - 128000;
    var2 = var1 * var1 * dig_P6;
    var2 += (var1 * dig_P5) << 17;
    var2 += ((s64)dig_P4) << 35;
    var1 = ((var1 * var1 * dig_P3) >> 8) +
           ((var1 * dig_P2) << 12);
    var1 = (((((s64)1) << 47) + var1) * dig_P1) >> 33;

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (dig_P9 * (p >> 13) * (p >> 13)) >> 25;
    var2 = (dig_P8 * p) >> 19;

    p = ((p + var1 + var2) >> 8) + ((s64)dig_P7 << 4);
    *pressure = p / 256;

    return 0;
}
static ssize_t bmp_read(struct file *file,
                        char __user *buf,
                        size_t len, loff_t *off)
{
    char kbuf[96];
    int t, p, n;

    /* Always allow read */
    *off = 0;

    bmp280_read_temp(&t);
    bmp280_read_pressure(&p);

    n = snprintf(kbuf, sizeof(kbuf),
                 "Temp: %d C  Pressure: %d Pa\n", t, p);

    if (copy_to_user(buf, kbuf, n))
        return -EFAULT;

    return n;
}

static const struct file_operations bmp_fops = {
    .owner = THIS_MODULE,
    .read  = bmp_read,
};

/* ---------- probe ---------- */
static int bmp280_probe(struct i2c_client *client)
{
    int chip;

    bmp_client = client;

    chip = i2c_smbus_read_byte_data(client, REG_CHIP_ID);
    if (chip != BMP280_CHIP_ID)
        return -ENODEV;

    bmp280_read_calib(client);
    bmp280_configure(client);

    alloc_chrdev_region(&dev_num, 0, 1, DEV_NAME);
    cdev_init(&bmp_cdev, &bmp_fops);
    cdev_add(&bmp_cdev, dev_num, 1);

    bmp_class = class_create(DEV_NAME);
    bmp_class->devnode = bmp_devnode;
    device_create(bmp_class, NULL, dev_num, NULL, DEV_NAME);

    pr_info("BMP280 normal-mode driver loaded\n");
    return 0;
}

static void bmp280_remove(struct i2c_client *client)
{
    device_destroy(bmp_class, dev_num);
    class_destroy(bmp_class);
    cdev_del(&bmp_cdev);
    unregister_chrdev_region(dev_num, 1);
}

static const struct of_device_id bmp280_of_match[] = {
    { .compatible = "bosch,bmp280" },
    { }
};
MODULE_DEVICE_TABLE(of, bmp280_of_match);

static struct i2c_driver bmp280_driver = {
    .driver = {
        .name = "bmp280_final",
        .of_match_table = bmp280_of_match,
    },
    .probe  = bmp280_probe,
    .remove = bmp280_remove,
};

module_i2c_driver(bmp280_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Subbu");
MODULE_DESCRIPTION("BMP280 Final Normal-Mode Temp + Pressure Driver");