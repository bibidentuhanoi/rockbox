# 02_Bootloader_and_Hardware_Initialization.md

## Abstract
This document traces the boot process from the moment of power-on reset (POR) to the transfer of control to the main kernel. It analyzes the dual-stage bootloader architecture, the raw assembly startup code (`crt0.S`) responsible for setting up the C runtime environment (stack, BSS, vectors), and the critical hardware initialization sequence that configures the PLL, SDRAM, and caches before the OS can run.

## 1. The Bootloader Architecture
The Rockbox bootloader (`bootloader/`) is a minimal firmware shim designed to coexist with the original manufacturer's firmware (OF). It typically resides in a reserved flash sector or is appended to the OF image. Its primary purpose is to initialize basic hardware (SDRAM, LCD, Storage) and load the main Rockbox payload (`rockbox.mi4`, `rockbox.ipod`, etc.) into RAM.

### 1.1 Primary vs. Secondary Bootloaders
*   **Primary Bootloader:** Hard-coded in the CPU's Mask ROM or Flash. It initializes the bare minimum and jumps to a fixed address. Rockbox rarely replaces this.
*   **Secondary Bootloader:** Often part of the OF. Rockbox replaces or patches this stage.
    *   **iPod:** Uses a patched `apple_os.bin` or a custom `bootloader.bin` stored in a hidden partition.
    *   **Sansa:** Uses a patched `mi4` file which is encrypted/signed. Rockbox tools (`scramble`) handle this.
    *   **Hosted (New):** For ESP32, the "bootloader" is the ESP-IDF standard bootloader, and Rockbox is just an `app_main` task. However, the legacy logic remains relevant for understanding the hardware state expectation.

### 1.2 The Boot Flow
1.  **Power On Reset (POR):** CPU jumps to the Reset Vector (usually `0x00000000` or `0xFFFF0000`).
2.  **Hardware Init (Assembly):** `crt0.S` sets up stacks, disables interrupts, and configures the PLL/SDRAM controller.
3.  **Bootloader C-Main:** `bootloader/main.c` scans buttons to decide whether to boot Rockbox or the Original Firmware.
4.  **Payload Loading:** The bootloader reads `/.rockbox/rockbox.mi4` from disk into SDRAM (e.g., at `0x10000000`).
5.  **The Jump:** It disables all peripherals/DMA and jumps to the entry point of the loaded payload.

---

## 2. `crt0.S`: The Genesis (Assembly Startup)
This file is the absolute first code executed by the CPU. It handles the raw metal setup before any C code can run.

**Source Analysis: `firmware/target/arm/crt0.S` (ARMv4T Example)**

### 2.1 The Exception Vector Table
The vector table is placed at `0x00000000`. It contains branch instructions to handlers for various CPU exceptions.

```assembly
    .section .init.text,"ax",%progbits
    .global    start
start:
    /* Exception vectors */
    b   newstart                /* Reset Vector: Jump to startup code */
    b   undef_instr_handler     /* Undefined Instruction */
    b   software_int_handler    /* SWI (System Call) */
    b   prefetch_abort_handler  /* Instruction Fetch Memory Abort */
    b   data_abort_handler      /* Data Access Memory Abort */
    b   reserved_handler        /* Reserved */
    b   irq_handler             /* IRQ (Interrupt Request) */
    b   fiq_handler             /* FIQ (Fast Interrupt Request) */
```
*Note: The `b newstart` instruction at offset 0x00 is critical. It is the first instruction executed.*

### 2.2 System Mode & Stack Setup
The startup code switches the CPU to Supervisor (SVC) mode and disables interrupts (IRQ/FIQ) to ensure a stable environment.

```assembly
newstart:
    /* Enter Supervisor Mode, Disable IRQ/FIQ */
    msr     cpsr_c, #0xd3

    /* Initialize Stack Pointer (SP) */
    ldr     sp, =stackend   /* Stack grows downwards from top of RAM */
```

