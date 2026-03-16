# Rockbox Architectural Deep-Dive & Execution Research

## I. Abstract & Current Codebase State

The Rockbox codebase represents over two decades of iterative development across dozens of highly heterogeneous embedded architectures. Treat it as an entire independent Operating System, rather than a mere "application."

**Coupling to Bare-Metal Assumptions:**
The core application logic (`apps/`) is remarkably decoupled from the underlying hardware execution, largely due to the rigid monolithic linking model enforced by the Application-Firmware Interface (AFI) via headers in `firmware/export/`. High-level applications never access registers directly. However, the OS kernel (`firmware/kernel/`) assumes it has absolute, unmitigated control over the CPU pipeline, memory maps, and the interrupt controller. It is not designed to be pre-empted by a higher-privilege hypervisor or RTOS (like FreeRTOS) without significant translation layers.

**The Application-to-Firmware Boundary Philosophy:**
The boundary is purely declarative (C headers) rather than dynamic (no syscalls, no dependency injection at runtime). The UI logic (e.g., `apps/action.c`) trusts that `button_get()` will synchronously return a debounced bitmask. It relies on the Kernel having previously set up a hardware timer ISR (`tick.c`) to fill a queue behind the scenes. This synchronous, cooperative expectation pervades the system.

---

## II. Kernel, Threading, & Execution Context (CRITICAL)

### The Scheduler & Context Switching
Rockbox's custom kernel (`firmware/kernel/`) manages threads using a **hybrid cooperative/preemptive scheduler**.

*   **The Thread Context (`struct thread_entry`)**: Located in `firmware/kernel/thread-internal.h`. It contains:
    *   `void *stack` and `size_t stack_size`.
    *   `unsigned int state` (e.g., `STATE_RUNNING`, `STATE_BLOCKED`, `STATE_FROZEN`).
    *   `struct context` (architecture-dependent CPU register dumps).
    *   Priority inheritance fields (`struct priority_distribution pdist`).
    *   Scheduler queue links (e.g., `lld_node` for wait queues).
