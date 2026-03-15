# Report 02: Kernel and Memory

## The Bare-Metal RTOS

Rockbox implements a custom, highly optimized hybrid cooperative/preemptive real-time operating system (RTOS). Its thread management and context-switching primitives are designed to operate efficiently on bare-metal hardware with extremely limited resources (e.g., small ARM or ColdFire CPUs with little RAM).

### Thread Management

The threading system is located in `firmware/kernel/thread.c` and `firmware/kernel/thread-internal.h`.

1. **Thread Creation (`create_thread`)**:
   When `create_thread` is called, it allocates a stack and initializes a `struct thread_entry` containing the execution context.
   ```c
   unsigned int create_thread(void (*function)(void),
                              void* stack, int stack_size,
                              unsigned flags, const char *name
                              IF_PRIO(, int priority)
                              IF_COP(, unsigned int core))
   ```
2. **Context Switching (`switch_thread`)**:
   Context switching happens either cooperatively via `yield()` or preemptively via timer interrupts calling `switch_thread()`.
   The actual context state (CPU registers) is saved and restored using target-specific Assembly routines (typically invoked during `switch_thread()` when `corep->running` is updated).
3. **Yielding**:
   Threads can voluntarily give up the CPU using `yield()`. This function evaluates `should_switch_tasks(thread)` to determine if a higher-priority thread is waiting in the scheduler queue. If so, it invokes `switch_thread()`.

### Memory Management

Rockbox does not rely on a standard `malloc()` for its core bare-metal execution, as standard heap fragmentation would quickly exhaust the small RAM footprints of target devices. Instead, it uses custom memory allocators.

#### The `buflib` Allocator

The primary dynamic memory manager is `buflib`, implemented in `firmware/buflib_mempool.c`. It is designed specifically to solve memory fragmentation in a flat memory model.

*   **Compacting Memory Pool**: `buflib` treats memory as an array of `union buflib_data`. When memory becomes fragmented, the system can dynamically relocate allocations.
*   **Handle-based Allocation**: Unlike `malloc()` which returns a raw pointer, `buflib_alloc()` returns a "handle" (an integer ID). To use the memory, code must call `buflib_get_data(ctx, handle)` to dereference the handle to a physical pointer.
*   **Compaction Engine**: Because memory blocks are accessed via handles, `buflib_compact(struct buflib_context *ctx)` can physically `memmove` blocks of memory in RAM to collapse free space into a single contiguous block, updating the handle tables automatically.

#### The `core_alloc` Allocator

Located in `firmware/core_alloc.c`, this acts as a wrapper around `buflib` for the system's "audio buffer".
*   Rockbox statically reserves a large portion of RAM for the `audiobuffer`.
*   `core_alloc` utilizes `buflib` to manage dynamic allocations (like metadata parsing buffers, album art, and plugin code) out of the remaining space in the `audiobuffer`, acting as a moving boundary between the audio decoding pipeline and system memory needs.