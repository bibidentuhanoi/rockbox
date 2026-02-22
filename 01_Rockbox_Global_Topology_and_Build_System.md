# 01_Rockbox_Global_Topology_and_Build_System.md

## 1. The Master ASCII Tree

The following ASCII tree illustrates the high-level structure of the Rockbox repository, focusing on the core firmware, bootloader, applications, and build tools. This tree is derived from the `list_files` output of the repository root and key subdirectories.

```text
.
├── apps/                       # User-space applications and GUI
│   ├── bitmaps/                # System bitmaps (icons, logos)
│   ├── codecs/                 # Audio codec interfaces (not implementations)
│   ├── gui/                    # The graphical user interface framework
│   │   ├── bitmap/             # Low-level bitmap handling
│   │   ├── skin_engine/        # Theming and skinning engine
│   │   ├── wps.c               # While Playing Screen logic
│   │   ├── list.c              # List widget implementation
│   │   └── ...
│   ├── lang/                   # Localization files (.lang)
│   ├── menus/                  # Menu definitions
│   ├── plugins/                # Dynamic plugins (.rock)
│   │   ├── bitmaps/
│   │   ├── lib/                # Plugin support libraries
│   │   └── ...                 # Hundreds of game and utility sources
│   ├── recorder/               # Recording specific code
│   ├── main.c                  # Main application entry point (apps/main.c)
│   ├── menu.c                  # Menu handling logic
│   ├── playback.c              # High-level audio playback engine
│   ├── plugin.c                # Plugin loader and API bridge
│   └── ...
├── bootloader/                 # Bootloader source code
│   ├── common.c
│   ├── crt0.S                  # Generic/Template startup assembly
│   └── ...
├── firmware/                   # Core kernel and hardware drivers
│   ├── asm/                    # Architecture-specific assembly headers
│   ├── common/                 # Common kernel utilities
│   ├── drivers/                # Hardware drivers
│   │   ├── audio/              # I2S, AC97, DAC drivers
│   │   ├── lcd/                # LCD controller drivers
│   │   └── ...
│   ├── export/                 # Public APIs exported to apps/
│   │   ├── config.h            # Main configuration header
│   │   ├── system.h            # System-wide definitions
│   │   ├── thread.h            # Threading API
│   │   └── ...
│   ├── include/                # Internal kernel headers
│   ├── kernel/                 # Core OS logic
│   │   ├── thread.c            # Scheduler and threading implementation
│   │   ├── mutex.c             # Synchronization primitives
│   │   ├── queue.c             # Message queues
│   │   └── ...
│   ├── libc/                   # Minimal C library implementation
│   ├── target/                 # Target-specific implementations
│   │   ├── arm/                # ARM architecture support
│   │   │   ├── as3525/         # AS3525 SoC specific code
│   │   │   ├── ipod/           # iPod specific code
│   │   │   ├── crt0.S          # ARM startup code
│   │   │   └── ...
│   │   ├── coldfire/           # ColdFire architecture support
│   │   ├── mips/               # MIPS architecture support
│   │   └── hosted/             # Hosted ports (Linux, Android, SDL)
│   │       ├── android/        # Android-specific code
│   │       ├── sdl/            # SDL (Simulator) code
│   │       ├── system-hosted.c # Hosted system abstraction
│   │       └── ...
│   ├── usbstack/               # USB device stack
│   ├── elf_loader.c            # Custom ELF loader for plugins
│   ├── pcm.c                   # PCM audio buffer management
│   └── ...
├── lib/                        # Third-party and support libraries
│   ├── rbcodec/                # The Rockbox Codec API and DSP
│   │   ├── codecs/             # Actual codec implementations (libmad, tremolo, etc.)
│   │   ├── dsp/                # DSP routines (EQ, crossfeed)
│   │   └── ...
│   ├── arm_support/            # Optimized ARM assembly routines
│   ├── fixedpoint/             # Fixed-point math library
│   └── ...
├── tools/                      # Build tools and scripts
│   ├── configure               # Main build configuration script (Perl)
│   ├── genlang                 # Language file generator
│   ├── scramble.c              # Firmware scrambling tool (for obfuscation)
│   ├── ucl/                    # UCL compression tools
│   ├── root.make               # Master Makefile template
│   └── ...
└── ...
```

## 2. Directory Purview

### `apps/` (The Application Layer)
This directory contains the "userland" of Rockbox. It sits on top of the `firmware/` kernel.
*   **Role:** Handles all UI interactions, audio playback logic, playlist management, and settings.
*   **Key Files:** `main.c` is the entry point after the kernel boots. `playback.c` manages the audio buffer and codec thread. `gui/` contains the windowing system.
*   **Relation to ESP32:** This code is largely platform-independent C, but it relies heavily on the `firmware/export` API.

### `bootloader/` (The Stage 1/2 Loader)
Contains the code that runs immediately after the device powers on (or is chained from the original firmware).
*   **Role:** Initialize basic hardware (clocks, SDRAM), load the main Rockbox binary (`rockbox.mi4` or similar) from disk to RAM, and jump to it.
*   **Relation to ESP32:** For the "Hosted" ESP32 port, this directory is largely irrelevant as ESP-IDF's bootloader (`bootloader.bin`) and `app_main` will handle startup.

