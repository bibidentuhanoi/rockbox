# 01_Rockbox_Global_Topology_and_Build_System.md

## 1. The Master ASCII Tree: Rockbox Global Topology

The Rockbox repository is a sprawling 20-year-old codebase containing over 4,000 files. Below is a high-level topographical map of the root directory and its primary sub-architectures, expanded to show key structural components.

```text
/ (Rockbox Root)
├── apps/                       # High-level Applications & User Interface
│   ├── bitmaps/                # UI assets (icons, backdrops)
│   ├── codecs/                 # (Legacy path, now mostly in lib/rbcodec)
│   ├── gui/                    # The Graphical User Interface Engine
│   │   ├── skin_engine/        # The WPS skin parser
│   │   ├── list.c              # Generic list widget
│   │   └── wps.c               # While Playing Screen logic
│   ├── lang/                   # Localization files (.lang)
│   ├── menus/                  # Menu definition structures (.c files)
│   ├── plugins/                # Dynamic user plugins (.rock)
│   │   ├── bitmaps/            # Plugin-specific assets
│   │   └── lib/                # Shared plugin libraries (simplegraph, grey, etc.)
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
│   │   ├── rtc.c               # Real Time Clock
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
│   │   │   ├── system-target.h # System definitions
│   │   │   └── as3525/         # Austria Microsystems SoC support
│   │   ├── coldfire/           # Motorola ColdFire support
│   │   ├── mips/               # MIPS architecture support
│   │   └── hosted/             # Simulation/Hosted ports (SDL, Android)
│   ├── elf_loader.c            # Dynamic Linker/Loader for Plugins
│   └── pcm.c                   # Audio DMA Manager
├── lib/                        # Static Libraries
│   └── rbcodec/                # The Audio Decoding Engine
│       ├── codecs/             # Codec Implementations (MP3, FLAC, etc.)
│       │   ├── libmad/         # MPEG Audio Decoder
│       │   ├── libfaad/        # AAC Decoder
│       │   └── codecs.h        # Codec API Interface
│       └── dsp/                # Digital Signal Processing (EQ, Crossfeed)
├── tools/                      # The Build System & Host Tools
│   ├── configure               # The Master Build Script (Perl/Shell)
│   ├── root.make               # The Root Makefile Template
│   ├── buildzip.pl             # Packaging script
│   ├── scramble.c              # Firmware encryption/obfuscation tool
│   ├── bmp2rb.c                # Bitmap converter
│   ├── voice.pl                # Voice file generator
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
3.  **Toolchain Selection:** It locates the appropriate cross-compiler (e.g., `arm-elf-eabi-gcc`, `m68k-elf-gcc`). Rockbox is notoriously specific about compiler versions due to custom linker scripts and bare-metal assumptions.
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

#### Advanced Build Types

The configuration system isn't just for standard firmware. It supports several build "types" passed via the `--type` argument or interactively selected:

*   **Normal (`N`):** The standard firmware build.
*   **Simulator (`S`):** Builds a PC version of Rockbox (SDL-based) for debugging UI/logic.
*   **Bootloader (`B`):** Builds the bootloader binary instead of the main OS. This completely changes the linker script and available libraries.
*   **Voice (`V`):** Configures the build for voice generation (using TTS engines like Festival/Flite) to support accessibility.
*   **CheckWPS (`C`):** A specialized simulator build for validating theme files (`.wps`).

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
#define CONFIG_CPU AS3525

#endif /* __BUILD_AUTOCONF_H */
```

### 2.2 The Makefile Hierarchy

The generated `Makefile` includes `tools/root.make`, which serves as the master orchestrator. The build system uses a recursive make approach but often flattens dependencies for the final link.

*   **`root.make`**: The entry point. It defines standard targets like `all`, `clean`, `zip`. It includes sub-makefiles.
*   **`firmware/firmware.make`**: Compiles the kernel and drivers. It handles the assembly of `crt0.S` and the core kernel logic.
*   **`apps/apps.make`**: Compiles the main application logic.
*   **`bootloader/bootloader.make`**: Separate build rules for the bootloader, often requiring different link scripts and minimizing code size.
*   **`lib/rbcodec/rbcodec.make`**: Handles the compilation of the codec library and DSP code.

