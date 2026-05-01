/*
 * thread-esp32.c — Cooperative threading for ESP32 using FreeRTOS.
 * Translated from thread-ctru.c (Nintendo 3DS port).
 * One global recursive mutex serialises all Rockbox threads (cooperative sim).
 */
#include "autoconf.h"
#include "debug.h"

#include <stdbool.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "system-esp32.h"
#include "thread-esp32.h"
#include "lib/sys_thread.h"
#include "lib/sys_timer.h"
#include "../kernel-internal.h"
#include "core_alloc.h"


/* Define this as 1 to show informational messages that are not errors. */
#define THREAD_DEBUGF_ENABLED 0

#if THREAD_DEBUGF_ENABLED
#define THREAD_DEBUGF(...) DEBUGF(__VA_ARGS__);
static char __name[sizeof (((struct thread_debug_info *)0)->name)];
#define THREAD_GET_NAME(thread) \
    ({ format_thread_name(__name, sizeof (__name), thread); __name; })
#else
#define THREAD_DEBUGF(...);
#define THREAD_GET_NAME(thread)
#endif

#define THREAD_PANICF(str...) \
    ({ fprintf(stderr, str); exit(-1); })

/* Jump buffers for graceful exit - kernel threads don't stay neatly
 * in their start routines responding to messages so this is the only
 * way to get them back in there so they may exit */
static jmp_buf thread_jmpbufs[MAXTHREADS];
/* this mutex locks out other Rockbox threads while one runs,
 * that enables us to simulate a cooperative environment even if
 * the host is preemptive */
static SemaphoreHandle_t rb_mutex = NULL;
#define THREADS_RUN                 0
#define THREADS_EXIT                1
#define THREADS_EXIT_COMMAND_DONE   2
static volatile int threads_status = THREADS_RUN;

extern long start_tick;

void sim_thread_shutdown(void)
{
    int i;

    /* This *has* to be a push operation from a thread not in the pool
       so that they may be dislodged from their blocking calls. */

    /* Do this before trying to acquire lock */
    threads_status = THREADS_EXIT;

    /* Take control */
    xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);

    /* Signal all threads on delay or block */
    for (i = 0; i < MAXTHREADS; i++)
    {
        struct thread_entry *thread = __thread_slot_entry(i);
        if (thread->context.s == NULL)
            continue;
        xSemaphoreGive((SemaphoreHandle_t)thread->context.s);
    }

    /* Wait for all threads to finish and cleanup old ones. */
    for (i = 0; i < MAXTHREADS; i++)
    {
        struct thread_entry *thread = __thread_slot_entry(i);
        sysThread *t = thread->context.t;

        if (t != NULL)
        {
            xSemaphoreGiveRecursive(rb_mutex);
            /* Wait for it to finish */
            sys_wait_thread(t, NULL);
            /* Relock for next thread signal */
            xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);
            /* Already waited and exiting thread would have waited .told,
             * replacing it with t. */
            thread->context.told = NULL;
        }
        else
        {
            /* Wait on any previous thread in this location-- could be one not
             * quite finished exiting but has just unlocked the mutex. If it's
             * NULL, the call returns immediately. */
            sys_wait_thread(thread->context.told, NULL);
        }
    }

    xSemaphoreGiveRecursive(rb_mutex);

    /* Signal completion of operation */
    threads_status = THREADS_EXIT_COMMAND_DONE;
}

void sim_thread_exception_wait(void)
{
    while (1)
    {
        sys_delay(HZ/10);
        if (threads_status != THREADS_RUN)
            thread_exit();
    }
}

/* A way to yield and leave the threading system for extended periods */
void sim_thread_lock(void *me)
{
    xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);
    __running_self_entry() = (struct thread_entry *)me;

    if (threads_status != THREADS_RUN)
        thread_exit();
}

void * sim_thread_unlock(void)
{
    struct thread_entry *current = __running_self_entry();
    xSemaphoreGiveRecursive(rb_mutex);
    return current;
}

