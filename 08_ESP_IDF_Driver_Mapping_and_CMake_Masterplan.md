# 08_ESP_IDF_Driver_Mapping_and_CMake_Masterplan.md

## 1. CMake & Build System Translation

Rockbox's `configure` script is powerful but archaic. ESP-IDF mandates CMake. We will create a hybrid approach:
1.  **`esp32-build/`**: A standard ESP-IDF project directory.
2.  **`components/rockbox/`**: The Rockbox source tree (symlinked or copied).
3.  **`CMakeLists.txt`**: A manual translation of `apps.make` and `firmware.make`.

### The Theoretical CMake Script (`components/rockbox/CMakeLists.txt`)

```cmake
idf_component_register(
    SRCS
        # Apps
        "apps/main.c"
        "apps/playback.c"
        "apps/menu.c"
        # ... (hundreds of files) ...

        # Firmware / Kernel
        "firmware/kernel/thread-freertos.c"
        "firmware/common/time.c"

        # Drivers (ESP32 Specific)
        "firmware/target/hosted/esp32/pcm-esp32.c"
        "firmware/target/hosted/esp32/lcd-esp32.c"
        "firmware/target/hosted/esp32/button-esp32.c"

    INCLUDE_DIRS
        "apps"
        "firmware/export"
        "firmware/include"
        "firmware/target/hosted/esp32"

    REQUIRES
        driver
        esp_lcd
        fatfs
        spiffs
)

# Defines from autoconf.h must be passed here
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    -DROCKBOX
    -DHAVE_LCD_COLOR
    -DLCD_WIDTH=320
    -DLCD_HEIGHT=240
    -DMODEL_NAME="ESP32-S3"
    -DCONFIG_CPU="ESP32"
    -DPLATFORM_HOSTED
)
```

## 2. Audio / I2S Mapping

**Rockbox File:** `firmware/target/hosted/esp32/pcm-esp32.c`

| Rockbox API | ESP-IDF API | Logic |
| :--- | :--- | :--- |
| `pcm_init()` | `i2s_driver_install()` | Setup I2S0, standard mode, 16-bit, 44.1kHz. |
| `pcm_set_frequency(sample_rate)` | `i2s_set_clk()` | Dynamic sample rate switching. |
| `pcm_play_lock()` | `xSemaphoreTake` | Prevent concurrent access. |
| `pcm_play_data(start, size)` | `i2s_write()` | **Critical:** Rockbox expects this to trigger DMA. `i2s_write` is blocking. usage requires a separate "feeder task" or callback. |

**The Feeder Task Strategy:**
Rockbox's `pcm.c` logic is often interrupt-driven ("DMA finished, give me more").
ESP-IDF `i2s_write` blocks until space is available.
We should create a `pcm_feeder_task` that waits on a queue from Rockbox's mixer and calls `i2s_write`.

## 3. Storage & VFS

**Rockbox File:** `firmware/target/hosted/esp32/storage-esp32.c`

*   **Rockbox expects:** `open()`, `read()`, `write()`, `opendir()`.
*   **ESP-IDF provides:** `vfs_fat` (Virtual File System).
*   **Bridge:** Rockbox `system-hosted.c` already maps `open` to standard POSIX `open`.
*   **Initialization:**
    *   Initialize SDMMC Host.
    *   `esp_vfs_fat_sdmmc_mount("/sdcard", ...)`
    *   Rockbox assumes root is `/`. We might need to `chroot` or alias `/` to `/sdcard`.

## 4. Display & Inputs

**Display:**
*   **Driver:** ESP-LCD (Generic I8080 or SPI).
*   **Buffer:** Allocate frame buffer in SPIRAM.
*   **Flush:** `lcd_update()` calls `esp_lcd_panel_draw_bitmap()`.

**Inputs:**
*   **Buttons:** GPIO ISRs -> `xQueueSendFromISR` -> Rockbox `button_queue`.
*   **Touch:** I2C Driver (GT911/FT6x06) -> Poll in a timer task -> Rockbox `touchscreen` API.

## 5. The Master Action Plan

1.  **Environment Setup:** Install ESP-IDF v5.x.
2.  **Scaffolding:** Create `esp32-build/` and populate `CMakeLists.txt` with a minimal subset of files (just `main.c` and dummy `system_init`).
3.  **Kernel Port:** Implement `thread-freertos.c`. Get `create_thread` working.
4.  **LCD Port:** Get the Rockbox logo to display (`lcd_init`, `lcd_update`).
5.  **Storage Port:** Mount SD card. Verify `fopen` works.
6.  **Audio Port:** Implement `pcm-esp32.c`. Play a sine wave.
7.  **Codec Integration:** Compile `libmad` statically. Link it.
8.  **UI Bring-up:** Compile `apps/` and fix thousands of compiler errors (missing defines, struct mismatches).
9.  **First Boot:** See the Rockbox Main Menu.
