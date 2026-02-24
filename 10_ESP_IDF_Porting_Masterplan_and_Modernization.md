# 10_ESP_IDF_Porting_Masterplan_and_Modernization.md

## Abstract
This final document synthesizes the archaeological findings into a concrete engineering strategy for porting Rockbox to the ESP32 using the ESP-IDF framework. It outlines the transition from `configure/make` to `CMake`, the mapping of Rockbox's cooperative threads to pinned FreeRTOS tasks to handle the dual-core architecture, and the critical use of PSRAM for the `audiobuf`. It also addresses the Harvard Architecture constraints that necessitate a static compilation approach for plugins.

## 1. The Modernization Mandate
The final phase of this architectural excavation is to design the definitive porting strategy for the ESP32 using the Espressif IoT Development Framework (ESP-IDF). We will pivot from the legacy Perl/Make system to a modern CMake component-based build.

### 1.1 The Architecture Shift
We are moving from "Bare Metal" to "Hosted on FreeRTOS." Rockbox will run as a high-priority FreeRTOS task, managing its own sub-threads. The goal is to leverage the ESP32's dual-core architecture and connectivity while preserving Rockbox's low-latency audio core.

---

## 2. CMake Translation (`CMakeLists.txt`)
The legacy `tools/configure` script is a monolithic Perl beast. We replace it with a modular `CMakeLists.txt` structure that registers Rockbox as an ESP-IDF component.

### 2.1 The Root Project File
This file sets up the project-wide settings and pulls in the Rockbox component.

```cmake
# CMakeLists.txt (Project Root)
cmake_minimum_required(VERSION 3.16)

# Include ESP-IDF build system
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Project Name
project(rockbox-esp32)

# Set Default Configuration
# We need aggressive optimization for size and speed
sdkconfig_defaults()
```

### 2.2 The Rockbox Component
The core logic lives in `components/rockbox/CMakeLists.txt`. This file must meticulously collect thousands of source files while excluding target-specific assembly that won't link.

```cmake
# components/rockbox/CMakeLists.txt

# 1. Source Collection
# We use a glob for simplicity, but a manual list is safer for production
file(GLOB_RECURSE SOURCES
    "firmware/kernel/*.c"
    "firmware/common/*.c"
    "firmware/drivers/*.c"
    "apps/*.c"
    "lib/rbcodec/*.c"
)

# 2. Exclude Incompatible Files
# Remove ASM files meant for ARM/ColdFire
list(FILTER SOURCES EXCLUDE REGEX ".*crt0\\.S$")
list(FILTER SOURCES EXCLUDE REGEX ".*target/arm/.*")
list(FILTER SOURCES EXCLUDE REGEX ".*target/coldfire/.*")

# 3. Register Component
idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS
        "firmware/export"
        "firmware/include"
        "apps"
        "lib/rbcodec/codecs"
        "lib/rbcodec/dsp"
    PRIV_REQUIRES
        driver          # For I2S, SPI, GPIO
        fatfs           # For filesystem
        spiffs          # For internal flash storage
        nvs_flash       # For settings storage
        audio_pipeline  # For advanced audio features (optional)
        esp_lcd         # For display drivers
        lwip            # For networking
        esp_wifi        # For WiFi
        esp_https_ota   # For Firmware Updates
        bt              # For Bluetooth Stack
)

# 4. Compiler Flags (Critical for Legacy Code)
# Rockbox code is old and generates many warnings with modern GCC
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -Wno-address-of-packed-member # Struct packing is heavy in Rockbox
    -fno-builtin                  # Don't use built-in string functions
    -ffreestanding                # We provide our own environment
    -DROCKBOX_LITTLE_ENDIAN
    -DHAVE_LCD_COLOR
    -DMODEL_NAME="ESP32"
)
```

---

## 3. The Partition Table (`partitions.csv`)
Rockbox requires significant storage. The default ESP32 partition table is insufficient. We need a custom layout to accommodate the firmware, an OTA slot, and a virtual disk for resources (`.rock` files, themes).

