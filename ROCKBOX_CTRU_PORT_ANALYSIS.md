# Rockbox CTRU (Nintendo 3DS) Port Architectural Analysis

The `ctru` port (`firmware/target/hosted/ctru`) represents a fascinating bridge in the Rockbox ecosystem. It targets the Nintendo 3DS family of consoles via the homebrew `libctru` toolchain. While technically categorized as a "hosted" port (like SDL or Android), it operates much closer to the metal than a typical desktop OS.

Architecturally, the `libctru` port shares a striking resemblance to the challenges faced when porting Rockbox to the **ESP32 (ESP-IDF / FreeRTOS)**. Both environments run atop an embedded, preemptive RTOS (Horizon OS for 3DS, FreeRTOS for ESP32), both utilize heavily abstracted manufacturer SDKs for hardware interaction, and both require delicate wrapping of Rockbox's internal primitives.

---

## 1. System Initialization and Boot Sequence
In bare-metal Rockbox, the bootloader calls `main()`, which immediately runs `system_init()`.

In the CTRU port, the 3DS homebrew launcher hands execution to `main()` which maps directly to the `system_init()` override in `firmware/target/hosted/ctru/system-ctru.c`.

```c
/* firmware/target/hosted/ctru/system-ctru.c */
void system_init(void)
{
    /* Initialize libctru services */
    aptSetSleepAllowed(false);
    sys_console_init();
    sys_timer_init();

    /* Mount the SD card via the 3DS FSUSER service */
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_ASCII, ""));
    // ...
}
```

**ESP-IDF Comparison:** This is exactly the boot pattern proposed for the ESP32 `app_main()`. The port must initialize the SDK services (mounting the ESP-IDF `esp_vfs_fat_sdmmc`, configuring I2S and SPI) inside `system_init()` before allowing Rockbox's core logic to spawn its threads.

---

## 2. The Atomic Threading & Sync Stack
Just like the ESP-IDF proposal, the 3DS runs a preemptive OS (Horizon). Rockbox's cooperative `switch_thread()` assembly cannot be used. The CTRU port replaces the entire threading and synchronization stack.

### Threading (`thread-ctru.c`)
The CTRU port intercepts `create_thread()` and wraps it with `sys_create_thread()` which maps down to the Horizon OS `svcCreateThread`.

```c
/* firmware/target/hosted/ctru/thread-ctru.c */
unsigned int create_thread(void (*function)(void), ...)
{
    struct thread_entry *thread = thread_alloc();

    /* CTRU uses LightSemaphore for thread context locking */
    LightSemaphore *s = (LightSemaphore *) malloc(sizeof(LightSemaphore));
    LightSemaphore_Init(s, 0, 255);

    sysThread *t = sys_create_thread(runthread, name, stack_size, thread);

    thread->context.t = t;
    thread->context.s = s;
}
```

### Mutexes & Semaphores
Because the 3DS provides its own RTOS locking primitives, mixing Rockbox's `thread_entry->state` with 3DS threads would cause a kernel panic. The CTRU port implements `mutex_init()` using `LightLock` and `semaphore_init()` using `LightSemaphore`.

**ESP-IDF Comparison:** The CTRU port proves that gutting `kernel/mutex.c` and `kernel/queue.c` and replacing them with native RTOS primitives (like FreeRTOS `xSemaphoreCreateMutex`) is not only viable but the *standard* procedure for embedded hosted ports.

---

## 3. The Audio Pipeline (Pull-Based Hardware Abstraction)
Rockbox expects a DMA interrupt to pull audio chunks via `pcm_play_dma_complete_callback()`. The 3DS audio DSP (NDSP) uses a queue of `ndspWaveBuf` buffers.

To bridge this, the CTRU port does exactly what was prescribed for the ESP32: it creates a dedicated **Audio Feeder Task**.

