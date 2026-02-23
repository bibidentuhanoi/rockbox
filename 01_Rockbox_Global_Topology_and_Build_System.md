# 01_Rockbox_Global_Topology_and_Build_System.md

## Abstract
This document provides a comprehensive map of the Rockbox repository, detailing the 6-level deep directory structure, the legacy build system orchestrated by Perl and Make, and the cross-compilation toolchain. It explores how the `configure` script dynamically generates `autoconf.h` to manage the massive variability of supported embedded targets (ARM, ColdFire, MIPS) and how the linker scripts (`.lds`) enforce a strict bare-metal memory layout.

## 1. The Master Archaeological Map
This document serves as the foundational map for the Rockbox codebase, a 20+ year-old open-source firmware project designed for portable media players. The architecture reflects a "bare-metal" design philosophy from the early 2000s, where every byte of RAM and every CPU cycle was precious.

### 1.1 The Directory Tree (Depth: 6 Levels)
The following ASCII tree maps the critical structures relevant to the core firmware, bootloader, and build system, digging down to the specific driver implementations.

```text
.
├── apps/                                   # User-space applications and main event loop
│   ├── codecs/                             # Audio codec wrappers
│   │   ├── liba52/                         # AC3 decoder library
│   │   ├── libfaad/                        # AAC decoder library
│   │   ├── libmad/                         # MPEG audio decoder
│   │   │   ├── layer12.c                   # Layer I/II decoding
│   │   │   ├── layer3.c                    # Layer III (MP3) decoding
│   │   │   └── stream.c                    # Stream processing
│   │   └── libtremor/                      # Fixed-point Vorbis decoder
│   ├── gui/                                # The Graphical User Interface engine
│   │   ├── bitmaps/                        # Bitmap handling
│   │   │   └── native/                     # Target-specific native bitmaps
│   │   ├── skin_engine/                    # The WPS (While Playing Screen) engine
│   │   │   ├── skin_display.c              # Skin rendering logic
│   │   │   ├── skin_parser.c               # .wps file parsing
│   │   │   └── wps_internals.c             # Internal WPS state
│   │   └── viewport.c                      # Viewport management
│   ├── plugins/                            # Dynamic loadable plugin source
│   │   ├── bitmaps/                        # Plugin assets
│   │   │   └── native/                     # Native plugin bitmaps
│   │   └── lib/                            # Plugin library helpers
│   │       ├── pluginlib_actions.c         # Action handling helper
│   │       └── xlcd_core.c                 # Plugin LCD core
│   ├── playback.c                          # The high-level audio playback engine
│   └── tree.c                              # Database and file browser logic
├── bootloader/                             # Second-stage bootloaders for various targets
│   ├── common/                             # Shared bootloader logic
│   │   ├── ata_menu.c                      # ATA diagnostic menu
│   │   └── serial_menu.c                   # Serial port diagnostic menu
│   └── main.c                              # Bootloader entry point
├── firmware/                               # The Kernel and Hardware Abstraction Layer (HAL)
│   ├── common/                             # Generic driver implementations
│   │   ├── disk.c                          # Disk handling
│   │   ├── dram.c                          # SDRAM controller init
│   │   └── fat.c                           # Custom FAT12/16/32 driver
│   ├── drivers/                            # Hardware drivers
│   │   ├── ata.c                           # ATA/IDE driver
│   │   ├── audio/                          # I2S/AC97 Audio controller drivers
│   │   │   ├── wm8758.c                    # Wolfson WM8758 codec driver
│   │   │   └── wm8975.c                    # Wolfson WM8975 codec driver
│   │   ├── button.c                        # Button matrix scanning
│   │   ├── lcd/                            # LCD controller drivers
│   │   │   ├── lcd-16bit.c                 # 16-bit color LCD driver
│   │   │   └── lcd-ssd1306.c               # SSD1306 OLED driver
│   │   └── usb/                            # USB stack (Mass Storage, HID)
│   ├── kernel/                             # The core OS kernel
│   │   ├── core_alloc.c                    # Core memory allocator
│   │   ├── kernel.c                        # Main kernel initialization
│   │   └── thread.c                        # Cooperative/Preemptive scheduler
│   ├── target/                             # Target-specific hardware definitions
│   │   ├── arm/                            # ARM architecture targets
│   │   │   ├── as3525/                     # AMS AS3525 SoC (Sansa Clip)
│   │   │   │   ├── sansa-clip/             # Sansa Clip target files
│   │   │   │   │   ├── lcd-clip.c          # Clip-specific LCD setup
│   │   │   │   │   └── powermgmt-clip.c    # Clip power management
│   │   │   │   ├── audio-as3525.c          # AS3525 I2S driver
│   │   │   │   └── usb-drv-as3525.c        # AS3525 USB controller
│   │   │   ├── s3c2440/                    # Samsung S3C2440 SoC (Gigabeat)
│   │   │   │   ├── gigabeat-fx/            # Gigabeat F/X target files
│   │   │   │   │   ├── ata-meg-fx.c        # Gigabeat ATA driver
│   │   │   │   │   └── lcd-target.h        # LCD config macros
│   │   │   │   └── system-s3c2440.c        # S3C2440 system init
│   │   │   └── crt0.S                      # C Runtime Startup (Assembly)
│   │   ├── hosted/                         # Hosted ports (SDL, Android, Linux)
│   │   │   ├── android/                    # Android Application port
│   │   │   │   ├── app/                    # Java Application layer
│   │   │   │   └── pcm-android.c           # Android AudioTrack shim
│   │   │   └── sdl/                        # SDL Simulator port
│   │   │       └── thread-sdl.c            # Pthread wrapper
│   │   └── mips/                           # MIPS architecture targets
│   │       └── ingenic_jz47xx/             # Ingenic JZ4740 SoC
│   │           └── ata-jz4740.c            # JZ4740 MMC/SD driver
│   └── usbstack/                           # USB protocol stack
│       └── usb_core.c                      # Core USB logic
├── lib/                                    # Standalone libraries
│   ├── rbcodec/                            # The core audio decoding library (DSP, Codecs)
│   │   ├── codecs/                         # Fixed-point codec implementations
│   │   │   └── libmad/                     # MAD MP3 decoder
│   │   └── dsp/                            # DSP chain (EQ, Crossfeed, Resampler)
│   │       ├── dsp_filter.c                # Filter processing
│   │       └── dsp_resampler.c             # Polyphase resampler
│   └── skin_parser/                        # .wps file parser
└── tools/                                  # Build tools and scripts
    ├── configure                           # The massive Perl configuration script
    ├── gen_make.pl                         # Makefile generator
    ├── root.make                           # The root Makefile template
    ├── scramble                            # Firmware image scrambler/packer
    └── ucl/                                # Compression tools for bootloaders
```

