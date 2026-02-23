# 01_Rockbox_Global_Topology_and_Build_System.md

## 1. The Master ASCII Tree: Rockbox Global Topology

The Rockbox repository is a sprawling 20-year-old codebase. Below is a high-level topographical map of the root directory and its primary sub-architectures.

```text
/ (Rockbox Root)
├── apps/                       # High-level Applications & User Interface
│   ├── bitmaps/                # UI assets (icons, backdrops)
│   ├── codecs/                 # (Legacy path, now mostly in lib/rbcodec)
│   ├── gui/                    # The Graphical User Interface Engine
│   ├── lang/                   # Localization files (.lang)
│   ├── menus/                  # Menu definition structures
│   ├── plugins/                # Dynamic user plugins (.rock)
│   ├── recorder/               # Recording application logic
│   ├── main.c                  # Main application entry point
│   ├── playback.c              # The high-level audio playback engine
│   ├── plugin.c                # Plugin loader and API exporter
│   ├── screen_access.c         # Display abstraction layer
│   └── tree.c                  # Database and file browser logic
├── bootloader/                 # Second-stage Bootloaders
│   ├── common.c                # Common bootloader logic
│   ├── ipod.c                  # Apple iPod specific boot code
│   ├── sansa_as3525.c          # SanDisk Sansa (AMS AS3525) boot code
│   └── main-*.c                # Target-specific main routines
├── firmware/                   # The Core OS / Kernel
│   ├── common/                 # Architecture-independent drivers
│   │   └── fat.c               # Custom FAT16/FAT32 Driver
│   ├── drivers/                # Hardware Drivers
│   │   ├── ata.c               # IDE/ATA Driver
│   │   ├── button.c            # Keypad Driver
│   │   ├── lcd-*.c             # LCD Controller Drivers
│   │   └── usb.c               # USB Stack
│   ├── export/                 # Public API Headers
│   │   ├── config.h            # Build-time configuration
│   │   ├── kernel.h            # Kernel primitives
│   │   └── system.h            # System control
│   ├── include/                # Internal Kernel Headers
│   ├── kernel/                 # The Microkernel
│   │   ├── thread.c            # Cooperative/Preemptive Scheduler
│   │   ├── mutex.c             # Synchronization Primitives
│   │   └── queue.c             # Message Queues
│   ├── target/                 # Architecture-Specific Code (HAL)
│   │   ├── arm/                # ARM architecture support
│   │   │   ├── crt0.S          # C Runtime Startup
│   │   │   └── system-target.h
│   │   ├── coldfire/           # Motorola ColdFire support
│   │   ├── mips/               # MIPS architecture support
│   │   └── hosted/             # Simulation/Hosted ports (SDL, Android)
│   ├── elf_loader.c            # Dynamic Linker/Loader for Plugins
│   └── pcm.c                   # Audio DMA Manager
├── lib/                        # Static Libraries
│   └── rbcodec/                # The Audio Decoding Engine
│       ├── codecs/             # Codec Implementations (MP3, FLAC, etc.)
│       │   ├── libmad/         # MPEG Audio Decoder
│       │   └── codecs.h        # Codec API Interface
│       └── dsp/                # Digital Signal Processing (EQ, Crossfeed)
├── tools/                      # The Build System & Host Tools
│   ├── configure               # The Master Build Script (Perl/Shell)
│   ├── root.make               # The Root Makefile Template
│   ├── scramble.c              # Firmware encryption/obfuscation tool
│   └── ucl/                    # Compression library for firmware
└── uisimulator/                # PC-based Simulator
    └── sdl/                    # SDL implementation for simulator
```

## 2. The Build System Genesis: `tools/configure`

The Rockbox build system is a artifact of the early 2000s, predating modern tools like CMake or Meson. It relies heavily on **Perl** and **GNU Make**, orchestrating a complex cross-compilation process.

### 2.1 The Configuration Script (`tools/configure`)

The heart of the build system is the `configure` script. Unlike `autoconf` generated scripts, this is a hand-written shell/perl hybrid that performs the following critical tasks:

1.  **Target Selection:** It prompts the user for a target platform (e.g., "Apple iPod Video", "SanDisk Sansa Clip"). This selection maps to a `target_id` and a `modelname`.
2.  **Architecture Resolution:** Based on the `modelname`, it determines the CPU architecture (`arm`, `coldfire`, `mips`), manufacturer (`ipod`, `sandisk`), and specific SoC model.
3.  **Toolchain Selection:** It locates the appropriate cross-compiler (e.g., `arm-elf-eabi-gcc`, `m68k-elf-gcc`).
4.  **Makefile Generation:** It generates a `Makefile` in the build directory by interpolating variables into a template.

#### Key Configuration Variables (Extracted from `tools/configure`)

The script defines several critical variables that control the compilation:

*   `ARCH`: The CPU architecture (e.g., `arch_arm`).
*   `CPU`: The specific CPU core variant (e.g., `arm7tdmi`, `arm926ej-s`).
*   `MANUFACTURER`: The SoC or device manufacturer (e.g., `ipod`, `as3525`).
*   `MODELNAME`: The specific device model string (e.g., `ipodvideo`).
*   `MEMORYSIZE`: Amount of RAM in MB (critical for memory layout).
*   `LCDWIDTH` / `LCDHEIGHT`: Display resolution.
*   `RBDIR`: The installation directory on the device (default `/.rockbox`).