The build system creates a hierarchy of object files in the build directory, mirroring the source tree. For example, `apps/main.c` becomes `$(BUILDDIR)/apps/main.o`.

### 2.3 Cross-Compilation Toolchain

Rockbox maintains its own patched GCC toolchain (often an older version like GCC 4.x or 9.x for stability on legacy arches). `tools/configure` attempts to find these tools:

```bash
# Example tool search in configure
findarmgcc() {
  prefixtools arm-elf-eabi-
  gccchoice="9.5.0"
}
```

It sets `CC`, `AR`, `LD`, `OC` (objcopy), etc., to the cross-compiler binaries. This ensures that the code is compiled for the correct instruction set (e.g., Thumb vs ARM) and ABI.

### 2.4 Utility Scripts in `tools/`

The `tools/` directory is critical for the build process.

*   **`scramble`**: This tool encrypts or obfuscates the compiled binary to match the format expected by the original manufacturer's bootloader. For example, it might XOR the binary or add a checksum header.
*   **`mkboot`**: A utility to patch bootloaders into firmware files.
*   **`bmp2rb`**: Converts standard BMP images into Rockbox's internal bitmap format (used for icons and fonts).
*   **`buildzip.pl`**: A Perl script that packages the final build artifacts (firmware, fonts, plugins, etc.) into a release ZIP file.
*   **`codepages`**: Generates character mapping tables for internationalization support.

## 3. Target Architectures Map

Rockbox supports a diverse museum of embedded architectures. The build system isolates architecture-specific code using directory structures and preprocessor macros.

### 3.1 Supported Architectures

| Architecture | CPU Family | Example Devices | `tools/configure` Hook |
| :--- | :--- | :--- | :--- |
| **ARM** | ARM7TDMI | iPod 1G-3G, Sansa e200 | `arm7tdmicc` |
| **ARM** | ARM926EJ-S | iPod 5G/Classic, Sansa Clip+ | `arm926ejscc` |
| **ARM** | Cortex-M | HiFi Walker H2, Surfans F20 | `armcortexmcc` |
| **ColdFire** | MCF5249/5250 | iRiver H100/H300, iAudio X5 | `coldfirecc` |
| **MIPS** | MIPS32 | Ingenic JZ47xx (Onda, FiiO) | `mipselcc` |
| **SH** | SH-1 | Archos Jukebox (Ancient) | (Deprecated/Removed) |

### 3.2 Physical Isolation via `FIRMDIR`

In the `Makefile`, the `TARGET_INC` variable is constructed to include directories in a specific order of precedence. This allows target-specific headers to override generic ones. This is a form of polymorphism via include paths.

```makefile
# Constructed in configure
TARGET_INC = -I$(FIRMDIR)/target/$(CPU)/$(MANUFACTURER)/$(MODEL) \
             -I$(FIRMDIR)/target/$(CPU)/$(MANUFACTURER) \
             -I$(FIRMDIR)/target/$(CPU) \
             -I$(FIRMDIR)/include
```

For an iPod Video (`ipodvideo`), the include path search order would be:
1.  `firmware/target/arm/ipod/video` (Model specific: defines button mappings, LCD pinout)
2.  `firmware/target/arm/ipod` (SoC/Manufacturer specific: defines PP5022 registers)
3.  `firmware/target/arm` (Architecture specific: defines IRQ handling, context switching)
4.  `firmware/include` (Generic: defines kernel API)

### 3.3 Linker Scripts (`.lds`)

The build system generates or selects a linker script (often `rom.lds` or `app.lds`) based on the target's memory map.

*   **Flash-based Targets:** Code runs from RAM but is stored in Flash. The linker script defines `LOADADDR` and `EXECADDR`. It handles the `copy_data` logic where initialized data is copied from Flash to RAM at startup.
*   **RAM-based Targets (Hosted):** Code runs as a standard executable.
*   **Bootloader vs Firmware:** The bootloader often has a different linker script because it runs from a fixed address in internal SRAM or a specific Flash sector, whereas the main firmware is loaded into the main SDRAM.

The `tools/scramble` tool is often used post-link to obfuscate or package the binary into a format the original bootloader accepts (e.g., `.mi4` for Sansa, encrypted firmware for iRiver).

