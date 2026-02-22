# 06_The_Plugin_Paradox_and_Dynamic_Execution.md

## 1. The Dynamic Plugin System

Rockbox is extensible. Games (Doom, Solitaire), Utilities (Calculator), and Demos are not compiled into the core firmware. They are standalone binaries (`.rock` files).

### The `.rock` Format
*   Basically an **ELF** file (Executable and Linkable Format).
*   **Compilation:** Compiled with `-fPIC` (Position Independent Code) or relocated at load time.
*   **API Table:** Plugins cannot link directly to kernel symbols. Instead, they receive a pointer to a struct `plugin_api`.
    *   This struct contains function pointers to every Rockbox API (LCD, Audio, Buttons).
    *   This ensures binary compatibility (mostly) without static linking.

## 2. ELF Loading & Execution

### The Loader (`firmware/elf_loader.c`)
On native targets, Rockbox implements its own ELF loader.
1.  **Read Header:** Validates magic bytes, architecture.
2.  **Parse Segments:** Iterates `PT_LOAD` segments.
3.  **Allocate Memory:** Allocates a chunk in the `pluginbuf` (a dedicated RAM region).
4.  **Copy:** Reads the file content into that RAM.
5.  **Relocate:** Applies relocation entries (`.rel.dyn`, `.rela.dyn`) if the code is not fully PIC.
6.  **Jump:** Casts the entry point address to a function pointer and calls it.

### Hosted Mode (`dlopen`)
On Linux/Android:
*   Rockbox relies on the OS loader.
*   `lc_open` calls `dlopen()`.
*   Plugins are shared objects (`.so`).

## 3. The ESP32 Harvard Architecture Blocker

**The Problem:**
*   **Harvard Architecture:** The ESP32 (Xtensa LX7) has separate buses for Instruction RAM (IRAM) and Data RAM (DRAM).
*   **Execution:** You typically **cannot** execute code located in DRAM.
*   **Loading:** Standard `malloc()` allocates from DRAM.
*   **IRAM Scarcity:** ESP32 has very little IRAM (~192KB usable). Loading a 500KB "Doom" plugin into IRAM is impossible.

## 4. Creative Porting Solutions

We need to invent a way to run plugins on ESP32.

### Solution A: The "Hosted" Approach (Static Linking)
*   **Concept:** Disable the dynamic loader entirely.
*   **Implementation:**
    1.  Compile *all* plugins as static libraries (`libdoom.a`, `libsnake.a`).
    2.  Link them into the main firmware.
    3.  Replace `plugin_load("doom.rock")` with a lookup table:
        ```c
        if (strcmp(name, "doom") == 0) return doom_entry(api);
        ```
*   **Pros:** Easy, stable, uses flash execution (XIP).
*   **Cons:** Increases firmware size. OTA updates required to add plugins. Rockbox build system isn't designed for this.

### Solution B: The "Espressif Dynamic Loader"
*   **Concept:** Use the ESP-IDF `app_loader` or `esp_dl` mechanisms if available, or write a custom one that writes to IRAM.
*   **Problem:** IRAM is too small.

### Solution C: The "Flash Mapping" Trick (Winner)
*   **Concept:** The ESP32 can execute code from External Flash via the cache (XIP).
*   **Mechanism:**
    1.  Store the plugin `.rock` file on the FAT partition (SD card).
    2.  **MMU Mapping:** Use ESP-IDF's MMU API (`spi_flash_mmap` or custom virtual filesystem driver) to map the file's offset on the SD card (or a partition) into the CPU's instruction address space (`0x42000000`).
    3.  **Execution:** Jump to the mapped address.
*   **Challenges:**
    *   SD Card random read latency might stall the instruction cache.
    *   Alignment requirements for MMU mapping (64KB blocks).
    *   Security/MPU protections.

### Recommendation for Phase 1
**Solution A (Static Linking)** is the only viable path for a "Hello World" port.
1.  Select a subset of plugins (Solitaire, Calculator).
2.  Modify `apps/plugin.c` to support a "builtin" mode.
3.  Modify `tools/configure` to add a `STATIC_PLUGINS` define.

Later, research **Solution C** for true dynamic loading.
