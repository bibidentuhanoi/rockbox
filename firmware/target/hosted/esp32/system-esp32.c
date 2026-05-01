#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "system.h"
#include "kernel.h"
#include "thread-esp32.h"
#include "system-esp32.h"
extern void sys_console_init(void);
#include "panic.h"
#include "debug.h"
#include "lib/sys_timer.h"
#include "esp_timer.h"
#include "esp_freertos_hooks.h"

static bool idle_hook_cb(void);  /* forward decl — defined after wait_for_interrupt() */

const char *audiodev = NULL;

#ifdef DEBUG
bool debug_audio = false;
#endif

long start_tick = 0;

uintptr_t *stackbegin;
uintptr_t *stackend;

/* ---- Interrupt simulation state machine (translated from kernel-ctru.c) ---- */

/* Mutex to serialize changing levels and exclude other threads while
 * inside a handler. SEPARATE from rb_mutex in thread-esp32.c. */
static SemaphoreHandle_t sim_irq_mtx = NULL;
/* Binary semaphore replacing ctru's CondVar — works because we have
 * exactly one tick timer source (single waiter constraint). */
static SemaphoreHandle_t irq_gate_sem = NULL;
/* Level: 0 = enabled, not 0 = disabled */
static volatile int interrupt_level = HIGHEST_IRQ_LEVEL;
/* How many handlers waiting to run? */
static int handlers_pending = 0;
/* 1 = executing a handler; prevents signals in set_irq_level */
static int status_reg = 0;

bool sim_kernel_init(void)
{
    sim_irq_mtx = xSemaphoreCreateRecursiveMutex();
    if (sim_irq_mtx == NULL) {
        DEBUGF("SYSTEM: sim_kernel_init: failed to create sim_irq_mtx\n");
        return false;
    }

    irq_gate_sem = xSemaphoreCreateBinary();
    if (irq_gate_sem == NULL) {
        DEBUGF("SYSTEM: sim_kernel_init: failed to create irq_gate_sem\n");
        return false;
    }

    interrupt_level = HIGHEST_IRQ_LEVEL;
    handlers_pending = 0;
    status_reg = 0;
    /* Register light-sleep idle hook on CPU 0 */
    esp_register_freertos_idle_hook_for_cpu(idle_hook_cb, 0);

    return true;
}

int set_irq_level(int level)
{
    xSemaphoreTakeRecursive(sim_irq_mtx, portMAX_DELAY);

    int oldlevel = interrupt_level;

    if (status_reg == 0 && level == 0 && oldlevel != 0)
    {
        /* Not in a handler and "interrupts" going disabled->enabled;
         * signal any pending handler still waiting */
        if (handlers_pending > 0)
            xSemaphoreGive(irq_gate_sem);
    }

    interrupt_level = level;

    xSemaphoreGiveRecursive(sim_irq_mtx);
    return oldlevel;
}

void sim_enter_irq_handler(void)
{
    xSemaphoreTakeRecursive(sim_irq_mtx, portMAX_DELAY);
    handlers_pending++;

    while (interrupt_level != 0)
    {
        xSemaphoreGiveRecursive(sim_irq_mtx);
        xSemaphoreTake(irq_gate_sem, portMAX_DELAY);
        xSemaphoreTakeRecursive(sim_irq_mtx, portMAX_DELAY);
    }

    status_reg = 1;
    /* Note: sim_irq_mtx stays held — released by sim_exit_irq_handler */
}

void sim_exit_irq_handler(void)
{
    /* sim_irq_mtx is already held from sim_enter_irq_handler */
    if (--handlers_pending > 0)
        xSemaphoreGive(irq_gate_sem);

    status_reg = 0;
    xSemaphoreGiveRecursive(sim_irq_mtx);
}

void sim_kernel_shutdown(void)
{
    /* Enable interrupts so any pending handler can drain */
    interrupt_level = 0;
    if (handlers_pending > 0)
        xSemaphoreGive(irq_gate_sem);

    /* Spin until handlers finish */
    while (handlers_pending > 0)
        vTaskDelay(pdMS_TO_TICKS(10));

    if (sim_irq_mtx) { vSemaphoreDelete(sim_irq_mtx); sim_irq_mtx = NULL; }
    if (irq_gate_sem) { vSemaphoreDelete(irq_gate_sem); irq_gate_sem = NULL; }
}

void system_init(void)
{
    volatile uintptr_t stack = 0;
    stackbegin = stackend = (uintptr_t *)&stack;

    sys_console_init();
    sys_timer_init();
    start_tick = sys_get_ticks();
}

void system_reboot(void)
{
    sim_thread_exception_wait();
}

void system_exception_wait(void)
{
    system_reboot();
}

int hostfs_init(void)
{
    return 0;
}

#ifdef HAVE_STORAGE_FLUSH
int hostfs_flush(void)
{
    return 0;
}
#endif

void wait_for_interrupt(void)
{
    asm volatile ("waiti 0");
}

/* FreeRTOS idle hook — halts the CPU core until the next interrupt. */
static bool idle_hook_cb(void)
{
    wait_for_interrupt();
    return true;   /* keep calling us */
}

extern void esp32_power_off(void) __attribute__((noreturn));

void power_off(void)
{
    esp32_power_off();
}

void sim_do_exit(void)
{
    esp32_power_off();
}