*   **The Switch Mechanism (`switch_thread`)**: Found in `firmware/kernel/thread.c`. On bare metal, `switch_thread()` disables interrupts, calls `thread_store_context()` (which jumps to architecture-specific assembly like `firmware/target/arm/asm/thread.c` to push CPU registers onto the current thread's stack), selects the highest priority thread from the `core_entry`'s Ready-To-Run (`rtr`) queue, and executes `thread_load_context()` to pop the new thread's registers into the CPU.

### Synchronization
*   **Mutexes (`firmware/kernel/mutex.c`)**: Defines `struct mutex` containing a wait queue (`struct __wait_queue queue`), a `struct blocker` pointing to the owning thread, and a `corelock` for multicore targets.
*   **Blocking (`mutex_lock`)**: If a thread attempts to take a locked mutex, it absolutely does *not* halt the OS. It calls `block_thread(current, TIMEOUT_BLOCK, &m->queue, &m->blocker)`. This function:
    1.  Registers the thread on the mutex's wait queue.
    2.  Sets the thread's state to `STATE_BLOCKED`.
    3.  Calls `switch_thread()`, yielding the CPU entirely.

### The System Tick (`tick.c` / `timeout.c`)
*   **The Tick Logic**: `firmware/kernel/tick.c` defines an array of function pointers `tick_funcs[]`. A hardware timer interrupt fires at a constant rate (`HZ`, usually 100Hz). The ISR calls `call_tick_tasks()`, which increments `current_tick` and sequentially executes every function in `tick_funcs[]`.
*   **Context Constraints**: Because `call_tick_tasks()` executes directly inside a Hardware ISR, **no tick task may attempt to yield, sleep, or lock a standard mutex**. Doing so would trigger a kernel panic (`THREAD_PANICF`), as `switch_thread()` cannot execute from an interrupt context.
*   **FreeRTOS Software Timer Implication**: If we map this tick to a FreeRTOS software timer, the routine runs in the FreeRTOS `Tmr Svc` task instead of a true hardware ISR. While this theoretically allows blocking, Rockbox's strict non-blocking design within tick tasks must be preserved, or the FreeRTOS timer daemon will stall, starving all other software timers in the system.

### Hosted Kernel Overrides (e.g., SDL/POSIX)
The `uisimulator` and SDL/POSIX targets (`firmware/target/hosted/`) completely override the bare-metal kernel logic by tricking Rockbox into thinking the Host OS scheduler is its own.
*   **Threading (`thread-sdl.c`)**: `create_thread()` is mapped directly to `SDL_CreateThread()`. The bare-metal `switch_thread()` is replaced by a massive cooperative lock mechanism. Rockbox threads execute under a global `SDL_mutex *m`. When `switch_thread()` is called, it `SDL_UnlockMutex(m)`, waits on a thread-specific semaphore (`SDL_SemWait(current->context.s)`), and re-locks `m` when signaled by the host scheduler. This prevents OS-level preemption from breaking Rockbox's cooperative assumptions.
*   **System Tick (`kernel-unix.c`)**: `tick_start()` uses `timer_create()` to set up a POSIX signal (`SIGALRM` or `SIGIO`). The signal handler invokes `call_tick_tasks()`. An alternative signal stack (`sigaltstack`) is configured to ensure safe execution without trashing the active thread's stack.

---

## III. Memory & Buffer Architecture

### Static vs. Dynamic Allocation
*   **Static/Boot Allocation (`core_alloc.c`)**: The `core_alloc` subsystem manages memory during early boot. It is used for permanent allocations like thread stacks, initial DMA buffers, and system structures. It manages memory from a large array (e.g., `core_buffer`) and rarely frees memory after boot.
*   **Runtime Application Memory**: For dynamic app-level needs, Rockbox relies almost exclusively on `buflib`.

### The Audio Buffer (`buflib.c` & `apps/pcmbuf.c`)
Rockbox does not use `malloc()` for the massive audio buffer, as fragmentation over long listening sessions would lead to catastrophic memory exhaustion.
*   **The Compactor**: `firmware/include/buflib.h` (and `buflib.c`) defines a **compacting memory allocator**.
*   **Handles, not Pointers**: `buflib` never returns a raw pointer to memory; it returns a handle (`int hid`). To access the memory, a thread calls `core_get_data(hid)`.
*   **The Mechanics of Compaction**: When memory is freed (`buflib_free`), `buflib` sweeps the arena. It uses `memmove()` to physically slide higher allocations down in RAM, sealing the "hole". It then updates an internal lookup table so that the next call to `core_get_data()` returns the new, shifted pointer.
*   **Handling in Hosted Ports**: Hosted ports implement `buflib` identically to bare metal. They allocate a massive chunk of host RAM via standard `malloc()` at boot, and hand that pointer to `buflib_init()` to act as the arena.

---

## IV. Hardware Abstraction Layer (HAL) & Peripherals

### Display / LCD (`lcd.h`)
*   **The Framebuffer**: The UI does not draw directly to hardware registers. Functions in `apps/screen_access.c` (and plugin drawing primitives) manipulate a contiguous RAM buffer called `lcd_framebuffer`.
*   **The Update Trigger**: `lcd_update()` (`firmware/export/lcd.h`) is called to push the framebuffer to the screen. On bare-metal targets, this triggers a hardware DMA or DBOP (Data Block Output Port) transfer.
*   **Hosted Redirection (`lcd-sdl.c`)**: In the SDL port, `lcd_update()` converts the 16-bit RGB565 `lcd_framebuffer` into a 32-bit SDL Surface, then executes `SDL_UpdateRect()` to blit the internal RAM directly to the OS window.

### Input (Buttons, Touch, ADC)
*   **Input Polling**: Buttons are *not* typically interrupt-driven. `firmware/drivers/button.c` adds `button_tick` to the system tick array. Every 10ms, it polls the hardware GPIOs or ADC matrix (`button_read_device()`).
*   **Debouncing**: `button_tick` requires the hardware state to remain constant across several consecutive ticks. Once debounced, the chord (e.g., `BUTTON_PLAY | BUTTON_LEFT`) is pushed to `button_queue` using `queue_post()`.
*   **ADC Constraints**: `firmware/drivers/adc.c` manages slow ADC reads (battery voltage). It does not block. `adc_tick()` initiates a conversion. On the *next* 10ms tick, it reads the result and initiates the next channel.
*   **Hosted Injection (`button-sdl.c`)**: The host OS's event loop (e.g., `SDL_PollEvent`) captures keyboard/mouse events. A Rockbox host thread translates `SDLK_RIGHT` to `BUTTON_RIGHT` and injects it directly into the `button_queue`, bypassing the hardware polling entirely.

### USB Stack (`firmware/usbstack/`)
Rockbox implements its own bare-metal USB stack (Mass Storage Class, HID, Audio). When a USB cable is inserted, a hardware interrupt fires, setting a flag. The main loop (`apps/main.c`) halts normal playback operations, initializes the USB stack (`usb_init()`), and takes control of the disk block drivers to mount as a drive on the host PC. In hosted ports, USB is generally `#undef`'d or stubbed out, as the host OS manages its own disk access.

---

## V. Audio Pipeline, Codecs, & Plugins

### The Audio Path (`pcmbuf.c` to DAC)
1.  **Decoding (`codec_thread.c`)**: The codec thread reads from storage, decodes audio into raw PCM, and pushes it into the ring buffer via `pcmbuf_write_complete()`.
2.  **The Ring Buffer (`pcmbuf.c`)**: `pcmbuf_buffer` stores chunks of PCM data. It applies crossfading (`crossfade_mix_fade`) and software volume attenuation here.
3.  **The Hardware Output (`firmware/pcm.c`)**: The `audio_thread` does not continuously pull data. Instead, the hardware DMA controller is the consumer. When the DAC finishes a buffer, a hardware ISR fires (`pcm_dma_isr`). This ISR calls back into `pcmbuf_pcm_callback()`, updating read indices and triggering the codec thread to refill the buffer.

### Codec & Plugin Loading (The XIP Problem)
This is Rockbox's most complex bare-metal feature.
*   **Position Independent Code (PIC)**: `apps/plugins/` and `lib/rbcodec/codecs/` are compiled as PIC binaries using specialized linker scripts (`apps/plugins/plugin.lds`).
*   **The Jump Table (`plugin_api`)**: Dynamically loaded code cannot link against the Rockbox kernel. Rockbox passes a massive structure (`struct plugin_api` in `apps/plugin.c`) containing hundreds of function pointers (`lcd_puts`, `open`, `mutex_lock`) to the plugin/codec upon execution.
*   **Execution (`apps/open_plugin.c`)**: The kernel allocates RAM via `buflib`, loads the `.rock` or `.codec` flat binary from the SD card into RAM, manually resolves BSS and relocations, and jumps to its entry point in RAM.
*   **Hosted Workarounds**: Standard OS environments forbid executing data in RAM (W^X protections). Thus, hosted ports define `HAVE_STATIC_CODECS`. Codecs are not loaded dynamically; they are compiled as standard static libraries and linked into the monolithic executable. `apps/codecs.c` bypasses the `plugin_api` and calls the codec functions directly.

### Software vs. Hardware Codecs
Rockbox overwhelmingly relies on highly optimized, fixed-point software decoders (macros like `FRAC_MUL` in `lib/rbcodec/`). It avoids hardware DSPs to maintain strict cross-platform compatibility, performing all decoding on the primary CPU.

---

## VI. UI, Assets, & Accessibility

### Localization & Fonts
*   **Language Files**: Translated strings are stored in `.lng` files. `lang_init()` loads these into a RAM array, mapping `LANG_PLAY` to the correct unicode string.
*   **Fonts**: Rockbox uses custom `.fnt` files (bitmap fonts). `font_load()` reads these into memory, and `lcd_puts()` iterates through the glyph arrays, writing pixels directly to the `lcd_framebuffer`.

### Images & Themes
*   **The Theme Engine**: The While Playing Screen (WPS) and menus are defined by `.wps` text files using a custom, cryptic tag syntax (e.g., `%s` for scrolling text). The UI parses these at runtime to arrange elements on the framebuffer.
*   **Bitmaps**: `.bmp` images are loaded into RAM. Because memory is tight, they are often cached by the UI layers.

### Voice UI (Accessibility)
*   **The Hook**: `apps/talk.c` intercepts almost all UI rendering calls. If a user highlights a menu item, `gui_synclist_speak_item()` is called.
*   **The Voice Files**: Pre-recorded voice snippets are stored in a massive `.voice` file on the SD card (which acts as an indexed archive of raw Speex data).
*   **The Mixer (`apps/voice_thread.c`)**: When triggered, `Q_VOICE_PLAY` is posted. The `voice_thread` wakes up, decodes the Speex audio, and injects it into a secondary `pcm_mixer` channel. The `pcm_mixer` software-mixes the voice PCM data *over* the playing music data before handing it to the DMA, allowing the blind user to hear spoken menus without interrupting the music.

---

## VII. Potential FreeRTOS / ESP-IDF Porting Summary

### Theoretical Path
To run Rockbox on ESP32, we must abandon bare-metal compilation and treat FreeRTOS as the "Host OS," mirroring the Android/SDL port strategies. We will wrap `create_thread()` in `xTaskCreatePinnedToCore()`, map Rockbox mutexes to FreeRTOS semaphores, route `lcd_update()` to `spi_device_transmit()`, and bypass Rockbox's internal FAT driver (`fat.c`) by bridging `open()` and `read()` directly to the ESP-IDF VFS SDMMC driver.

### Top 3 Architectural Incompatibilities & Solutions

1.  **Dynamic Codec Loading vs. ESP32 Memory Maps**
    *   *Problem*: The ESP32 cannot load and execute 100KB `.codec` PIC binaries into RAM due to IRAM constraints; it must execute from SPI Flash (XIP).
    *   *Solution*: Define `HAVE_STATIC_CODECS`, abandon the plugin API for decoders, and statically link the MP3/FLAC libraries directly into the ESP-IDF monolithic firmware.
2.  **`buflib` Compaction vs. PSRAM Latency**
    *   *Problem*: `buflib` uses `memmove` to slide megabytes of memory to prevent fragmentation. Doing this in the ESP32's external SPI PSRAM will be catastrophically slow, starving the I2S DMA.
    *   *Solution*: Allocate the massive `audiobuf` in PSRAM, but force the heavily-accessed `pcm_mixer` output buffers and FreeRTOS I2S DMA descriptors strictly into fast Internal SRAM (`MALLOC_CAP_INTERNAL`).
3.  **Rockbox Tick Handler vs. FreeRTOS Blocking Rules**
    *   *Problem*: `button_tick` and other tasks run in a strict hardware ISR and must not block. FreeRTOS software timers run in a task context but still forbid indefinite blocking.
    *   *Solution*: Use an ESP-IDF High-Resolution Timer (`esp_timer`) configured for `ESP_TIMER_ISR` dispatch to trigger `call_tick_tasks()`, guaranteeing the legacy code never accidentally invokes a blocking RTOS primitive during a tick.