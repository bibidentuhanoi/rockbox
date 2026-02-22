# 03_OS_Core_Kernel_and_Memory_Map.md

## 1. Thread Architecture

Rockbox implements a custom cooperative and preemptive multitasking kernel. The core logic resides in `firmware/kernel/thread.c`.

### `struct thread_entry`
This is the Control Block (TCB) for a Rockbox thread. (Reconstructed from `firmware/export/thread.h` and usage in `thread.c`):

```c
struct thread_entry {
    void *stack;                /* Pointer to the thread's stack */
    size_t stack_size;          /* Size of the stack */
    const char *name;           /* Thread name for debugging */
    struct thread_context context; /* Architecture-specific CPU context (SP, PC, Registers) */

    unsigned int state;         /* STATE_RUNNING, STATE_BLOCKED, STATE_SLEEPING */
    int priority;               /* Priority level (HIGHEST_PRIORITY to LOWEST_PRIORITY) */

    struct event_queue queue;   /* Message queue for this thread */
    struct thread_entry *next;  /* Linked list pointer for scheduler queues */

    /* Priority Inheritance fields */
    struct priority_distribution pdist;
    struct blocker *blocker;    /* What this thread is waiting for */

#if NUM_CORES > 1
    unsigned int core;          /* The CPU core this thread is pinned to */
#endif
};
```

### The Scheduler
Rockbox uses a priority-based round-robin scheduler.
*   **Queues:**
    *   `run_queue`: List of threads ready to run.
    *   `sleep_queue`: Ordered list of threads waiting for a timeout.
*   **Context Switching:**
    *   `switch_thread()`: The core context switch function. It saves the current context (`thread_store_context`) and loads the next (`thread_load_context`).
    *   `core_sleep()`: Puts the CPU to sleep when no threads are ready (idle loop).

### SMP (Symmetric Multi-Processing)
Rockbox supports dual-core architectures (e.g., PP502x, AS3525).
*   **`core_entry`**: A per-core structure holding the `running` thread and the `rtr` (Ready-To-Run) queue.
*   **Locking:** Uses `corelock` (spinlocks) to protect scheduler structures in SMP.

## 2. Memory Management

Rockbox uses a flat memory model. It does **not** use virtual memory (MMU is usually only used for caching/protection, not paging).

### The Static Layout
Memory is statically partitioned at compile/link time (defined in `rom.lds`).

```text
+-----------------------+ 0x00000000
| Exception Vectors     |
+-----------------------+
| Text (Code)           |
| (crt0, kernel, libs)  |
+-----------------------+
| Read-Only Data (.ro)  |
+-----------------------+
| Data (.data)          |
+-----------------------+
| BSS (.bss)            |
+-----------------------+
| ...                   |
+-----------------------+
| Audio Buffer          | (The largest chunk, ~RAM_SIZE - Code - Stacks)
| (audiobuf)            |
+-----------------------+
| Stacks                |
| (Main, IRQ, FIQ)      |
+-----------------------+ RAM_END
```

### Dynamic Allocation
Rockbox has **two** allocators:
1.  **Core Allocator (`core_alloc`):** A simple, upward-growing bump pointer allocator used during initialization. It allocates "permanent" buffers.
2.  **Buffer Library (`buflib`):** A sophisticated movable-memory allocator.
    *   Used for the `audiobuf`.
    *   **Moveable:** Blocks can be shifted to defragment memory.
    *   **Handle-based:** Consumers hold an integer handle, not a raw pointer. They must "lock" the handle to get a pointer.
    *   **Usage:** Audio data, Codecs (loaded into RAM), Album Art, Plugins.

### The "Plugin Buffer"
Plugins are loaded into a reserved area at the end of the Audio Buffer (or a dedicated `pluginbuf`). This is a critical constraint: **Plugins must be Position Independent (PIC)** or relocated at load time because their load address varies.

## 3. Synchronization Primitives

Rockbox defines its own synchronization objects, tailored for its non-preemptive (cooperative) roots but adapted for SMP.

### 1. Mutex (`mutex.c`)
Used for mutual exclusion. Supports Priority Inheritance Protocol (PIP) to prevent priority inversion.
```c
struct mutex {
    struct thread_entry *owner;
    struct queue_head queue; /* Threads waiting on this mutex */
};
```

### 2. Semaphore (`semaphore.c`)
Standard counting semaphore.
```c
struct semaphore {
    int count;
    int max;
    struct queue_head queue;
};
```

### 3. Message Queues (`queue.c`)
The primary IPC mechanism. Threads send events (`queue_post`) to other threads.
*   **Events:** 32-bit ID + data pointer.
*   **Usage:** The UI thread blocks on `queue_wait(&button_queue)`. The button driver ISR calls `queue_post`.

## 4. Creative Analysis: The ESP32 Collision

This architecture presents massive challenges for the ESP32 port.

### 1. The Threading Clash
*   **Rockbox:** Assumes it owns the bare metal. `thread.c` manipulates stack pointers directly (`store_context`).
*   **ESP32:** Runs FreeRTOS. We cannot simply replace the FreeRTOS scheduler because the ESP32 WiFi/BT blobs rely on it.
*   **Solution:** We must **wrap** Rockbox threads as FreeRTOS tasks.
    *   `create_thread` -> `xTaskCreate`.
    *   `thread_wait` -> `vTaskSuspend` / `xSemaphoreTake`.
    *   `switch_thread` -> `taskYIELD`.
    *   **Problem:** Rockbox threads expect to share a single address space and often pass stack pointers. FreeRTOS handles stacks differently.

### 2. The Memory Clash
*   **Rockbox:** Expects a huge contiguous `audiobuf` in DRAM.
*   **ESP32:** DRAM is fragmented (Internal SRAM ~320KB, but disjoint). External PSRAM (SPIRAM) is large (4MB-8MB) but slower and accessed via cache.
*   **Solution:**
    *   Map the Rockbox `audiobuf` **entirely** to SPIRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`).
    *   Keep critical kernel structures (stacks, ISR handlers) in Internal SRAM (`MALLOC_CAP_INTERNAL`).
    *   **Buflib Adaptation:** We must ensure `buflib` is initialized with the SPIRAM pointer.

### 3. The ISR Latency
Rockbox audio drivers (PCM) are often extremely tight ISRs that copy samples directly. On ESP32, interrupt latency can be higher due to flash cache misses.
*   **Workaround:** Use ESP32's DMA (I2S DMA) extensively. The Rockbox `pcm` callback should just fill a DMA descriptor chain, not the hardware FIFO directly.

### 4. IRQ Handling
Rockbox disables IRQs globally (`disable_irq()`) for critical sections.
*   **ESP32:** `taskENTER_CRITICAL()` / `taskEXIT_CRITICAL()`.
*   **Danger:** If Rockbox holds a critical section for too long (doing software decoding), it will starve the WiFi/BT tasks and cause a Watchdog Reset.
*   **Fix:** Audit `disable_irq` usage. Replace with fine-grained mutexes where possible, or ensure the critical sections are microsecond-short.
