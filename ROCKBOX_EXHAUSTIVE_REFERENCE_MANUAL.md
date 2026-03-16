# Rockbox Architectural Deep-Dive & Exhaustive Reference Manual

## I. Abstract & Current Codebase State

This document serves as a foundational engineering reference for the Rockbox open-source firmware, treating it not merely as an application, but as a complete, independent Real-Time Operating System (RTOS). Rockbox encapsulates over two decades of embedded software development, exhibiting a highly optimized, monolithic architecture designed to execute on extremely resource-constrained, bare-metal hardware.

**The Application-Firmware Interface (AFI):**
Rockbox explicitly separates high-level application logic (`apps/`) from low-level hardware drivers (`firmware/`). However, this boundary is purely declarative, established via shared C headers in `firmware/export/` (e.g., `lcd.h`, `audio.h`, `button.h`). There is no dynamic dependency injection, no system call (syscall) overhead, and no hardware abstraction layer (HAL) struct passed at runtime. High-level functions compile directly against the specific bare-metal implementations defined for the chosen `CONFIG_PLATFORM` during the build process.

**Bare-Metal Coupling & Hosted Ports:**
While the `apps/` layer remains remarkably hardware-agnostic, the core OS (`firmware/kernel/`) assumes total sovereignty over the CPU, memory map, and interrupt controllers. To port Rockbox to a modern RTOS (such as FreeRTOS on the ESP32), one cannot simply run the Rockbox kernel alongside FreeRTOS. Instead, the entire bare-metal kernel must be excised, and Rockbox's internal threading, mutex, and timer primitives must be mapped directly to the host OS. This strategy is vividly demonstrated by the existing "hosted" ports (`firmware/target/hosted/sdl` and `firmware/target/hosted/linux`), which trick the Rockbox core into executing on top of POSIX APIs.

---

## II. Kernel, Threading, & Execution Context (CRITICAL)

The Rockbox kernel (`firmware/kernel/`) implements a custom, highly optimized **hybrid cooperative/preemptive scheduler**.

### 2.1 The Thread Context Structure

The definition of a thread is encapsulated in `struct thread_entry`, located in `firmware/kernel/thread-internal.h`.

```c
/* firmware/kernel/thread-internal.h */

struct thread_entry
{
    unsigned int id;          /* Thread ID */
    const char *name;         /* Thread name for debugging */
    void **stack;             /* Pointer to the start of the stack */
    size_t stack_size;        /* Size of the stack in bytes */
    struct context context;   /* CPU-specific register context */
    unsigned int state;       /* STATE_RUNNING, STATE_BLOCKED, etc. */

    /* Scheduler Queues */
    struct lld_node queue;    /* Node for wait queues */
    struct __wait_queue *wqp; /* Pointer to the wait queue we are on */

    /* Priority & Inheritance */
    int base_priority;
    int priority;
    struct priority_distribution pdist;
    struct blocker *blocker;  /* The object blocking this thread */

    /* Timers */
    long tmo_tick;            /* Tick at which the thread should wake */

#if NUM_CORES > 1
    unsigned int core;        /* Core ID this thread runs on */
    struct corelock slot_cl;  /* Lock protecting this thread structure */
    struct corelock waiter_cl;
#endif
};
```

### 2.2 Thread Creation & Stack Initialization

Threads are spawned via `create_thread()` in `firmware/kernel/thread.c`.

1.  **Allocation**: A `struct thread_entry` is allocated from a static pool (`thread_alloc()`).
2.  **Stack Preparation**: The stack pointer is aligned, and the entire stack region is filled with a sentinel value (`DEADBEEF`) to detect stack overflows during context switches.
3.  **Context Initialization**: `THREAD_STARTUP_INIT` (a macro defined in the architecture-specific `asm/thread.h`) sets up the initial CPU registers, pointing the instruction pointer (PC) to the provided function and the stack pointer (SP) to the top of the allocated stack.
4.  **Scheduling**: The thread is added to the core's Ready-To-Run (RTR) queue (`core_rtr_add()`), and if priorities permit, `switch_thread()` may be invoked immediately.

