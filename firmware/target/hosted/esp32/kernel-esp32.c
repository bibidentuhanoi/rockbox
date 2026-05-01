/*
 * kernel-esp32.c — Hybrid tick for ESP32.
 *
 * Approach C: A lightweight esp_timer callback increments current_tick
 * (safe — trivial work, no mutexes, no stack concerns).  The actual
 * tick task callbacks are processed inside switch_thread(), which runs
 * in the cooperative Rockbox context with proper stack and locking.
 *
 * This eliminates all cross-task mutex contention and WDT starvation
 * that plagued the dedicated-task and esp_timer-callback approaches.
 */
#include <stdlib.h>
#include "config.h"
#include "debug.h"
#include "system-esp32.h"
#include "lib/sys_timer.h"
#include "kernel-internal.h"
#include "panic.h"
#include "esp_timer.h"


extern long start_tick;

/* Tick counter that the esp_timer callback bumps.  switch_thread()
   compares this against current_tick to know how many tick tasks
   are pending. */
static volatile long timer_tick = 0;

static esp_timer_handle_t tick_timer_handle = NULL;

/* Called from esp_timer task — must be trivial (no mutexes, no mallocs). */
static void tick_timer_cb(void *arg)
{
    (void)arg;
    timer_tick++;
}

/* Called from switch_thread() in cooperative context.
   Processes any pending tick callbacks.  Safe: runs under rb_mutex
   with full thread stack. */
void esp32_process_pending_ticks(void)
{
    while (current_tick < timer_tick)
    {
        call_tick_tasks();   /* increments current_tick internally */
    }
}

void tick_start(unsigned int interval_in_ms)
{
    if (!sim_kernel_init())
    {
        DEBUGF("KERNEL: tick_start: sim_kernel_init failed!\n");
        panicf("Could not initialize kernel!");
        return;
    }

    /* Stop existing timer if restarting */
    if (tick_timer_handle != NULL)
    {
        esp_timer_stop(tick_timer_handle);
        esp_timer_delete(tick_timer_handle);
        tick_timer_handle = NULL;
    }
    else
    {
        start_tick = sys_get_ticks();
    }

    timer_tick = 0;
    current_tick = 0;

    esp_timer_create_args_t args = {
        .callback = tick_timer_cb,
        .arg      = NULL,
        .name     = "rb_tick",
    };
    esp_timer_create(&args, &tick_timer_handle);
    esp_timer_start_periodic(tick_timer_handle,
                             (uint64_t)interval_in_ms * 1000ULL);
}