```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,        0x4000,
otadata,  data, ota,     ,        0x2000,
phy_init, data, phy,     ,        0x1000,
factory,  app,  factory, ,        2M,
ota_0,    app,  ota_0,   ,        2M,
ota_1,    app,  ota_1,   ,        2M,
vfs,      data, fat,     ,        4M,
coredump, data, coredump,,        64K,
```
*   **Factory/OTA (2MB):** Rockbox binary is large (approx 1.5MB with all codecs linked statically). We need at least 2MB slots.
*   **VFS (4MB):** This internal FAT partition is critical. It allows the device to boot and show a basic UI (themes, fonts) even if the SD card is missing or corrupted.
*   **Coredump:** Essential for debugging crashing plugins in the field.

---

## 4. FreeRTOS OS Collision & Threading
Rockbox's scheduler assumes it owns the CPU and disables interrupts freely. On ESP32, FreeRTOS owns the CPU. We must map Rockbox threads to FreeRTOS tasks.

### 4.1 Priority Mapping Strategy
Rockbox uses an inverted priority scheme (0=High, 32=Low). FreeRTOS uses (0=Low, 24=High). We must map them inversely.

| Rockbox Priority | Value | FreeRTOS Priority | Description |
| :--- | :--- | :--- | :--- |
| `PRIORITY_REALTIME` | 1 | 23 | Audio DMA / Mixer |
| `PRIORITY_USER_INTERFACE` | 16 | 10 | GUI / Main Thread |
| `PRIORITY_BACKGROUND` | 20 | 5 | Database Scan / Metadata |
| `PRIORITY_IDLE` | 32 | 0 | Idle |

### 4.2 The `create_thread` Wrapper
We reimplement Rockbox's `create_thread` in `firmware/target/hosted/esp32/thread-esp32.c`.

```c
/* Implementation of Rockbox create_thread */
unsigned int create_thread(void (*function)(void), void* stack, size_t size, const char *name)
{
    TaskHandle_t handle;
    int priority = PRIORITY_ROCKBOX_DEFAULT;

    /* Map Rockbox Priority to FreeRTOS */
    /* Note: Ideally pass priority as arg, but legacy signature is fixed */
    /* We might need to peek at 'current_thread->priority' or context */

    BaseType_t res = xTaskCreatePinnedToCore(
        (TaskFunction_t)function,
        name,
        size / 4, /* Stack depth in words (ESP32 specific!) */
        NULL,
        10, /* Default Priority */
        &handle,
        1 /* Run Rockbox on Core 1 (App Core) */
    );

    if (res != pdPASS) {
        panic("Thread Creation Failed");
    }

    return (unsigned int)handle;
}
```

### 4.3 Core Pinning Strategy
*   **Core 0 (Pro CPU):** Wi-Fi, Bluetooth, LwIP, ESP-IDF System Tasks.
*   **Core 1 (App CPU):** Rockbox Main Thread, Audio Decoding, GUI.
    *   *Rationale:* Keeps real-time audio (I2S DMA) away from Wi-Fi interrupts which cause non-deterministic jitter.

---

## 5. Memory Management: The PSRAM Lifeline
The ESP32 has ~520KB internal RAM. Rockbox expects multimegabyte buffers. External PSRAM (SPIRAM) is mandatory.

### 5.1 `core_alloc` Redesign
Rockbox's `core_alloc.c` typically grabs a huge static array. We must replace this with `heap_caps_malloc`.

```c
/* firmware/target/hosted/esp32/system-esp32.c */
void system_init(void)
{
    /* Initialize PSRAM */
    if (esp_spiram_init() != ESP_OK) {
        panic("PSRAM Init Failed");
    }

    /* Allocate 4MB for audio buffer in PSRAM */
    /* This is critical. Internal RAM is too small for buffering 30s of audio */
    audiobuffer = heap_caps_malloc(4 * 1024 * 1024, MALLOC_CAP_SPIRAM);

    /* Allocate Plugin Buffer */
    pluginbuffer = heap_caps_malloc(512 * 1024, MALLOC_CAP_SPIRAM);

    if (!audiobuffer || !pluginbuffer) {
        panic("OOM: No PSRAM for Audio!");
    }
}
```

