# 03_OS_Core_Kernel_and_Memory_Map.md

## 1. The Kernel Architecture: Threads & Scheduling
Rockbox implements a bespoke cooperative/preemptive hybrid kernel. It is not POSIX-compliant, nor does it use a standard RTOS like FreeRTOS or ThreadX. It was built from scratch to be extremely lightweight (measured in kilobytes) and deterministic for audio playback.

### 1.1 The Thread Control Block (`struct thread_entry`)
The core of the kernel is the `struct thread_entry`. This structure holds the entire execution context of a thread.

**Source Analysis: `firmware/kernel/thread-internal.h`**
```c
struct thread_entry
{
    /* 1. Context Block (Architecture Specific) */
    struct core_context context; /* Registers (R0-R15, CPSR, PC) */

    /* 2. Scheduler Queues */
    struct __rtr_queue_node rtr; /* Runnable queue node */
    struct __tmo_queue_node tmo; /* Timeout queue node */
    struct __wait_queue_node wq; /* Wait queue node (mutex/semaphore) */

    long tmo_tick;               /* Absolute tick for timeout */
    struct __wait_queue *wqp;    /* Pointer to queue currently waiting on */

    /* 3. Metadata */
    const char *name;            /* Thread name (for debugging) */
    uint32_t id;                 /* Thread ID */
    int __errno;                 /* Thread-local errno */

    /* 4. Priority Scheduling (If enabled) */
#ifdef HAVE_PRIORITY_SCHEDULING
    struct blocker *blocker;     /* Object blocking this thread */
    unsigned char base_priority; /* Initial priority */
    unsigned char priority;      /* Current (possibly boosted) priority */
#endif

    /* 5. State & Stack */
    unsigned char state;         /* STATE_RUNNING, STATE_BLOCKED, etc. */
#ifndef HAVE_SDL_THREADS
    size_t stack_size;           /* Allocated stack size */
#endif
};
```
*Note: The `context` member is typically assembly-optimized to be at offset 0, allowing context switch code (`switch_thread`) to access it directly without pointer arithmetic.*

### 1.2 The Scheduler: Cooperative & Preemptive
Rockbox uses a tick-based scheduler (typically 100Hz or 1000Hz).
*   **Cooperative:** Most threads call `yield()` or `sleep()` voluntarily when waiting for I/O.
*   **Preemptive:** Interrupts (Audio DMA, Timer) can wake high-priority threads (e.g., the audio filling thread), causing an immediate context switch upon return from interrupt.

**Key Function: `switch_thread()` (Conceptual C Logic)**
```c
void switch_thread(void)
{
    struct thread_entry *current = __running_self_entry();
    struct thread_entry *next;

    /* 1. Save Context */
    save_context(&current->context);

    /* 2. Pick Next Thread (Highest Priority Runnable) */
    next = scheduler_pick_next();

    /* 3. Switch Stacks */
    current_core->running = next;

    /* 4. Restore Context */
    restore_context(&next->context);

    /* Jump to new PC */
}
```

### 1.3 Context Switching (Assembly)
The actual context switch is written in pure assembly for speed.
It pushes all registers (R4-R11 on ARM) onto the stack, saves the Stack Pointer (SP) into `thread->context`, loads the new thread's SP, and pops the registers.

---

## 2. The Memory Map: Bare Metal Layout
Rockbox operates in a flat memory model (no virtual memory paging). The physical RAM is statically partitioned at link time (`app.lds`).

### 2.1 The Flat Memory Model
RAM Usage Diagram (Typical 32MB System):
```text
+----------------------+ 0x00000000 (Physical RAM Start)
| Exception Vectors    | (Interrupt handlers)
+----------------------+
| Kernel Code (.text)  | (The firmware binary)
+----------------------+
| Kernel Data (.data)  | (Initialized globals)
+----------------------+
| Kernel BSS (.bss)    | (Zero-initialized globals)
+----------------------+
| Stacks               | (Main stack + IRQ stacks)
+----------------------+
| Audio Buffer         | (The Massive Ring Buffer)
| (.audiobuf)          |
|                      |
| (Size: ~16MB+)       |
+----------------------+ 0x01FFFFFF (Top of RAM)
```

### 2.2 The Audio Buffer (`audiobuf`)
This is the single largest allocation in the system. Rockbox buffers the *entire* decoded track (or as much as fits) into RAM to spin down the hard drive and save battery.
*   **Allocation:** Defined in the linker script (`.lds`) or allocated via `core_alloc()`.
*   **Usage:** Circular buffer. The codec thread writes to the head; the DMA (I2S) reads from the tail.

### 2.3 The Plugin Buffer (`pluginbuf`)
A dedicated region (often overlaying the end of the audio buffer or a separate section) reserved for loading dynamic plugins (`.rock` files). Since Rockbox supports only one plugin at a time, this memory is reused.

---

## 3. Synchronization Primitives
Rockbox implements standard OS primitives but optimized for its cooperative nature.

### 3.1 Mutexes & Semaphores
Defined in `firmware/include/thread.h` and `kernel.h`.

**The `struct corelock` (Spinlock)**
Used for SMP (Symmetric Multi-Processing) on dual-core targets (like PP502x).
```c
struct corelock
{
    volatile int lock; /* 0 = Unlocked, 1 = Locked */
    int core;          /* Owner Core ID */
};
```

**The `struct blocker`**
An abstraction for any object that can block a thread (Mutex, Semaphore, Queue).
```c
struct blocker
{
    struct thread_entry * volatile thread; /* Owner thread */
#ifdef HAVE_PRIORITY_SCHEDULING
    int priority;                          /* Highest waiter priority */
#endif
};
```

### 3.2 Message Queues
Rockbox uses message queues extensively for inter-thread communication (e.g., UI events to the main thread).
*   **Struct:** `struct event_queue`
*   **Mechanism:** A circular buffer of `struct event`.
*   **Blocking:** If the queue is empty, `queue_wait()` calls `block_thread()` putting the caller to sleep until an event is posted.

---

## 4. Interrupt Handling
Rockbox takes over the CPU's interrupt vector table.
*   **IRQ:** Standard interrupts (Timer, GPIO, UART).
*   **FIQ:** Fast Interrupts (reserved for extremely low-latency tasks like Software I2S or bit-banging).

**The `irq_handler` Trampoline:**
1.  Save minimal context (R0-R3, PC, CPSR).
2.  Call `interrupt_vector()` (C dispatcher).
3.  Check if a context switch is required (e.g., Audio thread woke up).
4.  If yes, call `switch_thread()` immediately.
5.  Restore context.

This architecture ensures Rockbox can play gapless audio with <10ms latency even on 50MHz CPUs.