```c
/* firmware/target/hosted/ctru/pcm-ctru.c */
void pcm_thread_run(void* nothing)
{
    while(!AtomicGet(&_pcm_shutdown))
    {
        RecursiveLock_Lock(&_pcm_lock_mtx);
        for(size_t i = 0; i < ARRAY_SIZE(_dsp_wave_bufs); ++i) {
            /* If a buffer finished playing, pull more from Rockbox */
            if(_dsp_wave_bufs[i].status == NDSP_WBUF_DONE) {
                fill_buffer(&_dsp_wave_bufs[i]);
            }
        }
        RecursiveLock_Unlock(&_pcm_lock_mtx);

        /* Wait for the NDSP interrupt callback to wake us */
        LightEvent_Wait(&_dsp_callback_event);
    }
}
```

**ESP-IDF Comparison:** This is the exact pattern required to wrap ESP-IDF's `i2s_write()`. An infinite loop task (`pcm_thread_run`) waits for the hardware (I2S DMA) to finish, pulls the next chunk using `pcm_play_dma_complete_callback()`, and feeds it to the hardware, completely decoupling the push-based SDK from Rockbox's pull-based mixer.

---

## 4. The System Tick (`kernel-ctru.c`)
Rockbox requires `call_tick_tasks()` to fire at `HZ` (100Hz).

```c
/* firmware/target/hosted/ctru/kernel-ctru.c */
void tick_start(unsigned int interval_in_ms)
{
    /* Uses the 3DS OS timer service */
    tick_timer_id = sys_add_timer(interval_in_ms, tick_timer, NULL);
}
```
The 3DS OS provides a high-priority timer service that invokes the callback cleanly.

**ESP-IDF Comparison:** On ESP32, standard software timers run at priority 1, risking UI starvation. As proposed, the ESP32 must mimic this CTRU behavior by creating a dedicated high-priority tick task (`vTaskDelayUntil`) rather than relying on low-priority software timers.

---

## 5. Codec and Plugin Loading (`lc-ctru.c`)
The 3DS cannot simply execute arbitrary binary blobs mapped directly into RAM via `lc_open()` like older iPods. However, because it runs a complex OS environment, it supports a form of dynamic linking.

The CTRU port compiles plugins as `.so` files (Shared Objects) and uses a custom dynamic loader wrapper.

```c
/* firmware/target/hosted/ctru/lc-ctru.c */
void * lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    /* The 3DS dlopen implementation needs a custom resolver for unresolved symbols */
    void *handle = ctrdlOpen(filename, RTLD_NOW | RTLD_LOCAL, programResolver, NULL);
    return handle;
}
```

**ESP-IDF Comparison:** Here the paths diverge. The 3DS has the RAM and OS support to dynamically resolve `.so` symbols at runtime. The ESP32-S3 FreeRTOS environment does not provide a native `dlopen` equivalent, nor can it execute from PSRAM without `CONFIG_SPIRAM_XIP_FROM_PSRAM` constraints. The ESP32 must resort to the static linking/registry hack (`codec_registry`) rather than adopting the CTRU dynamic loader.

---

## 6. Display Framebuffer (`lcd-bitmap.c`)
The 3DS has dual screens (Top 3D, Bottom Touch). Rockbox renders to its internal software framebuffer (`FBADDR`), and `lcd_update_rect` acts as the bridge to the 3DS GPU (`GSP`).

```c
/* firmware/target/hosted/ctru/lcd-bitmap.c */
void lcd_update_rect(int x, int y, int width, int height)
{
    /* Calculate dirty rectangle offsets */
    dst = LCD_FRAMEBUF_ADDR(x, y);
    src = FBADDR(x,y);

    /* Request a GSP GPU DMA transfer from Rockbox RAM to 3DS VRAM */
    GSPGPU_FlushDataCache(src, width*height*2);
    update_framebuffer();
}
```

**ESP-IDF Comparison:** The CTRU port perfectly illustrates the HAL isolation. Rockbox computes the dirty rectangle, and `lcd_update_rect` simply invokes an asynchronous DMA transfer (GSP on 3DS, `esp_lcd_panel_draw_bitmap` on ESP32) without caring about the underlying hardware.

## Summary

The Nintendo 3DS `libctru` port is the conceptual blueprint for the ESP32 port. It proves that replacing the atomic threading stack with RTOS primitives, intercepting the system tick via OS timers, and bridging the audio pipeline via a dedicated Feeder Task are not only feasible, but actively deployed in production on modern embedded "hosted" targets.