---

## 2. The Build System Genesis
Rockbox does not use CMake, Autotools, or Kconfig. Instead, it relies on a bespoke, highly complex build system orchestrated by `tools/configure` (a Perl script masquerading as a shell script in some versions, or pure shell/perl hybrids).

### 2.1 `tools/configure`: The Brain
This script is the entry point. It queries the user (or command line arguments) for the target device and generates the build environment.

**Key Responsibilities:**
1.  **Platform Selection:** It maintains a massive internal database of supported targets (MP3 players).
2.  **Compiler Selection:** It determines the cross-compiler prefix (e.g., `arm-elf-eabi-`, `m68k-elf-`) based on the target CPU.
3.  **Preprocessor Definitions:** It generates `autoconf.h`, a header file containing thousands of `#define` directives that control conditional compilation throughout the C codebase.
4.  **Makefile Generation:** It creates a `Makefile` that includes `tools/root.make` and sets variables like `TARGET_ID`, `MEMORYSIZE`, and `BMP2RB_MONO`.

**Deep Dive: The `configure` Logic**
The script parses command line arguments like `--target=ipodvideo` and `--ram=64`.
It then sets variables that cascade into the Makefile:

```bash
# Example logic from tools/configure for iPod Video
   22|ipodvideo)
    target_id=15
    modelname="ipodvideo"
    target="IPOD_VIDEO"
    memory=64 # always. This is reduced at runtime if needed
    arm7tdmicc # Calls function to set ARM7TDMI compiler flags
    tool="$rootdir/tools/scramble -add=ipvd" # Output packer
    bmp2rb_mono="$rootdir/tools/bmp2rb -f 0"
    bmp2rb_native="$rootdir/tools/bmp2rb -f 4"
    output="rockbox.ipod"
    appextra="recorder:gui:radio"
    plugins="yes"
    bootoutput="bootloader-$modelname.ipod"
    toolset=$ipodbitmaptools
    t_cpu="arm"
    t_soc="pp"
    t_manufacturer="ipod"
    t_model="video"
    ;;
```

