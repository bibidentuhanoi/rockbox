#include "config.h"
#include "timer.h"
#include "lib/sys_timer.h"

static int timer_prio = -1;
static void (*global_timer_callback)(void);
static int timer_id = -1;

static uint32_t _timer_callback(uint32_t interval, void *param)
{
    (void)param;
    if (global_timer_callback)
        global_timer_callback();
    return interval;
}

#define cycles_to_ms(cycles) \
    ((uint32_t)((1000UL * (unsigned long)(cycles)) / TIMER_FREQ))

bool timer_register(int reg_prio, void (*unregister_callback)(void),
                    long cycles, void (*timer_callback)(void))
{
    (void)unregister_callback;
    if (reg_prio <= timer_prio || cycles == 0)
        return false;

    uint32_t ms = cycles_to_ms(cycles);
    if (ms == 0) ms = 1;

    timer_prio = reg_prio;
    global_timer_callback = timer_callback;
    timer_id = sys_add_timer(ms, _timer_callback, NULL);
    return true;
}

bool timer_set_period(long cycles)
{
    uint32_t ms = cycles_to_ms(cycles);
    if (ms == 0) ms = 1;
    sys_remove_timer(timer_id);
    timer_id = sys_add_timer(ms, _timer_callback, NULL);
    return true;
}

void timer_unregister(void)
{
    sys_remove_timer(timer_id);
    timer_prio = -1;
    timer_id = -1;
    global_timer_callback = NULL;
}
