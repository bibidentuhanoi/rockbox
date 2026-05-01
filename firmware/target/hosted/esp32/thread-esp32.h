#ifndef __THREAD_ESP32_H__
#define __THREAD_ESP32_H__

/* extra thread functions that only apply when running on hosting platforms */
void sim_thread_lock(void *me);
void * sim_thread_unlock(void);
void sim_thread_exception_wait(void);
void sim_thread_shutdown(void); /* Shut down all kernel threads gracefully */

#endif /* __THREAD_ESP32_H__ */
