# Report 01: Architecture and Build

## High-Level Block Diagram

Rockbox is an open-source firmware for digital music players, capable of running as a bare-metal RTOS or as a hosted application on existing OSs like Android or Linux.

The architecture is divided into the following layers:
1. **Bootloader (`bootloader/`)**: Initializes hardware (SDRAM, LCD, Storage) and loads the main firmware payload (`.mi4`, `.ipod`, etc.) into RAM.
2. **Firmware Core (`firmware/`)**: Contains the RTOS (`firmware/kernel/`), hardware abstraction, common drivers (`firmware/common/`, `firmware/drivers/`), memory management, and file systems.
3. **Application Layer (`apps/`)**: Contains the user interface (`apps/gui/`), application logic, playback engine, plugin manager, and settings.
4. **Codec & DSP Library (`lib/rbcodec/`)**: A standalone library containing audio decoders (FLAC, MP3, etc.) and the DSP pipeline.
5. **Tools & Build System (`tools/`)**: Contains `configure` and build scripts, packaging utilities, and generators.
6. **Hosted Application Ports (`firmware/target/hosted/`, `android/`)**: Adapts the Rockbox interface to run as a user-space application on top of POSIX APIs, SDL, or Android JNI.

## Directory Mapping

The primary directories in the Rockbox codebase are:

*   **`tools/`**: Build utilities. The central script is `tools/configure`, which generates makefiles for specific targets.
*   **`firmware/`**: The core OS and drivers.
    *   `firmware/kernel/`: Threading, mutexes, yielding, memory allocators (`buflib`, `core_alloc`).
    *   `firmware/common/`: Common drivers like disk storage (`disk.c`).
    *   `firmware/target/`: Target-specific CPU and SoC drivers (e.g., ARM, ColdFire, MIPS, Hosted).
*   **`apps/`**: The main application and user interface.
    *   `apps/action.c`: The core event loop.
    *   `apps/plugin.h`, `apps/plugin.c`: The Plugin API interface.
    *   `apps/playback.c`: Audio engine and playback control.
*   **`lib/rbcodec/`**: Standalone audio codec and DSP library.
*   **`bootloader/`**: Target-specific boot sequences.
*   **`android/` & `firmware/target/hosted/`**: Application/hosted port layers (POSIX, SDL, Android UI/Audio).

## Build System: tools/configure

The build system in Rockbox is driven by `tools/configure`. This script asks for a target device (or accepts `--target=TARGET`) and configures a build environment specific to that architecture and device.

It defines multiple macros and include paths depending on the target selected. Let's look at the logic inside `tools/configure`:

*   **Target Definitions**: Targets are matched via string matching to define specific CPU architectures (`t_cpu="arm"`, `t_cpu="coldfire"`, `t_cpu="mips"`, `t_cpu="hosted"`).
*   **Target Include Paths (`TARGET_INC`)**: The script dynamically constructs include paths based on CPU, manufacturer, model, and SoC. This maps directly to directories in `firmware/target/`.
    *   Example: `TARGET_INC="-I\$(FIRMDIR)/target/$t_cpu/$t_manufacturer/$t_model"`
    *   For hosted targets, it includes paths like `-I\$(FIRMDIR)/target/hosted/sdl` and `-I\$(FIRMDIR)/target/hosted/tinyalsa/include`.
*   **Make Configuration**: `configure` ultimately exports variables like `TARGET_ID`, `TARGET`, and `TARGET_INC` to `Makefile`, allowing the build to pull in the correct source files and export headers for the requested device.

This approach creates a heavily customized and modular build system, permitting Rockbox to support a matrix of wildly different CPU architectures and devices from a single unified codebase.