### 5.2 Memory Map Diagram
```text
+-----------------------+ 0x3F800000 (External PSRAM Start)
| Audio Buffer (4MB)    |  <-- Circular Buffer for compressed audio
+-----------------------+
| Plugin Buffer (512KB) |  <-- Loaded Games / Codecs overlay
+-----------------------+
| Framebuffer (300KB)   |  <-- 2x 320x240x16b Buffers
+-----------------------+
| General Heap          |  <-- Database, Metadata, ID3
+-----------------------+ 0x3FFFFFFF (External PSRAM End)

+-----------------------+ 0x3FFxxxxx (Internal SRAM)
| DMA Buffers           |  <-- I2S / SPI DMA (Must be internal)
+-----------------------+
| Stacks                |  <-- FreeRTOS Task Stacks
+-----------------------+
| ISR Vectors           |
+-----------------------+
```

---

## 6. The Complete ESP-IDF HAL Translation

### 6.1 Storage: VFS Bridge (`fat.c` -> `esp_vfs_fat.h`)
Rockbox's `fat.c` expects raw sector access. While we could map `sdmmc_read_sectors`, it is cleaner to use the ESP-IDF VFS and wrap Rockbox's file API.

**Wrapper Implementation (`firmware/target/hosted/esp32/file-esp32.c`):**
```c
int open(const char *path, int flags) {
    char vfs_path[MAX_PATH];
    snprintf(vfs_path, sizeof(vfs_path), "/sdcard/%s", path);

    const char *mode;
    if (flags & O_RDWR) mode = "r+";
    else if (flags & O_WRONLY) mode = "w";
    else mode = "r";

    FILE *f = fopen(vfs_path, mode);
    return (int)f; /* Cast FILE* to fd (dangerous, needs mapping table) */
}
```
*Note: A real implementation requires a file descriptor table to map integer `fd`s to `FILE*` pointers.*

### 6.2 Audio: I2S Driver (`pcm.c` -> `driver/i2s_std.h`)
We map Rockbox's `pcmbuf` to the ESP32 I2S driver.

```c
/* firmware/target/hosted/esp32/pcm-esp32.c */
static i2s_chan_handle_t tx_chan;

void pcm_init(void) {
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_chan, NULL);

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = GPIO_NUM_0,
            .bclk = GPIO_NUM_14,
            .ws = GPIO_NUM_15,
            .dout = GPIO_NUM_22,
            .din = I2S_GPIO_UNUSED,
        },
    };
    i2s_channel_init_std_mode(tx_chan, &std_cfg);
    i2s_channel_enable(tx_chan);
}

void pcm_play_dma_start(const void *addr, size_t size)
{
    size_t bytes_written;
    /* Blocking write to I2S DMA buffer - FreeRTOS will context switch if full */
    i2s_channel_write(tx_chan, addr, size, &bytes_written, portMAX_DELAY);
}
```

### 6.3 Display: SPI LCD (`lcd-*.c` -> `esp_lcd`)
We use the `esp_lcd` component which handles SPI DMA efficiently.

```c
/* firmware/target/hosted/esp32/lcd-esp32.c */
static esp_lcd_panel_handle_t panel_handle = NULL;

void lcd_update_rect(int x, int y, int w, int h) {
    /* Flush logical framebuffer to display */
    /* Framebuffer is in PSRAM, supports larger sizes (320x240) */
    esp_lcd_panel_draw_bitmap(panel_handle, x, y, x + w, y + h,
                              &framebuffer[y][x]);
}
```

---

## 7. Input: GPIO & Queues
Rockbox expects a `button_read()` function to poll state. We can improve this with interrupts.

### 7.1 ISR-Based Button Driver
```c
/* firmware/target/hosted/esp32/button-esp32.c */
static QueueHandle_t gpio_evt_queue = NULL;

static void IRAM_ATTR gpio_isr_handler(void* arg) {
    uint32_t gpio_num = (uint32_t) arg;
    xQueueSendFromISR(gpio_evt_queue, &gpio_num, NULL);
}

/* Replaces legacy polling in button_read() */
int button_read(int *data) {
    uint32_t io_num;
    if (xQueueReceive(gpio_evt_queue, &io_num, 0)) {
        /* Map IO number to Rockbox Button ID */
        return map_gpio_to_button(io_num);
    }
    return BUTTON_NONE;
}
```

---

