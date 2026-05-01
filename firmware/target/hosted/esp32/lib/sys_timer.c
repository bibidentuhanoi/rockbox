#include <string.h>
#include "sys_timer.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define MAX_TIMERS 16

typedef struct {
    esp_timer_handle_t handle;
    timer_callback_ptr callback;
    void *param;
    uint32_t interval_ms;
    bool active;
} sys_timer_entry_t;

static sys_timer_entry_t timers[MAX_TIMERS];

static void esp_timer_cb(void *arg)
{
    sys_timer_entry_t *e = (sys_timer_entry_t *)arg;
    if (e->active && e->callback)
        e->callback(e->interval_ms, e->param);
}

int sys_timer_init(void)
{
    memset(timers, 0, sizeof(timers));
    return 0;
}

void sys_timer_quit(void)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (timers[i].active) {
            esp_timer_stop(timers[i].handle);
            esp_timer_delete(timers[i].handle);
            timers[i].active = false;
        }
    }
}

int sys_add_timer(uint32_t interval_ms, timer_callback_ptr callback, void *param)
{
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!timers[i].active) {
            timers[i].callback    = callback;
            timers[i].param       = param;
            timers[i].interval_ms = interval_ms;
            timers[i].active      = true;

            esp_timer_create_args_t args = {
                .callback = esp_timer_cb,
                .arg      = &timers[i],
                .name     = "rb_timer",
            };
            esp_timer_create(&args, &timers[i].handle);
            esp_timer_start_periodic(timers[i].handle,
                                     (uint64_t)interval_ms * 1000ULL);
            return i + 1; /* 1-based id */
        }
    }
    return 0; /* no slot */
}

bool sys_remove_timer(int id)
{
    if (id < 1 || id > MAX_TIMERS) return false;
    sys_timer_entry_t *e = &timers[id - 1];
    if (!e->active) return false;
    esp_timer_stop(e->handle);
    esp_timer_delete(e->handle);
    e->active = false;
    return true;
}

uint32_t sys_get_ticks(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

uint64_t sys_get_ticks64(void)
{
    return (uint64_t)(esp_timer_get_time() / 1000ULL);
}

void sys_delay(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}
