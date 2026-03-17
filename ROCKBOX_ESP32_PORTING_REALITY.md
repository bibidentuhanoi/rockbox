# The Brutal Reality of Porting Rockbox to ESP32

Theoretical architectural reports often gloss over the messy reality of 20-year-old C codebases. This document strips away the academic abstraction and details exactly what happens when you attempt to compile Rockbox for an entirely new, RTOS-driven architecture like the ESP32 using ESP-IDF.

## I. The Myth of the Rockbox "HAL"

The most painful realization when porting Rockbox is that **there is no clean Hardware Abstraction Layer (HAL) contract**.

There is no `HAL.h` interface that says "Implement these 47 functions and you're done." Instead, the contract is implicit. It is a massive, undocumented, tangled web of dependencies discovered purely through linker errors.

The actual porting process looks like this:
1. You create `config-esp32.h` with your best guess at the required `#define` macros (like `LCD_WIDTH`, `HAVE_LCD_COLOR`, `MEMORYSIZE`). You get it wrong. You iterate 50 times.
2. You compile. The linker spits out 200+ `undefined reference` errors. Some are obvious (`lcd_update()`, `button_read_device()`). Some are obscure (`audiohw_set_prescaler()`, `adc_read()`, `power_off()`). Some are dangerous traps—deep kernel internals you thought were abstract but actually bleed into the UI layer.
3. You stub them all out just to get the build to link.
4. It compiles. It crashes immediately at runtime.

Why? Because the *call order* between these functions contains hidden, undocumented assumptions. For example, depending on a specific `#ifdef` chain you can't easily see without tracing the Makefiles, `lcd_init_device()` might be called *before* `system_init()` on some targets, and *after* it on others.

This is what it actually means to "satisfy the linker one missing symbol at a time." It is an agonizing, iterative process of reverse-engineering the boot sequence.


## II. The Clean Overrides (The Easy Parts)

If your project scope is explicitly: **UI + File Browser + SD Playback (MP3/FLAC) + I2S Output + EQ + Codecs (NO Plugins, NO Radio, NO WiFi)**, then the following subsystems genuinely *are* modular and work exactly as you’d hope. Write your ESP-IDF implementation, and Rockbox truly doesn't care how you talk to the hardware.

### Audio Output (`pcm-esp32.c`)
- `pcm_play_dma_init()`: Configure the ESP32 I2S peripheral.
- `pcm_play_dma_start()`: Start your dedicated FreeRTOS audio feeder task.
- `pcm_play_dma_stop()`: Stop it.
- `pcm_play_dma_complete_callback()`: The callback you invoke from your feeder task when `i2s_write()` returns, asking Rockbox for the next mixed chunk.
- `pcm_play_lock()` / `pcm_play_unlock()`: Wrap a standard FreeRTOS Mutex.

### Audio Hardware Control (`audiohw-esp32.c`)
- `audiohw_init()`: Configure the DAC. If you are using a PCM5102A (which has no I2C config), this is literally an empty function (`return;`).
- `audiohw_set_volume()`: Write to I2C if your DAC supports it. Otherwise, return early, and Rockbox seamlessly falls back to software DSP volume control.
- `audiohw_mute()` / `audiohw_close()`: Standard hardware sleep/shutdown commands.

### Display (`lcd-esp32.c`)
- `lcd_init_device()`: Initialize the SPI bus and send the ILI9341/ST7789 init sequence.
- `lcd_update()`: Blast the full `FBADDR` array over SPI DMA (`esp_lcd_panel_draw_bitmap`).
- `lcd_update_rect(x, y, w, h)`: Calculate the memory offset and blast a partial frame to the LCD dirty rect.
- `lcd_enable()`, `lcd_set_contrast()`: Usually empty stubs or simple PWM backlight control.

### Buttons (`button-esp32.c`)
- `button_read_device()`: Read your ESP32 GPIOs and return a raw bitmask (e.g., `BUTTON_PLAY | BUTTON_LEFT`). *That's it.* Rockbox’s internal `button_tick()` handles all the complex debouncing, repeat delays, and hold states. Do not overcomplicate this.

### Storage (`storage-esp32.c` / `file.c`)
This is where the ESP-IDF VFS (Virtual File System) saves you weeks of work.
You do *not* override Rockbox's internal FAT drivers. You initialize the SD card (`esp_vfs_fat_sdmmc_mount("/sdcard", ...)`) in `app_main()`. You then tell Rockbox its root directory is `/sdcard`.

All of Rockbox's internal POSIX calls (`open()`, `read()`, `readdir()`, `lseek()`) pass transparently through the newlib VFS layer down to ESP-IDF's FatFs driver. Rockbox doesn't know or care.

---

## III. The Messy Overrides (The Atomic Sync Stack)

Here is where the modularity completely shatters.