**Compiler Flags (`arm7tdmicc` function):**
```bash
arm7tdmicc () {
 findarmgcc
 GCCOPTS="$CCOPTS -mcpu=arm7tdmi"
 GCCOPTIMIZE="-fomit-frame-pointer"
 endian="little"
}
```
Note the explicit `-mcpu=arm7tdmi` and `endian="little"`. Rockbox is extremely sensitive to these flags.

### 2.2 `autoconf.h`: The Global Config
The `configure` script generates `autoconf.h`. This file is included by almost every C file in the project via `config.h`.

**Example Content of `autoconf.h`:**
```c
/* This header was made by configure */
#ifndef __BUILD_AUTOCONF_H
#define __BUILD_AUTOCONF_H

#define ARCH_ARM 1
#define ROCKBOX_LITTLE_ENDIAN 1
#define IPOD_VIDEO 1
#define CONFIG_CPU PP5022
#define CONFIG_LCD LCD_IPODVIDEO
#define HAVE_LCD_COLOR 1
#define LCD_WIDTH 320
#define LCD_HEIGHT 240
#define HAVE_DISK_STORAGE 1
#define HAVE_ATA_DMA 1
#define MEMORYSIZE 64
#define BATTERY_CAPACITY_MIN 1200
#define BATTERY_CAPACITY_MAX 1200

#endif /* __BUILD_AUTOCONF_H */
```
This file is the "Pre-processor Glue" that binds the generic kernel to the specific hardware. Code sections are guarded:
```c
#ifdef HAVE_ATA_DMA
    ata_dma_read(...);
#else
    ata_pio_read(...);
#endif
```

### 2.3 `root.make` and the Compilation Graph
The build system uses a recursive make strategy but flattened into a single inclusion logic where possible for dependency tracking.

**Structure of the generated `Makefile`:**
```makefile
# Variables set by configure
export ROOTDIR=...
export FIRMDIR=$(ROOTDIR)/firmware
export APPSDIR=$(ROOTDIR)/apps
export MODELNAME=ipodvideo
export TARGET=-DIPOD_VIDEO
export OBJDIR=$(pwd)

# Include the master makefile
include $(TOOLSDIR)/root.make
```

**`root.make` Logic:**
It defines the rules to build the subcomponents:
1.  **Firmware (`libfirmware.a`):** The kernel and drivers.
2.  **Screens (`libscreens.a`):** UI bitmaps and layout logic.
3.  **Codecs:** Built as standalone shared libraries (`.codec` or overlaid code).
4.  **Plugins:** Built as position-independent executables (`.rock`).
5.  **The Main App (`rockbox.elf`):** Linked against `libfirmware.a`, `libscreens.a`, etc.
6.  **The Final Image (`rockbox.ipod`):** `objcopy` extracts the binary, and `scramble` adds the checksums/headers required by the bootloader.

---

## 3. Target Architectures Map
Rockbox supports a zoo of legacy embedded architectures. The build system isolates them via the `firmware/target/` directory and preprocessor macros.

### 3.1 The `firmware/target/` Abstraction
Directory structure:
`firmware/target/[CPU]/[MANUFACTURER]/[MODEL]/`

**Example: iPod Video**
*   `firmware/target/arm/` (Architecture: ARMv4T)
*   `firmware/target/arm/ipod/` (Manufacturer: Apple)
*   `firmware/target/arm/ipod/video/` (Model: Video 5G)

**Target-Specific Files:**
*   `crt0.S`: The assembly startup code.
*   `system-target.h`: Definitions for CPU frequency, cache layout.
*   `ata-target.h`: GPIO pin definitions for the HDD controller.
*   `lcd-target.h`: Timing parameters for the specific LCD panel.

### 3.2 Supported CPU Families
| Architecture | CPU Family | Example Device | Key Characteristics |
| :--- | :--- | :--- | :--- |
| **ARM** | ARM7TDMI | iPod 1G-5G, Sansa e200 | 32-bit, Thumb mode (sometimes), No FPU, No MMU use. |
| **ARM** | ARM9E | Sansa Clip+ | DSP extensions, higher clock speeds. |
| **ColdFire** | MCF5249/5250 | iRiver H100/H300 | Motorola 68k derivative. Big Endian. |
| **MIPS** | Ingenic JZ4740 | Sansa Fuze+ | MIPS32. Little Endian. |
| **SH** | SH-1 | Archos Jukebox | Hitachi SuperH. Very old. |