### 2.3 Stack Painting & Overflow Detection
Rockbox employs a "stack painting" technique to detect stack overflows at runtime. The entire stack memory region is filled with a known pattern (`0xDEADBEEF`).

```assembly
    /* Fill stack with 0xDEADBEEF for overflow checking */
    ldr     r0, =stackbegin
    ldr     r1, =stackend
    ldr     r2, =0xDEADBEEF
1:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     1b
```
*   **Purpose:** If the stack pointer ever descends into `stackbegin`, or if `0xDEADBEEF` is overwritten near the bottom, a stack overflow has occurred.
*   **Runtime Check:** The kernel periodically checks `*stackbegin`. If it is not `0xDEADBEEF`, it triggers a kernel panic ("Stack Overflow").

### 2.4 BSS Clearing (The "Zero" Initialization)
C standards require uninitialized global variables (`static int x;`) to be zero at startup. `crt0.S` performs this manually.

```assembly
    /* Clear BSS section */
    ldr     r0, =_bss_start
    ldr     r1, =_bss_end
    mov     r2, #0
2:
    cmp     r0, r1
    strlo   r2, [r0], #4
    blo     2b
```

### 2.5 The Jump to C
Finally, the assembly code jumps to `main()` (which is usually renamed to `rockbox_main` via macros).

```assembly
    /* Jump to C code */
    ldr     pc, =main
```

---

## 3. Hardware Initialization Calls
Before the OS kernel starts, the hardware must be brought up to a usable state. This is typically done in `system_init()` called early in `main()`.

### 3.1 The PLL & Clock Tree
*   **Oscillator:** The external crystal (e.g., 24MHz).
*   **PLL (Phase Locked Loop):** Multiplies the crystal frequency to CPU/Bus speeds (e.g., 200MHz CPU, 100MHz Bus).
*   **Dividers:** Peripheral clocks (I2S, SPI, UART) are derived from the PLL via dividers.

**Example C Call Sequence:**
```c
void system_init(void)
{
    /* 1. Configure PLL */
    CPU_CCR = CCR_FSEL_200MHZ; // Set CPU clock

    /* 2. Configure SDRAM Controller */
    /* Set timing parameters (CAS latency, Refresh rate) */
    SDRAM_CONFIG = 0x...;

    /* 3. Enable Peripheral Clocks */
    CLK_GATING |= (CLK_LCD | CLK_I2S | CLK_USB);
}
```

### 3.2 The Memory Management Unit (MMU) / MPU
On ARM processors (ARM926EJ-S), Rockbox uses the MMU primarily for **caching control**, not for virtual memory address translation (it uses a flat 1:1 mapping).

*   **Cacheable Regions:** SDRAM (for code and data).
*   **Uncacheable Regions:** Memory-Mapped I/O (MMIO) registers (0x50000000+).

**Configuration:**
The page table is often set up in `system-target.c`.
```c
/* Enable I-Cache and D-Cache */
mmu_init();
enable_caches();
```
*Wait: Rockbox typically runs with 1:1 physical-to-virtual mapping. It does not use paging/swapping.*

---

## 4. The Handoff: Bootloader -> Firmware
When the bootloader decides to run Rockbox, it performs a specific sequence to hand over control.

### 4.1 Loading the Payload
The bootloader uses its own mini-driver to read the `rockbox.mi4` file from the FAT32 partition into a high memory address (e.g., `0x02000000`).
It then parses the file header (checking for compression, usually UCL or NRV2B) and decompresses it to the execution address (e.g., `0x00000000` in SDRAM).

