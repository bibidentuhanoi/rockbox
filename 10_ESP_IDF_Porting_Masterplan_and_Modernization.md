# 10_ESP_IDF_Porting_Masterplan_and_Modernization.md

## 1. The Modernization Mandate
The final phase of this architectural excavation is to design the definitive porting strategy for the ESP32 using the Espressif IoT Development Framework (ESP-IDF). We will pivot from the legacy Perl/Make system to a modern CMake component-based build.

### 1.1 The Architecture Shift
We are moving from "Bare Metal" to "Hosted on FreeRTOS." Rockbox will run as a high-priority FreeRTOS task, managing its own sub-threads.

---

## 2. CMake Translation (`CMakeLists.txt`)
We replace `tools/configure` with a root `CMakeLists.txt` that registers Rockbox as an ESP-IDF component.

```cmake
# ESP-IDF Project Configuration
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

project(rockbox-esp32)

# Define Rockbox Component
idf_component_register(
    SRCS
        "firmware/kernel/kernel.c"
        "firmware/kernel/thread.c"
        "firmware/common/fat.c"
        "apps/main.c"
        "apps/playback.c"
        # ... (hundreds of files) ...
    INCLUDE_DIRS
        "firmware/export"
        "firmware/include"
        "apps"
        "lib/rbcodec/codecs"
    REQUIRES
        driver
        fatfs
        spiffs
        nvs_flash
        audio_pipeline
)

# Compiler Flags (Critical for Legacy Code)
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-unused-parameter
    -Wno-sign-compare
    -fno-builtin
    -ffreestanding
)
```

---

## 3. FreeRTOS OS Collision
Rockbox's scheduler assumes it owns the CPU. On ESP32, FreeRTOS owns the CPU. We must map Rockbox threads to FreeRTOS tasks.

### 3.1 Thread Mapping (`firmware/target/hosted/esp32/thread-esp32.c`)
We use FreeRTOS `xTaskCreate` to implement `create_thread`.

```c
/* Implementation of Rockbox create_thread */
unsigned int create_thread(void (*function)(void), void* stack, size_t size, const char *name)
{
    TaskHandle_t handle;
    BaseType_t res = xTaskCreatePinnedToCore(
        (TaskFunction_t)function,
        name,
        size / 4, /* Stack depth in words */
        NULL,
        PRIORITY_ROCKBOX_DEFAULT, /* Map priorities carefully! */
        &handle,
        1 /* Run Rockbox on Core 1 (App Core) */
    );

    return (unsigned int)handle;
}
```

### 3.2 Core Pinning
*   **Core 0 (Pro CPU):** Wi-Fi, Bluetooth, ESP-IDF System Tasks (TCP/IP).
*   **Core 1 (App CPU):** Rockbox Main Thread, Audio Decoding, GUI.
    *   *Why?* Keeps real-time audio (I2S DMA) away from Wi-Fi interrupts.

---

## 4. The Complete ESP-IDF HAL Translation

### 4.1 Storage: SD Card (`fat.c` -> `esp_vfs_fat.h`)
Rockbox's `fat.c` expects raw sector access. We can either:
1.  **Wrap:** Use `esp_vfs_fat_sdmmc_mount()` and rewrite Rockbox's `file` functions to use standard `fopen`/`fread`. (Recommended).
2.  **Raw:** Use `sdmmc_read_sectors()` to feed Rockbox's `fat.c`. (Harder, but authentic).

### 4.2 Audio: I2S DMA (`pcm.c` -> `driver/i2s_std.h`)
We map Rockbox's `pcmbuf` to the ESP32 I2S driver.

```c
/* firmware/target/hosted/esp32/pcm-esp32.c */
void pcm_play_dma_start(const void *addr, size_t size)
{
    size_t bytes_written;
    /* Blocking write to I2S DMA buffer */
    i2s_channel_write(tx_chan, addr, size, &bytes_written, portMAX_DELAY);
}
```

### 4.3 Display: SPI LCD (`lcd-*.c` -> `esp_lcd_panel_io.h`)
We use the optimized `esp_lcd` component which uses SPI DMA.
*   **Framebuffer:** Allocated in PSRAM (320x240 x 16-bit = 150KB).
*   **Update:** `esp_lcd_panel_draw_bitmap()` flushes the framebuffer asynchronously.

---

## 5. ESP32 Memory Map & PSRAM
The ESP32 has limited internal RAM (SRAM) but supports up to 8MB of external PSRAM (SPIRAM).

### 5.1 The `audiobuf` Allocation
Rockbox needs a massive buffer for audio. It **MUST** go to PSRAM.

```c
/* firmware/target/hosted/esp32/system-esp32.c */
void system_init(void)
{
    /* Allocate 4MB for audio buffer in PSRAM */
    audiobuffer = heap_caps_malloc(4 * 1024 * 1024, MALLOC_CAP_SPIRAM);

    if (!audiobuffer) {
        panic("No PSRAM for Audio!");
    }
}
```

### 5.2 The Executable Code (IRAM vs Flash)
*   **Code:** Runs from Flash (XIP - Execute In Place) via cache.
*   **Critical ISRs:** Must be in IRAM (`IRAM_ATTR`) to avoid cache misses during interrupts.

---

## 6. The Harvard Architecture Plugin Blocker
The ESP32 is a Harvard architecture (separate instruction/data buses). It cannot execute code from Data RAM (DRAM/PSRAM) easily due to NX (No-Execute) protections and cache incoherency.
This breaks Rockbox's dynamic plugin loader (`elf_loader.c`), which loads code into a `malloc`'d buffer.

### 6.1 Solution: Static Plugins (The "Built-in" Approach)
Since we can't easily load ELF files at runtime:
1.  **Compile Plugins Statically:** Link `doom`, `quake`, etc., directly into the main firmware image.
2.  **Menu Integration:** Modify the "Plugins" menu to launch internal functions instead of loading files.

### 6.2 Alternative: ESP-IDF Dynamic Loading (Experimental)
Recent ESP-IDF versions support dynamic loading, but it requires MMU remapping. A simpler hack:
*   **Flash Partition:** Write the plugin to a dedicated OTA partition.
*   **Map:** `esp_partition_mmap()` the partition to the instruction bus range.
*   **Jump:** Execute directly from the memory-mapped flash region.

---

## 7. Conclusion
Porting Rockbox to ESP32 is a monumental task of bridging two eras of embedded computing. It requires dissecting the bare-metal assumptions of 2005 and mapping them to the RTOS primitives of 2024, all while respecting the constraints of memory and real-time audio latency.
