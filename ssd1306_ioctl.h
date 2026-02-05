#ifndef _SSD1306_IOCTL_H_
#define _SSD1306_IOCTL_H_
#define OLED_FORCE_ON   _IO('o', 10)
#define OLED_FORCE_RAM  _IO('o', 11)
#include <linux/ioctl.h>
#define OLED_MAGIC 'o'
#define OLED_CLEAR _IO(OLED_MAGIC, 1)
#endif