bool sim_thread_holds_mutex(void)
{
    if (!rb_mutex)
        return false;
    return xSemaphoreGetMutexHolder(rb_mutex) == xTaskGetCurrentTaskHandle();
}

static void *vfs_yield_enter_cb(void)
{
    if (sim_thread_holds_mutex())
        return sim_thread_unlock();
    return NULL;
}

static void vfs_yield_leave_cb(void *me)
{
    if (me) sim_thread_lock(me);
}

void switch_thread(void)
{
    struct thread_entry *current = __running_self_entry();

    /* Process any pending tick callbacks in cooperative context
       (see kernel-esp32.c approach C). */
    extern void esp32_process_pending_ticks(void);
    esp32_process_pending_ticks();

    enable_irq();

    switch (current->state)
    {
    case STATE_RUNNING:
    {
        xSemaphoreGiveRecursive(rb_mutex);
        /* Let FreeRTOS IDLE task run (feeds soft WDT, runs cleanup).
           Without this, the cooperative sim starves all lower-priority
           tasks because rb_mutex unlock/relock alone only gives time
           to tasks *contending* for the mutex, not IDLE. */
        taskYIELD();
        /* Any other thread waiting already will get it first */
        xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);
        break;
        } /* STATE_RUNNING: */

    case STATE_BLOCKED:
    {
        int oldlevel;

        xSemaphoreGiveRecursive(rb_mutex);
        sys_sem_wait((SemaphoreHandle_t)current->context.s);
        xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);

        oldlevel = disable_irq_save();
        current->state = STATE_RUNNING;
        restore_irq(oldlevel);
        break;
        } /* STATE_BLOCKED: */

    case STATE_BLOCKED_W_TMO:
    {
        int result, oldlevel;

        xSemaphoreGiveRecursive(rb_mutex);
        result = sys_sem_wait_timeout((SemaphoreHandle_t)current->context.s, current->tmo_tick);
        xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);

        oldlevel = disable_irq_save();

        current->state = STATE_RUNNING;

        if (result != 0)
        {
            /* Other signals from an explicit wake could have been made before
             * arriving here if we timed out waiting for the semaphore. Make
             * sure the count is reset. */
            while (sys_sem_value((SemaphoreHandle_t)current->context.s) > 0)
                sys_sem_try_wait((SemaphoreHandle_t)current->context.s);
        }

        restore_irq(oldlevel);
        break;
        } /* STATE_BLOCKED_W_TMO: */

    case STATE_SLEEPING:
    {
        /* Break the sleep into tick-sized chunks so that tick tasks
           (especially button_tick) run at their normal ~HZ rate instead
           of being batch-processed in one burst when sleep finishes.
           Without this, plugins using rb->sleep + rb->button_get(false)
           miss button events entirely. */
        int remaining_ms = current->tmo_tick;
        const int chunk_ms = (1000 / HZ);  /* one Rockbox tick period */

        while (remaining_ms > 0) {
            int wait_ms = remaining_ms < chunk_ms ? remaining_ms : chunk_ms;
            xSemaphoreGiveRecursive(rb_mutex);
            int result = sys_sem_wait_timeout(
                (SemaphoreHandle_t)current->context.s, wait_ms);
            xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);
            __running_self_entry() = current;

            esp32_process_pending_ticks();

            if (result == 0)
                break;  /* signaled to wake early */

            remaining_ms -= wait_ms;
        }
        current->state = STATE_RUNNING;
        break;
        } /* STATE_SLEEPING: */
    }

#ifdef BUFLIB_DEBUG_CHECK_VALID
    core_check_valid();
#endif
    __running_self_entry() = current;

    if (threads_status != THREADS_RUN)
        thread_exit();
}

void sleep_thread(int ticks)
{
    struct thread_entry *current = __running_self_entry();
    int rem;

    current->state = STATE_SLEEPING;

    rem = (sys_get_ticks() - start_tick) % (1000/HZ);
    if (rem < 0)
        rem = 0;

    current->tmo_tick = (1000/HZ) * ticks + ((1000/HZ)-1) - rem;
}

