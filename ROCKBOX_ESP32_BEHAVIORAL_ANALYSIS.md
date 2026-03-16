# Rockbox Behavioral & Architectural Deep-Dive (ESP32 Feasibility Study)

## Executive Summary
This document provides an exhaustive, line-by-line behavioral analysis of the Rockbox open-source firmware. The objective is to understand exactly how Rockbox operates mechanically under the hood, focusing on its custom RTOS primitives, massive audio buffer compaction (`buflib`), PIC codec overlays, hardware abstraction boundaries, and DMA synchronization. This research serves to identify the most severe architectural roadblocks when attempting to port Rockbox to the ESP32 as a "hosted application" utilizing bare ESP-IDF and FreeRTOS.

---

## Phase 1: Kernel, Threading, and Concurrency

### The Scheduler & Context Switching
Rockbox's custom OS (located in `firmware/kernel/`) implements a **hybrid cooperative/preemptive scheduler** (`firmware/kernel/thread.c`).

*   **Thread Context**: A thread's execution context is defined by `struct thread_entry` (found in `firmware/kernel/thread-internal.h`). It contains stack bounds (`stack`, `stack_size`), an embedded context structure (`thread->context.sp`, etc.), priority distribution tracking (`struct priority_distribution pdist`), and wait queue links.
*   **Context Switching (Bare Metal)**: On real hardware (e.g., ARM), context switching is handled via pure assembly (included via `asm/thread.c`). The `switch_thread()` function saves the current CPU registers onto the thread's stack (`thread_store_context`), pops the highest priority runnable thread from the `core_entry`'s Ready-To-Run (RTR) list (`rtr_queue`), and loads its context (`thread_load_context`).
*   **Context Switching (Hosted Ports)**: In environments like the SDL simulator (`firmware/target/hosted/sdl/thread-sdl.c`), Rockbox threads are mapped directly to OS threads (`SDL_CreateThread`). However, to maintain Rockbox's cooperative semantics, they execute under a giant global lock. `switch_thread()` in a hosted environment releases the global mutex (`SDL_UnlockMutex(m)`), blocks on a thread-specific semaphore (`SDL_SemWait(current->context.s)`), and re-acquires the lock when signaled to run by the host scheduler.

### Mutexes & Blocking
*   **Mutex Implementation**: Located in `firmware/kernel/mutex.c`. `struct mutex` holds a wait queue and a `struct blocker` pointing to the owning `thread_entry`.
*   **Blocking Mechanics**: When `mutex_lock()` is called and the lock is contended, it does *not* busy-wait or halt the OS. Instead, it calls `block_thread(current, TIMEOUT_BLOCK, &m->queue, &m->blocker)`. This function removes the current thread from the RTR list, adds it to the mutex's wait queue, sets its state to `STATE_BLOCKED`, and executes a `switch_thread()` to yield the CPU to the next runnable task. Priority inheritance (`inherit_priority`) is strictly enforced to prevent priority inversion.

### The System Tick (`tick.c` / `timeout.c`)
*   **Timer Interrupt**: The system is driven by a hardware timer firing at `HZ` (usually 100Hz).
*   **Tick Handler**: The ISR context (`init_tick()` -> `tick_start()`) increments `current_tick` and iterates over an array of registered functions `tick_funcs[]` (via `call_tick_tasks()`).
*   **ISR Constraints**: These tick tasks (e.g., button debouncing) **must execute strictly in a non-blocking ISR context**.
*   **FreeRTOS Conflict**: If we map this to a FreeRTOS Software Timer (which runs in the `Tmr Svc` task), we bypass the strict ISR requirement. However, FreeRTOS software timers still forbid indefinite blocking. If a Rockbox tick task inadvertently calls a blocking Rockbox kernel primitive, it could stall the FreeRTOS timer daemon. We must instead use an ESP-IDF High-Resolution Timer (`esp_timer_create`) with `dispatch_method = ESP_TIMER_ISR` to enforce the ISR constraint.

---

## Phase 2: Memory Model and the Codec Architecture

### Buffer Management (`buflib`)
Rockbox eschews traditional `malloc()` for its massive audio pipeline to prevent fragmentation over hours of playback.

*   **The Allocator**: The core audio allocator is `buflib` (`firmware/include/buflib.h`). It is a **compacting memory allocator**.
*   **Handles over Pointers**: It returns a handle (`int hid`) rather than a raw pointer. Threads must call `core_get_data(hid)` to obtain the physical address.
*   **Compaction Mechanics**: When an allocation is freed, `buflib` physically uses `memmove()` to slide higher allocations down, eliminating the "hole" and keeping free space completely contiguous. It then updates the internal pointer registry. If a thread is currently using the memory, it must "pin" the handle (`buf_pin_handle`) to temporarily halt compaction of that block.