Rockbox’s core scheduler (`thread.c`) is deeply entangled with its synchronization primitives. The `struct thread_entry` isn’t just an opaque handle. Mutexes, semaphores, and message queues in Rockbox read and write directly to `thread_entry->state` and `thread_entry->wqp` (Wait Queue Pointer).

### The "All or Nothing" Rule
If you decide to replace `create_thread()` with FreeRTOS’s `xTaskCreatePinnedToCore()` (which you must, to use ESP-IDF), you **cannot** keep Rockbox’s internal `mutex.c` or `queue.c`. If you try, the Rockbox queue will attempt to manipulate thread states that your FreeRTOS tasks don't possess, instantly causing a kernel panic.

**You must replace the entire synchronization stack together as a single, atomic unit:**
1.  **Threads:** `create_thread()`, `thread_wait()`, `thread_exit()` → FreeRTOS Tasks.
2.  **Mutexes:** `mutex_init()`, `mutex_lock()`, `mutex_unlock()` → `xSemaphoreCreateMutex()`, `xSemaphoreTake()`, `xSemaphoreGive()`.
3.  **Semaphores:** `semaphore_init()`, `semaphore_wait()`, `semaphore_release()` → FreeRTOS Counting Semaphores.
4.  **Queues:** `queue_init()`, `queue_post()`, `queue_wait()`, `queue_wait_w_tmo()` → `xQueueCreate()`, `xQueueSend()`, `xQueueReceive()`.

*This is precisely what the Rockbox SDL port does.* The SDL port (`firmware/target/hosted/sdl/thread-sdl.c`) is your exact template for this port. It replaces the entire stack natively with POSIX primitives. You must replicate that approach identically using FreeRTOS primitives.

### The System Tick Constraint (`kernel-esp32.c`)
You need Rockbox's `current_tick` variable to increment at exactly the `HZ` rate (typically 100Hz, or 10ms).

As established in the errata, you *cannot* use an ESP-IDF software timer (`xTimerCreate`). Software timers run in the low-priority Timer Daemon task. If the UI or Audio threads preempt the daemon, your debouncing and UI timers lag.

You must create a dedicated, high-priority FreeRTOS task utilizing `vTaskDelayUntil()`.

```c
void rockbox_tick_task(void *pvParameters)
{
    TickType_t xLastWakeTime = xTaskGetTickCount();
    const TickType_t xFrequency = pdMS_TO_TICKS(1000 / HZ);

    while (1) {
        vTaskDelayUntil(&xLastWakeTime, xFrequency);
        current_tick++;
        call_tick_tasks(); /* Runs button_tick() and timeout checkers */
    }
}
```

**Critical Subtlety:** In bare-metal Rockbox, `call_tick_tasks()` executes inside a hardware ISR, meaning the functions it calls *cannot block*. By moving it to a FreeRTOS Task (following the SDL model), it technically gains the ability to block. However, you must meticulously ensure that functions like `button_tick()` remain fast and do not sleep, or you will ruin the real-time response of the UI.

---

## IV. The Codec Loading Reality (The Core Patch)

This is the largest deviation from the "Just override the HAL" philosophy.

Rockbox’s codec system is hardcoded to assume that `lc_open()` will read a flat `.codec` binary from the SD card into a RAM buffer, returning an executable pointer.

On ESP32, you **cannot** execute dynamically loaded binaries from PSRAM or standard SRAM due to Instruction Cache (I-Cache) mapping constraints. You must statically link the codecs (MP3, FLAC, AAC) into the `.text` segment of your main ESP-IDF firmware binary.

### The Dilemma: No Clean Override Point
There is no existing HAL hook for this. This is not a hardware abstraction—it's a modification to the core `apps/` layer.

You have two choices:
1.  **The Massive Code Churn Fork:** Rewrite `apps/codecs.c` and `apps/codec_thread.c` entirely to bypass `lc_open()`, maintaining a massive, un-upstreamable hard fork of core Rockbox logic.
2.  **The `lc_open()` Override Hack (Recommended):** Instead of rewriting `codecs.c`, you override the `lc_open()` function itself inside your target directory. You hijack its functionality so that it behaves like a lookup table rather than a file loader.

```c
/* esp32-rockbox/firmware/target/xtensa/esp32/lc-esp32.c */

/* Extern the entry points of your statically linked codecs */
extern void* codec_mp3_entry;
extern void* codec_flac_entry;

void * lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    /* Intercept the filename request and return a static pointer! */
    if (strstr(filename, "mp3.codec")) return &codec_mp3_entry;
    if (strstr(filename, "flac.codec")) return &codec_flac_entry;

    return NULL;
}
```

By hijacking `lc_open()`, the core Rockbox UI remains completely unaware that the codecs are statically linked in flash rather than dynamically loaded into RAM. This isolates the "dirty" changes to your target-specific ESP32 files, vastly reducing the patch surface area and preserving sanity when merging upstream Rockbox updates.