### `firmware/` (The Kernel)
The core operating system.
*   **Role:** Provides threading, memory management, hardware abstraction (drivers), and the standard library.
*   **Key Directories:** `kernel/` (scheduler), `drivers/` (peripherals), `target/` (CPU/SoC specific code).
*   **Relation to ESP32:** This is where the heaviest porting work lies. The `target/hosted` directory provides a blueprint for running Rockbox as a task on top of another OS (like Linux or FreeRTOS).

### `lib/` (Libraries)
Contains code that is statically linked into the firmware or plugins.
*   **Role:** `rbcodec` is the most critical part here, housing the audio decoders (MP3, FLAC, etc.) and the DSP chain.
*   **Relation to ESP32:** Codecs are CPU-intensive. The ESP32's Xtensa LX7 dual-core setup is powerful, but we must ensure these libraries compile cleanly with the Xtensa toolchain.

### `tools/` (The Build System)
Contains the scripts required to configure and build Rockbox.
*   **Role:** `configure` is a massive Perl script that generates the `Makefile` based on the selected target.
*   **Relation to ESP32:** We will likely bypass this for the initial port, using ESP-IDF's `CMake` system, but we must understand `configure` to know which defines (`-D`) are required.

## 3. The Build System Genesis

Rockbox uses a custom build system centered around a Perl script named `configure`. It does not use Autotools or standard CMake (historically).

### `tools/configure` Deep Dive
This script is the orchestrator. When run, it:
1.  **Prompts for Target:** It asks the user to select a target device (e.g., "Sansaclip", "IpodVideo").
2.  **Sets Variables:** Based on the selection, it sets variables like `modelname`, `target_id`, `memory`, `cpu`, `manufacturer`.
    *   Example: For `ipodvideo`:
        ```perl
        target_id=15
        modelname="ipodvideo"
        target="IPOD_VIDEO"
        memory=64
        arm7tdmicc # Sets up the ARM compiler
        ```
3.  **Compiler Selection:** It defines helper functions (e.g., `arm7tdmicc`, `sdlcc`) that set `CC`, `AR`, `LD`, and `GCCOPTS`.
    *   For Hosted/SDL:
        ```bash
        simcc () {
            # ...
            GCCOPTS="$GCCOPTS -D_GNU_SOURCE=1 -D_REENTRANT"
            # ...
        }
        ```
4.  **Generates `autoconf.h`:** This header file contains C preprocessor definitions derived from the configuration.
    *   Example:
        ```c
        #define ARCH_ARM 1
        #define HAVE_LCD_COLOR 1
        #define LCD_WIDTH 320
        #define LCD_HEIGHT 240
        ```
5.  **Generates `Makefile`:** It concatenates `tools/root.make` with target-specific variables to create the final Makefile in the build directory.

### Build Flow
1.  `make` is invoked.
2.  **Bitmap Generation:** `bmp2rb` converts bitmaps in `apps/bitmaps` to C source files.
3.  **Language Generation:** `genlang` compiles `.lang` files into string tables.
4.  **Firmware Compilation:** `firmware/` sources are compiled into `libfirmware.a`.
5.  **Bootloader Compilation:** `bootloader/` sources are compiled (if selected).
6.  **Linking:** The objects are linked into `rockbox.elf`.
7.  **Post-Processing:** `scramble` (or `mkmi4`, etc.) obfuscates or formats the binary for the specific device bootloader.

## 4. Target Architectures Map

Rockbox has historically supported a wide range of architectures. The build system isolates these via the `firmware/target/` directory structure.

| Architecture | Directory Path | Key SoCs / Devices | Notes |
| :--- | :--- | :--- | :--- |
| **ARM** | `firmware/target/arm/` | iPods (PP502x), Sansa (AS3525), Gigabeat (S3C2440) | The most common architecture. Includes ARM7TDMI, ARM926EJ-S, ARM11. |
| **ColdFire** | `firmware/target/coldfire/` | iRiver H100/H300, iAudio X5 | Motorola 68k derivative. |
| **MIPS** | `firmware/target/mips/` | Ondavx (JZ47xx), Fiio M3K (X1000) | Ingenic MIPS XBurst cores. |
| **SH** | `firmware/target/sh/` | Archos (SH-1) | Historical support (likely deprecated or removed in modern tree). |
| **Hosted** | `firmware/target/hosted/` | **SDL Simulator**, **Android**, **Linux** | **CRITICAL FOR ESP32**. Runs Rockbox as an application on top of an OS. |

### The "Hosted" Architecture
This is the bridge for the ESP32 port.
*   **Path:** `firmware/target/hosted/`
*   **Mechanism:** Instead of raw hardware register access, "drivers" call host OS APIs.
    *   `pcm-alsa.c` -> ALSA Audio
    *   `lcd-sdl.c` -> SDL Surface
    *   `kernel-unix.c` -> pthreads / POSIX primitives
*   **ESP32 Strategy:** We will create a new sub-target under `hosted` (or similar parallel structure) called `esp32`.
    *   `pcm-esp32.c` -> ESP-IDF I2S
    *   `lcd-esp32.c` -> ESP-LCD / SPI
    *   `kernel-freertos.c` -> FreeRTOS Tasks/Queues

The `configure` script already has logic for `android` and `sdlapp`. We will need to inject logic for `esp32` that sets up the Xtensa toolchain and ESP-IDF paths.