### 2.3 Context Switching (`switch_thread`)

The heart of the OS is the `switch_thread()` function (`firmware/kernel/thread.c`).

```c
/* firmware/kernel/thread.c */
void switch_thread(void)
{
    const unsigned int core = CURRENT_CORE;
    struct core_entry *corep = __core_id_entry(core);
    struct thread_entry *thread = corep->running;

    if (thread)
    {
        /* Save CPU registers to the current thread's stack */
        thread_store_context(thread);

        /* Stack overflow detection */
        if (UNLIKELY(thread->stack[0] != DEADBEEF) && thread->stack_size > 0)
            thread_stkov(thread);
    }

    for (;;)
    {
        disable_irq();
        check_tmo_expired(corep); /* Process soft timers */
        RTR_LOCK(corep);

        if (!RTR_EMPTY(&corep->rtr))
            break; /* We have a thread to run */

        thread = NULL;
        RTR_UNLOCK(corep);
        core_sleep(IF_COP(core)); /* Idle the CPU (e.g., WFI) */
    }

    /* ... Priority selection logic ... */

    corep->running = thread;
    RTR_UNLOCK(corep);
    enable_irq();

    /* Restore CPU registers from the new thread's stack */
    thread_load_context(thread);
}
```

On bare metal, `thread_store_context` and `thread_load_context` jump to pure assembly (e.g., `firmware/target/arm/asm/thread.c`) to push/pop registers.

### 2.4 Mutexes & Blocking Mechanisms

Rockbox mutexes (`firmware/kernel/mutex.c`) do not use spinlocks for thread synchronization; they explicitly yield the CPU.

```c
/* firmware/kernel/include/mutex.h */
struct mutex
{
    struct __wait_queue queue;  /* Threads waiting for this mutex */
    int recursion;              /* Recursion counter */
    struct blocker blocker;     /* Points to the owning thread */
#ifdef HAVE_CORELOCK_OBJECT
    struct corelock cl;         /* Multi-core spinlock protecting this struct */
#endif
};
```

**The `mutex_lock()` Mechanics:**
1.  **Check Owner**: If `m->blocker.thread == NULL`, the lock is free. The current thread takes ownership, and the function returns.
2.  **Contention**: If the lock is held, `block_thread()` is called (`firmware/kernel/thread.c`).
3.  **Blocking**: `block_thread()` removes the current thread from the RTR queue, adds it to the mutex's `m->queue`, sets its state to `STATE_BLOCKED`, and executes `switch_thread()`. Priority inheritance is calculated to temporarily boost the priority of the thread owning the mutex.

### 2.5 The System Tick & Timer Logic

The OS is driven by a hardware timer firing at `HZ` (typically 100Hz).

*   **The Tick ISR**: The hardware timer triggers an Interrupt Service Routine (ISR) that calls `tick_start()` / `init_tick()`.
*   **Task Execution**: The ISR iterates over `tick_funcs[]`, a list of function pointers registered via `tick_add_task()`.
*   **Crucial Constraint**: **Tick tasks execute strictly in a Hardware ISR context.** They cannot yield, sleep, or lock a standard mutex. Attempting to call `switch_thread()` from within `call_tick_tasks()` will trigger a kernel panic.
*   **Soft Timers**: Threads blocking for a specific duration (`sleep_thread(ticks)`) have their `tmo_tick` value set. During `switch_thread()`, the scheduler calls `check_tmo_expired()`. If `current_tick >= thread->tmo_tick`, the thread is moved back to the RTR queue.

### 2.6 Hosted Kernel Overrides (SDL/POSIX emulation)

To run as an application on a host OS (e.g., Android, Linux, Windows), the `firmware/target/hosted/` tree intercepts all kernel APIs and maps them to host OS primitives.

