#include "config.h"
#include "lcd.h"
#include "lcd-target.h"

extern unsigned short *esp32_lcd_get_fb(void);
extern void esp32_lcd_update(const unsigned short *src, int x, int y, int w, int h);

fb_data *dev_fb = NULL;

void sys_console_init(void) { }

void lcd_init_device(void)
{
    dev_fb = (fb_data *)esp32_lcd_get_fb();
}

void lcd_update(void)
{
    lcd_update_rect(0, 0, LCD_WIDTH, LCD_HEIGHT);
}

void lcd_update_rect(int x, int y, int width, int height)
{
    if (!dev_fb) return;

    if (y < 0) { height += y; y = 0; }
    if (y + height > LCD_HEIGHT) height = LCD_HEIGHT - y;
    if (height <= 0) return;

    esp32_lcd_update((const unsigned short *)FBADDR(0, y), x, y, width, height);
}

int lcd_get_dpi(void) { return 167; }
