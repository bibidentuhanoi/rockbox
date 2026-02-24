# 11_Rockbox_API_Reference_and_System_Integration.md

## Abstract
This document provides a comprehensive, encyclopedic reference for the internal and external APIs that form the "central nervous system" of the Rockbox firmware. Unlike standard operating systems that rely on syscalls (e.g., `ioctl`, `read`, `write`) to bridge User Space and Kernel Space, Rockbox uses a monolithic linking model where "applications" (plugins and codecs) interact with the firmware via massive function pointer tables (`struct plugin_api`, `struct codec_api`). This file documents these APIs, along with the internal Hardware Abstraction Layer (HAL) contracts (USB, LCD, Storage) that must be implemented for any new port. It serves as the definitive interface specification for the ESP32 porting effort.

---

## 1. The Plugin API (`docs/PLUGIN_API`)
The Plugin API is the mechanism by which dynamic Position Independent Code (PIC) binaries (`.rock` files) interact with the main firmware. It is defined in `apps/plugin.h` and documented in `docs/PLUGIN_API`.

### 1.1 The API Trampoline Strategy
Because plugins are loaded at runtime without a dynamic linker, they cannot resolve symbols like `printf` or `lcd_update` directly. Instead, the firmware passes a pointer to a `struct plugin_api` table.

**Versioning:**
The API is strictly versioned using `PLUGIN_API_VERSION`.
*   **Current Version:** 279 (as seen in `apps/plugin.h`).
*   **Mechanism:** The loader checks `hdr->api_version == PLUGIN_API_VERSION`. If they mismatch, the plugin is rejected to prevent a crash caused by a mismatched function pointer table layout.

### 1.2 API Categories
The `plugin_api` struct contains over 400 function pointers. They are categorized as follows:

#### 1.2.1 Core Kernel Services
These allow plugins to participate in the cooperative multitasking environment.
*   `void yield(void)`: **CRITICAL.** Plugins must call this frequently to let the audio thread refill the DMA buffer.
*   `unsigned sleep(unsigned ticks)`: Sleep for $N$ ticks (usually 1/HZ seconds).
*   `int create_thread(void (*func)(void), ...)`: Plugins can spawn their own threads (e.g., Doom spawns a game logic thread).
*   `void thread_wait(unsigned int thread_id)`: Join a thread.
*   `void thread_exit(void)`: Terminate current thread.
*   `void mutex_init(struct mutex *m)`: Initialize a mutex.
*   `void mutex_lock(struct mutex *m)`: Acquire lock (blocking).
*   `void mutex_unlock(struct mutex *m)`: Release lock.

#### 1.2.2 Graphical User Interface (LCD)
Direct access to the logical framebuffer.
*   `void lcd_update(void)`: Pushes the dirty framebuffer to the hardware display.
*   `void lcd_clear_display(void)`: Fills the buffer with the background color.
*   `void lcd_bitmap(const fb_data *src, int x, int y, int w, int h)`: Blits a bitmap to the screen.
*   `void lcd_drawline(int x1, int y1, int x2, int y2)`: Bresenham line draw.
*   `void lcd_drawrect(int x, int y, int w, int h)`: Draw rectangle outline.
*   `void lcd_fillrect(int x, int y, int w, int h)`: Fill rectangle.
*   `void lcd_puts(int x, int y, const char *str)`: Draw text using current font.
*   `void lcd_setfont(int font)`: Select system or user font.
*   `void lcd_set_contrast(int val)`: Hardware-specific contrast control.
*   `void lcd_set_drawmode(int mode)`: Set ROP (DRMODE_SOLID, DRMODE_XOR).

#### 1.2.3 File System (FatFs Wrapper)
Rockbox provides a POSIX-like file I/O layer.
*   `int open(const char *path, int oflag, ...)`
*   `ssize_t read(int fd, void *buf, size_t count)`
*   `off_t lseek(int fd, off_t offset, int whence)`
*   `int close(int fd)`
*   `int rename(const char *old, const char *new)`
*   `int remove(const char *path)`
*   `DIR* opendir(const char *name)`
*   `struct dirent* readdir(DIR *dir)`
*   `int closedir(DIR *dir)`
*   `int mkdir(const char *path)`