```c
/* firmware/target/hosted/sdl/thread-sdl.c */

/* A global mutex ensuring only ONE Rockbox thread executes at a time */
static SDL_mutex *m;

void switch_thread(void)
{
    struct thread_entry *current = __running_self_entry();
    enable_irq();

    switch (current->state)
    {
    case STATE_BLOCKED:
        SDL_UnlockMutex(m);                 /* Release the global OS lock */
        SDL_SemWait(current->context.s);    /* Block the OS thread on this semaphore */
        SDL_LockMutex(m);                   /* Re-acquire the lock when awakened */
        current->state = STATE_RUNNING;
        break;
    }
}
```

*   **Thread Mapping**: `create_thread()` invokes `SDL_CreateThread()` (or `pthread_create()`). The underlying OS thread begins executing `runthread()`.
*   **Mutex Mapping**: Rockbox's `mutex_lock()` is bypassed entirely in favor of native `SDL_LockMutex()` / `pthread_mutex_lock()`.
*   **Tick Emulation**: In `firmware/target/hosted/kernel-unix.c`, `tick_start()` uses `timer_create(CLOCK_REALTIME, ...)` to generate a `SIGIO` or `SIGALRM` signal every 10ms. The signal handler executes `call_tick_tasks()`.

---

## III. Memory & Buffer Architecture

### 3.1 Static Boot Allocation (`core_alloc.c`)

During early boot, before the complex allocators are initialized, memory is carved out of a large, statically defined array (e.g., `core_buffer`). `core_alloc` operates strictly as a watermark allocator: it advances a pointer forward. Memory allocated here (e.g., thread stacks, DMA buffers) is typically never freed.

### 3.2 The Compacting Audio Allocator (`buflib.c`)

Rockbox relies on a massive RAM buffer to hold audio data, enabling seamless gapless playback and long disk spin-down times for battery savings. Using a standard `malloc()` for this buffer would inevitably lead to memory fragmentation over hours of playback, eventually causing a failure to allocate a contiguous block for a large audio chunk or a `.codec` overlay.

To solve this, Rockbox uses `buflib` (`firmware/include/buflib.h`), a **compacting memory allocator**.

*   **Handles, Not Pointers**: `buflib_alloc()` returns an `int hid` (handle ID).
*   **Dereferencing**: To read or write to the memory, a thread must call `core_get_data(hid)` to obtain the current physical memory pointer.
*   **Compaction (`buflib_free` -> `buflib_compact`)**: When a block is freed, `buflib` does not leave a "hole" in memory. It immediately executes a `memmove()` to slide all higher-addressed allocations down in physical RAM, perfectly closing the gap. It then updates its internal registry so the next call to `core_get_data(hid)` returns the newly shifted pointer.
*   **Pinning**: If a thread is actively writing to a buffer (e.g., the SD card DMA driver is reading into it), the buffer must not move. The thread calls `buf_pin_handle(hid, true)`. The compactor will halt sliding operations at the boundary of any pinned handle.

---

## IV. Hardware Abstraction Layer (HAL) & Peripherals

### 4.1 Display & LCD Routing (`lcd.h`)

Rockbox decouples UI rendering from physical screen updates via an internal framebuffer.

1.  **The Framebuffer**: The UI functions (`apps/screen_access.c`, `lcd_puts`, `lcd_drawline`) manipulate a contiguous block of RAM defined as `lcd_framebuffer`. The pixel format (e.g., 1-bit monochrome, 16-bit RGB565) depends on the target's `LCD_DEPTH`.
2.  **The Update Trigger**: Once rendering is complete, the application calls `lcd_update()`.
3.  **Hardware Implementation**: On bare metal (e.g., `firmware/target/arm/as3525/lcd-clip.c`), `lcd_update()` configures the Data Block Output Port (DBOP) or a DMA controller to stream the `lcd_framebuffer` RAM over an 8-bit/16-bit parallel bus or SPI bus to the display controller (e.g., ST7789, ILI9341).
4.  **Hosted Implementation**: In `firmware/target/hosted/sdl/lcd-sdl.c`, `lcd_update()` reads the Rockbox `lcd_framebuffer`, translates the raw RGB565 data into a 32-bit `SDL_Surface`, and calls `SDL_UpdateRect()` to paint it onto the host OS window.