## 8. The Harvard Architecture Plugin Blocker
The ESP32 is a Harvard architecture (separate instruction/data buses). It cannot execute code from Data RAM (DRAM/PSRAM) easily due to NX (No-Execute) protections and cache incoherency.
This breaks Rockbox's dynamic plugin loader (`elf_loader.c`), which loads code into a `malloc`'d buffer.

### 8.1 Solution: Static Plugins (The "Built-in" Approach)
Since we can't easily load ELF files at runtime:
1.  **Compile Plugins Statically:** Link `doom`, `quake`, etc., directly into the main firmware image.
2.  **Menu Integration:** Modify the "Plugins" menu to launch internal functions instead of loading files.
3.  **Overlay System:** If flash space is tight, use ESP-IDF's OTA partition mechanism to swap "Game Packs" (partitions containing different sets of static plugins).

### 8.2 Alternative: Memory Mapping (Experimental)
A risky hack involves writing the ELF executable segments to a reserved Flash partition and using `esp_partition_mmap()` to map it into the Instruction Bus address space (`0x400xxxxx`). This allows execution but wears out the flash if done frequently.

---

## 9. Connectivity: LwIP Integration
Rockbox typically lacks a network stack. With ESP32, we can implement streaming.

### 9.1 Wi-Fi Initialization
```c
void wifi_init_sta(void)
{
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_start();
}
```

### 9.2 The "Streaming File" Abstraction
To stream audio, we can implement a `stream_open()` function that creates a ring-buffered HTTP client.
1.  **Buffer:** Allocate 512KB in PSRAM.
2.  **Task:** `http_reader_task` fills the buffer from LwIP.
3.  **API:** `read()` calls return data from this buffer, blocking if empty.
Rockbox's codec engine won't know it's reading from the network instead of an SD card.

---

## 10. Advanced Debugging & Panic Handling
Debugging embedded Rockbox is historically done via "Sim" or "Panic Screen". On ESP32, we have JTAG.

### 10.1 JTAG Configuration
1.  Connect ESP-Prog to IO 12, 13, 14, 15 (TMS, TCK, TDI, TDO).
2.  Run OpenOCD: `idf.py openocd`.
3.  GDB: `xtensa-esp32-elf-gdb build/rockbox-esp32.elf`.

### 10.2 Panic Integration
Rockbox's `panic()` function dumps a screen of text. We should map this to ESP-IDF's Panic Handler to get backtraces.

```c
/* firmware/target/hosted/esp32/system-esp32.c */
void panicf(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    /* Trigger ESP-IDF Panic */
    abort();
}
```

---

## 11. The API Porting Checklist & Compliance Matrix
Based on the extensive API documentation in `11_Rockbox_API_Reference_and_System_Integration.md`, we must systematically map each Rockbox subsystem to its ESP-IDF equivalent.

### 11.1 The "Must-Have" APIs
These are blocking issues. The firmware cannot compile or boot without them.

#### 11.1.1 The Plugin API (`struct plugin_api`)
**Constraint:** ESP32 is Harvard Architecture (NX RAM).
**Strategy:** Static Linking & Overlay Manager.
*   **Legacy:** `dlopen()` style loading of `.rock` ELF files.
*   **Port:**
    *   Compile `doom`, `snake`, `viewer` as static libraries (`.a`).
    *   Create a lookup table: `{"doom", &doom_entry}`.
    *   Patch `plugin_load(path)` to search this table instead of the filesystem.
    *   **Memory:** Use `heap_caps_malloc(MALLOC_CAP_SPIRAM)` for the plugin's heap, matching the legacy `pluginbuf` behavior.

#### 11.1.2 The USB API (`docs/usb-api.md`)
**Constraint:** Rockbox has its own USB stack. ESP-IDF uses TinyUSB.
**Strategy:** Shim Layer.
*   **Legacy:** `usb_core_control_request` (Core handles setup packets).
*   **Port:**
    *   Initialize TinyUSB in `app_main` using `tusb_init()`.
    *   Register a callback for `tud_control_xfer_cb`.
    *   Inside the callback, invoke Rockbox's `usb_core_control_request`.
    *   Map `USB_CONTROL_ACK` to `tud_control_status`.