#### 1.2.4 Audio & Sound Control
Plugins can control playback or generate their own sound.
*   `void audio_play(ulong elapsed, ulong offset)`: Resume music playback.
*   `void audio_stop(void)`: Stop the playback engine (releasing the `audiobuf`).
*   `void audio_pause(void)`: Pause playback (keep `audiobuf`).
*   `void audio_resume(void)`: Resume from pause.
*   `void audio_next(void)`: Skip to next track.
*   `void audio_prev(void)`: Skip to previous track.
*   `void sound_set(int setting, int value)`: Adjust volume, bass, treble, balance.
*   `void pcm_play_data(void *start, size_t size)`: Injection point for raw PCM data (used by game emulators).

#### 1.2.5 Input Subsystem
*   `long button_get(bool block)`: Pops the next button event from the queue.
*   `int button_status(void)`: Returns the current bitmask of pressed buttons (non-blocking).
*   `void button_clear_queue(void)`: Flush pending input.

#### 1.2.6 Memory Management (Buflib)
The plugin API exposes the `buflib` relocatable memory allocator.
*   `void buflib_init(struct buflib_context* ctx, void* buf, size_t size)`
*   `int buflib_alloc(struct buflib_context* ctx, size_t size)`
*   `int buflib_free(struct buflib_context* ctx, int handle)`
*   `void* buflib_get_data(struct buflib_context* ctx, int handle)`
*   `bool buflib_shrink(struct buflib_context* ctx, int handle, void* new_start, size_t new_size)`

#### 1.2.7 Playlist Control
*   `int playlist_amount(void)`: Get track count.
*   `struct playlist_info* playlist_get_current(void)`
*   `int playlist_insert_track(struct playlist_info* playlist, const char *filename, int position, ...)`
*   `int playlist_remove_all_tracks(struct playlist_info *playlist)`
*   `int playlist_shuffle(int random_seed, int start_index)`

#### 1.2.8 Settings Access
Plugins can read/write global settings.
*   `struct user_settings *global_settings`: Pointer to the massive settings struct.
*   `bool set_option(const char* string, const void* variable, ...)`
*   `int settings_save(void)`: Persist changes to `config.cfg`.

#### 1.2.9 DSP Control
*   `void dsp_eq_enable(bool enable)`: Toggle equalizer.
*   `int32_t dsp_get_timestretch(void)`
*   `void dsp_set_timestretch(int32_t percent)`
*   `void pcm_apply_settings(void)`: Commit hardware registers.

#### 1.2.10 USB & Power
*   `bool usb_inserted(void)`: Check VBUS state.
*   `int battery_level(void)`: Get percentage (0-100).
*   `int battery_voltage(void)`: Get millivolts.
*   `void sys_poweroff(void)`: Shut down the device.
*   `void sys_reboot(void)`: Reset the device.

---

## 2. The Codec API (`lib/rbcodec/codecs/codecs.h`)
While similar to the Plugin API, the Codec API is specialized for audio decoding. Codecs are stripped-down binaries that do one thing: convert a file stream to PCM samples.

### 2.1 The `codec_api` Structure
Defined in `lib/rbcodec/codecs/codecs.h`.

```c
struct codec_api {
    /* File I/O (Read-Only) */
    size_t (*read_filebuf)(void *ptr, size_t size);
    void   (*advance_buffer)(size_t amount);
    bool   (*seek_buffer)(size_t newpos);
    void   (*seek_complete)(void);

    /* Output Pipeline */
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);
    void (*set_elapsed)(unsigned long value);
    void (*set_offset)(size_t value);

    /* DSP Chain */
    struct dsp_config *dsp;
    void (*configure)(int setting, intptr_t value);

    /* Cooperative Multitasking */
    void (*yield)(void);
    unsigned (*sleep)(unsigned ticks);

    /* Threading (Multi-core targets only) */
#if NUM_CORES > 1
    unsigned int (*create_thread)(void (*function)(void), ...);
    void (*thread_wait)(unsigned int thread_id);
#endif
};
```