### 4.2 Button Input & Debouncing (`button.c`)

Rockbox eschews hardware GPIO interrupts for buttons, relying entirely on tick-based polling to manage debouncing and chording natively.

```c
/* firmware/drivers/button.c */

/* Registered in init() via tick_add_task(button_tick); */
void button_tick(void)
{
    /* Read physical hardware state (target-specific) */
    int btn = button_read_device();

    /* Debounce: State must remain constant for BUTTON_DEBOUNCE ticks */
    if (btn != last_btn) {
        debounce_counter = 0;
        last_btn = btn;
    } else {
        debounce_counter++;
    }

    if (debounce_counter >= BUTTON_DEBOUNCE) {
        /* State is stable. Push to queue. */
        queue_post(&button_queue, btn, 0);
    }
}
```

*   **The Event Loop**: The main UI thread blocks in `apps/action.c` on `get_action()`, which calls `button_get_w_tmo()`. This pulls the debounced bitmask from `button_queue`.
*   **Hosted Injection**: In SDL/POSIX builds, `button-sdl.c` monitors host events (`SDL_KEYDOWN`). It translates the PC keyboard scancode into a Rockbox macro (e.g., `BUTTON_LEFT`) and injects it directly into `button_queue` via `button_queue_post()`, bypassing the `button_tick` hardware polling entirely.

### 4.3 Analog-to-Digital (ADC) & Power Management (`adc.c`)

Reading battery voltage or analog button matrices via ADC is often slow. Rockbox cannot block the system tick or a thread waiting for an ADC conversion.

*   **Asynchronous Polling**: `adc_tick()` is registered as a tick task.
*   On tick *N*: It initiates a hardware conversion for Channel 0 and returns immediately.
*   On tick *N+1* (10ms later): It reads the result register for Channel 0, stores it, initiates a conversion for Channel 1, and returns.
*   The raw readings are passed through an Infinite Impulse Response (IIR) filter (`powermgmt.c`) to smooth out battery voltage fluctuations before display.

### 4.4 USB Stack (`firmware/usbstack/`)

Rockbox implements a complete, custom bare-metal USB stack supporting Mass Storage Class (MSC) and Human Interface Device (HID).
*   When a USB cable is inserted, a hardware interrupt fires, setting the `usb_inserted` state.
*   The main application loop (`apps/main.c`) detects this and halts music playback and disk operations.
*   It calls `usb_init()`, surrendering control of the block device drivers (e.g., `sdmmc.c`) to the USB MSC driver, allowing the host PC to mount the player as a mass storage drive.
*   In hosted environments, the USB stack is usually stubbed out, as the host OS natively manages disk access and charging.

---

## V. Audio Pipeline, Codecs, & Plugins

### 5.1 The Audio Path & DMA Synchronization

The audio engine bridges the gap between file storage and the DAC.

1.  **File I/O (`firmware/common/fat.c`)**: The codec thread calls `fat_readwrite()` to pull an MP3/FLAC chunk from storage into memory. **This is a strictly blocking call.** The thread stalls until the storage driver (`storage_read_sectors`) completes the read.
2.  **Decoding (`apps/codec_thread.c`)**: The codec parses the audio frames and decodes them into raw 16-bit interleaved PCM data.
3.  **The Ring Buffer (`apps/pcmbuf.c`)**: The codec thread calls `pcmbuf_write_complete()`, inserting the decoded PCM chunk into the massive `pcmbuf_buffer` ring buffer. If the ring buffer is full, the codec thread blocks.
4.  **DSP & Mixing (`firmware/pcm_mixer.c`)**: As audio leaves the buffer, the software mixer applies volume scaling (`FRAC_MUL`), crossfading, EQ, and mixes in the Voice UI overlays.
5.  **DMA Output (`firmware/pcm.c`)**: `pcm_play_data()` hands the mixed PCM frames to the target's hardware DMA controller (e.g., `dma-pl081.c`).
6.  **Synchronization**: The `audio_thread` does not continuously pull data in a loop. When the hardware DMA finishes transmitting a buffer to the I2S/DAC, it triggers a **hardware ISR** (`pcm_dma_isr`). This ISR invokes `pcmbuf_pcm_callback()`, which updates the ring buffer read pointers and posts a message to wake the `audio_thread`/`codec_thread` to refill the buffer.