### 3.3 The "Hosted" Architecture
A special target exists: `firmware/target/hosted/`.
This is NOT a CPU architecture but an abstraction layer that maps Rockbox calls to a host OS (Linux/SDL, Android, Windows).
*   **Threads:** Mapped to `pthreads` or Windows Threads.
*   **Display:** Mapped to an SDL Window or Android Surface.
*   **Audio:** Mapped to ALSA, PulseAudio, or Android `AudioTrack`.

This architecture is the key precursor to the ESP32 port, as it proves Rockbox can run as a "task" within a larger OS.

---

## 4. The Linker Script (`.lds`)
The build system generates a custom linker script (`app.lds`) for each target. This script is crucial for the "Bare Metal" nature of Rockbox.

**Key Sections:**
1.  **`.text`**: The code. Placed in SDRAM (usually starting at `0x00000000` or a specific offset).
2.  **`.data`**: Initialized globals. Copied from Flash to RAM at startup.
3.  **`.bss`**: Uninitialized globals. Zeroed at startup.
4.  **`audiobuf`**: A specific large section reserved for the audio ring buffer, often placed at the *end* of RAM to maximize contiguous space.
5.  **`pluginbuf`**: A reserved region for loading dynamic plugins (`.rock` files).

**Example `app.lds` Snippet (Conceptual):**
```ld
ENTRY(start)
SECTIONS
{
    . = 0x10000000; /* SDRAM Base */
    .text : {
        *(.text*)
        *(.rodata*)
    } > DRAM

    .data : {
        _data_start = .;
        *(.data*)
        _data_end = .;
    } > DRAM

    .bss : {
        _bss_start = .;
        *(.bss*)
        _bss_end = .;
    } > DRAM

    .audiobuf (NOLOAD) : {
        _audiobuf_start = .;
        . += 0x1000000; /* 16MB Audio Buffer */
        _audiobuf_end = .;
    } > DRAM
}
```

---

## 5. Firmware Export Headers and Macros (`firmware/export/`)

The `firmware/export/` directory contains the public API for the Rockbox kernel. These headers are included by applications and plugins.

### 5.1 `config.h`: The Configuration Hub
`config.h` is the most important header file. It includes `autoconf.h` (generated by `configure`) and then sets up default values and derived macros.

**Logic Flow in `config.h`:**
1.  **Include `autoconf.h`**: Loads target-specific defines (e.g., `IPOD_VIDEO`).
2.  **Include `system.h`**: Loads system-wide types and macros.
3.  **Feature Detection**: Sets derived macros based on hardware features.

```c
/* Example feature detection in config.h */
#if defined(IPOD_VIDEO) || defined(SANSA_E200)
#define HAVE_LCD_COLOR 1
#endif

#ifdef HAVE_LCD_COLOR
#define LCD_DEPTH 16
#else
#define LCD_DEPTH 2
#endif
```

### 5.2 `system.h`: System-Wide Definitions
`system.h` defines core types and macros used throughout the codebase.

*   **`HZ`**: The system tick frequency (usually 100).
*   **`MAX_PATH`**: Maximum file path length (usually 260).
*   **`MIN(a, b)` / `MAX(a, b)`**: Standard utility macros.
*   **`container_of`**: Linux-kernel-style macro for finding a struct from a member.

```c
/* firmware/export/system.h */
#define HZ 100
#define MAX_PATH 260
#define container_of(ptr, type, member) ({          \
    const typeof( ((type *)0)->member ) *__mptr = (ptr);    \
    (type *)( (char *)__mptr - offsetof(type,member) );})
```

### 5.3 `cpu.h`: CPU-Specific Macros
`cpu.h` abstracts CPU-specific instructions like interrupt disabling/enabling and cache control.

```c
/* firmware/export/cpu.h (ARM) */
#define disable_irq() asm volatile ("msr cpsr_c, #0x93")
#define enable_irq()  asm volatile ("msr cpsr_c, #0x13")
```

---

## 6. The `scramble` Tool and Firmware Image Format

The `scramble` tool (`tools/scramble.c`) is essential for creating bootable firmware images. It performs encryption, checksum calculation, and header generation required by the manufacturer's bootloader.

