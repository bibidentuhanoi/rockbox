# 10_ESP_IDF_Porting_Masterplan_and_Modernization.md

## 1. The Modernization Mandate

Porting Rockbox to the ESP32 (Xtensa LX6/LX7 or RISC-V) via the ESP-IDF framework requires a fundamental shift from a "Bare Metal" philosophy to an "RTOS-hosted" philosophy. We will treat the ESP-IDF/FreeRTOS environment as a hybrid between a hardware target and a hosted port.

## 2. Build System Transformation: Perl to CMake

The legacy `tools/configure` + Makefile system is incompatible with the modern ESP-IDF build flow (`idf.py`). We must gut it.

### 2.1 The New Structure

We will create a standard ESP-IDF project structure where Rockbox is a **Component**.

```text
esp32-rockbox/
├── CMakeLists.txt              # Project root CMake
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   └── main.c                  # app_main() entry point
└── components/
    └── rockbox/                # The Rockbox Source Mirror
        ├── CMakeLists.txt      # The Massive Build Script
        ├── firmware/
        ├── apps/
        └── lib/
```

### 2.2 `components/rockbox/CMakeLists.txt` Strategy

Instead of generating a Makefile, we write a CMake script that:
1.  **Glob-gathers** source files (carefully excluding target-specific files from other arches).
2.  **Defines Macros:** Sets `ROCKBOX_LITTLE_ENDIAN`, `HAVE_FREERTOS`, `HAVE_LCD_COLOR`.
3.  **Includes Paths:** Replicates the `TARGET_INC` precedence logic using `priv_include_dirs`.

```cmake
idf_component_register(
    SRCS
        "firmware/kernel/mutex.c"
        "firmware/kernel/queue.c"
        "firmware/common/fat.c"
        "apps/main.c"
        # ... thousands of files ...
    INCLUDE_DIRS
        "firmware/export"
        "firmware/include"
        "firmware/target/hosted/esp32" # Our new HAL
    REQUIRES
        driver
        fatfs
        spiffs
)
```

## 3. The OS Collision: FreeRTOS vs. Rockbox Kernel

Rockbox has its own scheduler. ESP-IDF has FreeRTOS. They cannot coexist as peers. We must wrap Rockbox threads as FreeRTOS tasks.

### 3.1 Thread Mapping (`firmware/target/hosted/esp32/kernel-esp32.c`)

We implement the Rockbox threading API using FreeRTOS primitives.

| Rockbox Primitive | FreeRTOS Equivalent | Notes |
| :--- | :--- | :--- |
| `create_thread` | `xTaskCreatePinnedToCore` | Map priorities 1-128 to FreeRTOS 1-25. |
| `mutex_lock` | `xSemaphoreTake(mutex, portMAX_DELAY)` | Enable PIP in FreeRTOS config. |
| `queue_wait` | `xQueueReceive` | Rockbox queues are complex; might need custom wrapper. |
| `sleep` | `vTaskDelay` | Direct mapping. |

### 3.2 Core Pinning

ESP32 is dual-core. We should exploit this:
*   **Core 0 (Protocol CPU):** Wi-Fi, Bluetooth, TCP/IP stack.
*   **Core 1 (App CPU):** The Rockbox UI thread, Codec thread, and DSP.

## 4. Storage and Filesystem

Rockbox has a great FAT driver (`firmware/common/fat.c`), but ESP-IDF has a robust VFS (Virtual Filesystem) layer that integrates with SDMMC and Flash.

### 4.1 The Strategy: Bridge to VFS

We should **discard** `firmware/common/fat.c` and implement the `storage` abstraction using `fopen`/`fread` (Posix) via ESP-IDF's VFS.
*   **Pros:** Supports long filenames, exFAT (if enabled), and Wear Leveling out of the box.
*   **Cons:** Higher memory overhead than Rockbox's lean driver.

We map `firmware/drivers/fat.c` calls to standard C `stdio.h` calls, effectively treating the ESP32 like a "Hosted" target (Linux).

## 5. Audio Pipeline: I2S

We will replace the manual DMA management with the ESP-IDF I2S driver.

### 5.1 `firmware/target/hosted/esp32/pcm-esp32.c`

```c
void pcm_play_dma_start(const void *addr, size_t size) {
    size_t bytes_written;
    /* Blocking write? No, Rockbox expects non-blocking DMA start.
       We might need a separate FreeRTOS task to feed the I2S buffer
       if i2s_write blocks. */
    i2s_write(I2S_NUM_0, addr, size, &bytes_written, 0);
}
```

**Optimization:** Use the ESP32's DMA linked lists to create a true circular buffer that matches Rockbox's `audiobuf` expectations.

## 6. The Harvard Architecture Plugin Blocker

**The Problem:** ESP32 cannot execute code from Data RAM (DRAM). It can only execute from Instruction RAM (IRAM) or Flash (via ICache). Rockbox loads plugins (`.rock` ELFs) into a `pluginbuf` which is usually in generic DRAM.

### 6.1 Solution A: The "Static" Plugins (Immediate Fix)
Compile critical plugins (Viewers, Games) *statically* into the main firmware binary. We modify the plugin loader to look up internal function pointers instead of loading files.

### 6.2 Solution B: The Overlays (Complex)
Reserve a block of **IRAM** (Instruction RAM) for plugins.
*   IRAM is scarce (192KB total, much used by Wi-Fi).
*   We can maybe reserve 64KB for small plugins.

### 6.3 Solution C: XIP from Partition (Best)
1.  The `.rock` file is actually a binary image ready for flash.
2.  We use `esp_partition_write` to write the plugin to a reserved "Plugin Execution Partition" in SPI Flash.
3.  We use `esp_partition_mmap` to map that flash region into the instruction address space (0x40000000).
4.  We jump to the mapped pointer.
*   **Latency:** High (writing to flash is slow).
*   **Wear:** High wear on flash if users switch plugins constantly.

**Recommendation:** Start with **Solution A** (Static) for the port, then investigate **Solution C** with a RAM-disk overlay if possible (ESP32-S3 supports octal SPI RAM which might be executable?). *Correction:* ESP32 PSRAM is generally *not* executable on the original core, but S3 might allow it via cache. Stick to Solution A/C.

## 7. Display

Map `screen_access.c` to a standard SPI Master driver for an ILI9341 or ST7789 LCD.
*   Use **DMA** for screen updates to avoid blocking the CPU.
*   Double buffer in PSRAM (SPI RAM) if available, as a 320x240x16bpp framebuffer (150KB) fits easily in internal SRAM, but dual buffers might be tight.

## 8. Summary of Work

1.  **Scaffold:** Create `CMakeLists.txt` and empty stub drivers.
2.  **Kernel:** Get `create_thread` wrapping `xTaskCreate`.
3.  **UI:** Implement `lcd_update` via SPI. Get the splash screen.
4.  **FS:** Mount SD card via VFS. Bridge `open`/`read`.
5.  **Audio:** Hook up `pcm_play` to I2S.
6.  **Codecs:** Compile `libmad` and `tremor` as components.