### 5.2 Dynamic Codec Loading & The XIP Problem

Rockbox's approach to loading custom code on bare metal relies heavily on executing data out of RAM.

**Position Independent Code (PIC) & Overlays:**
*   Codecs (`lib/rbcodec/codecs/`) and Plugins (`apps/plugins/`) are compiled as flat, Position Independent Code binaries (`.codec` and `.rock`).
*   They are loaded dynamically from the SD card into a contiguous block of RAM (allocated by `buflib`) via `apps/open_plugin.c`.
*   **The Jump Table**: Because the PIC binary cannot link against the kernel at compile time, Rockbox passes a massive structure (`struct plugin_api`, defined in `apps/plugin.c`) to the entry point. This struct contains hundreds of function pointers (`lcd_puts`, `open`, `mutex_lock`), allowing the loaded code to call back into the core OS.

**The Execute-In-Place (XIP) Roadblock (ESP32):**
*   Modern MCUs like the ESP32 (Xtensa/RISC-V) execute code via an MMU/Cache directly from SPI Flash (Execute-In-Place). They possess extremely limited executable RAM (IRAM).
*   Loading a 150KB MP3 `.codec` PIC binary into ESP32 RAM is impossible due to IRAM constraints. Furthermore, modifying RAM and jumping to it violates modern W^X (Write XOR Execute) security policies enforced by host OS environments.
*   **The Static Workaround (`HAVE_STATIC_CODECS`)**: Hosted ports (SDL, Android) and devices with XIP architectures bypass this entirely by defining `HAVE_STATIC_CODECS`. Instead of compiling codecs as external `.codec` files, the codec sources are compiled as standard static libraries and linked directly into the monolithic firmware binary. `apps/codecs.c` intercepts the load request and calls the statically linked `codec_main()` function directly, entirely bypassing the `plugin_api` and RAM loading mechanisms.

### 5.3 Software vs. Hardware Codecs

Rockbox overwhelmingly relies on highly optimized, hand-written **software decoders** utilizing fixed-point math (`FRAC_MUL`, `FRAC_DIV`). Hardware-accelerated DSP decoding (e.g., offloading MP3 decoding to a dedicated hardware block) is extremely rare in the Rockbox codebase. The philosophy is to decode everything in software on the main CPU, allowing Rockbox to guarantee gapless playback, crossfading, and unified EQ across all audio formats.

---

## VI. UI, Assets, & Accessibility

### 6.1 Localization & Fonts
*   **Language Files**: UI strings are stored in text-based `.lng` files. During boot, `lang_init()` (`apps/language.c`) parses the binary compiled language file into a RAM array, mapping constants like `LANG_PLAY` to specific unicode strings.
*   **Fonts**: Bitmap fonts are stored in custom `.fnt` files. `font_load()` reads the glyph data into memory. When `lcd_puts()` is called, it iterates through the string, looks up the bitmap data for each character in the font array, and plots the pixels directly onto the `lcd_framebuffer`.

### 6.2 Images & Themes
*   **The Theme Engine**: The main menu and the While Playing Screen (WPS) layouts are completely customizable via `.wps` text files. The engine parses a custom tag syntax (e.g., `%s` for scrolling text, `%pb` for a progress bar) at runtime to arrange elements on the screen.
*   **Bitmaps**: `.bmp` files are loaded from disk into RAM. To save memory, heavily used bitmaps (like play/pause icons) are often embedded directly into the firmware binary as C byte arrays.