void block_thread_(struct thread_entry *current, int ticks)
{
    if (ticks < 0)
        current->state = STATE_BLOCKED;
    else
    {
        current->state = STATE_BLOCKED_W_TMO;
        current->tmo_tick = (1000/HZ)*ticks;
    }

    wait_queue_register(current);
}

unsigned int wakeup_thread_(struct thread_entry *thread
                            IF_PRIO(, enum wakeup_thread_protocol proto))
{
    switch (thread->state)
    {
    case STATE_BLOCKED:
    case STATE_BLOCKED_W_TMO:
        wait_queue_remove(thread);
        thread->state = STATE_RUNNING;
        xSemaphoreGive((SemaphoreHandle_t)thread->context.s);
        return THREAD_OK;
    }

    return THREAD_NONE;
}

void thread_thaw(unsigned int thread_id)
{
    struct thread_entry *thread = __thread_id_entry(thread_id);

    if (thread->id == thread_id && thread->state == STATE_FROZEN)
    {
        thread->state = STATE_RUNNING;
        xSemaphoreGive((SemaphoreHandle_t)thread->context.s);
    }
}

static int runthread(void *data)
{
    /* Cannot access thread variables before locking the mutex as the
       data structures may not be filled-in yet. */
    xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);

    struct thread_entry *current = (struct thread_entry *)data;
    __running_self_entry() = current;

    jmp_buf *current_jmpbuf = &thread_jmpbufs[THREAD_ID_SLOT(current->id)];

    /* Setup jump for exit */
    if (setjmp(*current_jmpbuf) == 0)
    {
        /* Run the thread routine */
        if (current->state == STATE_FROZEN)
        {
            xSemaphoreGiveRecursive(rb_mutex);
            sys_sem_wait((SemaphoreHandle_t)current->context.s);
            xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);
            __running_self_entry() = current;
        }

        if (threads_status == THREADS_RUN)
        {
            current->context.start();
            THREAD_DEBUGF("Thread Done: %d (%s)\n",
                              THREAD_ID_SLOT(current->id),
                              THREAD_GET_NAME(current));
            /* Thread routine returned - suicide */
        }

        thread_exit();
    }
    else
    {
        /* Unlock and exit */
        xSemaphoreGiveRecursive(rb_mutex);
    }

    return 0;
}

unsigned int create_thread(void (*function)(void),
                           void* stack, size_t stack_size,
                           unsigned flags, const char *name
                           IF_PRIO(, int priority)
                           IF_COP(, unsigned int core))
{
    THREAD_DEBUGF("Creating thread: (%s)\n", name ? name : "");

    struct thread_entry *thread = thread_alloc();
    if (thread == NULL)
    {
        DEBUGF("THREAD: create_thread: no free thread slot for '%s'\n", name ? name : "?");
        return 0;
    }

    SemaphoreHandle_t s = xSemaphoreCreateCounting(255, 0);
    if (s == NULL)
    {
        DEBUGF("THREAD: create_thread: semaphore alloc failed for '%s'\n", name ? name : "?");
        return 0;
    }

    /* Rockbox DEFAULT_STACK_SIZE can be as low as 0x100 on hosted builds,
       but FreeRTOS on ESP32 needs more per task for the scheduler and
       mutex overhead.  The codec thread needs extra for AAC IMDCT. */
    if (name && strcmp(name, "codec") == 0) {
        if (stack_size < 32768)
            stack_size = 32768;
    } else {
        if (stack_size < 4096)
            stack_size = 4096;
    }

    sysThread *t = sys_create_thread(runthread,
                                     name,
                                     stack_size,
                                     thread
                                     );
    if (t == NULL)
    {
        DEBUGF("THREAD: create_thread: sys_create_thread failed for '%s'\n", name ? name : "?");
        vSemaphoreDelete(s);
        return 0;
    }

    thread->name = name;
    thread->state = (flags & CREATE_THREAD_FROZEN) ?
        STATE_FROZEN : STATE_RUNNING;
    thread->context.start = function;
    thread->context.t = t;
    thread->context.s = (void *)s;

    THREAD_DEBUGF("New Thread: %lu (%s)\n",
                      (unsigned long)thread->id,
                      THREAD_GET_NAME(thread));

    return thread->id;
    (void)stack;
}