### Codec Loading (The XIP Problem)
*   **Bare Metal (Dynamic Overlays)**: On devices with copious RAM, codecs (MP3, FLAC) are compiled as Position Independent Code (PIC) `.codec` binaries (`lib/rbcodec/`). When playback starts, `apps/open_plugin.c` uses `bufopen()` to allocate a massive chunk in the `buflib` arena, loads the `.codec` file from disk into RAM, resolves relocations, and jumps to its entry point, passing a massive jump table (`struct plugin_api`) so the codec can call back into the Rockbox kernel.
*   **Hosted Ports (Static Linking)**: In hosted ports (like Android/Linux), mapping arbitrary code into RAM and executing it is restricted by the host OS (W^X protections). Hosted ports `#define HAVE_STATIC_CODECS`. Instead of loading a `.codec` file, `apps/codecs.c` directly calls statically linked functions (e.g., `codec_main()`) built into the monolithic binary.
*   **The ESP32 Constraint**: The ESP32 executes code from SPI Flash via the cache (Execute-In-Place / XIP) and has severely limited IRAM (executable RAM). Loading a 100KB PIC `.codec` binary into RAM is impossible on the ESP32. We **must** compile all required codecs as statically linked modules directly into the ESP-IDF `.bin` firmware and enable `HAVE_STATIC_CODECS`.

---

## Phase 3: Hardware Abstraction Layer (HAL) Deep Dive

### LCD & Display Routing
*   **The Framebuffer**: The UI (`apps/gui/` and `apps/screen_access.c`) does *not* draw directly to hardware registers. It draws lines, bitmaps, and text into a contiguous block of RAM known as the `lcd_framebuffer`.
*   **The Update Call**: When rendering is complete, the UI calls `lcd_update()` (`firmware/export/lcd.h`).
*   **Hardware Implementation**: On bare metal (e.g., Sansa Clip), `lcd_update()` uses a custom DMA transfer or DBOP (Data Block Output Port) to blast the `lcd_framebuffer` out to the physical screen (`firmware/target/arm/as3525/lcd-clip.c`).
*   **Hosted Implementation**: In the SDL port (`firmware/target/hosted/sdl/lcd-sdl.c`), `lcd_update()` converts the RGB565 framebuffer into a 32-bit SDL Surface and calls `SDL_UpdateRect()` to push the pixels to the host PC window. For ESP32, this maps perfectly to an `spi_device_transmit()` call pushing the buffer to an ST7789 TFT driver.

### Button Input & Debouncing
*   **Tick-Based Polling**: Rockbox rarely uses hardware interrupts for buttons to save power and simplify debouncing. Instead, `button_init()` registers a tick task: `tick_add_task(button_tick)`.
*   **Debouncing**: Every 10ms, `button_tick` (`firmware/drivers/button.c`) reads the raw GPIOs or ADC matrix. It requires a button to remain in the same state for several consecutive ticks to be considered "pressed."
*   **Routing**: The debounced state is pushed onto `button_queue` using `queue_post()`. The UI thread, blocked on `get_action()` (`apps/action.c`), wakes up, interprets the button chord (e.g., `BUTTON_LEFT | BUTTON_RIGHT`), and translates it to a UI command.

### ADC & Power Management
*   **Asynchronous Reads**: ADC conversions (for battery voltage) are notoriously slow. Rockbox implements `adc_tick()` (`firmware/drivers/adc.c`), which initiates a hardware conversion on one channel and returns immediately. On the *next* tick (10ms later), it reads the result register, kicks off the next channel's conversion, and smooths the data using an IIR filter. It **never blocks** the thread waiting for the ADC.

### USB Stack
*   **Decoupled Operation**: Rockbox contains its own full-stack USB driver (`firmware/usbstack/`) for Mass Storage and HID. When USB is inserted, an interrupt sets a flag. The main loop (`main.c`) or `usb_thread` detects this, halting almost all normal operations. It mounts the device as a block drive to the host PC. On the ESP32, we would likely bypass this entirely and rely on ESP-IDF's TinyUSB stack, providing a completely different host-side interface.

---

## Phase 4: Audio Pipeline and Hardware DMA Sync

### The PCM Ring Buffer (`apps/pcmbuf.c`)
*   **Structure**: `pcmbuf_buffer` is a massive ring buffer divided into `chunkdesc` segments (typically 8KB each).
*   **The Push**: The `codec_thread` decodes MP3/FLAC data into raw PCM and calls `pcmbuf_write_complete()`. If the ring buffer is full, the codec thread blocks.
*   **The Pull & DSP**: The `audio_thread` does not pull data directly. Instead, a hardware DMA controller acts as the ultimate consumer. Before handing a chunk to the DMA, `pcm_mixer` applies crossfading, software volume, and EQ.

