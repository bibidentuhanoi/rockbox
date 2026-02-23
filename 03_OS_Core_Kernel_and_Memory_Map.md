# 03_OS_Core_Kernel_and_Memory_Map.md

## 1. The Rockbox Microkernel: A Hybrid Scheduler

Rockbox runs on a custom, bespoke kernel found in `firmware/kernel/`. It is **not** Linux, and it is **not** standard FreeRTOS (historically). It implements a hybrid **Cooperative/Preemptive** scheduling model with priority inheritance.

### 1.1 The Thread Structure (`struct thread_entry`)

Defined in `firmware/kernel/thread-internal.h` (inferred from usage in `thread.c`), the thread control block is the heart of the system.

```c
struct thread_entry {
    /* Architecture-specific context (registers: r0-r15, cpsr, etc.) */
    struct thread_context context;

    /* Stack management */
    void *stack;
    size_t stack_size;

    /* Identification */
    const char *name;
    int priority;           /* Current priority */
    int base_priority;      /* Initial priority */

    /* State management */
    volatile int state;     /* STATE_RUNNING, STATE_BLOCKED, etc. */

    /* Synchronization */
    struct wait_queue queue; /* List of threads waiting on this thread */
    struct blocker *blocker; /* What this thread is blocked on */

    /* Scheduling metrics */
    unsigned int skip_count; /* Anti-starvation counter */

    /* Multi-core support (recent additions) */
    unsigned int core;
};
```

### 1.2 The Scheduler (`switch_thread`)

The scheduling logic in `firmware/kernel/thread.c` is tick-based but also relies on cooperative yielding.

**Key Characteristics:**
*   **Tick Rate:** typically `HZ` (often 100Hz or 1000Hz depending on target).
*   **Priorities:** Strict priority scheduling. Higher priority threads *always* preempt lower ones unless explicitly yielding.
*   **Aging:** A simple aging mechanism (`skip_count`) prevents starvation of lower-priority threads.

```c
/* Simplified Scheduler Logic */
void switch_thread(void)
{
    /* 1. Save Context of Current Thread */
    if (current_thread) {
        store_context(&current_thread->context);
    }

    /* 2. Find Next Best Thread */
    thread = RTR_THREAD_FIRST(&core->rtr); // Ready-To-Run list

    /* 3. Priority Aging Check */
    // If a lower priority thread has been skipped too many times,
    // it gets a boost.

    /* 4. Restore Context */
    thread_load_context(thread);
}
```

### 1.3 Context Switching

The actual context switch is architecture-specific assembly. On ARM, it typically involves:
1.  Pushing `R4-R11` and `LR` to the current stack.
2.  Saving the Stack Pointer (`SP`) to the `thread_entry`.
3.  Loading the `SP` of the new thread.
4.  Popping `R4-R11` and `PC` from the new stack.

## 2. Synchronization Primitives

Rockbox implements classic synchronization objects, optimized for its uniprocessor (historically) roots.

### 2.1 Mutexes (`mutex.c`)
Standard mutual exclusion with **Priority Inheritance Protocol (PIP)** to prevent priority inversion.

```c
struct mutex {
    struct thread_entry *owner;
    struct wait_queue queue; /* Threads blocked on this mutex */
    int recursive_count;
};
```

### 2.2 Semaphores (`semaphore.c`)
Counting semaphores for resource tracking.

```c
struct semaphore {
    int count;
    int max;
    struct wait_queue queue;
};
```

### 2.3 Message Queues (`queue.c`)
Used extensively for inter-thread communication (e.g., sending button events to the GUI thread).

## 3. The Flat Memory Map

Rockbox operates in a flat physical memory model. There is no MMU, no virtual memory, and no memory protection.

### 3.1 The Layout

```text
+-----------------------+ 0x00000000
| Exception Vectors     |
+-----------------------+
| Kernel .text (Code)   |
+-----------------------+
| Kernel .data          |
+-----------------------+
| Kernel .bss           |
+-----------------------+
| Audio Buffer          | <--- The "audiobuf"
| (Huge Ring Buffer)    |
|                       |
|                       |
|   +---------------+   |
|   | Plugin Buffer |   | <--- Stolen from Audio Buffer
|   +---------------+   |
|   | Codec Buffer  |   | <--- Stolen from Audio Buffer
|   +---------------+   |
+-----------------------+
| Stacks (IRQ/SVC)      |
+-----------------------+ RAM_END
```

### 3.2 Buffer Stealing (`buflib`)

Rockbox uses a unique memory allocator called **`buflib`** (Buffer Library). Instead of a heap `malloc`/`free` that fragments, `buflib` is a relocatable memory manager.
*   **Moveable Blocks:** `buflib` can compact memory by moving blocks around.
*   **Handles:** Consumers access memory via handles, not raw pointers (which might become invalid after compaction).
*   **Audio Buffer Dominance:** The majority of RAM is given to the `audiobuf`. When a plugin loads or the GUI needs a large bitmap, it "steals" space from the audio buffer, temporarily reducing the anti-skip buffer size.

### 3.3 Thread Stacks

Thread stacks are allocated statically or from a dedicated stack pool. They are filled with `0xDEADBEEF` at creation to allow for runtime stack overflow checking.