void thread_exit(void)
{
    struct thread_entry *current = __running_self_entry();

    int oldlevel = disable_irq_save();

    sysThread *t = current->context.t;
    SemaphoreHandle_t s = (SemaphoreHandle_t)current->context.s;

    /* Wait the last thread here and keep this one or it will leak since
     * it doesn't free its own allocations unless a wait is performed.
     * Such behavior guards against the memory being invalid by the time
     * sys_wait_thread is reached and also against two different threads having
     * the same pointer. It also makes sys_wait_thread a non-concurrent function.
     */
    sys_wait_thread(current->context.told, NULL);

    current->context.t = NULL;
    current->context.s = NULL;
    current->context.told = t;

    unsigned int id = current->id;
    new_thread_id(current);
    current->state = STATE_KILLED;
    wait_queue_wake(&current->queue);

    vSemaphoreDelete(s);

    /* Do a graceful exit - perform the longjmp back into the thread
       function to return */
    restore_irq(oldlevel);

    thread_free(current);

    longjmp(thread_jmpbufs[THREAD_ID_SLOT(id)], 1);

    /* This should never and must never be reached - if it is, the
     * state is corrupted */
    THREAD_PANICF("thread_exit->K:*R (ID: %d)", id);
    while (1);
}

void thread_wait(unsigned int thread_id)
{
    struct thread_entry *current = __running_self_entry();
    struct thread_entry *thread = __thread_id_entry(thread_id);

    if (thread->id == thread_id && thread->state != STATE_KILLED)
    {
        block_thread(current, TIMEOUT_BLOCK, &thread->queue, NULL);
        switch_thread();
    }
}

extern void stream_vfs_set_yield(void *(*)(void), void (*)(void *));

/* Initialize threading */
void init_threads(void)
{
    rb_mutex = xSemaphoreCreateRecursiveMutex();
    xSemaphoreTakeRecursive(rb_mutex, portMAX_DELAY);

    stream_vfs_set_yield(vfs_yield_enter_cb, vfs_yield_leave_cb);

    thread_alloc_init();

    struct thread_entry *thread = thread_alloc();
    if (thread == NULL)
    {
        DEBUGF("THREAD: init_threads: main thread alloc failed\n");
        return;
    }

    /* Slot 0 is reserved for the main thread - initialize it here and
       then create the thread - it is possible to have a quick, early
       shutdown try to access the structure. */
    thread->name = __main_thread_name;
    thread->state = STATE_RUNNING;
    thread->context.s = (void *)xSemaphoreCreateCounting(255, 0);
    thread->context.t = NULL; /* NULL for the implicit main thread */
    __running_self_entry() = thread;

    if (thread->context.s == NULL)
    {
        DEBUGF("THREAD: init_threads: failed to create main semaphore\n");
        return;
    }

    /* Setup jump for exit */
    if (setjmp(thread_jmpbufs[THREAD_ID_SLOT(thread->id)]) == 0)
    {
        THREAD_DEBUGF("Main Thread: %lu (%s)\n",
                          (unsigned long)thread->id,
                          THREAD_GET_NAME(thread));
        return;
    }

    xSemaphoreGiveRecursive(rb_mutex);

    /* Set to 'COMMAND_DONE' when other rockbox threads have exited. */
    while (threads_status < THREADS_EXIT_COMMAND_DONE)
        sys_delay(10);

    /* We're the main thread - perform exit - doesn't return. */
    sim_do_exit();
}
