// SPDX-License-Identifier: GPL-2.0
#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/gpio/consumer.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/ioctl.h>

#define DEVICE_NAME     "oled"
#define CLASS_NAME      "oled"

#define OLED_WIDTH      128
#define OLED_HEIGHT     64
#define OLED_PAGES      (OLED_HEIGHT/8)
#define OLED_FB_SIZE    (OLED_WIDTH * OLED_PAGES)

#define MAX_WRITE_BUF   256

/* ---------------- IOCTL interface ---------------- */
#define OLED_IOC_MAGIC      'o'
#define OLED_IOC_CLEAR      _IO(OLED_IOC_MAGIC, 0x01)
#define OLED_IOC_SET_CURSOR _IOW(OLED_IOC_MAGIC, 0x02, struct oled_cursor)
#define OLED_IOC_SET_CONTR  _IOW(OLED_IOC_MAGIC, 0x03, __u8)

struct oled_cursor {
    __u8 page; /* 0..7 */
    __u8 col;  /* 0..127 */
};

/* ---------------- Driver state ---------------- */
struct oled_dev {
    struct spi_device *spi;
    struct gpio_desc  *dc_gpio;
    struct gpio_desc  *rst_gpio;

    dev_t              devno;
    struct cdev        cdev;
    struct class      *cls;
    struct device     *dev;

    u8                *fb;          /* framebuffer pointer */
    struct mutex       lock;        /* serialize access */

    u8                 cur_page;
    u8                 cur_col;
    u8                 contrast;
};


static struct oled_dev *g_oled;
static struct oled_dev *g_oled;