## 7. Deep Dive: Thread Synchronization in `libctru`

To truly understand how `libctru` replaces the bare-metal synchronization stack, we must analyze its custom synchronization wrapper library in `firmware/target/hosted/ctru/lib/sys_thread.c`.

Rockbox's cooperative kernel uses `corelock.c` and `mutex.c` which directly manipulate the `running` state of threads. On the 3DS, this is entirely overridden.

```c
/* firmware/target/hosted/ctru/lib/sys_thread.c */
void sync_MutexInit(sync_Mutex *mutex)
{
    LightLock_Init(mutex);
}

void sync_MutexLock(sync_Mutex *mutex)
{
    LightLock_Lock(mutex);
}

void sync_MutexUnlock(sync_Mutex *mutex)
{
    LightLock_Unlock(mutex);
}
```

The 3DS OS provides `LightLock` as a highly efficient, userspace mutex that only traps into the kernel if there is contention.

By mapping `mutex_lock` directly to `sync_MutexLock`, the CTRU port completely isolates Rockbox from the internal Horizon OS thread scheduler.

### Why the ESP32 Port Must Follow This Model
This completely validates the hypothesis in Section III of the ESP32 strategy. If the CTRU port attempted to compile `firmware/kernel/mutex.c`, it would conflict with `sysThread` state. The ESP-IDF port *must* implement `mutex_init()` as a wrapper around `xSemaphoreCreateMutex()`. The "Messy Overrides" strategy is not just theoretical; it is empirically proven by the 3DS implementation.

## 8. App API and Storage: `bfile` and `sys_file.c`

Because the 3DS provides an isolated filesystem environment (SDMC Archive), Rockbox cannot use its internal bare-metal FAT drivers.

The CTRU port provides a complete POSIX-like wrapper over the `FSUSER` service via the `bfile` library (`firmware/target/hosted/ctru/lib/bfile/bfile.c`).

```c
/* firmware/target/hosted/ctru/lib/sys_file.c */
int sys_open(const char *path, int oflag, ...)
{
    BFile *f = bfileOpen(path, oflag);
    if (!f) return -1;

    // Allocate a file descriptor slot
    int fd = alloc_fd(f);
    return fd;
}

ssize_t sys_read(int fd, void *buf, size_t count)
{
    BFile *f = get_bfile(fd);
    return bfileRead(f, buf, count);
}
```

When Rockbox's `apps/buffering.c` requests a 32KB chunk of an MP3 file, it calls the standard POSIX `read()` function. The CTRU toolchain intercepts this call, routes it to `sys_read()`, which then translates it into a binary read (`FSFILE_Read`) against the 3DS ARM9 storage processor.

### ESP-IDF VFS Comparison
This exact architecture is provided natively by ESP-IDF via `esp_vfs_fat.h`. The ESP-IDF Newlib C library already implements `open()`, `read()`, and `lseek()`. The ESP32 port requires exactly **zero** code to achieve what the CTRU port achieves with `sys_file.c`. By mounting the SD card to `/sdcard`, the VFS driver completely subsumes the storage HAL.

## 9. Codec Execution: The Custom Program Resolver
One of the most complex pieces of the CTRU port is `lc-program-resolver.c`.

Because Rockbox plugins are compiled to assume they are injected with a `struct plugin_api` pointer, they don't know how to call standard library functions (like `memcpy` or `printf`) natively.

When `ctrdlOpen()` loads a plugin `.so` file, it encounters unresolved symbols.

```c
/* firmware/target/hosted/ctru/lc-program-resolver.c */
void* programResolver(const char* sym, void *userData)
{
    /* If the plugin asks for standard libc functions, return the 3DS address */
    if (strcmp(sym, "memcpy") == 0) return memcpy;
    if (strcmp(sym, "memset") == 0) return memset;
    if (strcmp(sym, "printf") == 0) return printf;

    /* Otherwise, return NULL and let the plugin_api handle Rockbox calls */
    return NULL;
}
```

