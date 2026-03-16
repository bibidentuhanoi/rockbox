# Rockbox Architectural Deep-Dive & ESP32 FreeRTOS Port Feasibility

## Executive Summary
This document provides a highly detailed, line-by-line architectural analysis of the legacy Rockbox firmware. The objective is to map out the hidden relationships between its subsystems, understand how it abstracts hardware, and analyze its Application-Firmware Interface (AFI). Finally, this research serves as a feasibility study for porting Rockbox to the ESP32 as a "hosted app" running on top of FreeRTOS, strictly utilizing bare ESP-IDF drivers (bypassing ESP-ADF entirely).

---

## Phase 1: Subsystem Architecture & API Relationships

### The Call Stack: From UI to Hardware
Rockbox utilizes a monolithic linking model. The `apps/` directory contains the high-level application logic, which communicates directly with the `firmware/` directory (the Kernel and Drivers) via shared headers in `firmware/export/`. There are no system calls or formal dependency injection mechanisms.

**Tracing a Core Action: Selecting a Song**
1.  **User Input**: The user presses a button. The hardware interrupt is serviced in a target-specific driver (e.g., `firmware/target/arm/.../button-target.c`).
2.  **Debouncing & Queuing**: A kernel tick task (`button_tick` in `firmware/drivers/button.c`) debounces the raw input and pushes it onto a global message queue using `queue_post(&button_queue, ...)`.
3.  **UI Event Loop**: The main application thread, running the UI loop (often `tree_thread` or `root_menu()`), blocks on `get_action()` in `apps/action.c`.
4.  **Action Mapping**: `get_action()` pops the raw button event from the queue (`button_get_w_tmo()`) and translates it into a logical UI action (e.g., `ACTION_STD_OK`) based on the current context (`CONTEXT_TREE`).
5.  **Playlist & Audio Dispatch**: The UI logic determines a song was selected. It calls into the playlist API (`apps/playlist.c`), which eventually signals the audio thread to start playback via a queue message: `audio_queue_post(Q_AUDIO_PLAY, ...)`.
6.  **Audio Engine Processing**: The `audio_thread` (`apps/playback.c`) receives `Q_AUDIO_PLAY`. It manages the massive `audiobuf`, kicks off the `codec_thread` to decode the file into PCM data, and inserts that data into `pcmbuf` (`apps/pcmbuf.c`).
7.  **Hardware Output**: The `pcmbuf` logic eventually calls into `firmware/pcm.c` (`pcm_play_data()`), which invokes the target-specific DMA driver (e.g., `dma-pl081.c`) to push the PCM stream to the I2S/DAC hardware.

### Kernel Primitives
The Rockbox kernel (`firmware/kernel/`) implements a custom hybrid cooperative/preemptive scheduler.

*   **Threading (`firmware/kernel/thread.c`)**:
    *   **Core Structure**: `struct thread_entry` (defined in `firmware/kernel/thread-internal.h`) holds the thread context (stack pointer, state, priority) and scheduler queue links (`lld_node`).
    *   **Creation**: `create_thread()` allocates a `thread_entry`, sets up the initial stack (filling it with `DEADBEEF` to detect overflows), and adds it to the `core_entry`'s Ready-To-Run (RTR) queue.
    *   **Context Switching**: On bare metal, `switch_thread()` saves the CPU registers to the current thread's stack and loads the registers from the next thread's stack. This is highly architecture-dependent and requires assembly (e.g., `asm/thread.c`).
*   **Mutexes (`firmware/kernel/mutex.c`)**:
    *   **Structure**: `struct mutex` contains a wait queue (`struct __wait_queue queue`), a `struct blocker` indicating the owning thread, and a `corelock` for multi-core safety.
    *   **Locking**: `mutex_lock()` attempts to claim the `blocker.thread`. If owned by another thread, it calls `block_thread()`, placing the current thread on the mutex's wait queue and calling `switch_thread()` to yield. Priority inheritance is implemented to prevent priority inversion.
    *   **Unlocking**: `mutex_unlock()` clears the `blocker.thread` and uses `wakeup_thread()` to move the highest-priority waiting thread back to the RTR queue.
*   **System Tick (`firmware/kernel/tick.c` & `timeout.c`)**:
    *   A hardware timer generates an interrupt at `HZ` (usually 100Hz).
    *   The ISR increments `current_tick` and executes a list of registered functions via `call_tick_tasks()`.
    *   `timeout.c` manages soft timers and `sleep_thread()` delays by checking thread `tmo_tick` values against `current_tick`.

### Memory & Buffer Management
Rockbox employs a flat memory model, generally eschewing an MMU, relying on two primary allocators:

1.  **Core Allocator (`firmware/core_alloc.c`)**: Used for static/system allocations that exist for the lifetime of the system or are rarely freed (e.g., thread stacks, initial buffers).
2.  **Buflib (`firmware/include/buflib.h`)**: This is the heart of Rockbox's dynamic memory management, specifically designed for the massive `audiobuf`.
    *   **Compaction**: To prevent fragmentation in the large audio buffer (which must hold contiguous chunks of audio data), `buflib` is a compacting allocator.
    *   **Handles, Not Pointers**: It returns handles (`int hid`) rather than raw pointers. To access memory, a thread must request the pointer (`core_get_data(hid)`). When memory is freed, `buflib` physically moves blocks in RAM to keep free space contiguous, updating the internal pointers associated with the handles.

---

## Phase 2: The HAL Boundary (Exhaustive Mapping)

### The Hardware Abstraction Layer (HAL)
The HAL interface is defined by headers in `firmware/export/`. These functions must be implemented by the specific target architecture (in `firmware/target/`).

**Key HAL Interfaces requiring translation:**
*   `firmware/export/audio.h` & `pcm.h`:
    *   `pcm_init()`: Initialize the audio hardware.
    *   `pcm_play_data()`: Push PCM frames to the DAC/DMA.
    *   `pcm_set_mixer_volume()`: Hardware volume control.
*   `firmware/export/lcd.h`:
    *   `lcd_init()`: Initialize the display controller.
    *   `lcd_update()`: Flush the internal framebuffer (`lcd_framebuffer`) to the physical screen.
    *   `lcd_set_contrast()` / `lcd_backlight()`.
*   `firmware/export/button.h`:
    *   `button_init()`: Initialize GPIOs/ADC for buttons.
    *   Target-specific implementations push physical button states into the generic `button_queue`.
*   `firmware/export/storage.h` & `disk.h`:
    *   `storage_init()`: Initialize the SD/MMC or Flash controller.
    *   `storage_read_sectors()` / `storage_write_sectors()`: Block-level I/O.
*   `firmware/export/i2c.h` / `spi.h`:
    *   `i2c_transfer()` / `spi_transfer()`: Bus-level communication, often used to configure external DACs or displays.
*   `firmware/export/system.h` & `cpu.h`:
    *   `system_init()`: Low-level clock and pin muxing setup.
    *   `enable_irq()` / `disable_irq()`: Global interrupt control.

### Storage & File System
1.  **POSIX Abstraction (`firmware/common/file.c`)**: Provides `open()`, `read()`, `write()`, `lseek()` implementations.
2.  **FAT Filesystem (`firmware/common/fat.c`)**: If a file is on a FAT partition, `file.c` routes calls here. `fat_readwrite()` translates file offsets into cluster numbers, traverses the File Allocation Table, and determines the logical sector numbers.
3.  **Disk Manager (`firmware/common/disk.c`)**: Manages logical partitions and volumes (`disk_mount()`).
4.  **Storage HAL (`firmware/export/storage.h`)**: The FAT layer calls `storage_read_sectors()`, which maps to the target-specific block device driver (e.g., `firmware/target/arm/.../sdmmc.c`).

### Codecs and Plugins Architecture
Rockbox's approach to dynamic loading on bare metal is highly specialized.
*   **Position Independent Code (PIC)**: Codecs (`lib/rbcodec/`) and plugins (`apps/plugins/`) are compiled as PIC. They do not know where they will be loaded in RAM.
*   **The Plugin API (`apps/plugin.h` & `apps/plugin.c`)**: Since the dynamically loaded code cannot link against the kernel symbols, Rockbox uses a massive jump table (`struct plugin_api`). This struct contains function pointers to hundreds of kernel and UI functions (e.g., `lcd_puts`, `open`, `pcmbuf_insert`).
*   **Loading and Execution (`apps/open_plugin.c`)**:
    1.  The firmware reads the `.rock` (plugin) or `.codec` file from storage into a contiguous block of RAM (usually allocated via `buflib`).
    2.  It resolves relocations if necessary.
    3.  It calls the entry point of the loaded binary, passing a pointer to the `plugin_api` struct.
    4.  The plugin/codec executes, using the provided function pointers to interact with the system.

---

## Phase 3: The Hosted Port Strategy (Lessons from the Simulator)

The `firmware/target/hosted/` directory reveals how Rockbox can run as an application on top of a full OS (Linux, Windows, Android) by replacing bare-metal assembly with OS primitives.

### The OS Illusion
Instead of executing directly on hardware, the hosted port provides software implementations of the HAL headers (`firmware/export/`), routing Rockbox's requests to the host OS.

### Thread & Hardware Stubbing (`thread-sdl.c` / `kernel-unix.c`)
*   **POSIX Threading**: Instead of custom assembly context switches, `create_thread()` is mapped to `pthread_create()` (or `SDL_CreateThread()`). The Rockbox `struct thread_entry` simply stores the underlying OS thread handle.
*   **Mutex Mapping**: Rockbox's internal `mutex_lock()` and `mutex_unlock()` are mapped directly to `pthread_mutex_lock()` and `pthread_mutex_unlock()`.
*   **Yielding/Blocking**: When a Rockbox thread needs to block (e.g., waiting on a queue), the hosted port uses OS Condition Variables (`pthread_cond_wait`) or Semaphores (`SDL_SemWait`) to suspend the thread at the OS level, allowing the host scheduler to take over.
*   **System Tick**: The hardware timer interrupt is replaced by a POSIX timer (`timer_create()` in `kernel-unix.c`) that sends a signal (e.g., `SIGALRM`) every 10ms. The signal handler invokes `call_tick_tasks()` to drive Rockbox's internal timing.

