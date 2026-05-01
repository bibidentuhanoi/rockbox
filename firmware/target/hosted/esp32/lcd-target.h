#ifndef LCD_TARGET_H
#define LCD_TARGET_H

#include "lcd.h"

extern fb_data *dev_fb;
#define LCD_FRAMEBUF_ADDR(col, row) (dev_fb + (row) * LCD_WIDTH + (col))

#endif