### 6.1 Why Scramble?
Original manufacturers (Apple, Sandisk, iRiver) often Obfuscate or Encrypt their firmware updates. To load Rockbox, we must mimic this format.
*   **iPod:** Uses a simple checksum and header format.
*   **Sansa:** Uses "mi4" encryption (XTEA-based) with a specific key.
*   **iRiver:** Uses a checksum and a specific "decoding" algorithm (bit-shuffling).

### 6.2 The Scramble Process
1.  **Input:** Takes the raw binary (`rockbox.bin`) produced by `objcopy`.
2.  **Header Generation:** Creates a header with:
    *   Magic Number (e.g., `MI4V`)
    *   Firmware Length
    *   Checksum (CRC32 or custom)
    *   Platform ID
3.  **Encryption/Obfuscation:** Applies the target-specific algorithm.
4.  **Output:** Writes the final file (`rockbox.mi4` or `rockbox.ipod`).

**Example `scramble` logic (Concept):**
```c
/* tools/scramble.c */
void scramble_ipod(unsigned char *buf, int len) {
    uint32_t checksum = 0;
    /* Simple additive checksum */
    for (int i = 0; i < len; i++) {
        checksum += buf[i];
    }

    /* Prepend Header */
    struct ipod_header hdr;
    hdr.checksum = checksum;
    hdr.len = len;
    write_header(&hdr);
    write_data(buf, len);
}
```

This tool is invoked by the `Makefile` as the final build step.

---

## 7. Deep Dive: `apps/` Structure

The `apps/` directory contains the high-level application logic. It is separated from `firmware/` to maintain a clean separation between "OS" and "Userland" (even though they share the same address space).

### 7.1 `metadata/`: The Tag Parser
Rockbox parses metadata tags (ID3v1, ID3v2, Vorbis, APE) to display track information.
*   **Performance:** It parses tags on-the-fly or caches them in a database (`tagcache`).
*   **Memory:** It uses a small buffer to store only the necessary fields (Title, Artist, Album).

### 7.2 `recorder/`: The Recording App
Handles audio recording from the built-in mic or line-in.
*   **Encoder:** Uses `lib/rbcodec/codecs/` encoders (WAV, MP3).
*   **Buffer:** Uses `audiobuf` in reverse (filling from ADC, writing to Disk).

### 7.3 `plugins/`: The Extension System
Plugins are compiled separately but live in the source tree.
*   **Categories:**
    *   **Games:** Doom, Duke3D, Solitaire.
    *   **Demos:** Plasma, Snow.
    *   **Viewers:** Text viewer, JPEG viewer.
*   **Build:** `apps/plugins/Makefile` handles the compilation of `.rock` files using the `plugin_api`.

---

## 8. The Application-Firmware Interface (AFI)
The interaction between `apps/` (Application Layer) and `firmware/` (Kernel/HAL) is not mediated by system calls (syscalls). Instead, it relies on direct function calls to APIs defined in `firmware/export/`. This "Monolithic Linking Model" means the entire firmware is a single executable binary.

### 8.1 The `global_settings` Nervous System
The `global_settings` struct (defined in `apps/settings.h`) is the central repository for user configuration. It acts as the primary data bridge between the UI and the hardware drivers.

**How Settings Propagate:**
1.  **User Action:** User changes "Backlight Timeout" in the menu.
2.  **Logic:** `apps/settings.c` updates `global_settings.backlight_timeout`.
3.  **Driver Update:** The backlight thread (in `firmware/drivers/backlight.c`) polls this variable (or registers a callback) to adjust hardware behavior.

```c
/* apps/settings.h */
struct user_settings {
    /* ... 100s of settings ... */
    int volume;                 /* -128 to 0 dB */
    int backlight_timeout;      /* Seconds */
    int disk_spindown;          /* Seconds */
    bool hold_lr_for_scroll;    /* UI Behavior */
    /* ... */
};

extern struct user_settings global_settings;
```

### 8.2 The Event Bus (`action.h`)
The `apps/` layer does not poll GPIOs directly. It relies on the **Action System** (`apps/action.c`) to translate raw hardware events (`BUTTON_HOME`) into semantic actions (`ACTION_WPS_PLAY`).