/* ---------------- 5x7 ASCII FONT (32..126) ---------------- */
static const u8 font5x7[95][5] = {
    {0x00,0x00,0x00,0x00,0x00}, /* 32 ' ' */
    {0x00,0x00,0x5F,0x00,0x00}, /* 33 '!' */
    {0x00,0x07,0x00,0x07,0x00}, /* 34 '"' */
    {0x14,0x7F,0x14,0x7F,0x14}, /* 35 '#' */
    {0x24,0x2A,0x7F,0x2A,0x12}, /* 36 '$' */
    {0x23,0x13,0x08,0x64,0x62}, /* 37 '%' */
    {0x36,0x49,0x55,0x22,0x50}, /* 38 '&' */
    {0x00,0x05,0x03,0x00,0x00}, /* 39 ''' */
    {0x00,0x1C,0x22,0x41,0x00}, /* 40 '(' */
    {0x00,0x41,0x22,0x1C,0x00}, /* 41 ')' */
    {0x14,0x08,0x3E,0x08,0x14}, /* 42 '*' */
    {0x08,0x08,0x3E,0x08,0x08}, /* 43 '+' */
    {0x00,0x50,0x30,0x00,0x00}, /* 44 ',' */
    {0x08,0x08,0x08,0x08,0x08}, /* 45 '-' */
    {0x00,0x60,0x60,0x00,0x00}, /* 46 '.' */
    {0x20,0x10,0x08,0x04,0x02}, /* 47 '/' */
    {0x3E,0x51,0x49,0x45,0x3E}, /* 48 '0' */
    {0x00,0x42,0x7F,0x40,0x00}, /* 49 '1' */
    {0x42,0x61,0x51,0x49,0x46}, /* 50 '2' */
    {0x21,0x41,0x45,0x4B,0x31}, /* 51 '3' */
    {0x18,0x14,0x12,0x7F,0x10}, /* 52 '4' */
    {0x27,0x45,0x45,0x45,0x39}, /* 53 '5' */
    {0x3C,0x4A,0x49,0x49,0x30}, /* 54 '6' */
    {0x01,0x71,0x09,0x05,0x03}, /* 55 '7' */
    {0x36,0x49,0x49,0x49,0x36}, /* 56 '8' */
    {0x06,0x49,0x49,0x29,0x1E}, /* 57 '9' */
    {0x00,0x36,0x36,0x00,0x00}, /* 58 ':' */
    {0x00,0x56,0x36,0x00,0x00}, /* 59 ';' */
    {0x08,0x14,0x22,0x41,0x00}, /* 60 '<' */
    {0x14,0x14,0x14,0x14,0x14}, /* 61 '=' */
    {0x00,0x41,0x22,0x14,0x08}, /* 62 '>' */
    {0x02,0x01,0x51,0x09,0x06}, /* 63 '?' */
    {0x32,0x49,0x79,0x41,0x3E}, /* 64 '@' */
    {0x7E,0x11,0x11,0x11,0x7E}, /* 65 'A' */
    {0x7F,0x49,0x49,0x49,0x36}, /* 66 'B' */
    {0x3E,0x41,0x41,0x41,0x22}, /* 67 'C' */
    {0x7F,0x41,0x41,0x22,0x1C}, /* 68 'D' */
    {0x7F,0x49,0x49,0x49,0x41}, /* 69 'E' */
    {0x7F,0x09,0x09,0x09,0x01}, /* 70 'F' */
    {0x3E,0x41,0x49,0x49,0x7A}, /* 71 'G' */
    {0x7F,0x08,0x08,0x08,0x7F}, /* 72 'H' */
    {0x00,0x41,0x7F,0x41,0x00}, /* 73 'I' */
    {0x20,0x40,0x41,0x3F,0x01}, /* 74 'J' */
    {0x7F,0x08,0x14,0x22,0x41}, /* 75 'K' */
    {0x7F,0x40,0x40,0x40,0x40}, /* 76 'L' */
    {0x7F,0x02,0x0C,0x02,0x7F}, /* 77 'M' */
    {0x7F,0x04,0x08,0x10,0x7F}, /* 78 'N' */
    {0x3E,0x41,0x41,0x41,0x3E}, /* 79 'O' */
    {0x7F,0x09,0x09,0x09,0x06}, /* 80 'P' */
    {0x3E,0x41,0x51,0x21,0x5E}, /* 81 'Q' */
    {0x7F,0x09,0x19,0x29,0x46}, /* 82 'R' */
    {0x46,0x49,0x49,0x49,0x31}, /* 83 'S' */
    {0x01,0x01,0x7F,0x01,0x01}, /* 84 'T' */
    {0x3F,0x40,0x40,0x40,0x3F}, /* 85 'U' */
    {0x1F,0x20,0x40,0x20,0x1F}, /* 86 'V' */
    {0x7F,0x20,0x18,0x20,0x7F}, /* 87 'W' */
    {0x63,0x14,0x08,0x14,0x63}, /* 88 'X' */
    {0x07,0x08,0x70,0x08,0x07}, /* 89 'Y' */
    {0x61,0x51,0x49,0x45,0x43}, /* 90 'Z' */
    {0x00,0x7F,0x41,0x41,0x00}, /* 91 '[' */
    {0x02,0x04,0x08,0x10,0x20}, /* 92 '\' */
    {0x00,0x41,0x41,0x7F,0x00}, /* 93 ']' */
    {0x04,0x02,0x01,0x02,0x04}, /* 94 '^' */
    {0x40,0x40,0x40,0x40,0x40}, /* 95 '_' */
    {0x00,0x01,0x02,0x00,0x00}, /* 96 '`' */
    {0x20,0x54,0x54,0x54,0x78}, /* 97 'a' */
    {0x7F,0x48,0x44,0x44,0x38}, /* 98 'b' */
    {0x38,0x44,0x44,0x44,0x20}, /* 99 'c' */
    {0x38,0x44,0x44,0x48,0x7F}, /* 100 'd' */
    {0x38,0x54,0x54,0x54,0x18}, /* 101 'e' */
    {0x08,0x7E,0x09,0x01,0x02}, /* 102 'f' */
    {0x0C,0x52,0x52,0x52,0x3E}, /* 103 'g' */
    {0x7F,0x08,0x04,0x04,0x78}, /* 104 'h' */
    {0x00,0x44,0x7D,0x40,0x00}, /* 105 'i' */
    {0x20,0x40,0x44,0x3D,0x00}, /* 106 'j' */
    {0x7F,0x10,0x28,0x44,0x00}, /* 107 'k' */
    {0x00,0x41,0x7F,0x40,0x00}, /* 108 'l' */
    {0x7C,0x04,0x18,0x04,0x78}, /* 109 'm' */
    {0x7C,0x08,0x04,0x04,0x78}, /* 110 'n' */
    {0x38,0x44,0x44,0x44,0x38}, /* 111 'o' */
    {0x7C,0x14,0x14,0x14,0x08}, /* 112 'p' */
    {0x08,0x14,0x14,0x18,0x7C}, /* 113 'q' */
    {0x7C,0x08,0x04,0x04,0x08}, /* 114 'r' */
    {0x48,0x54,0x54,0x54,0x20}, /* 115 's' */
    {0x04,0x3F,0x44,0x40,0x20}, /* 116 't' */
    {0x3C,0x40,0x40,0x20,0x7C}, /* 117 'u' */
    {0x1C,0x20,0x40,0x20,0x1C}, /* 118 'v' */
    {0x3C,0x40,0x30,0x40,0x3C}, /* 119 'w' */
    {0x44,0x28,0x10,0x28,0x44}, /* 120 'x' */
    {0x0C,0x50,0x50,0x50,0x3C}, /* 121 'y' */
    {0x44,0x64,0x54,0x4C,0x44}, /* 122 'z' */
    {0x00,0x08,0x36,0x41,0x00}, /* 123 '{' */
    {0x00,0x00,0x7F,0x00,0x00}, /* 124 '|' */
    {0x00,0x41,0x36,0x08,0x00}, /* 125 '}' */
    {0x10,0x08,0x08,0x10,0x08}, /* 126 '~' */
};