#### 11.1.3 The LCD API (`firmware/export/lcd.h`)
**Constraint:** Rockbox writes to framebuffers. ESP-IDF writes to SPI.
**Strategy:** Shadow Buffer.
*   **Legacy:** `lcd_update()` flushes `framebuffer[]` to hardware.
*   **Port:**
    *   Allocate `framebuffer` in PSRAM (320x240x16b = ~150KB).
    *   In `lcd_update()`, call `esp_lcd_panel_draw_bitmap()`.
    *   Use `esp_lcd_panel_io_spi_config_t` with `dma_chan = auto` for zero-copy transfers (if SRAM allows) or bounce buffers.

### 11.2 Compliance Matrix: Rockbox vs. ESP-IDF

| Rockbox API Group | Rockbox Function | ESP-IDF Component | Status |
| :--- | :--- | :--- | :--- |
| **Threading** | `create_thread` | `xTaskCreatePinnedToCore` | **Mapped** (Wrapper required) |
| **Sync** | `mutex_lock` | `xSemaphoreTake` | **Mapped** |
| **File I/O** | `open`, `read` | `esp_vfs_fat.h` | **Mapped** (VFS shim) |
| **Storage** | `storage_read_sectors` | `sdmmc_read_sectors` | **Mapped** (Critical Path) |
| **Audio** | `pcm_play_data` | `i2s_channel_write` | **Mapped** (DMA Blocking) |
| **Display** | `lcd_update_rect` | `esp_lcd_panel_draw_bitmap` | **Mapped** |
| **Input** | `button_read_device` | `gpio_get_level` | **Mapped** (ISR preferred) |
| **USB** | `usb_drv_control_response` | `tud_control_status` | **Complex** (Requires Shim) |
| **Power** | `battery_voltage` | `adc_oneshot_read` | **Easy** |
| **Boot** | `crt0.S` | `app_main()` | **Replaced** |

---

## 12. Power Management: ULP Coprocessor
To achieve Rockbox's legendary standby time, we must use the ULP (Ultra Low Power) coprocessor during Deep Sleep.

### 11.1 The ULP State Machine
The main CPU powers down (consuming < 10uA). The ULP remains active.
1.  **Program:** Write assembly code for ULP to scan GPIO pins (Buttons).
2.  **Execution:** ULP wakes up every 100ms, reads GPIO.
3.  **Wakeup:** If GPIO changes (button press), ULP triggers the `RTC_CNTL` wake interrupt.
4.  **Main Boot:** ESP32 boots, Rockbox checks wakeup cause, and resumes playback or enters menu.

---

## 12. Filesystem Wrappers (`dirent.h` Shim)
The VFS translation layer must handle directory listing.

```c
/* firmware/target/hosted/esp32/dir-esp32.c */
#include <dirent.h>

DIR* opendir(const char* name)
{
    char vfs_path[MAX_PATH];
    snprintf(vfs_path, sizeof(vfs_path), "/sdcard/%s", name);
    return (DIR*)fopen(vfs_path, "r"); // Pseudo-code: use real opendir
}

struct dirent* readdir(DIR* dir)
{
    // Native readdir returns standard struct
    // Rockbox expects 'struct dirent' with d_name
    return (struct dirent*) native_readdir(dir);
}
```

---

## 13. OTA Update Strategy
Updating Rockbox on ESP32 is easier than legacy targets. We use ESP-IDF's native OTA mechanism.

### 13.1 Partition Switching
1.  **Current Running:** `ota_0` partition.
2.  **Download:** Stream new `.bin` from HTTP server to `ota_1`.
3.  **Verify:** SHA256 checksum check by ESP-IDF.
4.  **Switch:** Set `otadata` boot flags to `ota_1`.
5.  **Reboot:** Device boots new firmware.

This allows unbrickable updates, unlike legacy Rockbox which required a careful bootloader replacement.

---

## 14. Bluetooth A2DP Source
Rockbox can act as a Bluetooth source, streaming audio to wireless headphones.

### 14.1 Audio Routing
We tap into `pcm_play_dma_start`.
```c
void pcm_play_dma_start(void *buf, size_t size) {
    if (audio_output == OUTPUT_BT) {
        /* Send to Bluedroid Stack */
        esp_a2d_media_ctrl(ESP_A2D_MEDIA_CTRL_CHECK_SRC_RDY);
        esp_a2d_source_data_ready(buf, size);
    } else {
        /* Send to I2S DAC */
        i2s_write(..., buf, ...);
    }
}
```

