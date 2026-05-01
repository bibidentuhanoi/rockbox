#include <stdlib.h>
#include <string.h>
#include "sys_thread.h"
#include "debug.h"
#include "esp_heap_caps.h"

#define PSRAM_STACK_THRESHOLD 8192
#define CODEC_INTERNAL_STACK_CAP 16384

static int thread_id_counter = 0;

static int needs_internal_ram(const char *name)
{
    /* Threads that call dlopen (SPI flash cache flush) need internal RAM stacks */
    return name && strcmp(name, "codec") == 0;
}

static void thread_trampoline(void *arg)
{
    sysThread *t = (sysThread *)arg;
    t->status = t->userfunc(t->userdata);
    t->state = THREAD_STATE_ZOMBIE;
    vTaskDelete(NULL);
}

sysThread *sys_create_thread(int (*fn)(void *), const char *name,
                              const size_t stacksize, void *data)
{
    sysThread *t = calloc(1, sizeof(sysThread));
    if (!t) return NULL;

    t->threadid  = ++thread_id_counter;
    t->userfunc  = fn;
    t->userdata  = data;
    t->stacksize = stacksize ? stacksize : 4096;
    t->state     = THREAD_STATE_ALIVE;

    int prio = 5;
    int core = 0;
    t->stack_buf = NULL;

    int force_internal = needs_internal_ram(name);

    if (force_internal && t->stacksize > CODEC_INTERNAL_STACK_CAP)
        t->stacksize = CODEC_INTERNAL_STACK_CAP;

    if (!force_internal && t->stacksize >= PSRAM_STACK_THRESHOLD) {
        t->stack_buf = heap_caps_malloc(t->stacksize, MALLOC_CAP_SPIRAM);
        if (!t->stack_buf) { free(t); return NULL; }
        t->handle = xTaskCreateStaticPinnedToCore(
            thread_trampoline, name,
            t->stacksize / sizeof(StackType_t),
            t, prio, t->stack_buf, &t->task_tcb, core);
        if (!t->handle) {
            heap_caps_free(t->stack_buf);
            free(t);
            return NULL;
        }
    } else {
        BaseType_t ret = xTaskCreatePinnedToCore(thread_trampoline, name,
                             t->stacksize / sizeof(StackType_t),
                             t, prio, &t->handle, core);
        if (ret != pdPASS) {
            free(t);
            return NULL;
        }
    }
    return t;
}

void sys_run_thread(sysThread *thread)
{
    (void)thread; /* thread is already running after create */
}

void sys_wait_thread(sysThread *thread, int *status)
{
    if (!thread) return;
    while (thread->state != THREAD_STATE_ZOMBIE)
        vTaskDelay(pdMS_TO_TICKS(1));
    if (status) *status = thread->status;
    thread->state = THREAD_STATE_CLEANED;
    if (thread->stack_buf)
        heap_caps_free(thread->stack_buf);
    free(thread);
}

int sys_thread_id(void)
{
    return (int)(uintptr_t)xTaskGetCurrentTaskHandle();
}

int sys_set_thread_priority(sysThread *thread, int priority)
{
    if (!thread || !thread->handle) return -1;
    vTaskPrioritySet(thread->handle, priority);
    return 0;
}

int sys_sem_wait(SemaphoreHandle_t sem)
{
    return xSemaphoreTake(sem, portMAX_DELAY) == pdTRUE ? 0 : -1;
}

int sys_sem_wait_timeout(SemaphoreHandle_t sem, uint32_t timeout_ms)
{
    return xSemaphoreTake(sem, pdMS_TO_TICKS(timeout_ms)) == pdTRUE ? 0 : -1;
}

int sys_sem_try_wait(SemaphoreHandle_t sem)
{
    return xSemaphoreTake(sem, 0) == pdTRUE ? 0 : -1;
}

uint32_t sys_sem_value(SemaphoreHandle_t sem)
{
    return (uint32_t)uxSemaphoreGetCount(sem);
}

sysCond *sys_cond_create(void)
{
    sysCond *c = calloc(1, sizeof(sysCond));
    if (!c) return NULL;
    c->lock      = xSemaphoreCreateRecursiveMutex();
    c->wait_sem  = xSemaphoreCreateCounting(128, 0);
    c->wait_done = xSemaphoreCreateCounting(128, 0);
    return c;
}

void sys_cond_destroy(sysCond *cond)
{
    if (!cond) return;
    vSemaphoreDelete(cond->lock);
    vSemaphoreDelete(cond->wait_sem);
    vSemaphoreDelete(cond->wait_done);
    free(cond);
}

int sys_cond_signal(sysCond *cond)
{
    xSemaphoreTakeRecursive(cond->lock, portMAX_DELAY);
    if (cond->waiting > 0) {
        cond->signals++;
        xSemaphoreGive(cond->wait_sem);
        xSemaphoreGiveRecursive(cond->lock);
        xSemaphoreTake(cond->wait_done, portMAX_DELAY);
    } else {
        xSemaphoreGiveRecursive(cond->lock);
    }
    return 0;
}

int sys_cond_broadcast(sysCond *cond)
{
    xSemaphoreTakeRecursive(cond->lock, portMAX_DELAY);
    if (cond->waiting > 0) {
        cond->signals = cond->waiting;
        for (int i = 0; i < cond->waiting; i++)
            xSemaphoreGive(cond->wait_sem);
        xSemaphoreGiveRecursive(cond->lock);
        for (int i = 0; i < cond->signals; i++)
            xSemaphoreTake(cond->wait_done, portMAX_DELAY);
    } else {
        xSemaphoreGiveRecursive(cond->lock);
    }
    return 0;
}

int sys_cond_wait(sysCond *cond, SemaphoreHandle_t mutex)
{
    xSemaphoreTakeRecursive(cond->lock, portMAX_DELAY);
    cond->waiting++;
    xSemaphoreGiveRecursive(cond->lock);

    xSemaphoreGiveRecursive(mutex);
    xSemaphoreTake(cond->wait_sem, portMAX_DELAY);
    xSemaphoreGive(cond->wait_done);

    xSemaphoreTakeRecursive(mutex, portMAX_DELAY);

    xSemaphoreTakeRecursive(cond->lock, portMAX_DELAY);
    cond->waiting--;
    xSemaphoreGiveRecursive(cond->lock);
    return 0;
}