/* ---------------- Low-level helpers ---------------- */
static inline void oled_cmd(struct oled_dev *od, u8 c)
{
    gpiod_set_value(od->dc_gpio, 0);
    spi_write(od->spi, &c, 1);
}
static inline void oled_data(struct oled_dev *od, const u8 *d, int len)
{
    gpiod_set_value(od->dc_gpio, 1);
    spi_write(od->spi, d, len);
}

/* Set GDDRAM cursor (page/col) */
static void oled_set_cursor(struct oled_dev *od, u8 page, u8 col)
{
    oled_cmd(od, 0xB0 | (page & 0x07));
    oled_cmd(od, 0x00 | (col & 0x0F));
    oled_cmd(od, 0x10 | ((col >> 4) & 0x0F));
}

/* Flush full framebuffer to panel */
static void oled_flush_all(struct oled_dev *od)
{
    int page;
    for (page = 0; page < OLED_PAGES; page++) {
        oled_set_cursor(od, page, 0);
        oled_data(od, &od->fb[page * OLED_WIDTH], OLED_WIDTH);
    }
}

/* Flush rectangular region (page-aligned vertically) */
static void oled_flush_rect(struct oled_dev *od, u8 page_start, u8 page_end, u8 col_start, u8 col_end)
{
    u8 p;
    if (page_end >= OLED_PAGES) page_end = OLED_PAGES - 1;
    if (col_end >= OLED_WIDTH) col_end = OLED_WIDTH - 1;

    for (p = page_start; p <= page_end; p++) {
        oled_set_cursor(od, p, col_start);
        oled_data(od, &od->fb[p * OLED_WIDTH + col_start], col_end - col_start + 1);
    }
}

/* Clear framebuffer and panel */
static void oled_clear(struct oled_dev *od)
{
    memset(od->fb, 0x00, OLED_FB_SIZE);
    oled_flush_all(od);
    od->cur_page = 0;
    od->cur_col  = 0;
}

