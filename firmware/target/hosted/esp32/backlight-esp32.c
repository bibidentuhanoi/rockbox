#include "config.h"
#include "backlight.h"

extern void esp32_backlight_on(void);
extern void esp32_backlight_off(void);
extern void esp32_backlight_set(int brightness);

bool backlight_hw_init(void) { return true; }

void backlight_hw_on(void)  { esp32_backlight_on(); }
void backlight_hw_off(void) { esp32_backlight_off(); }

void backlight_hw_brightness(int brightness)
{
    esp32_backlight_set(brightness);
}
