# Rockbox ESP32-S3 Architectural Design Strategy

This document outlines the hard decisions necessary to constrain the infinite porting possibilities of Rockbox to a realistic, maintainable ESP-IDF project.

## 1. Build System Strategy (CMake Component vs Native Rockbox Makefile)
**Decision: Wrap Rockbox Source into an ESP-IDF Component (CMake).**
*   **Why:** Attempting to force the Rockbox `tools/configure` Makefiles to cross-compile for an Xtensa target using ESP-IDF toolchains, while linking FreeRTOS libraries, LwIP, and `esp_lcd_panel` drivers, is an insurmountable technical debt.
*   **How:** We will extract the exact C files required for our target (e.g., `apps/`, `firmware/common/`, `lib/rbcodec/`) into a massive `CMakeLists.txt` file within the `esp32-rockbox/components/rockbox/` directory. The ESP-IDF build system (`idf.py build`) will handle compilation, linking, PSRAM allocation (`CONFIG_SPIRAM_XIP_FROM_PSRAM`), and partition table generation.

## 2. Navigating the `#define` Maze (`config-esp32.h`)
**Decision: Construct a comprehensive `firmware/export/config/esp32s3.h` header.**
*   **Why:** Rockbox conditionally includes files and structs based on hundreds of target macros.
*   **How:** This header will define critical architecture capabilities: `#define HAVE_LCD_COLOR`, `#define HAVE_TOUCHSCREEN` (if applicable), `#define LCD_WIDTH 320`, `#define LCD_HEIGHT 240`, `#define MEMORYSIZE 8` (8MB PSRAM), `#define USB_NONE`, `#define HAVE_RECORDING` (if mic is present). We will iteratively comment these out until the `CMakeLists.txt` builds without missing symbols.

## 3. SDL Simulator Parity
**Decision: The SDL Build is Mandatory for UI/Theme Development.**
*   **Why:** Flashing an ESP32 over serial takes 10–30 seconds. Debugging UI layouts or tracking down a `wps_parser` bug on physical hardware is incredibly inefficient.
*   **How:** Because we are adopting the `firmware/target/hosted` override paradigm (POSIX/SDL wrappers), the existing Rockbox `make simulator` command on Linux/macOS will compile out of the box with our new `config-esp32.h` dimensions. This "free" emulator is essential for rapid iteration of the 240x320 UI.

## 4. The Display Controller (SPI TFT ILI9341 / ST7789)
**Decision: SPI Polling / DMA via `esp_lcd_panel_draw_bitmap`.**
*   **Why:** Most 240x320 displays (ILI9341, ST7789) utilize an 8-bit or SPI interface natively expecting RGB565 formats.
*   **How:** In `app_main()`, we initialize the SPI bus (`spi_bus_initialize`), the panel IO (`esp_lcd_new_panel_io_spi`), and the LCD driver (`esp_lcd_new_panel_st7789`). If the hardware dictates BGR565, we flip `MADCTL` using `esp_lcd_panel_swap_xy(panel_handle, true, true)`. Rockbox's `lcd_update_rect(x, y, w, h)` simply calculates the dirty PSRAM buffer offset and passes it to the non-blocking DMA `esp_lcd_panel_draw_bitmap()`.

## 5. Forking vs. Upstreamable Patches
**Decision: Clean Fork (With the Intent of Eventual Upstream).**
*   **Why:** Trying to push architectural changes (like statically compiling 50+ codecs via `codec_registry` or replacing `ci.yield` behaviors) directly into the Rockbox `master` branch will trigger intense mailing-list debates and delay development indefinitely.
*   **How:** The project begins as a hard fork. We aggressively create `target/xtensa/esp32/` files, gut `apps/codecs.c` to use static function pointers instead of `lc_open`, and completely replace `queue.c` and `thread.c` with FreeRTOS wrappers. Once the port is stable and plays FLAC audio smoothly on an ESP32-S3, we can cleanly extract the target-specific files into patches that conform to the `firmware/target/hosted/` paradigm for upstream review.