/* Put a 5x7 glyph into framebuffer at current cursor (page/col) */
static void fb_put_glyph(struct oled_dev *od, u8 page, u8 col, const u8 glyph[5])
{
    int i;
    if (col + 5 >= OLED_WIDTH) return;
    for (i = 0; i < 5; i++) {
        od->fb[page * OLED_WIDTH + col + i] = glyph[i];
    }
}
/* Render one character, with 1-column spacing, wrapping by page */
static void oled_putc(struct oled_dev *od, char c)
{
    if (c == '\n') {
        od->cur_page = (od->cur_page + 1) % OLED_PAGES;
        od->cur_col  = 0;
        return;
    }
    if (c == '\f') { /* form-feed => clear */
        oled_clear(od);
        return;
    }
    if (c < 32 || c > 126) c = ' ';

    if (od->cur_col + 6 > OLED_WIDTH) {
        od->cur_page = (od->cur_page + 1) % OLED_PAGES;
        od->cur_col  = 0;
    }

    fb_put_glyph(od, od->cur_page, od->cur_col, font5x7[c - 32]);
    od->fb[od->cur_page * OLED_WIDTH + od->cur_col + 5] = 0x00; /* spacing */
    oled_flush_rect(od, od->cur_page, od->cur_page, od->cur_col, od->cur_col + 5);
    od->cur_col += 6;
}

/* ---------------- Character device ---------------- */
static ssize_t oled_write(struct file *f, const char __user *buf, size_t len, loff_t *off)
{
    struct oled_dev *od = g_oled;
    char kbuf[MAX_WRITE_BUF];
    size_t i;

    if (!od) return -ENODEV;
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    if (copy_from_user(kbuf, buf, len)) return -EFAULT;

    mutex_lock(&od->lock);
    for (i = 0; i < len; i++)
        oled_putc(od, kbuf[i]);
    mutex_unlock(&od->lock);

    return len;
}

static long oled_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
    struct oled_dev *od = g_oled;
    long ret = 0;

    if (!od) return -ENODEV;

    mutex_lock(&od->lock);

    switch (cmd) {
    case OLED_IOC_CLEAR:
        oled_clear(od);
        break;

    case OLED_IOC_SET_CURSOR: {
        struct oled_cursor cur;
        if (copy_from_user(&cur, (void __user *)arg, sizeof(cur))) {
            ret = -EFAULT;
            break;
        }
        if (cur.page >= OLED_PAGES || cur.col >= OLED_WIDTH) {
            ret = -EINVAL;
            break;
        }
        od->cur_page = cur.page;
        od->cur_col  = cur.col;
        break;
    }

    case OLED_IOC_SET_CONTR: {
        u8 c;
        if (copy_from_user(&c, (void __user *)arg, sizeof(c))) {
            ret = -EFAULT;
            break;
        }
        od->contrast = c;
        oled_cmd(od, 0x81);
        oled_cmd(od, od->contrast);
        break;
    }

    default:
        ret = -ENOTTY;
        break;
    }

    mutex_unlock(&od->lock);
    return ret;
}

static const struct file_operations oled_fops = {
    .owner          = THIS_MODULE,
    .write          = oled_write,
    .unlocked_ioctl = oled_ioctl,
};
/* ---------------- Hardware init ---------------- */
static void oled_hw_init(struct oled_dev *od)
{
    /* Hardware reset */
    gpiod_set_value(od->rst_gpio, 0);
    msleep(20);
    gpiod_set_value(od->rst_gpio, 1);
    msleep(20);

    /* SSD1306 init (128x64) */
    oled_cmd(od, 0xAE);                 /* Display OFF */
    oled_cmd(od, 0xD5); oled_cmd(od, 0x80); /* Clock divide */
    oled_cmd(od, 0xA8); oled_cmd(od, 0x3F); /* Multiplex 1/64 */
    oled_cmd(od, 0xD3); oled_cmd(od, 0x00); /* Display offset */
    oled_cmd(od, 0x40);                 /* Start line = 0 */
    oled_cmd(od, 0x8D); oled_cmd(od, 0x14); /* Charge pump ON */
    oled_cmd(od, 0x20); oled_cmd(od, 0x00); /* Memory mode: horizontal */
    oled_cmd(od, 0xA1);                 /* Segment remap */
    oled_cmd(od, 0xC8);                 /* COM scan dec */
    oled_cmd(od, 0xDA); oled_cmd(od, 0x12); /* COM pins */
    oled_cmd(od, 0x81); oled_cmd(od, 0x7F); /* Contrast */
    oled_cmd(od, 0xD9); oled_cmd(od, 0xF1); /* Pre-charge */
    oled_cmd(od, 0xDB); oled_cmd(od, 0x40); /* VCOM detect */
    oled_cmd(od, 0xA4);                 /* Resume RAM content */
    oled_cmd(od, 0xA6);                 /* Normal display */
    oled_cmd(od, 0xAF);                 /* Display ON */

    od->contrast = 0x7F;
    od->cur_page = 0;
    od->cur_col  = 0;

    memset(od->fb, 0x00, OLED_FB_SIZE);
    oled_flush_all(od);
}