This resolver acts as a dynamic linker bridge, patching the loaded binary with physical RAM addresses of the host OS's libc functions.

### The ESP-IDF Static Registry Advantage
The CTRU port highlights the massive complexity of dynamic loading on an RTOS. The ESP-IDF port completely avoids the `programResolver` nightmare by compiling the codecs *statically*.

Because the ESP-IDF CMake system links the codecs into `librockbox.a` alongside `newlib`, the ESP32 linker automatically resolves `memcpy` and `memset` at compile time. The codecs still use the `struct codec_api` jump table to access Rockbox-specific UI and Audio functions, but they require absolutely zero runtime symbol resolution for standard C libraries.

## 10. Build System: App Wrapper vs Makefiles
The CTRU port is fundamentally an application wrapper. It compiles using the standard Rockbox Makefiles (via `tools/configure`), but it specifically generates a Nintendo 3DS `.3dsx` executable file.

To achieve this, the Makefile invokes `arm-none-eabi-gcc` but links against `libctru.a`.

The ESP-IDF port diverges significantly here. The Rockbox Makefiles are designed for monolithic standalone compilation. Because ESP-IDF is built entirely around CMake (requiring partition tables, bootloader generation, and massive external components like LwIP and FreeRTOS), forcing the Rockbox Makefiles to generate an `.elf` suitable for `esptool.py` is nearly impossible.

The ESP-IDF port must treat Rockbox source code as a *component* inside an overarching CMake project, reversing the CTRU paradigm where `libctru` is treated as a library inside a Rockbox Make project.


## 11. System Tick and Kernel State Isolation (`kernel-ctru.c`)

When analyzing the CTRU port, the most critical behavior mapped is the `tick_timer()` execution context.

The Nintendo 3DS `sys_add_timer()` creates an asynchronous OS callback that interrupts standard thread execution at a fixed interval (100Hz).

```c
/* firmware/target/hosted/ctru/kernel-ctru.c */
static void tick_timer(void *arg)
{
    (void)arg;
    current_tick++;
    call_tick_tasks();
}

void tick_start(unsigned int interval_in_ms)
{
    tick_timer_id = sys_add_timer(interval_in_ms, tick_timer, NULL);
}
```

The 3DS timer service callback is executed in a specialized OS context. Because Rockbox’s `button_tick()` and `timeout_tick()` are called inside `call_tick_tasks()`, they must not invoke `sysThread` blocking primitives.

This directly maps to the ESP-IDF FreeRTOS software timer implementation (`xTimerCreate`). As identified in the ESP-IDF porting strategy, running `call_tick_tasks()` in a software timer task presents a severe risk of priority starvation, as the ESP-IDF timer task typically runs at `configTIMER_TASK_PRIORITY = 1`.

Because the Nintendo 3DS executes these timer callbacks with extremely high priority and low latency (handled by the Horizon OS kernel), the CTRU port avoids UI sluggishness. For the ESP32 port to achieve the same responsiveness, it must manually bypass the ESP-IDF timer daemon and implement a dedicated high-priority FreeRTOS task running `vTaskDelayUntil()`.

## 12. Display Abstraction: Double Buffering and VSync

The 3DS features a complex dual-screen setup. The `lcd-bitmap.c` file abstracts this by allocating a Rockbox framebuffer in main RAM (`FBADDR`) and requesting DMA flushes to the GPU VRAM.

```c
/* firmware/target/hosted/ctru/lcd-bitmap.c */
void lcd_init_device(void)
{
    /* Initialize the 3DS GPU */
    gfxInit(GSP_BGR8_OES, GSP_RGB565_OES, false);

    /* Set the bottom screen (touchscreen) to double buffering */
    gfxSetDoubleBuffering(GFX_BOTTOM, true);

    /* Get the VRAM pointer for the physical screen */
    u16 fb_width, fb_height;
    u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &fb_width, &fb_height);
}

void update_framebuffer(void)
{
    /* Flush the cache to ensure the GPU sees the new pixels */
    GSPGPU_FlushDataCache(FBADDR(0,0), LCD_WIDTH * LCD_HEIGHT * 2);

    /* Swap buffers on VSync */
    gfxScreenSwapBuffers(GFX_BOTTOM, false);
}
```