### 3.4 The `features` File

Each target directory contains a `target.h` or `config.h` that defines the hardware capabilities (e.g., `HAVE_LCD_COLOR`, `HAVE_WHEEL_ACCELERATION`, `HAVE_RECORDING`). These macros drive conditional compilation throughout `apps/` and `firmware/`.

## 4. `tools/root.make` Internals

The `root.make` file is the central nervous system of the build. It is included by the generated `Makefile` and does the heavy lifting.

### 4.1 Compiler Flags and Defines

`root.make` aggregates flags into the `CFLAGS` variable:

```makefile
DEFINES = -DROCKBOX -DMEMORYSIZE=$(MEMORYSIZE) $(TARGET) \
	-DTARGET_ID=$(TARGET_ID) -DTARGET_NAME=\"$(MODELNAME)\" $(BUILDDATE) \
	$(EXTRA_DEFINES)

INCLUDES = -I$(BUILDDIR) -I$(BUILDDIR)/lang $(TARGET_INC)

CFLAGS = $(INCLUDES) $(DEFINES) $(GCCOPTS)
```

`GCCOPTS` comes from `configure` and typically includes `-Os` (optimize for size), `-nostdlib`, and `-ffreestanding` to ensure the compiler doesn't link against the standard C library (Rockbox provides its own libc subset).

### 4.2 Pattern Rules for Compilation

Rockbox defines precise pattern rules to handle source files located in different parts of the tree:

```makefile
# when source and object are in different locations (normal):
$(BUILDDIR)/%.o: $(ROOTDIR)/%.c
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@

$(BUILDDIR)/%.o: $(ROOTDIR)/%.S
	$(SILENT)mkdir -p $(dir $@)
	$(call PRINTS,CC $(subst $(ROOTDIR)/,,$<))$(CC) $(CFLAGS) -c $< -o $@
```

These rules ensure that the object files end up in the `build/` directory structure, keeping the source tree clean. The `$(call PRINTS,...)` macro handles the "pretty printing" of build status (e.g., `CC apps/main.c`) instead of showing the full command line.

### 4.3 Recursive Inclusion Strategy

Instead of calling `make` recursively for every subdirectory (which is slow and complicates dependency tracking), Rockbox uses a single-pass make strategy where possible.

`root.make` includes sub-makefiles:
*   `include $(FIRMDIR)/firmware.make`
*   `include $(APPSDIR)/apps.make`
*   `include $(ROOTDIR)/lib/rbcodec/rbcodec.make`

These sub-makefiles append their source files to a global `SRC` variable. For example:
```makefile
# In apps/apps.make
SRC += $(APPSDIR)/main.c $(APPSDIR)/playback.c ...
```
Then, `root.make` converts `SRC` to `OBJ` and links everything in one go.

## 5. Application Layer Integration (`apps/apps.make`)

The `apps/` directory is treated as a distinct layer. `apps.make` is responsible for aggregating all high-level logic.

### 5.1 Bitmap Generation

The build system automatically converts BMP files in `apps/bitmaps/` into C headers.
`apps/bitmaps/bitmaps.make` defines rules to run `bmp2rb` on `.bmp` files to generate `.c` and `.h` files, which are then compiled and linked.

### 5.2 Language Generation

Localization files (`.lang`) are processed by `tools/genlang`. This tool reads the english master file and the target language file, producing a binary `.lng` file for runtime loading and a `lang_enum.h` header for compile-time constants.

## 6. Simulator Build Specifics

When `tools/configure` is run with `--type=S` (Simulator), the build graph changes significantly.

*   **Compiler Switch:** `CC` changes from the cross-compiler to the host compiler (e.g., `gcc` or `mingw-gcc`).
*   **Target Switch:** `TARGET_INC` points to `firmware/target/hosted/sdl` instead of `firmware/target/arm/...`.
*   **Libraries:** It links against `libSDL` and `libpthread` instead of the bare-metal startup code.
*   **Defines:** `-DSIMULATOR` is defined, which conditionally compiles code to mock hardware interactions (like I2C or SPI) via the simulator framework.

This allows the exact same application code (`apps/`) to run on a Linux desktop as runs on an embedded MP3 player, with hardware drivers swapped out for software simulations.