### I/O Routing
*   **Display (`lcd-sdl.c` / `lcd-linuxfb.c`)**: Rockbox still renders to its internal RAM `lcd_framebuffer`. However, `lcd_update()` does not write to a hardware DBOP. Instead, it translates the internal framebuffer format (often 16-bit RGB565) into the host's format and uses SDL blitting (`SDL_UpdateRect`) or writes to a Linux framebuffer (`/dev/fb0` via `mmap`) to display the result in a window.
*   **Input (`button-sdl.c`)**: The host OS's event loop (e.g., `SDL_PollEvent`) captures keyboard or touch events. These are translated into Rockbox `BUTTON_*` macros and injected into Rockbox's `button_queue`.
*   **Audio (`pcm-alsa.c` / `pcm-sdl.c`)**: Instead of configuring DMA, `pcm_play_data()` pushes the mixed PCM frames to the host's audio API (e.g., ALSA `snd_pcm_writei()` or SDL audio callbacks).

---

## Phase 4: Short Potential ESP-IDF Port Plan

Based on the hosted port strategy, porting Rockbox to the ESP32 using bare ESP-IDF (without ESP-ADF) is feasible by treating FreeRTOS as the "Host OS".

### 1. RTOS Mapping (FreeRTOS Wrapper)
Create a new target: `firmware/target/hosted/espidf/`.
*   **Threads**: Implement `create_thread()` using `xTaskCreatePinnedToCore()`. Store the `TaskHandle_t` in `struct thread_entry`.
*   **Mutexes/Queues**: Map `mutex_init`/`lock`/`unlock` to `xSemaphoreCreateMutex()`, `xSemaphoreTake()`, and `xSemaphoreGive()`. Map Rockbox queues to `xQueueSend`/`xQueueReceive`.
*   **System Tick**: Use an ESP-IDF High-Resolution Timer (`esp_timer_create`) set to 10ms (100Hz) to call `call_tick_tasks()`. Map `sleep_thread(ticks)` to `vTaskDelay(pdMS_TO_TICKS(ticks * 10))`.

### 2. Driver Bridging (Bare ESP-IDF HAL)
*   **Storage (VFS)**: Bypass `firmware/common/fat.c` entirely. Route `open()`, `read()`, `write()` directly to ESP-IDF's POSIX VFS (`esp_vfs_fat_sdmmc_mount`). Let ESP-IDF handle the SD card via its hardware SDMMC driver.
*   **Audio (I2S)**: Implement `pcm-espidf.c`. Initialize the I2S peripheral (`i2s_driver_install()`). When `pcm_play_data()` is called by `pcmbuf`, use `i2s_write()` to push the PCM frames directly to the ESP32's I2S DMA buffers.
*   **Display (SPI)**: Implement `lcd-espidf.c`. Map `lcd_update()` to an `spi_device_transmit()` call, pushing the `lcd_framebuffer` directly to the SPI TFT driver.

### 3. Codec/Plugin Roadblock & Solution
*   **The Problem**: Rockbox relies on dynamically loading `.codec` (PIC) binaries into RAM at runtime. The ESP32 (Xtensa/RISC-V) typically executes code from SPI Flash via cache (XIP) and prefers statically linked binaries. Loading custom ELF/PIC binaries into ESP32 RAM and executing them is complex due to architectural differences and memory constraints.
*   **The Solution**: For the ESP32 port, abandon dynamic codec loading. We must statically link the required codecs (e.g., MP3, FLAC, Vorbis) directly into the main ESP-IDF firmware binary. We will modify `apps/codecs.c` to route codec initialization requests directly to the statically linked decoding functions, rather than attempting to `bufopen()` and execute a `.codec` file.

### 4. Execution Strategy (Dual-Core Pinning)
*   **Core 0 (PRO_CPU)**: Pin the FreeRTOS system tasks, the ESP-IDF Wi-Fi/Bluetooth stacks (if added later), and the SDMMC VFS driver interrupt handling here.
*   **Core 1 (APP_CPU)**: Pin the core Rockbox logic here using `xTaskCreatePinnedToCore`.
    *   Pin the `audio_thread`, `codec_thread`, and `pcmbuf` processing to Core 1. This ensures the heavy DSP math runs uninterrupted.
    *   Pin the `gui_thread` (event loop) to Core 1.
*   **PSRAM Mitigation**: The `audiobuf` MUST reside in external PSRAM due to size. To mitigate latency, the heavily accessed `pcm_mixer` output buffers and I2S DMA descriptors MUST be forced into Internal SRAM using `MALLOC_CAP_INTERNAL`.
