#ifndef _SYSTEM_ESP32_H_
#define _SYSTEM_ESP32_H_

#include <stdbool.h>
#include <stdint.h>
#include "config.h"
#include "gcc_extensions.h"

#define HIGHEST_IRQ_LEVEL 1

int set_irq_level(int level);

#define disable_irq() \
    ((void)set_irq_level(HIGHEST_IRQ_LEVEL))

#define enable_irq() \
    ((void)set_irq_level(0))

#define disable_irq_save() \
    set_irq_level(HIGHEST_IRQ_LEVEL)

#define restore_irq(level) \
    ((void)set_irq_level(level))

void wait_for_interrupt(void);

#include "system-hosted.h"

bool sim_kernel_init(void);
void sim_enter_irq_handler(void);
void sim_exit_irq_handler(void);
void sim_kernel_shutdown(void);
void sys_poweroff(void);
void sim_do_exit(void) NORETURN_ATTR;

extern long start_tick;

#endif /* _SYSTEM_ESP32_H_ */