/* ---------------- Probe/remove ---------------- */
static int oled_probe(struct spi_device *spi)
{
    struct oled_dev *od;
    int ret;

    od = kzalloc(sizeof(*od), GFP_KERNEL);
    if (!od) return -ENOMEM;

    od->fb = kzalloc(OLED_FB_SIZE, GFP_KERNEL);
    if (!od->fb) { ret = -ENOMEM; goto err_free_dev; }

    mutex_init(&od->lock);

    od->spi = spi;
    spi->mode = SPI_MODE_0;
    spi->max_speed_hz = 8000000;
    ret = spi_setup(spi);
    if (ret) goto err_free_fb;

    od->dc_gpio  = devm_gpiod_get(&spi->dev, "dc", GPIOD_OUT_LOW);
    od->rst_gpio = devm_gpiod_get(&spi->dev, "reset", GPIOD_OUT_HIGH);
    if (IS_ERR(od->dc_gpio) || IS_ERR(od->rst_gpio)) { ret = -ENODEV; goto err_free_fb; }

    ret = alloc_chrdev_region(&od->devno, 0, 1, DEVICE_NAME);
    if (ret) goto err_free_fb;

    cdev_init(&od->cdev, &oled_fops);
    ret = cdev_add(&od->cdev, od->devno, 1);
    if (ret) goto err_unreg;

    od->cls = class_create(CLASS_NAME);
    if (IS_ERR(od->cls)) { ret = PTR_ERR(od->cls); goto err_cdev; }

    od->dev = device_create(od->cls, NULL, od->devno, NULL, DEVICE_NAME);
    if (IS_ERR(od->dev)) { ret = PTR_ERR(od->dev); goto err_class; }

    g_oled = od;
    oled_hw_init(od);

    dev_info(&spi->dev, "SSD1306 SPI OLED ready: /dev/%s\n", DEVICE_NAME);
    return 0;

err_class:
    class_destroy(od->cls);
err_cdev:
    cdev_del(&od->cdev);
err_unreg:
    unregister_chrdev_region(od->devno, 1);
err_free_fb:
    kfree(od->fb);
err_free_dev:
    kfree(od);
    return ret;
}

static void oled_remove(struct spi_device *spi)
{
    struct oled_dev *od = g_oled;
    if (!od) return;

    device_destroy(od->cls, od->devno);
    class_destroy(od->cls);
    cdev_del(&od->cdev);
    unregister_chrdev_region(od->devno, 1);

    kfree(od->fb);
    kfree(od);
    g_oled = NULL;
}

/* ---------------- DT & ID tables ---------------- */
static const struct of_device_id oled_of_match[] = {
    { .compatible = "ssd1306-spi" },
    { }
};
MODULE_DEVICE_TABLE(of, oled_of_match);

static const struct spi_device_id oled_spi_ids[] = {
    { "ssd1306-spi", 0 },
    { }
};
MODULE_DEVICE_TABLE(spi, oled_spi_ids);

static struct spi_driver oled_driver = {
    .driver = {
        .name = "ssd1306-spi",
        .of_match_table = oled_of_match,
    },
    .probe    = oled_probe,
    .remove   = oled_remove,
    .id_table = oled_spi_ids,
};

module_spi_driver(oled_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("You");
MODULE_DESCRIPTION("SSD1306 SPI OLED driver with framebuffer and IOCTLs");