#### The `autoconf.h` Generation

One of the most important outputs of `configure` is `autoconf.h`. This header file contains C preprocessor definitions that globally configure the codebase.

```c
/* Example autoconf.h generated for an ARM target */
#ifndef __BUILD_AUTOCONF_H
#define __BUILD_AUTOCONF_H

#define ARCH_ARM 1
#define ARM_PROFILE_CLASSIC 0
#define ARCH arch_arm
#define ARCH_VERSION 4
#define ARCH_PROFILE ARM_PROFILE_CLASSIC

#define ROCKBOX_LITTLE_ENDIAN 1
#define GCCNUM 905

/* Feature flags */
#define HAVE_LOGF 1
#define HAVE_LCD_COLOR 1
#define LCD_WIDTH 320
#define LCD_HEIGHT 240

#endif /* __BUILD_AUTOCONF_H */
```

### 2.2 The Makefile Hierarchy

The generated `Makefile` includes `tools/root.make`, which serves as the master orchestrator. The build system uses a recursive make approach but often flattens dependencies for the final link.

*   **`root.make`**: The entry point. It defines standard targets like `all`, `clean`, `zip`. It includes sub-makefiles.
*   **`firmware/firmware.make`**: Compiles the kernel and drivers.
*   **`apps/apps.make`**: Compiles the main application logic.
*   **`bootloader/bootloader.make`**: Separate build rules for the bootloader, often requiring different link scripts.

The build system creates a hierarchy of object files in the build directory, mirroring the source tree.

### 2.3 Cross-Compilation Toolchain

Rockbox maintains its own patched GCC toolchain (often an older version like GCC 4.x or 9.x for stability on legacy arches). `tools/configure` attempts to find these tools:

```bash
# Example tool search in configure
findarmgcc() {
  prefixtools arm-elf-eabi-
  gccchoice="9.5.0"
}
```

It sets `CC`, `AR`, `LD`, `OC` (objcopy), etc., to the cross-compiler binaries.

## 3. Target Architectures Map

Rockbox supports a diverse museum of embedded architectures. The build system isolates architecture-specific code using directory structures and preprocessor macros.

### 3.1 Supported Architectures

| Architecture | CPU Family | Example Devices | `tools/configure` Hook |
| :--- | :--- | :--- | :--- |
| **ARM** | ARM7TDMI | iPod 1G-3G, Sansa e200 | `arm7tdmicc` |
| **ARM** | ARM926EJ-S | iPod 5G/Classic, Sansa Clip+ | `arm926ejscc` |
| **ColdFire** | MCF5249/5250 | iRiver H100/H300, iAudio X5 | `coldfirecc` |
| **MIPS** | MIPS32 | Ingenic JZ47xx (Onda, FiiO) | `mipselcc` |
| **SH** | SH-1 | Archos Jukebox (Ancient) | (Deprecated/Removed) |

### 3.2 Physical Isolation via `FIRMDIR`

In the `Makefile`, the `TARGET_INC` variable is constructed to include directories in a specific order of precedence. This allows target-specific headers to override generic ones.

```makefile
# Constructed in configure
TARGET_INC = -I$(FIRMDIR)/target/$(CPU)/$(MANUFACTURER)/$(MODEL) \
             -I$(FIRMDIR)/target/$(CPU)/$(MANUFACTURER) \
             -I$(FIRMDIR)/target/$(CPU) \
             -I$(FIRMDIR)/include
```

For an iPod Video (`ipodvideo`), the include path search order would be:
1.  `firmware/target/arm/ipod/video` (Model specific)
2.  `firmware/target/arm/ipod` (SoC/Manufacturer specific)
3.  `firmware/target/arm` (Architecture specific)
4.  `firmware/include` (Generic)

### 3.3 Linker Scripts (`.lds`)

The build system generates or selects a linker script (often `rom.lds` or `app.lds`) based on the target's memory map.

*   **Flash-based Targets:** Code runs from RAM but is stored in Flash. The linker script defines `LOADADDR` and `EXECADDR`.
*   **RAM-based Targets (Hosted):** Code runs as a standard executable.

The `tools/scramble` tool is often used post-link to obfuscate or package the binary into a format the original bootloader accepts (e.g., `.mi4` for Sansa, encrypted firmware for iRiver).

### 3.4 The `features` File

Each target directory contains a `target.h` or `config.h` that defines the hardware capabilities (e.g., `HAVE_LCD_COLOR`, `HAVE_WHEEL_ACCELERATION`). These macros drive conditional compilation throughout `apps/` and `firmware/`.

## 4. Build Flow Summary

1.  **Configure:** User runs `../tools/configure` -> selects target -> `Makefile` & `autoconf.h` generated.
2.  **Dependency Generation:** `make` generates `.d` files for dependency tracking.
3.  **Compilation:** C sources are compiled to `.o` files using the cross-compiler.
    *   Assembly files (`.S`) like `crt0.S` are assembled.
4.  **Linking:** Object files are linked into `rockbox.elf`.
5.  **Post-Processing:**
    *   `objcopy` extracts the binary image (`rockbox.bin`).
    *   `scramble` or `mkboot` packages it into the final firmware file (e.g., `rockbox.ipod`).
6.  **Codecs/Plugins:** Separately compiled as position-independent code (PIC) libraries, linked with specific flags to allow dynamic loading.

This intricate system allows a single codebase to support dozens of wildly different hardware platforms with a unified application layer.