**The Translation Flow:**
1.  **Hardware:** `firmware/drivers/button.c` detects press, posts `BUTTON_HOME` to `button_queue`.
2.  **Action Layer:** `get_action()` dequeues the button code.
3.  **Context Mapping:** It checks the current Context (e.g., `CONTEXT_MAINMENU`).
4.  **Lookup:** It looks up `BUTTON_HOME` in the `CONTEXT_MAINMENU` keymap.
5.  **Result:** It returns `ACTION_STD_OK` to the application loop.

```c
/* apps/action.h */
enum {
    CONTEXT_MAINMENU,
    CONTEXT_WPS,
    CONTEXT_LIST,
    /* ... */
};

/* Semantic Actions */
enum {
    ACTION_STD_PREV,
    ACTION_STD_NEXT,
    ACTION_WPS_PLAY,
    /* ... */
};
```

### 8.3 The Playback API Contract
The `apps/playback.c` module controls the audio engine but delegates the heavy lifting to `firmware/export/audio.h` and `firmware/export/pcm.h`.

**Key API Functions:**
*   `audio_init()`: Bootstraps the codec thread and I2S driver.
*   `audio_play(offset)`: Starts playback logic (disk reading, buffering).
*   `pcm_play_data(start, size)`: Low-level command to start DMA transfer.
*   `mixer_set_volume(l, r)`: Adjusts software or hardware gain.

**Call Graph:**
```
[User] -> [Action] -> [Audio Thread] -> [Codec Thread] -> [PCM Driver] -> [I2S HW]
```

### 8.4 The UI Stack (`screen` struct)
The `apps/gui/` code is hardware-agnostic. It draws to a `struct screen` object, which contains function pointers to the specific LCD driver routines.

**The `screen` Interface:**
```c
/* firmware/export/lcd.h */
struct screen {
    int width;
    int height;
    int depth;
    void (*update)(void);
    void (*put_pixel)(int x, int y, unsigned color);
    /* ... */
};

extern struct screen screens[NB_SCREENS];
```
This allows the same UI code to drive the Main LCD (`screens[0]`) and the Remote LCD (`screens[1]`) simply by passing a different pointer.

### 8.5 Architecture Layer Diagram (ASCII)
This massive diagram illustrates the strict layering of the Rockbox architecture.

```text
+---------------------------------------------------------------+
|                       USER INTERFACE                          |
|  (apps/gui/, apps/menu.c, apps/tree.c, apps/plugins/)         |
+------------------------------+--------------------------------+
                               |
          Calls 'get_action()' | Returns 'ACTION_STD_OK'
                               v
+------------------------------+--------------------------------+
|                       ACTION SYSTEM                           |
|  (apps/action.c, apps/keymaps/)                               |
+------------------------------+--------------------------------+
                               |
          Reads 'button_queue' | Returns 'BUTTON_HOME'
                               v
+------------------------------+--------------------------------+
|                       KERNEL / OS                             |
|  (firmware/kernel/thread.c, firmware/kernel/queue.c)          |
+------------------------------+--------------------------------+
                               ^
                  Posts Events | ISRs trigger 'queue_post'
                               |
+------------------------------+--------------------------------+
|               HARDWARE ABSTRACTION LAYER (HAL)                |
|  (firmware/drivers/, firmware/target/)                        |
|                                                               |
|  [LCD Driver]   [Button Driver]   [Audio Driver]   [Storage]  |
+-------+---------------+-------+----------+-------------+------+
        |               |                  |             |
        v               v                  v             v
+-------+---------------+-------+----------+-------------+------+
|                      BARE METAL HARDWARE                      |
|  [GPIO Registers]  [I2S/DMA]  [SD Controller]  [PMIC I2C]     |
+---------------------------------------------------------------+
```

## 9. Summary
The Rockbox build system is a monolithic, target-aware generator that constructs a complete bare-metal OS from source. It relies heavily on:
1.  **Perl** (`configure`) for configuration.
2.  **GNU Make** (`root.make`) for dependency management.
3.  **GCC/Binutils** for cross-compilation and linking.
4.  **Custom Packing Tools** (`scramble`) to satisfy proprietary bootloader checksums.

This structure allows Rockbox to support 50+ distinct hardware platforms with a single codebase, using extensive `#ifdef` guarding and modular driver organization. The separation of `firmware/` (kernel) and `apps/` (userland), combined with the powerful `configure` script, makes the codebase remarkably scalable despite its age.
