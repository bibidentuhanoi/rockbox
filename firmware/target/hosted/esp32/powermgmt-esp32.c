#include "config.h"
#include "powermgmt.h"
#include "power.h"

int _battery_level(void)
{
    return 100;
}

unsigned int power_input_status(void)
{
    return POWER_INPUT_NONE;
}

bool charging_state(void)
{
    return false;
}

void ide_power_enable(bool on)
{
    (void)on;
}

bool ide_powered(void)
{
    return true;
}