### 2.2 The Decoding Loop Protocol
1.  **Request Buffer:** The codec calls `read_filebuf` to get raw data from the disk.
2.  **Decode:** The codec runs its algorithm (MP3, FLAC, etc.) to produce PCM samples.
3.  **Insert:** The codec calls `pcmbuf_insert`. This function:
    *   Writes samples to the ring buffer.
    *   **BLOCKS** if the buffer is full.
    *   Calls `yield()` internally while waiting for space.
4.  **Repeat:** The process continues until EOF.

### 2.3 The Encoder API (`struct enc_callback_t`)
For recording, Rockbox uses a separate API (`enc_base.h`). It works in reverse:
*   **Capture:** The `pcm_record_data` callback pushes samples from the ADC/Mic.
*   **Encode:** The encoder (e.g., MP3/WAV) processes these chunks.
*   **Write:** The encoder calls `write()` to save the file.

---

## 3. The Hardware Abstraction Layer (HAL) APIs
These APIs are internal contracts that the `firmware/` drivers must satisfy.

### 3.1 The USB API (`docs/usb-api.md`)
Rockbox implements a USB stack that handles Control, Bulk, and Interrupt transfers.
*   **Core Function:** `usb_core_control_request(req, data)`
*   **Driver Responsibility:** The hardware driver (e.g., `usb-esp32.c`) must call this function when a SETUP packet arrives.
*   **Response Codes:**
    *   `USB_CONTROL_ACK`: Request handled.
    *   `USB_CONTROL_STALL`: Request not supported.
    *   `USB_CONTROL_RECEIVE`: Ready for Data OUT phase.

### 3.2 The LCD Driver API (`firmware/export/lcd.h`)
Every target must implement these low-level primitives:
*   `void lcd_init(void)`: Initialize the display controller (SPI/I2C/Parallel).
*   `void lcd_update(void)`: Flush the entire framebuffer.
*   `void lcd_update_rect(int x, int y, int w, int h)`: Flush a specific region.
*   `void lcd_set_drawmode(int mode)`: Set logical operations (COPY, XOR).
*   **Data Format:** The framebuffer format depends on `LCD_DEPTH` (1, 2, 16, 24).

### 3.3 The Storage API (`firmware/export/storage.h`)
The interface to the underlying block device (SD Card, HDD, NAND). Rockbox uses a simple block access model.
*   `int storage_init(void)`: Initialize the bus.
*   `int storage_read_sectors(unsigned long start, int count, void *buf)`: Read 512-byte sectors.
*   `int storage_write_sectors(unsigned long start, int count, const void *buf)`: Write 512-byte sectors.
*   `bool storage_present(int drive)`: Check for card insertion.
*   `bool storage_removable(int drive)`: True for SD cards.
*   **Concurrency:** These functions must be thread-safe as they are called by the `buffering` thread (high priority) and `main` thread (low priority). The `storage_read_sectors` function is often the bottleneck during playback and must be highly optimized (using DMA and multi-block transfers).

### 3.4 The PCM API (`firmware/export/pcm.h`)
The interface to the I2S/DAC hardware.
*   `void pcm_init(void)`: Setup I2S clocks and DMA.
*   `void pcm_play_lock(void)`: Acquire the PCM hardware.
*   `void pcm_play_data(void (*callback)(void), const void *start, size_t size)`: Start a DMA transfer.
    *   **Callback:** When the DMA finishes, it must call `pcmbuf_callback()` from the ISR.
*   `void pcm_set_frequency(unsigned int frequency)`: Set I2S sample rate.

### 3.5 The Button API (`firmware/export/button.h`)
The button driver provides the raw input events.
*   `int button_read_device(void)`: Called by `button_tick`. Returns bitmask.
*   **Debouncing:** Logic in `firmware/drivers/button.c` handles debounce state.
*   **Events:** `queue_post(&button_queue, ...)` injects events into the kernel.

### 3.6 The Power Management API (`firmware/export/powermgmt.h`)
Drivers must implement:
*   `int battery_voltage(void)`: Read ADC channel.
*   `bool charger_inserted(void)`: Check GPIO/PMIC status.
*   `void power_off(void)`: Cut power to the CPU (or deep sleep).

---

