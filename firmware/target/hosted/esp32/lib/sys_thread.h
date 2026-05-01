#ifndef __SYSTHREAD_H__
#define __SYSTHREAD_H__

#include "thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

/* Atomic helpers */
#define AtomicGet(ptr)          __atomic_load_n((uint32_t *)(ptr), __ATOMIC_SEQ_CST)
#define AtomicSet(ptr, value)   __atomic_store_n((uint32_t *)(ptr), (value), __ATOMIC_SEQ_CST)
#define AtomicCAS(ptr, oldval, newval) \
    ({ uint32_t _old = (oldval); \
       __atomic_compare_exchange_n((uint32_t *)(ptr), &_old, (newval), 0, \
                                   __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); })

enum thread_state_t {
    THREAD_STATE_ALIVE,
    THREAD_STATE_DETACHED,
    THREAD_STATE_ZOMBIE,
    THREAD_STATE_CLEANED,
};

typedef struct _thread {
    int threadid;
    TaskHandle_t handle;
    int status;
    int state;
    size_t stacksize;
    int (*userfunc)(void *);
    void *userdata;
    void *data;
    StackType_t *stack_buf;
    StaticTask_t task_tcb;
} sysThread;

typedef struct _cond {
    SemaphoreHandle_t lock;   /* recursive mutex */
    int waiting;
    int signals;
    SemaphoreHandle_t wait_sem;
    SemaphoreHandle_t wait_done;
} sysCond;

/* Semaphore operations (counting semaphore) */
int  sys_sem_wait(SemaphoreHandle_t sem);
int  sys_sem_wait_timeout(SemaphoreHandle_t sem, uint32_t timeout_ms);
int  sys_sem_try_wait(SemaphoreHandle_t sem);
uint32_t sys_sem_value(SemaphoreHandle_t sem);

/* Thread operations */
sysThread *sys_create_thread(int (*fn)(void *), const char *name,
                             const size_t stacksize, void *data);
void sys_run_thread(sysThread *thread);
void sys_wait_thread(sysThread *thread, int *status);
int  sys_thread_id(void);
int  sys_set_thread_priority(sysThread *thread, int priority);

/* Condition variable */
sysCond *sys_cond_create(void);
void     sys_cond_destroy(sysCond *cond);
int      sys_cond_signal(sysCond *cond);
int      sys_cond_broadcast(sysCond *cond);
int      sys_cond_wait(sysCond *cond, SemaphoreHandle_t mutex);

#endif /* __SYSTHREAD_H__ */