### DMA Synchronization
*   **Bare Metal**: When the hardware DAC finishes playing a buffer, it fires a hardware ISR (`pcm_dma_isr`). This ISR calls `pcmbuf_pcm_callback()`, which updates the ring buffer read pointers (`chunk_ridx`) and posts a message (`Q_AUDIO_TRACK_CHANGED`) to wake up the `audio_thread` to refill the buffer.
*   **Hosted ALSA**: In Linux (`firmware/target/hosted/pcm-alsa.c`), `snd_async_add_pcm_handler` installs a POSIX signal handler (`async_callback`). When ALSA needs more data, the signal fires, grabs a mutex, calls `copy_frames()` to pull data from `pcmbuf`, and pushes it to `snd_pcm_writei()`.
*   **ESP32 I2S**: On the ESP32, we will use `i2s_write()` inside an RTOS task, or use the newer ESP-IDF I2S callback mechanism to emulate the hardware DMA ISR, pulling from `pcmbuf` and feeding the I2S peripheral.

### Blocking I/O & The FAT Filesystem
*   **Strict Blocking**: When the `audio_thread` needs to fill the buffer, it opens the MP3 file (`fat_open`) and reads a massive chunk (`fat_readwrite` in `firmware/common/fat.c`). This eventually hits `storage_read_sectors()`.
*   **The Stall**: **This read is strictly blocking.** The thread calling it halts until the SD card returns the data.
*   **ESP32 Impact**: If we route this to ESP-IDF's VFS SDMMC driver, a 50ms SD card read will completely stall the calling Rockbox thread. Because Rockbox separates the UI thread (`tree_thread`) from the `audio_thread`, the UI will remain responsive. However, if the `audio_thread` blocks longer than the `pcmbuf` watermark holds audio, the DAC will underrun, causing audio stutter. We must ensure the `audiobuf` in PSRAM is large enough (several megabytes) to absorb SD card latency spikes.

---

## Phase 5: Current Code State & The Hosted Port Strategy

### Codebase Health Assessment
*   **The Good**: The separation of concerns between `apps/` (UI/Logic) and `firmware/export/` (HAL APIs) is remarkably clean for a 20-year-old embedded project. The hosted targets (`uisimulator`, `sdl`, `android`) prove that the kernel and hardware can be perfectly mocked out.
*   **The Cruft**: The codebase is monolithic. There is no dependency injection; if you call `lcd_update()`, the linker must find exactly one implementation. Furthermore, the codebase relies heavily on global state (e.g., `global_settings`) and deeply nested `#ifdef CONFIG_PLATFORM` macros scattered throughout the core logic, making porting tedious.

### The Hosted Blueprint
The Android and SDL ports trick the core by providing software replacements for bare-metal assembly:
1.  **OS Thread Wrapping**: `create_thread()` is mapped to `pthread_create()`.
2.  **Mutex Mapping**: Custom Rockbox locks are mapped to `pthread_mutex_t`.
3.  **Framebuffer Emulation**: `lcd_update()` blasts the internal RAM buffer to an OS window.
4.  **Signal Timers**: Bare-metal timer interrupts are replaced by POSIX timers (`timer_create()`) firing signals (`SIGALRM`) to drive `call_tick_tasks()`.

---

## Conclusion: Severe Architectural Roadblocks for ESP-IDF

To port Rockbox to the ESP32 using bare ESP-IDF and FreeRTOS, we face three critical roadblocks:

1.  **The XIP Dynamic Loading Block**: ESP32 cannot load and execute 100KB PIC `.codec` binaries into RAM due to IRAM constraints.
    *   *Solution*: We MUST define `HAVE_STATIC_CODECS` and compile all decoders (MP3, FLAC) directly into the ESP-IDF monolithic binary, abandoning Rockbox's plugin model.
2.  **The PSRAM Latency & `buflib` Conflict**: ESP32's internal SRAM (520KB) cannot hold the massive `audiobuf` required for gapless playback. We must use external SPI PSRAM. However, `buflib` physically moves memory (`memmove`) during compaction.
    *   *Solution*: Moving megabytes of data in PSRAM is incredibly slow. We must dedicate a massive, contiguous 4MB block of PSRAM exclusively to `buflib` at boot. Furthermore, the final mixed PCM data destined for the I2S DMA *must* be buffered into fast Internal SRAM, or the I2S peripheral will starve while waiting for PSRAM reads.
3.  **The Tick ISR Constraint**: Rockbox's `button_tick` and other timers expect to run in a strict ISR context that does not block.
    *   *Solution*: We cannot use FreeRTOS Software Timers, as a stray blocking call in a Rockbox tick task would stall the FreeRTOS timer daemon. We must use an ESP-IDF High-Resolution Hardware Timer (`esp_timer`) configured for ISR dispatch to drive `call_tick_tasks()`, and rigorously audit the tick array to ensure no FreeRTOS blocking calls (`vTaskDelay`, `xSemaphoreTake`) are accidentally invoked.