# 02_Bootloader_and_Hardware_Initialization.md

## 1. Bootloader Architecture

Rockbox typically uses a dual-stage boot process on native targets.
1.  **Bootloader:** A small firmware replacing (or chained from) the original vendor firmware. It initializes SDRAM, sets up clocks, reads the main Rockbox binary (`rockbox.mi4`, `rockbox.ipod`) from the disk/flash into RAM, and jumps to it.
2.  **Main Firmware:** The monolithic kernel + application binary.

### Hosted vs. Native
*   **Native:** The bootloader is critical. If it fails, the device is bricked. It deals with raw hardware registers immediately upon power-on.
*   **Hosted (SDL/Android):** There is no "bootloader" in the embedded sense. The host OS (Linux, Windows, Android) loads the executable.
*   **ESP32:** This falls in between. We rely on the **ESP-IDF Bootloader** (2nd stage bootloader) to setup the CPU and flash mapping. The "Rockbox Bootloader" step is effectively skipped or replaced by `app_main()` in the ESP-IDF application structure.

## 2. `crt0` and Vector Tables

On native ARM targets, the execution starts at `crt0.S` (C Run-Time 0). This is the assembly entry point.

### Analysis of `firmware/target/arm/crt0.S`
This file is the template for ARM-based players (iPod, Sansa, Gigabeat).

```assembly
.section .init.text,"ax",%progbits
.global    start
start:
    /* Exception Vector Table */
    b   newstart                /* Reset Vector */
    b   undef_instr_handler     /* Undefined Instruction */
    b   software_int_handler    /* SWI / Syscall */
    b   prefetch_abort_handler  /* Prefetch Abort */
    b   data_abort_handler      /* Data Abort */
    b   reserved_handler
    b   irq_handler             /* IRQ (Standard Interrupt) */
    b   fiq_handler             /* FIQ (Fast Interrupt) */

newstart:
    /* 1. Enter Supervisor Mode, Disable IRQ/FIQ */
    msr     cpsr_c, #0xd3

    /* 2. Zero out IRAM (Internal RAM) and copy IRAM code */
#ifdef USE_IRAM
    ldr     r2, =_iedata
    /* ... loop ... */
#endif

    /* 3. Zero out BSS (Uninitialized Data) */
    ldr     r2, =_edata
    ldr     r3, =_end
    /* ... loop ... */

    /* 4. Setup Stacks for different ARM Modes */
    /* IRQ Stack */
    msr     cpsr_c, #0xd2
    ldr     sp, =irq_stack
    /* FIQ Stack */
    msr     cpsr_c, #0xd1
    ldr     sp, =fiq_stack
    /* SYS Stack (Main Thread) */
    msr     cpsr_c, #0xdf
    ldr     sp, =stackend

    /* 5. Jump to C Main */
    ldr     ip, =main
    bx      ip
```

**ESP32 Contrast:**
On the ESP32, this low-level setup is handled by the ROM bootloader and the ESP-IDF startup code (`cpu_start.c`). We do **not** write a `crt0.S` for ESP32. Instead, we define `app_main()`, which is called after FreeRTOS is already running (usually).

## 3. Hardware Initialization

After jumping to `main()` in `apps/main.c`, the initialization sequence begins.

### Call Stack (`init()`)
The `init()` function in `apps/main.c` orchestrates the startup.

1.  **`system_init()`**: Platform specific low-level init.
    *   **Native:** Sets up CPU frequency, GPIOs, memory controllers.
    *   **SDL (`system-sdl.c`):** Initializes SDL video/audio subsystems, spawns the event thread.
    *   **Android (`system-android.c`):** Initializes telephony/device specific hooks.
    *   **ESP32 Strategy:** This will call `esp_peripherals_init()`, mount the SD card via `vfs_fat`, and initialize the I2S driver.

2.  **`kernel_init()`**: Starts the threading system.
    *   Calls `init_threads()` -> `thread_alloc_init()`.
    *   On Native: Initializes the custom cooperative/preemptive scheduler.
    *   On Hosted: Often a stub or wrapper around `pthread` logic (see File 3).

3.  **`lcd_init()`**: Initializes the display driver.
    *   **Native:** Writes to LCD controller registers (SPI/8080 parallel).
    *   **Hosted:** Sets up the framebuffer surface.

4.  **`storage_init()`**: Initializes the disk driver (ATA/MMC).
    *   **ESP32:** This is where we bridge Rockbox's block device interface to ESP-IDF's SDMMC driver.

## 4. The Handoff (Native vs. ESP32)

### Native Handoff Flowchart
```mermaid
graph TD
    A[Power On] --> B[Vendor ROM / Boot ROM]
    B --> C[Rockbox Bootloader]
    C --> D{Hold Button?}
    D -- Yes --> E[Original Firmware]
    D -- No --> F[Load rockbox.mi4 to RAM]
    F --> G[Jump to 0x00000000 / Entry]
    G --> H[crt0.S]
    H --> I[main()]
```

### ESP32 Handoff Flowchart
```mermaid
graph TD
    A[Power On] --> B[ESP32 ROM Bootloader]
    B --> C[ESP-IDF 2nd Stage Bootloader]
    C --> D[Partition Table Check]
    D --> E[Load 'factory' app (Rockbox)]
    E --> F[FreeRTOS Startup]
    F --> G[app_main()]
    G --> H[Create 'RockboxThread']
    H --> I[rockbox_main()]
```

## 5. System Initialization for Hosted Targets

### `firmware/target/hosted/sdl/system-sdl.c`
This file is the gold standard for how Rockbox runs on top of another OS.
*   **`system_init()`**: Creates an SDL semaphore and spawns `sdl_event_thread`.
*   **`sdl_event_thread()`**:
    *   Initializes SDL Video (`SDL_InitSubSystem`).
    *   Sets up the window (`sdl_window_setup`).
    *   Enters `gui_message_loop()` which pumps SDL events and feeds them into the Rockbox button queue.
*   **`stackbegin`/`stackend`**: These are faked. `stackbegin = stackend = (uintptr_t*)&s;`. Since the host OS manages the stack, Rockbox's stack overflow checking mechanisms are largely bypassed or stubbed.

### `firmware/target/hosted/android/system-android.c`
*   **JNI Entry:** `Java_org_rockbox_RockboxService_main` is the entry point.
*   **`setjmp`/`longjmp`**: Used for `power_off()`. Since we cannot actually shut down the Android device, `power_off` jumps back to the entry point to exit the native library cleanly.
*   **Signaling:** Uses `pthread_cond_t` (`btn_cond`) to synchronize readiness between Java and C.

### ESP32 Implications
The ESP32 port will mimic the Android/SDL approach more than the Native one.
*   We will **not** attempt to replace the ESP32 FreeRTOS scheduler with Rockbox's scheduler.
*   We **will** run Rockbox as a high-priority FreeRTOS task.
*   We **must** map `main()` to `rockbox_main()` to avoid symbol collision with ESP-IDF's `app_main`.
*   **Hardware Init:** Instead of writing to registers in `system_init`, we will call ESP-IDF driver installation functions (`i2s_driver_install`, `spi_bus_initialize`).