## 4. The Bootloader Interaction API
The bootloader and the main firmware communicate via a shared memory region or specific registers.

### 4.1 The Boot Vector (`crt0.S`)
The main firmware's entry point is defined in the linker script (`ENTRY(_start)`).
*   **Handshake:** The bootloader jumps to `_start`.
*   **Stack:** The bootloader sets up the initial stack pointer (`sp`).
*   **Arguments:** `r0`, `r1` (on ARM) may contain boot flags (e.g., `BOOT_ARG_ROCKBOX`, `BOOT_ARG_USB_MODE`).

### 4.2 The Payload Format (`.mi4` / `.ipod`)
The firmware binary is often wrapped in a container.
*   **MI4:** Used by SanDisk. Contains an encrypted header + scrambled payload.
*   **IPOD:** Used by Apple. Flat binary with a checksum at the end.
*   **ESP32:** We will use the standard ESP-IDF partition format (`app_main` entry).

### 4.3 The Argument Block
The bootloader passes essential data to the main firmware via a fixed structure in RAM.
```c
struct boot_args {
    unsigned long magic;      /* BOOT_MAGIC */
    unsigned long length;     /* Size of struct */
    unsigned char rid[32];    /* Random ID */
    unsigned char mac[6];     /* WiFi MAC (ESP32 specific) */
    int boot_volume;          /* Drive index */
};
```

---

## 5. System Integration Matrix
The following table maps which APIs are used by which major subsystems.

| API Name       | Used By (Consumer)       | Implemented By (Provider) | Criticality for ESP32 Port |
| :---           | :---                     | :---                      | :---                       |
| **Plugin API** | `apps/plugins/*.rock`    | `apps/plugin.c`           | **High** (Must be static)  |
| **Codec API**  | `apps/codecs/*.codec`    | `apps/codec_thread.c`     | **High** (Must be static)  |
| **LCD API**    | `apps/gui/`              | `firmware/drivers/lcd-*`  | **Medium** (Standard SPI)  |
| **USB API**    | `firmware/usb.c`         | `firmware/target/esp32/`  | **High** (TinyUSB Shim)    |
| **PCM API**    | `firmware/pcmbuf.c`      | `firmware/target/esp32/`  | **Critical** (I2S DMA)     |
| **Storage API**| `firmware/common/fat.c`  | `firmware/target/esp32/`  | **Critical** (SDMMC)       |
| **Button API** | `apps/action.c`          | `firmware/drivers/button*`| **Medium** (GPIO ISR)      |
| **Power API**  | `apps/gui/statusbar.c`   | `firmware/target/esp32/`  | **Low** (Mockable)         |

### 5.1 The Call Stack: A Trace Example
Tracing a call from a Plugin to the Hardware:
1.  **Plugin:** Calls `rb->lcd_update()`.
2.  **Trampoline:** Jumps to `plugin_api.lcd_update` (which points to `firmware/export/lcd.h:lcd_update`).
3.  **Firmware Kernel:** `lcd_update()` checks dirty bits in the framebuffer.
4.  **HAL Driver:** Calls `lcd_update_rect()` in `firmware/drivers/lcd-esp32.c`.
5.  **Hardware:** Driver writes SPI commands to the ST7789 display controller.

This layered approach allows the `apps/` code to remain completely agnostic of the underlying hardware.

---

## 6. The "Hosted" API Shim Layer
Historical ports (Simulator, Android) demonstrated how to "fake" these APIs on high-level OSs.

### 6.1 LCD Shim (Simulator)
Instead of writing to LCD registers, `sim_lcd_update` copies the framebuffer to an X11 window or SDL Surface.
```c
/* firmware/target/hosted/sdl/lcd-sdl.c */
void lcd_update(void) {
    // Lock SDL Surface
    // Memcpy rockbox_fb -> sdl_surface->pixels
    // Unlock and Flip
}
```

### 6.2 PCM Shim (Android)
Instead of DMA interrupts, the Android port uses a JNI thread to pull data.
```c
/* firmware/target/hosted/android/pcm-android.c */
void pcm_play_data(...) {
    // Write data to a Java ByteBuffer
    // Call AudioTrack.write() via JNI
}
```