### 4.2 Quiescing Hardware
Before jumping, the bootloader must "quiet" the hardware to prevent interrupts from firing during the transition (which would crash the new firmware since its vector table isn't set up yet).
1.  Disable all IRQs.
2.  Stop DMA transfers.
3.  Turn off LCD (optional, but common to avoid artifacts).

### 4.3 The Final Jump
The jump is an absolute branch to the entry point of the loaded firmware.

```c
/* Function pointer cast to jump to 0x00000000 */
void (*kernel_entry)(void) = (void (*)(void))0x00000000;

/* Disable interrupts one last time */
disable_interrupts();

/* Execute! */
kernel_entry();
```
At this exact moment, `crt0.S` of the main firmware starts executing, repeating the cycle described in Section 2, but this time for the full OS.

---

## 5. Bootloader Protocol: Checksums and Security
In the early days (Archos), Rockbox used simple binary dumps. Later targets (iPod, Sansa, Gigabeat) required encrypted or signed firmwares.

### 5.1 The MI4 Format (Sansa/Gigabeat)
The `.mi4` format is a container that supports encryption (TEA - Tiny Encryption Algorithm) and checksums.
*   **Magic:** `0x4D493420` ("MI4 ")
*   **Structure:**
    ```c
    struct mi4_header {
        uint32_t magic;      /* "MI4 " */
        uint32_t length;     /* Total file length */
        uint32_t crc32;      /* CRC32 of payload */
        uint32_t version;    /* Firmware version */
        uint32_t platform;   /* Target ID */
        uint32_t encryption; /* 0=None, 1=TEA */
        /* ... padding ... */
    };
    ```
*   **The "Scramble" Tool:** Rockbox's build system uses `tools/scramble` to generate these files. It takes the compiled `rockbox.bin` (raw ELF or binary), calculates the CRC, encrypts the payload with a device-specific key (reverse-engineered), and prepends the header.

### 5.2 The iPod Firmware Partition
Apple iPods do not use a file system for the bootloader. They use a raw partition table.
*   **Partition 1:** Apple Firmware.
*   **Partition 2:** Hibernation image.
*   **Hidden Area:** Rockbox installs its bootloader into the unused space following the Apple partition map.
*   **Boot Logic:** The primary Apple bootloader loads the first 64KB of the partition. Rockbox patches this 64KB block to jump to its own code instead of Apple's OS.

---

## 6. Detailed Trace: `main()` Initialization Sequence
Once `crt0.S` hands off to C, `apps/main.c` orchestrates the system bring-up. This sequence is synchronous and critical.

1.  **`system_init()`**: Sets up clocks and watchdog.
2.  **`kernel_init()`**: Initializes the scheduler (`thread_init`), queues, and synchronization primitives.
3.  **`enable_interrupts()`**: The first moment IRQs are live.
4.  **`lcd_init()`**: Configures the display controller.
    *   *Note:* The screen often displays "Rockbox loading..." here.
5.  **`storage_init()`**: Brings up the IDE/SDMMC bus.
6.  **`fs_mount()`**: Mounts the FAT32 partition.
7.  **`settings_load()`**: Reads `config.cfg` from disk.
    *   *Critical:* Before this, all settings (volume, backlight) are defaults.
8.  **`usb_init()`**: Starts the USB stack threads.
9.  **`audio_init()`**: Starts the high-priority audio thread.
10. **`app_main()`**: Enters the main event loop.

---

## 7. Memory Map: The Bootloader's View
During the boot process, RAM is fragmented.

```text
+-----------------------+ 0x00000000
| Exception Vectors     |
+-----------------------+ 0x00000020
| Bootloader Code (RO)  | <-- Executing here
+-----------------------+
| Bootloader Stack      |
+-----------------------+ 0x00100000
| ... Free RAM ...      |
+-----------------------+ 0x02000000
| Payload Buffer (Temp) | <-- rockbox.mi4 loaded here
+-----------------------+
| Framebuffer           |
+-----------------------+ 0x04000000 (End of RAM)
```

**The Decompression Overlap Hazard:**
The bootloader must decompress the payload from the "Payload Buffer" to `0x00001000`.
*   **Hazard:** If the decompressed size is large, it might overwrite the executing bootloader code if not careful.
*   **Solution:** The bootloader usually relocates itself to the very end of RAM before starting the decompression, ensuring the low memory is clear for the target OS.

---

## 8. Hardware Init Deep Dive: The PLL
Configuring the PLL (Phase Locked Loop) is the most dangerous part of boot. If you get the multipliers wrong, the CPU overclocks and hangs, or the memory timing desynchronizes.

### 8.1 Example: AS3525 (Sansa Clip) PLL
The AS3525 has complex clock domains (CCU - Clock Control Unit).
*   **Registers:** `CCU_PLLA`, `CCU_PLLB`, `CCU_DB_CLK`.
*   **Sequence:**
    1.  Read the "Fuse" registers to get factory calibration data.
    2.  Calculate `M` and `N` dividers for the desired frequency.
    3.  Write `CCU_PLLA`.
    4.  **Wait for Lock Bit:** The CPU must spin-wait until the PLL stabilizes (`while (!(CCU_STATUS & PLL_LOCK));`).
    5.  Switch the Master Clock Mux from `OSC` to `PLL`.

**Warning:** During step 4, the CPU is running on the slow crystal (e.g., 24MHz). Step 5 jumps it to 200MHz instantly. SDRAM timings must be updated *before* this jump if they depend on clock cycles, or *after* if the controller auto-scales. Rockbox drivers carefully sequence this.

---

## 9. Dual-Boot Protocol: The "Original Firmware" Handoff
Rockbox is designed to be a "good citizen". If the user holds a specific button (usually Left or Hold switch) during boot, the bootloader loads the Original Firmware (OF) instead of Rockbox.

### 9.1 Loading the OF
*   **Flash Targets:** The OF is usually in a separate flash partition. The bootloader reads it into RAM (similar to `rockbox.mi4`) and jumps.
*   **Disk Targets (iPod):** The bootloader reads `apple_os.bin` (a copy of the original firmware saved by the installer) from the hidden partition.

### 9.2 The "Recovery Mode"
If the filesystem is corrupted and `rockbox.mi4` cannot be found, the bootloader enters a failsafe "USB Mode".
*   **Minimal USB Stack:** The bootloader contains a tiny, read-only Mass Storage Class driver.
*   **Execution:** It enumerates as a USB disk, allowing the user to repair the FAT32 partition or re-flash the firmware. This code is entirely separate from the main Rockbox USB stack to ensure reliability.

---

## 10. Appendix: `crt0.S` Instruction-by-Instruction Analysis
To fully demystify the startup, we provide a deeper annotation of the ARM926EJ-S startup code (`firmware/target/arm/crt0.S`).

```assembly
    .global start
start:
    /* 1. Exception Vectors */
    /* The CPU fetches instruction at 0x00. We branch to 'newstart'. */
    b   newstart
    /* Vectors 0x04-0x1C contain branches to handlers */
    ldr pc, [pc, #24]   /* Undefined Instruction */
    ldr pc, [pc, #24]   /* Software Interrupt */
    ldr pc, [pc, #24]   /* Prefetch Abort */
    ldr pc, [pc, #24]   /* Data Abort */
    ldr pc, [pc, #24]   /* Reserved */
    ldr pc, [pc, #24]   /* IRQ */
    ldr pc, [pc, #24]   /* FIQ */

    /* 2. Vector Address Table (Literals) */
    .word   undef_instr_handler
    .word   software_int_handler
    /* ... pointers to C functions ... */

newstart:
    /* 3. Disable Interrupts */
    /* MRS: Move Register from Status (CPSR) to r0 */
    mrs     r0, cpsr
    /* BIC: Bit Clear (Enable Interrupts? No, logic inverted on ARM) */
    /* ORR: Bit Set. 0xC0 sets I and F bits (Disable IRQ/FIQ) */
    orr     r0, r0, #0xc0
    /* MSR: Move Status from Register */
    msr     cpsr_c, r0

    /* 4. Configure Memory Controller (Target Specific) */
    /* On some SoCs, SDRAM isn't mapped yet. We run from SRAM/Flash. */
    /* We must write to Memory Interface registers to enable SDRAM. */
    ldr     r0, =MEM_CTRL_BASE
    ldr     r1, =SDRAM_CONFIG_VAL
    str     r1, [r0, #SDRAM_OFFSET]

    /* 5. Initialize Stack Pointers for different modes */
    /* We need separate stacks for IRQ, FIQ, SVC, and ABT modes. */

    /* Switch to IRQ Mode */
    msr     cpsr_c, #0xd2
    ldr     sp, =irq_stack_top

    /* Switch to FIQ Mode */
    msr     cpsr_c, #0xd1
    ldr     sp, =fiq_stack_top

    /* Switch back to Supervisor (SVC) Mode */
    msr     cpsr_c, #0xd3
    ldr     sp, =svc_stack_top  /* Main C stack */

    /* 6. Copy Data Segment (Flash -> RAM) */
    /* Global initialized variables (int x = 5;) live in Flash. */
    /* We must copy them to RAM so they can be modified. */
    ldr     r0, =_data_start    /* RAM address */
    ldr     r1, =_data_end
    ldr     r2, =_data_load_start /* Flash address */
copy_loop:
    cmp     r0, r1
    ldrlo   r3, [r2], #4
    strlo   r3, [r0], #4
    blo     copy_loop

    /* 7. Clear BSS (as described in Section 2.4) */
    /* ... */

    /* 8. Jump to Main */
    b       main

    /* 9. Catch Return (Should never happen) */
hang:
    b       hang
```

---

## 11. Memory Setup: The MMU and Caches
On ARM9/11 targets, the bootloader configures the MMU (Memory Management Unit) not for virtual memory, but for caching policies.

### 11.1 The Translation Table
The bootloader sets up a "Flat Mapping" (Virtual Address = Physical Address) in a 16KB Translation Table (L1 Page Table).
*   **SDRAM (0x00000000 - 0x02000000):** Cacheable, Bufferable (C=1, B=1). Fast access for code/data.
*   **Peripherals (0x10000000+):** Non-Cacheable, Non-Bufferable (C=0, B=0). Critical for memory-mapped I/O (registers).
*   **LCD Framebuffer:** Often Write-Through or Non-Cacheable to avoid cache flushing artifacts.

### 11.2 Enable Sequence
```assembly
    /* 1. Invalidate Caches */
    mov     r0, #0
    mcr     p15, 0, r0, c7, c7, 0

    /* 2. Set TTB Base Address */
    ldr     r0, =translation_table_base
    mcr     p15, 0, r0, c2, c0, 0

    /* 3. Enable MMU and Caches in Control Register (C1) */
    mrc     p15, 0, r0, c1, c0, 0
    orr     r0, r0, #0x1000     /* Enable I-Cache */
    orr     r0, r0, #0x0004     /* Enable D-Cache */
    orr     r0, r0, #0x0001     /* Enable MMU */
    mcr     p15, 0, r0, c1, c0, 0
```

---

## 12. ColdFire Architecture Specifics (`crt0.S`)
While ARM is dominant, Rockbox's roots are in ColdFire (m68k). The startup code here is different but achieves the same goals.

### 12.1 Vector Table (m68k)
On ColdFire, the vector table is at `0x00000000` but consists of 32-bit pointers, not branch instructions.
```assembly
_vectors:
    .long   _stack_top      /* Initial Stack Pointer (SP) */
    .long   _start          /* Initial Program Counter (PC) */
    .long   _access_error
    .long   _address_error
    .long   _illegal_instruction
    /* ... 256 vectors ... */
```

### 12.2 Module Base Address Register (MBAR)
ColdFire CPUs have a relocatable register map. The bootloader must set the `MBAR` register to a known location (e.g., `0x10000000`) before accessing any peripherals.
```assembly
    move.l  #0x10000001, %d0    /* Base Address + Valid Bit */
    movec   %d0, %mbar          /* Write to MBAR */
```

### 12.3 Chip Selects (CS)
Unlike ARM SoCs which often have a dedicated "Memory Controller", ColdFire uses programmable Chip Selects to map external Flash and RAM to the address space.
*   **CS0:** Boot Flash.
*   **CS1:** SDRAM.
*   **CS2:** HDD/IDE.

The bootloader calculates the mask and base address for each CS and writes them to the `CSAR`, `CSMR`, and `CSCR` registers.

---

## 13. MIPS Architecture Specifics (Ingenic JZ47xx)
The MIPS architecture adds another flavor to `crt0.S`.

### 13.1 The Reset Vector
MIPS processors reset to virtual address `0xBFC00000` (KSEG1, Uncached).
```assembly
    .section .init
    .globl _start
_start:
    /* Disable Interrupts */
    mtc0    $0, $12         /* Status Register */

    /* Initialize Stack */
    la      $sp, _stack_top

    /* Jump to C code */
    la      $t9, main
    jr      $t9
    nop                     /* Branch Delay Slot */
```

### 13.2 Cache Initialization
MIPS requires explicit cache tag initialization loops because the tags are undefined at power-on.
```assembly
    /* Invalidate I-Cache */
    li      $t0, 0x80000000
    li      $t1, 0x80004000 /* 16KB Cache */
1:
    cache   0x8, 0($t0)     /* Index Invalidate */
    addiu   $t0, $t0, 32
    bne     $t0, $t1, 1b
    nop
```

---

## 14. Bootloader Communication: The Argument Block
Bootloaders often need to pass data to the main firmware (e.g., "I booted from disk 0" or "Battery was low").

### 14.1 The Fixed RAM Address
Since arguments cannot be passed on the stack easily (the firmware resets the stack), Rockbox uses a fixed RAM location (often `0x00000020` or similar).

```c
/* firmware/export/boot_args.h */
struct boot_args {
    unsigned long magic;      /* BOOT_MAGIC = 0xROCKB00T */
    unsigned long length;     /* sizeof(struct boot_args) */
    unsigned char rid[32];    /* Random ID (for disk encryption) */
    int boot_volume;          /* 0 or 1 */
    int battery_voltage;      /* Millivolts at boot */
};
```

### 14.2 The Checksum
The bootloader calculates a CRC32 of the argument block and stores it. The firmware verifies this checksum on startup. If it matches, it trusts the data.

---

## 15. The "Panic" State
If hardware initialization fails (e.g., SDRAM test fails), the bootloader cannot load the OS. It enters a "Panic" loop.
1.  **Disable Interrupts:** Ensure no ISRs run.
2.  **LED Blink:** Flash the debug LED (if available) in a specific pattern (SOS).
3.  **Screen Dump:** If the LCD is initialized, write "PANIC: SDRAM FAIL" to the framebuffer.
4.  **Halt:** Execute `while(1);` or `wfi` (Wait For Interrupt).

---

## 16. Appendix: JTAG Debugging in the Bootloader
Debugging the bootloader is notoriously difficult because GDB relies on an OS. We use JTAG.

### 16.1 OpenOCD Configuration
```tcl
# target/rockbox-debug.cfg
source [find interface/ftdi/jtag-lock-pick_tiny_2.cfg]
source [find target/imx233.cfg]

# Halt CPU immediately on reset
reset_config trst_and_srst
init
halt
```

### 16.2 GDB Commands
```bash
(gdb) load build/bootloader.elf
(gdb) break start
(gdb) continue
```
This allows us to step through `crt0.S` line by line and verify that the stack pointer (`sp`) is set correctly before C code runs.

## 17. Conclusion
The bootloader is a masterpiece of constraints. It must be tiny (<100KB), robust (recovery mode), and capable of driving complex filesystems and displays without an OS. Understanding `crt0.S` and the hardware initialization sequence is mandatory for porting Rockbox to any new SoC, including the ESP32, where the "Bootloader" concept shifts to the "Second Stage Bootloader" provided by ESP-IDF.