---

## 15. Recommended Hardware Wiring
Rockbox ESP32 Reference Design (based on ESP32-WROVER-E DevKit).

```text
ESP32 Pin | Function      | Connected To
----------|---------------|-------------
IO 0      | BOOT          | Button: HOME
IO 14     | I2S_BCLK      | DAC: PCM5102 BCLK
IO 15     | I2S_LRCK      | DAC: PCM5102 LRCK
IO 22     | I2S_DOUT      | DAC: PCM5102 DIN
IO 23     | SPI_MOSI      | LCD: ST7789 SDA / SD: CMD
IO 18     | SPI_CLK       | LCD: ST7789 SCL / SD: CLK
IO 5      | SPI_CS        | LCD: CS
IO 2      | SD_DAT0       | SD Card D0
IO 4      | SD_DAT1       | SD Card D1
IO 12     | SD_DAT2       | SD Card D2
IO 13     | SD_DAT3       | SD Card D3
```

---

## 16. Critical `sdkconfig` Parameters
The Kconfig settings are vital for Rockbox stability.

### 16.1 FreeRTOS
```ini
CONFIG_FREERTOS_HZ=1000
# Rockbox relies on 1ms granularity. Default is 100Hz which causes laggy UI.
CONFIG_FREERTOS_UNICORE=n
# Use both cores.
CONFIG_FREERTOS_ISR_STACKSIZE=2048
# Rockbox ISRs can be heavy.
```

### 16.2 SPIRAM (PSRAM)
```ini
CONFIG_ESP32_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_USE_MALLOC=y
# Allow malloc() to use PSRAM automatically (optional, but convenient)
CONFIG_SPIRAM_SPEED_80MHZ=y
# Audio buffering needs high bandwidth.
CONFIG_SPIRAM_CACHE_WORKAROUND=y
# Fixes silicon errata for PSRAM random access.
```

### 16.3 Compiler Options
```ini
CONFIG_COMPILER_OPTIMIZATION_PERF=y
# -O2 or -O3. Rockbox codecs are CPU intensive.
CONFIG_COMPILER_OPTIMIZATION_ASSERTION_LEVEL_SILENT=y
# Save flash space.
```

## 17. Build Performance Tuning
Building 3000+ files with CMake/Ninja takes time.

### 17.1 Ccache Integration
Install `ccache` on the host and enable it in `idf.py`.
```bash
export IDF_CCACHE_ENABLE=1
```

### 17.2 Ninja Parallelism
Ensure all cores are used.
```bash
idf.py -j 12 build
```

---

## 18. The "Static Codec" Build Option
To avoid the overhead of swapping codecs in and out of the constrained IRAM/DRAM, we introduce a `STATIC_CODECS` build flag.

### 18.1 CMake Logic
```cmake
if(CONFIG_ROCKBOX_STATIC_CODECS)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE STATIC_CODECS)
    list(APPEND SOURCES "lib/rbcodec/codecs/flac.c")
    list(APPEND SOURCES "lib/rbcodec/codecs/vorbis.c")
    # ... add all codecs ...
endif()
```

### 18.2 Firmware Logic (`apps/codec_thread.c`)
```c
#ifdef STATIC_CODECS
/* Function pointer table for internal codecs */
static const struct codec_entry static_codec_table[] = {
    { CODEC_FLAC, &flac_entry },
    { CODEC_VORBIS, &vorbis_entry },
};

int codec_load(int codec_id) {
    /* Instead of loading from disk, point to internal struct */
    curr_codec_api = static_codec_table[codec_id].api;
    return 0;
}
#endif
```

## 19. Conclusion
Porting Rockbox to ESP32 is a monumental task of bridging two eras of embedded computing. It requires dissecting the bare-metal assumptions of 2005 and mapping them to the RTOS primitives of 2024. The strategy outlined above—pinning the core logic to one core, offloading I/O to drivers, and strictly managing PSRAM/SRAM split—provides the most viable path to a stable, high-fidelity audio player on the ESP32.