### 6.3 Storage Shim (Linux)
The hosted port bypasses the raw sector access and uses standard file I/O.
```c
/* firmware/target/hosted/linux/storage-linux.c */
int storage_read_sectors(ulong start, int count, void *buf) {
    lseek(image_fd, start * 512, SEEK_SET);
    read(image_fd, buf, count * 512);
}
```

### 6.4 Threading Shim (SDL)
Rockbox threads are cooperative. SDL threads are preemptive.
*   **The Global Lock:** The shim uses a single `pthread_mutex_t` to serialize access to the kernel.
*   **Yield:** `thread_yield` releases the mutex and sleeps on a condition variable, allowing other threads to run.

---

## 7. Conclusion
Porting Rockbox to the ESP32 is essentially an exercise in implementing the **Provider** side of these APIs.
1.  **Storage:** Map `storage_read_sectors` to `sdmmc_read_sectors`.
2.  **LCD:** Map `lcd_update_rect` to `esp_lcd_panel_draw_bitmap`.
3.  **Audio:** Map `pcm_play_data` to `i2s_write`.
4.  **Plugins/Codecs:** The tricky part. Since we cannot load dynamic ELF files easily, we will statically link the "top 10" plugins and codecs into the main binary and patch the `plugin_load` function to jump to their internal addresses instead of opening a file. This preserves the API structure while bypassing the loader limitations.

## 8. Appendix: The Full API Checklist for Porting
To declare the ESP32 port "stable", the following functions must be implemented:

| Function Signature | Status | Notes |
| :--- | :--- | :--- |
| `adc_read` | Pending | Battery monitoring |
| `backlight_set_brightness` | Pending | PWM/LEDC |
| `button_read_device` | **Done** | GPIO ISR |
| `cpu_idle` | **Done** | `vTaskDelay` |
| `lcd_update` | **Done** | SPI Master |
| `pcm_play_data` | **Done** | I2S DMA |
| `storage_read_sectors` | **Done** | SDMMC |
| `usb_core_control_request` | Pending | TinyUSB |
| `rtc_read_datetime` | Pending | Internal RTC |
| `i2c_init` | Pending | For PMIC/FM |

## 9. The Extended Plugin API Reference
This section provides a deeper look into the less commonly used but vital parts of the plugin API.

### 9.1 Metadata & ID3 Parsing
Plugins often need to parse audio files. The firmware exports its robust metadata parser.
*   `bool get_metadata(struct mp3entry* id3, int fd, const char* trackname)`
*   `int count_mp3_frames(int fd, int startpos, ...)`
*   `char* get_codec_filename(int cod_spec)`

### 9.2 The "Talk" Interface (Voice UI)
Rockbox's accessibility features are exposed to plugins.
*   `int talk_id(int32_t id, bool enqueue)`: Speak a string ID (e.g., "Loading").
*   `int talk_file(const char *root, ...)`: Speak a filename (using .talk clips).
*   `int talk_number(long n, bool enqueue)`: Speak a number.
*   `void talk_force_shutup(void)`: Silence the voice immediately.

### 9.3 Custom UI Widgets
Plugins can use Rockbox's internal widget set.
*   **Lists:** `gui_synclist_init`, `gui_synclist_draw`.
*   **Menus:** `do_menu`.
*   **Keyboards:** `kbd_input` (On-screen keyboard).
*   **Progress Bars:** `splash_progress`.

### 9.4 Recording API (Encoders)
For plugins that capture audio (dictaphone).
*   `void pcm_init_recording(void)`
*   `void pcm_record_data(callback, ...)`
*   `void pcm_stop_recording(void)`
*   `void audio_set_recording_gain(int left, int right, int type)`

### 9.5 FM Radio API
*   `int radio_set_frequency(int freq)`
*   `int radio_get_frequency(void)`
*   `int radio_get_signal_strength(void)`
*   `bool radio_stereo_mode(bool stereo)`

---

## 10. HAL Implementation Guide: Bit-Banging the API
If a hardware driver (like I2S or SPI) is missing, the API can often be implemented via software bit-banging. This is slow but functional for initial ports.