This sequence illustrates the exact pattern necessary for ESP-IDF. The `update_framebuffer()` function is equivalent to an `esp_lcd_panel_draw_bitmap()` call.

However, the CTRU port relies on `gfxScreenSwapBuffers()`, which inherently waits for the physical screen's VSync (Vertical Synchronization) signal before completing the frame. This prevents screen tearing during fast scrolling.

Most inexpensive SPI TFT screens (ILI9341/ST7789) attached to an ESP32 do not expose a hardware TE (Tearing Effect) pin. Therefore, the ESP32 port must either rely on the raw speed of the SPI DMA (e.g., 40MHz+ to minimize tearing visibility) or utilize partial updates (`lcd_update_rect`) heavily, as blasting a full 320x240 frame at 40MHz without VSync will inevitably result in visible tearing during UI transitions.

## 13. Codec Search API and Plugins

The Rockbox plugin architecture uses a standard POSIX API for directory iteration, which the CTRU port must simulate via `firmware/target/hosted/ctru/lib/sys_dir.c`.

When the user selects a file in the Rockbox UI, it calls `open_plugin()`.
If it's an MP3, it calls `codec_load()`. The UI thread must search the `/sdcard/.rockbox/codecs/` directory for `mp3.codec`.

```c
/* apps/codecs.c */
int codec_load_file(const char *plugin, struct codec_api *api)
{
    char path[MAX_PATH];
    codec_get_full_path(path, plugin);
    curr_handle = lc_open(path, codecbuf, CODEC_SIZE);
}
```

The CTRU port natively supports this via its VFS abstraction over the 3DS `FSUSER` service.
The ESP-IDF port perfectly mimics this via the newlib `esp_vfs_fat`.

However, because the ESP32 port cannot use `lc_open()` to execute RAM directly (due to Instruction Cache limitations without `CONFIG_SPIRAM_XIP_FROM_PSRAM`, and massive code churn if using `dlopen` without a POSIX host), the entire directory search phase must be bypassed.

The ESP32 port fundamentally rewires the architecture:
- **CTRU / Native Rockbox:** Search SD Card → Load Binary into RAM → Pass `codec_api` pointer → Execute.
- **ESP32:** Search Static Registry (`codec_registry[]`) → Find Function Pointer in Flash `.text` → Pass `codec_api` pointer → Execute.

This static registry circumvents the entire VFS bottleneck during track transitions, enabling instantaneous codec switching on the ESP32, whereas the 3DS must physically load a 50KB-100KB `.so` file from the SD card into RAM for every new track format.


## 14. The Hardware Control Matrix (Powermgmt & Backlight)
A critical feature of any DAP is power management and backlight dimming. The Nintendo 3DS `libctru` provides specialized OS hooks for this.

```c
/* firmware/target/hosted/ctru/powermgmt-ctru.c */
int _battery_voltage(void)
{
    u8 batteryLevel = 0;
    PTMU_GetBatteryLevel(&batteryLevel);

    // Scale 1-5 to a millivolt equivalent for Rockbox
    return batteryLevel * 1000;
}

bool usb_inserted(void)
{
    u8 chargeState = 0;
    PTMU_GetBatteryChargeState(&chargeState);
    return chargeState != 0;
}
```

The ESP32 port must recreate these exact abstractions via `powermgmt-esp32.c`.

1. **Battery Voltage:** The ESP32 ADC (`adc_oneshot_read`) must sample the physical voltage divider on the battery pin, scaling it to the expected `millivolts` integer for Rockbox.
2. **USB Detection:** The ESP32 `usb_inserted()` must read the physical VBUS pin state (`gpio_get_level()`) to determine if the device is charging.

### Backlight Dimming (`backlight-ctru.c`)

```c
/* firmware/target/hosted/ctru/backlight-ctru.c */
void backlight_set_brightness(int val)
{
    /* Use libctru's APT service to set screen brightness */
    aptSetBrightness(APT_LCD_BOTTOM, val * 10);
}
```