### 6.3 Voice UI (Accessibility)
Rockbox features a legendary Voice UI for visually impaired users.
*   **The Hook**: Every visual UI element (menus, lists, files) can be hooked by `apps/talk.c`. When the user scrolls to a menu item, `gui_synclist_speak_item()` is invoked.
*   **Voice Assets**: The actual spoken words are stored in a massive `.voice` file on the SD card, which acts as an indexed archive of raw Speex-encoded audio snippets.
*   **The Mixer (`apps/voice_thread.c`)**: When a voice clip needs to play, the UI posts `Q_VOICE_PLAY`. The `voice_thread` wakes up, loads the Speex data from the SD card, decodes it into PCM, and passes it to the `pcm_mixer`.
*   **Overlay**: The `pcm_mixer` software-mixes the voice PCM data *on top of* the currently playing music data (temporarily attenuating the music volume if `talk_mixer_amp` is configured) before handing the final mixed frame to the hardware DMA.

---

## VII. Potential FreeRTOS / ESP-IDF Porting Summary

Based on this exhaustive analysis, porting Rockbox to the ESP32 using bare ESP-IDF is theoretically feasible if we treat FreeRTOS as the "Host OS," directly mirroring the strategy used by the SDL/POSIX ports.

### Theoretical Path
1.  **Abolish Bare-Metal Kernel**: Delete the ARM/ColdFire assembly context switchers. Map `create_thread()` directly to `xTaskCreatePinnedToCore()`. Map `mutex_lock()` to `xSemaphoreTake(..., portMAX_DELAY)`.
2.  **VFS Bridging**: Map Rockbox's POSIX-like `open()`, `read()`, and `write()` calls in `firmware/common/file.c` directly to the ESP-IDF Virtual File System (`esp_vfs_fat_sdmmc_mount`). Let ESP-IDF handle the low-level SD card SPI/SDIO communication.
3.  **Framebuffer Redirection**: Map `lcd_update()` to execute an ESP-IDF `spi_device_transmit()` call, pushing the `lcd_framebuffer` RAM directly to an SPI TFT driver (e.g., ST7789).
4.  **I2S Audio**: Implement `pcm_play_data()` to push `pcmbuf` frames directly into the ESP-IDF I2S driver (`i2s_channel_write`). Use the I2S callback interrupts to emulate the legacy `pcm_dma_isr` synchronization.

### Top 3 Architectural Roadblocks & Solutions

1.  **Roadblock**: **Dynamic Codec Loading vs. ESP32 Memory Maps**
    *   *Details*: ESP32 executes from SPI Flash (XIP) and lacks the IRAM required to load and execute 150KB `.codec` PIC binaries dynamically.
    *   *Solution*: Define `HAVE_STATIC_CODECS`. Abandon the dynamic plugin API and statically link the MP3/FLAC decoding libraries directly into the monolithic ESP-IDF firmware binary.
2.  **Roadblock**: **`buflib` Compaction vs. PSRAM Latency**
    *   *Details*: The `audiobuf` requires megabytes of RAM, mandating the use of ESP32 external SPI PSRAM. However, `buflib` frequently uses `memmove()` to slide allocations to prevent fragmentation. Executing massive `memmove` operations in high-latency PSRAM will stall the CPU and starve the I2S DMA.
    *   *Solution*: Allocate the main `audiobuf` in PSRAM, but force the heavily-accessed `pcm_mixer` output buffers and FreeRTOS I2S DMA descriptors strictly into fast Internal SRAM (`MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`).
3.  **Roadblock**: **Rockbox Tick Handler vs. FreeRTOS Blocking Rules**
    *   *Details*: `button_tick` and other timers expect to run in a strict hardware ISR context and must not block. Standard FreeRTOS Software Timers run in a task context but forbid indefinite blocking.
    *   *Solution*: Bypass FreeRTOS software timers. Use an ESP-IDF High-Resolution Hardware Timer (`esp_timer_create`) configured for `ESP_TIMER_ISR` dispatch to trigger `call_tick_tasks()`, guaranteeing the legacy code operates within a true ISR environment.
