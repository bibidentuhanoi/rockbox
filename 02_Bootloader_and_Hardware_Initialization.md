# 02_Bootloader_and_Hardware_Initialization.md

## 1. Bootloader Architecture: The Handoff

The Rockbox bootloader architecture is bifurcated into two primary stages on legacy devices:

1.  **Manufacturer Primary Bootloader:** The ROM-based code burned into the SoC by the vendor (Apple, Toshiba, etc.). This initializes the most basic hardware (sometimes) and loads the secondary bootloader from disk or flash.
2.  **Rockbox Secondary Bootloader:** This is the `bootloader/` directory in the repository. It is a small, specialized binary responsible for:
    *   Initializing SDRAM controller (if not done by Stage 1).
    *   Initializing the storage controller (IDE/ATA, SD/MMC).
    *   Initializing the LCD for basic debug output (`printf`).
    *   Reading the main firmware payload (`rockbox.mi4` or `rockbox.ipod`) into RAM.
    *   Jumping to the main firmware's entry point.

### 1.1 The Bootloader Directory Structure (`bootloader/`)

```text
bootloader/
├── common.c            # Generic loader logic (printf, error handling)
├── ipod.c              # Apple iPod (PortalPlayer) init code
├── sansa_as3525.c      # SanDisk Sansa (AMS AS3525) init code
├── main-*.c            # Entry points for various targets
├── fat32format.c       # Emergency FAT32 formatting tools
└── ucl/                # Decompression logic (for compressed firmware)
```

## 2. `crt0.S`: The Assembly Startup

Before any C code runs, the `crt0.S` (C Runtime Start) assembly file executes. This is the "Big Bang" of the firmware.

### 2.1 Vector Table (ARM Architecture)

From `firmware/target/arm/crt0.S`:

```assembly
    .section .init.text,"ax",%progbits
    .global    start
start:
    b   newstart                /* Reset Vector */
    b   undef_instr_handler     /* Undefined Instruction */
    b   software_int_handler    /* SWI (Software Interrupt) */
    b   prefetch_abort_handler  /* Prefetch Abort */
    b   data_abort_handler      /* Data Abort */
    b   reserved_handler        /* Reserved */
    b   irq_handler             /* IRQ (Standard Interrupt) */
    b   fiq_handler             /* FIQ (Fast Interrupt) */
```

### 2.2 The Startup Sequence (`newstart`)

1.  **Supervisor Mode:** The CPU immediately switches to Supervisor (SVC) mode and disables interrupts (`IRQ` and `FIQ`).
    ```assembly
    msr     cpsr_c, #0xd3 /* enter supervisor mode, disable IRQ/FIQ */
    ```

2.  **Memory Initialization (`memory_init`):** On some SoCs (like AS3525), the bootloader or early firmware must manually configure the SDRAM controller timing and refresh rates before RAM is usable.

3.  **BSS Clearing:** The `.bss` section (uninitialized globals) is zeroed out.
    ```assembly
    ldr     r2, =_edata
    ldr     r3, =_end
    mov     r4, #0
1:  cmp     r3, r2
    strhi   r4, [r2], #4
    bhi     1b
    ```

4.  **Stack Setup:** Rockbox sets up separate stacks for different CPU modes to ensure interrupt stability.
    *   `irq_stack`: For standard hardware interrupts.
    *   `fiq_stack`: For high-priority audio interrupts (DMA).
    *   `svc_stack`: For the main execution thread.

5.  **The Jump:** Finally, it branches to the C entry point.
    ```assembly
    ldr     ip, =main
    bx      ip
    ```

## 3. Hardware Initialization (C-Land)

Once in `main()` (renamed to `rockbox_main` in some contexts), the system performs a rigid initialization sequence.

### 3.1 The `system_init()` Call Stack

1.  **`cpu_init()`:** Configures PLLs and core clock dividers.
2.  **`int_init()`:** Initializes the Interrupt Controller (VIC/AIC). Registers default handlers.
3.  **`timer_init()`:** Sets up the microsecond system timer.
4.  **`kernel_init()`:** Initializes the thread scheduler and synchronization primitives.
5.  **`lcd_init()`:** Configures the LCD controller, sets parallel/serial interface timings, and clears the screen.
6.  **`storage_init()`:** Probes for the hard drive or SD card.

### 3.2 The Bootloader Loop

In `bootloader/common.c`, the logic is simpler than the main OS:

```c
void main(void)
{
    system_init();
    lcd_init();
    storage_init();

    int rc = load_firmware(loadbuffer, "rockbox.mi4", MAX_LOAD_SIZE);

    if (rc > 0) {
        launch_firmware(loadbuffer, rc);
    } else {
        error(EBOOTFILE, rc, true);
    }
}
```

## 4. The Firmware Handoff

The `launch_firmware` function is critical. It typically involves:

1.  **Disabling Interrupts:** To ensure the new kernel starts with a clean slate.
2.  **Cache Flushing:** Invalidating I-Cache and D-Cache to prevent stale instructions.
3.  **Relocation (Optional):** If the image is compressed (UCL/NRV), decompressing it to the execution address.
4.  **Jump:** Modifying the Program Counter (PC) to the entry point of the loaded image.

### Firmware File Formats

*   **`.mi4` (Sansa):** Encrypted/obfuscated container with magic headers.
*   **`.ipod`:** Raw binary or simple header wrapper.
*   **`.iriver`:** Scrambled binary (XOR/Rotation).

The `tools/scramble` utility handles the generation of these formats during the build process.