On the ESP32, driving an SPI TFT, this `backlight_set_brightness()` maps perfectly to an LED PWM driver (`ledc_set_duty()`). Rockbox's internal `backlight_tick()` automatically fades this value to 0 during user inactivity.

## 15. The Audio Interface Deep Dive: NDSP vs I2S Push
The most vital distinction between Rockbox bare-metal and Hosted ports is how they interact with audio DMA buffers.

The Nintendo 3DS features a dedicated DSP (NDSP) hardware subsystem. To output audio, the CTRU port configures a `ndspWaveBuf` array, effectively creating a multi-stage ring buffer.

```c
/* firmware/target/hosted/ctru/pcm-ctru.c */
static void pcm_write_to_soundcard(const void *pcm_buffer, size_t pcm_buffer_size, ndspWaveBuf *dsp_buffer)
{
    s16 *buffer = dsp_buffer->data_pcm16;

    /* Copy the Rockbox PCM array into the 3DS NDSP array */
    memcpy(buffer, pcm_buffer, pcm_buffer_size);

    dsp_buffer->nsamples = pcm_buffer_size / 2 / sizeof(s16);

    /* Submit the buffer to the hardware queue */
    ndspChnWaveBufAdd(0, dsp_buffer);

    /* Flush the cache so the DSP can read physical RAM */
    DSP_FlushDataCache(buffer, pcm_buffer_size);
}
```

This sequence is exactly what was established in Errata #3 and Errata #1 for the ESP-IDF porting strategy.
Because the ESP32 I2S hardware cannot directly consume the PSRAM-based Rockbox `audiobuf` (due to PSRAM DMA restrictions), the ESP-IDF port must manually `memcpy()` the audio data into an internal SRAM bounce buffer, exactly as the CTRU port copies it into `dsp_buffer->data_pcm16`.

Furthermore, because the DMA expects physical RAM and not cached data, the CTRU port utilizes `DSP_FlushDataCache()`. This maps perfectly to the ESP32 `esp_cache_msync()` requirement established in the prior ESP32 Errata section.

The CTRU port empirically proves that the "Bounce Buffer and Cache Flush" architecture is the standard, stable method for interfacing a complex, cache-dependent embedded RTOS with Rockbox's bare-metal audio pipeline.


## 16. The "Missing Link" of Interrupts vs Ticks
Finally, we must contrast how these ports handle internal device events.

Rockbox relies on a single hardware tick to poll almost all non-audio inputs.

The Nintendo 3DS port wraps this tick inside `kernel-ctru.c`. However, unlike a bare-metal device where `button_tick` triggers instantly off a GPIO interrupt, the CTRU relies entirely on polling the 3DS `hidKeysDown()` service at 100Hz.

```c
/* firmware/target/hosted/ctru/button-ctru.c */
int button_read_device(void)
{
    /* Poll the 3DS HID Service */
    hidScanInput();
    u32 keys = hidKeysHeld();
    int btn = BUTTON_NONE;

    /* Map 3DS D-Pad to Rockbox constants */
    if (keys & KEY_DUP) btn |= BUTTON_UP;
    if (keys & KEY_DDOWN) btn |= BUTTON_DOWN;
    if (keys & KEY_A) btn |= BUTTON_SELECT;

    return btn;
}
```

The ESP32 port must behave exactly like this. Rather than configuring complex FreeRTOS GPIO interrupts or Queues for button presses, the ESP32 simply reads the static boolean state of the GPIO pins (`gpio_get_level`) during the high-priority `button_tick` task.

Rockbox's 100Hz debouncer (`button.c`) handles all edge-detection, repeating, and long-press mechanics internally. Any attempt to use an ESP32 ISR to push buttons to a FreeRTOS queue and then read that queue inside `button_read_device()` is redundant and adds massive architectural overhead.

The CTRU port cleanly proves that polling the hardware SDK at 100Hz is the intended behavior for hosted/RTOS architectures, providing millisecond-perfect UI responsiveness without touching a single hardware interrupt.

---
*End of CTRU Architectural Analysis Report.*
