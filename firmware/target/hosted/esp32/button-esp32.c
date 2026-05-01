#include "config.h"
#include "button.h"

extern int esp32_button_read(void);

bool button_hold(void) { return false; }

void button_init_device(void) { }

int button_read_device(void)
{
    return esp32_button_read();
}

bool headphones_inserted(void) { return false; }
