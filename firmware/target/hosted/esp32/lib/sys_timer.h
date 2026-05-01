#ifndef __SYSTIMER_H__
#define __SYSTIMER_H__

#include <stdint.h>
#include <stdbool.h>
#include "sys_thread.h"

typedef uint32_t (*timer_callback_ptr)(uint32_t interval, void *param);

void sys_ticks_init(void);
void sys_ticks_quit(void);
uint32_t sys_get_ticks(void);
uint64_t sys_get_ticks64(void);
void sys_delay(uint32_t ms);

int  sys_timer_init(void);
void sys_timer_quit(void);
int  sys_add_timer(uint32_t interval_ms, timer_callback_ptr callback, void *param);
bool sys_remove_timer(int id);

#endif /* __SYSTIMER_H__ */
