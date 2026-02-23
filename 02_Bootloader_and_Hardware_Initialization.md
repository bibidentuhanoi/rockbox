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