### 10.1 Bit-Banged SPI (LCD)
```c
/* firmware/drivers/lcd-spi-sw.c */
void lcd_update_rect(int x, int y, int w, int h) {
    gpio_set(CS, 0);
    spi_write(0x2A); // CASET
    spi_write_data(x >> 8); spi_write_data(x & 0xFF);
    /* ... bit-bang loop ... */
    for(int i=0; i<w*h; i++) {
        uint16_t color = framebuffer[y][x];
        for(int b=0; b<16; b++) {
            gpio_set(CLK, 0);
            gpio_set(MOSI, (color >> (15-b)) & 1);
            gpio_set(CLK, 1);
        }
    }
    gpio_set(CS, 1);
}
```

### 10.2 Bit-Banged I2C (PMIC)
```c
/* firmware/drivers/i2c-sw.c */
void i2c_start(void) {
    gpio_set(SDA, 1); gpio_set(SCL, 1);
    delay_us(5);
    gpio_set(SDA, 0);
    delay_us(5);
    gpio_set(SCL, 0);
}
```

## 11. Legacy API Deprecation Strategy
Some APIs in `plugin.h` are vestigial from the Archos days (2002).
*   **`lcd_set_invert_display`:** Most modern color LCDs don't support hardware inversion. The driver should silently ignore this or implement it in software (slow).
*   **`lcd_blit_mono`:** On color targets, this must convert 1-bit data to 16-bit color on the fly.
*   **`fdprintf`:** Often unimplemented in favor of `snprintf` + `write`.

### 11.1 The "Stub" Implementation Pattern
To reach 100% link compatibility without implementing 100% of the features, we use a stub pattern.

```c
/* firmware/target/hosted/esp32/stubs.c */
void lcd_set_invert_display(bool yesno) {
    (void)yesno;
    /* ST7789 doesn't support inversion easily, ignore */
}

int radio_set_frequency(int freq) {
    (void)freq;
    return -1; /* No Radio Hardware */
}
```

---

## 12. Appendix: The Full API Checklist (Continued)

### 12.1 Audio & Mixer
| Function | Status | Notes |
| :--- | :--- | :--- |
| `pcm_set_frequency` | **Done** | I2S Clock Reconfig |
| `mixer_channel_play_data` | Pending | Software Mixer |
| `sound_set_bass` | Pending | DSP Lib |
| `sound_set_treble` | Pending | DSP Lib |

### 12.2 File System
| Function | Status | Notes |
| :--- | :--- | :--- |
| `open` | **Done** | VFS Shim |
| `read` | **Done** | VFS Shim |
| `write` | **Done** | VFS Shim |
| `lseek` | **Done** | VFS Shim |
| `opendir` | **Done** | VFS Shim |
| `readdir` | **Done** | VFS Shim |
| `dir_exists` | **Done** | `stat()` check |

### 12.3 System & Threading
| Function | Status | Notes |
| :--- | :--- | :--- |
| `create_thread` | **Done** | `xTaskCreate` |
| `thread_exit` | **Done** | `vTaskDelete` |
| `yield` | **Done** | `taskYIELD` |
| `sleep` | **Done** | `vTaskDelay` |
| `mutex_lock` | **Done** | `xSemaphoreTake` |
| `tick_get` | **Done** | `xTaskGetTickCount` |

### 12.4 Debugging
| Function | Status | Notes |
| :--- | :--- | :--- |
| `panicf` | **Done** | `abort()` |
| `debugf` | **Done** | `ESP_LOGI` |
| `splash` | Pending | LCD Text |

## 13. Conclusion
The Rockbox API is a massive, accumulated interface that reflects 20 years of embedded audio history. Porting it to ESP-IDF requires a "Bridge and Emulate" strategy:
1.  **Bridge** the modern drivers (I2S, SPI, SDMMC) to the HAL contracts.
2.  **Emulate** the legacy assumptions (direct framebuffer access, cooperative threading) using RTOS primitives and PSRAM.
3.  **Static Link** the dynamic components (plugins/codecs) to bypass the Harvard architecture limit.

## 14. Deprecation and Removal
Not all APIs will survive the transition. The following are officially deprecated for the ESP32 port.

### 14.1 ATA/IDE Interfaces
*   `storage_sleep_now`
*   `ata_read_sectors`
*   **Reasoning:** SD/MMC does not support ATA power states directly.

### 14.2 Archos Legacy
*   `mas_codec_writereg`
*   `dac_volume`
*   **Reasoning:** Specific to Micronas AS35xx chips. ESP32 uses software volume or I2S DAC volume.

### 14.3 Recording (Phase 1)
*   `pcm_record_data`
*   **Reasoning:** Initial port will focus on playback. I2S Input will be added in Phase 2.

## 15. Forward Compatibility
The `plugin_api` structure is designed to be append-only (mostly). However, for the ESP32 static linking approach, we can technically modify the struct without breaking binary compatibility (since there are no external binaries).
*   **Recommendation:** Maintain the layout strictly to allow future "Overlay" partitions where plugins might be loaded from flash partitions.

---

## 16. Archived API: The "H100" Era
To understand the evolution of the API, we document the deprecated calls from the iRiver H100 era. These are present in `plugin.h` but often stubbed.

### 16.1 ATA Power Management
*   `void ata_sleep_now(void)`: Forces the HDD to spin down immediately.
*   `bool ata_disk_is_active(void)`: Checks if the platter is spinning.
*   **Modern Equiv:** `storage_sleep()` and `storage_disk_is_active()`. The abstraction moved from "ATA" to "Storage".

### 16.2 Character-Cell LCDs
*   `void lcd_icon(int icon, bool enable)`: Used on players with segmented LCDs (like the Archos Player) to turn on the "Battery" or "Play" segment.
*   **Modern Equiv:** Pixel-based rendering in `statusbar.c`.

### 16.3 Hardware Codec Registers
*   `void mas_writereg(int reg, int val)`: Direct write to the Micronas MAS3507D DSP.
*   **Modern Equiv:** `dsp_configure()`. The firmware no longer exposes raw DSP registers to plugins, ensuring abstraction.

---

## 17. The `audiohw` Interface Detail
The `firmware/export/audiohw.h` file defines the contract between the high-level PCM driver and the specific codec chip (Wolfson, MAS, TLV320).

*   `void audiohw_set_volume(int vol_l, int vol_r)`: Hardware gain.
*   `void audiohw_set_frequency(int fsel)`: Sample rate configuration.
*   `void audiohw_mute(bool mute)`: Soft mute during transitions.
*   `void audiohw_close(void)`: Power down the DAC.
*   `void audiohw_init(void)`: Power up sequence.

**ESP32 Implementation:**
These functions will map to the `esp_codec_dev` component or manual I2C commands to the ES8388/AC101 audio chip on the development board.

---

## 18. Simulated API (UISimulator)
When running on the simulator, the hardware APIs are replaced by SDL/X11 wrappers.

### 18.1 Simulated Storage
*   `sim_disk_open(filename)`: Opens a `.disk` image file on the host.
*   `sim_disk_read_sectors`: Uses `pread` on the image file.
*   **Virtual FAT:** The simulator can also mount a host directory as a FAT drive using a custom VFAT implementation, allowing developers to drag-and-drop MP3s into the simulator window.

### 18.2 Simulated Audio
*   `sim_pcm_play`: Buffers data into an SDL_AudioCVT stream.
*   **Speed Control:** The simulator supports "Fast Forward" by increasing the SDL sample rate, allowing developers to test long playlists quickly.

### 18.3 Simulated LCD
*   `sim_lcd_update`: Loops over the framebuffer.
*   **Skinning:** Draws the "device skin" (PNG) around the framebuffer.
*   **On-Screen Display (OSD):** Draws red outlines around updated regions to debug dirty rect algorithms.

---

## 19. Conclusion
This document serves as the "Rosetta Stone" for the Rockbox API. Whether running on a 2005 iPod (Bare Metal), a 2010 Android Phone (Hosted), or a 2024 ESP32 (Hybrid), the `apps/` layer remains unchanged because it relies exclusively on these well-defined interfaces. The success of the ESP32 port depends entirely on the fidelity of the `firmware/target/hosted/esp32/` implementation of these 400+ functions.
