# 04_HAL_Part_1_Storage_Core_Buses_and_DMA.md

## Abstract
This document explores the foundational storage and bus abstraction layers. It provides a deep dive into the 10-level stack of the SD/MMC driver, from the kernel's `read_sectors` request down to the physical wire protocol and the PL081 DMA controller's descriptors. It also examines the custom, highly optimized FAT16/32 implementation that bypasses standard libraries for maximum performance on bare-metal targets. The ATA/IDE stack is also analyzed for historical context and its impact on the block device API design.

---

## 1. The Hardware Abstraction Layer (HAL)
Rockbox isolates hardware specifics through a comprehensive HAL in `firmware/target/` and `firmware/drivers/`. This allows the same FAT filesystem code (`fat.c`) to run on an iPod (IDE/ATA), a Sansa (SD/MMC), or an Android phone (Java/JNI).

### 1.1 Device Drivers (Without C++ Classes)
Rockbox uses C structures of function pointers to mimic polymorphism.
**`struct block_driver` (Conceptual):**
```c
struct block_driver {
    int (*init)(void);
    int (*read_sectors)(unsigned long start, int count, void* buf);
    int (*write_sectors)(unsigned long start, int count, const void* buf);
    int (*sleep)(void);
};
```
These are populated at link-time depending on the target macro definitions.

---

## 2. Storage & Block Devices: The SD/MMC Stack (Level 10 Depth)
For flash-based players like the Sansa Clip (AS3525), Rockbox implements a full SD/MMC protocol stack. This section traces a read request from the kernel down to the physical wire signals.

### Level 1: The Kernel Request (`disk.c`)
The filesystem requests a sector read.
```c
/* firmware/common/disk.c */
int storage_read_sectors(int drive, sector_t start, int count, void *buf)
{
    /* ... Cache checks ... */
    return driver->read_sectors(start, count, buf);
}
```

### Level 2: The Driver Logic (`sd-as3525.c`)
The driver converts logical sectors to byte addresses (for SDHC/SDXC compatibility) and sets up the DMA transfer.
```c
/* firmware/target/arm/as3525/sd-as3525.c */
int sd_read_sectors(unsigned long start, int count, void *buf)
{
    /* 1. Setup DMA Channel 0 for Peripheral -> Memory */
    dma_enable_channel(0,
        (void*)&SD_MCI_FIFO, /* Source: MCI FIFO Register */
        buf,                 /* Dest: RAM Buffer */
        DMA_PERI_MCI,        /* Peripheral ID */
        DMAC_FLOWCTRL_PERI_PERI_TO_MEM,
        false, true,         /* Src Fixed, Dst Increment */
        DMA_WIDTH_32, count * 512 / 4, /* Transfer Size */
        &dma_completion_cb   /* Callback */
    );

    /* 2. Send READ_MULTIPLE_BLOCK (CMD18) */
    mci_send_cmd(CMD18, start, RESPONSE_R1);
}
```

### Level 3: The Command State Machine
The AS3525 MCI (Multimedia Card Interface) controller handles the command/response transaction.
```c
void mci_send_cmd(int cmd, int arg, int response_type)
{
    /* 1. Write Argument */
    SD_MCI_ARG = arg;

    /* 2. Write Command & Enable */
    /* Bit 10: Enable CPSM (Command Path State Machine) */
    /* Bit 6:  Response Expected */
    SD_MCI_CMD = cmd | (1 << 10) | (response_type ? (1 << 6) : 0);

    /* 3. Wait for Command Sent (Bit 7 in Status) */
    while (!(SD_MCI_STA & (1 << 7)));
}
```

### Level 4: The Physical Wire Protocol (SD Bus)
When `SD_MCI_CMD` is written, the controller generates the following sequence on the `CMD` pin:
1.  **Start Bit:** 0
2.  **Transmission Bit:** 1
3.  **Command Index:** 6 bits (e.g., 010010 for CMD18)
4.  **Argument:** 32 bits (Sector Address)
5.  **CRC7:** 7 bits
6.  **End Bit:** 1

### Level 5: Data Transfer (4-bit Mode)
After the command is acknowledged by the card:
1.  Card drives `DAT0-DAT3` lines low (Start Bit).
2.  Card transmits 512 bytes of data (plus CRC16 per line).
3.  AS3525 MCI receives data, buffers it in a 64-byte FIFO.
4.  **DMA Request:** When FIFO reaches watermark (e.g., 32 bytes), MCI asserts `DMA_REQ`.

---

## 3. Direct Memory Access (DMA): The PL081 PrimeCell (Level 10 Depth)
The AS3525 uses an ARM PrimeCell PL081 DMA controller. Rockbox must manually configure channels, descriptors, and bus arbitration.

### Level 6: The DMA Register Map (`pl081.h`)
```c
/* firmware/export/pl081.h */
#define DMAC_BASE            0xC6020000
#define DMAC_INT_STATUS      (*(volatile uint32_t*)(DMAC_BASE + 0x00))
#define DMAC_CONFIGURATION   (*(volatile uint32_t*)(DMAC_BASE + 0x30))

/* Channel 0 Registers */
#define DMAC_CH0_SRC_ADDR    (*(volatile uint32_t*)(DMAC_BASE + 0x100))
#define DMAC_CH0_DST_ADDR    (*(volatile uint32_t*)(DMAC_BASE + 0x104))
#define DMAC_CH0_LLI         (*(volatile uint32_t*)(DMAC_BASE + 0x108)) /* Linked List Item */
#define DMAC_CH0_CONTROL     (*(volatile uint32_t*)(DMAC_BASE + 0x10C))
```

### Level 7: The Channel Control Word
The `CONTROL` register is packed with configuration bits for the AHB (Advanced High-performance Bus) transfer.
*   **Bit 31 (I):** Interrupt on terminal count.
*   **Bits 26-27 (DI/SI):** Increment Destination/Source address (1=Inc, 0=Fixed).
*   **Bits 21-23 (DWidth):** 2 = 32-bit word.
*   **Bits 12-14 (SBSize):** Burst Size (e.g., 4 transfers).
*   **Bits 0-11 (TransferSize):** Number of items to transfer (max 4095).

**Configuration Code (`dma-pl081.c`):**
```c
DMAC_CH_CONTROL(0) =
      (1 << 31)        /* TC Interrupt Enable */
    | (1 << 27)        /* Dest Increment */
    | (0 << 26)        /* Source Fixed (FIFO) */
    | (2 << 21)        /* Dest Width: 32-bit */
    | (2 << 18)        /* Src Width: 32-bit */
    | (4 << 15)        /* Dest Burst: 16 items */
    | (4 << 12)        /* Src Burst: 16 items */
    | (size & 0xFFF);  /* Transfer Count */
```

### Level 8: Linked List Items (LLI)
For transfers larger than 4095 items, Rockbox uses Linked List Items. The `LLI` register points to the next set of configuration registers in RAM.
*   **Struct:**
    ```c
    struct dma_lli {
        uint32_t src;
        uint32_t dst;
        uint32_t next_lli; /* Physical address of next struct */
        uint32_t control;
    };
    ```
*   **Logic:** The DMA controller automatically loads the next LLI when the current transfer completes, enabling scatter-gather DMA without CPU intervention.

---

## 4. The Custom FAT Implementation (`firmware/common/fat.c`)
Rockbox bypasses standard libraries for a highly optimized implementation.

### Level 9: The BIOS Parameter Block (BPB)
The `bpb` structure maps the exact on-disk layout of the Boot Sector.
```c
/* firmware/common/fat.c */
static struct bpb
{
    unsigned long bpb_bytspersec; /* Offset 11: Bytes/Sector */
    unsigned long bpb_secperclus; /* Offset 13: Sectors/Cluster */
    unsigned long bpb_rsvdseccnt; /* Offset 14: Reserved Sectors */
    unsigned long bpb_totsec32;   /* Offset 32: Total Sectors (FAT32) */
    unsigned long bpb_rootclus;   /* Offset 44: Root Dir Cluster */
    unsigned long firstdatasector;/* Calculated: Start of Data Area */
} fat_bpbs[NUM_VOLUMES];
```

### Level 10: Cluster Chain Traversal
To read a file, Rockbox must follow the Linked List of clusters in the FAT (File Allocation Table).
*   **FAT32 Entry:** 32-bit integer. Lower 28 bits point to next cluster.
*   **Optimization:** Rockbox caches a window of the FAT to avoid thrashing the SD card (Flash translation layers hate small random reads).

```c
long get_next_cluster(struct bpb *bpb, long cluster)
{
    /* Calculate Sector and Offset in FAT */
    unsigned long fat_offset = cluster * 4;
    unsigned long fat_sector = bpb->rsvdseccnt + (fat_offset / 512);
    unsigned long ent_offset = fat_offset % 512;

    /* Read FAT Sector into Cache */
    unsigned char *cache = fat_get_sector(bpb, fat_sector);

    /* Extract Entry (Little Endian) */
    uint32_t entry = BYTES2INT32(cache, ent_offset);

    /* Check End-of-Chain Mark */
    if (entry >= 0x0FFFFFF8) return -1;

    return entry & 0x0FFFFFFF;
}
```

### Level 11: The Write Path and Wear Leveling
Rockbox does not implement wear leveling. It relies on the SD card's internal controller.
*   **Write Strategy:** Rockbox minimizes writes. `settings.c` only writes the config file when settings change and the user exits a menu.
*   **FAT Update:** Updating a FAT chain involves Read-Modify-Write of the FAT sector.
*   **Safety:** To prevent corruption during power loss, Rockbox updates the directory entry size *after* writing the data clusters.

### LFN Parsing (Long File Names)
Rockbox manually reassembles LFNs from the `0x0F` attribute entries.
```c
union raw_dirent {
    struct {
        uint8_t ldir_ord;       /* Sequence Number */
        uint8_t ldir_name1[10]; /* UCS-2 Characters 1-5 */
        uint8_t ldir_attr;      /* 0x0F */
        uint8_t ldir_type;      /* 0x00 */
        uint8_t ldir_chksum;    /* Checksum of Short Name */
        uint8_t ldir_name2[12]; /* UCS-2 Characters 6-11 */
        /* ... */
    } lfn;
};
```
The driver iterates backward through these entries to build the filename string in a temporary buffer before committing it to the directory cache.

### Level 12: SD/MMC Wire Protocol State Machine
The SD bus is not just a pipe; it's a state machine governed by the Card Status Register (CSR).
*   **Idle (State 0):** Card accepts `CMD0` (Reset).
*   **Ready (State 1):** Card accepts `CMD1` (Init).
*   **Ident (State 2):** Card publishes RCA (Relative Card Address) via `CMD3`.
*   **Stby (State 3):** Card waits for selection (`CMD7`).
*   **Tran (State 4):** Card is selected and ready for data (`CMD17`/`CMD18`).

**The Initialization Dance:**
1.  **Power On:** Supply 3.3V. Wait 1ms.
2.  **Send 74 Clocks:** Send dummy clocks with MOSI high to wake up the card SPI logic.
3.  **CMD0:** Reset to Idle.
4.  **CMD8:** Check voltage range (SD 2.0).
5.  **ACMD41:** Initialize and check OCR (Operation Conditions Register).
6.  **CMD2:** Ask for CID (Card ID).
7.  **CMD3:** Ask for RCA.
8.  **CMD9:** Ask for CSD (Card Specific Data) to calculate capacity.
9.  **CMD7:** Select Card (Move to Transfer State).

### Level 13: Error Handling & CRC
Flash memory is unreliable. The SD protocol includes robust error checking.
*   **Command CRC (CRC7):** Every command packet includes a 7-bit checksum. If the card detects a mismatch, it ignores the command.
*   **Data CRC (CRC16):** Every 512-byte data block is followed by a 16-bit CRC per data line.
*   **Handling:**
    *   If `SD_MCI_STA` reports `CRC_FAIL`, the driver must reset the controller and retry the command.
    *   After 3 retries, Rockbox marks the sector as bad or the card as ejected.

### Level 14: Hot-Swap & Card Detection
Portable players have removable storage. Rockbox handles this via GPIO interrupts.
*   **Card Detect (CD) Pin:** Usually a mechanical switch in the SD slot, pulled high, grounded when card inserted.
*   **ISR Logic:**
    ```c
    /* firmware/target/arm/as3525/sd-as3525.c */
    void sd_cd_isr(void) {
        if (gpio_get(SD_CD_PIN) == 0) {
            /* Card Inserted */
            queue_post(&disk_queue, DISK_INSERTED);
        } else {
            /* Card Removed */
            /* PANIC: Stop all DMA immediately to prevent bus hang */
            mci_stop();
            queue_post(&disk_queue, DISK_REMOVED);
        }
    }
    ```

---

## 5. ATA/IDE Stack: The Legacy Giant
Before SD cards, Rockbox ran on 1.8" Hard Drives via the ATA protocol. This is relevant because the code still exists and defines the block API structure.

### 5.1 Register Block (Memory Mapped)
*   **Command Register (`0x1F7`):** Write `0x20` for READ_SECTORS.
*   **Data Register (`0x1F0`):** 16-bit wide FIFO.
*   **Sector Count (`0x1F2`):** Number of sectors to transfer.

### 5.2 The PIO Mode (Programmed I/O)
Without DMA, the CPU must poll the status register and manually copy data.
```c
/* firmware/drivers/ata.c */
void ata_read_sector_pio(uint16_t *buf) {
    /* Wait for DRQ (Data Request) */
    while (!(ATA_STATUS & ATA_SR_DRQ));

    /* Unroll loop for speed */
    for (int i=0; i<256; i+=8) {
        buf[i+0] = ATA_DATA;
        buf[i+1] = ATA_DATA;
        /* ... x8 unroll ... */
        buf[i+7] = ATA_DATA;
    }
}
```
**Impact:** During PIO, the CPU is 100% busy transferring data. Audio decoding must rely on the large `audiobuf` cushion.

### 5.3 UDMA Mode (Ultra DMA)
Later iPods (5G/Video) supported UDMA.
*   **Mechanism:** The ATA controller takes over the bus and transfers data at 66MB/s.
*   **CRC:** UDMA introduces CRC protection for data transfers (unlike PIO).
*   **Signaling:** Uses `DMARQ` and `DMACK` lines for handshaking.

---

## 6. Bus Arbitration and Locking
Rockbox is multi-threaded. What happens if the `audio_thread` wants to read music and the `gui_thread` wants to save settings?

### 6.1 The Storage Mutex
A global mutex protects the storage driver.
```c
/* firmware/common/disk.c */
void storage_read_sectors(...) {
    mutex_lock(&storage_mtx);
    driver->read_sectors(...);
    mutex_unlock(&storage_mtx);
}
```

### 6.2 Bus Locking (SPI)
For shared buses (e.g., LCD and SD card on same SPI), Rockbox uses `spi_lock()`.
*   **Priority Inversion:** If a low-priority thread holds the SPI lock for the LCD, the high-priority audio thread (fetching data from SD) must wait. Rockbox mitigates this by keeping LCD updates short and yield-able.

---

## 7. The Custom Directory Cache (`dircache.c`)
FAT directory traversal is slow (linked list of clusters). Rockbox implements a massive RAM cache for the directory structure.

### 7.1 The Cache Structure
*   **`struct dircache_entry`:** 12 bytes per file.
    *   `name_hash`: 32-bit hash of the filename.
    *   `sector`: Starting sector of the file.
    *   `parent`: Index of the parent directory.
*   **Building:** Scans the entire disk at boot (or background).
*   **Benefit:** Browsing files is instant. No disk access required until a file is opened.

### 7.2 Memory Usage
The dircache can consume MBs of RAM. It lives in the "Audio Buffer" until music playback starts. When playback begins, the dircache is compacted or partially discarded to make room for audio data.

---

## 8. Multi-Driver Storage (Dual Boot & Dual Card)
Rockbox supports targets with multiple storage mediums (e.g., iRiver H120 with Internal HDD + CF Card).

### 8.1 The Drive Map
```c
/* firmware/export/config.h */
#define DRIVE0_TYPE     DRIVE_ATA
#define DRIVE1_TYPE     DRIVE_MMC
```

### 8.2 The Dispatcher (`disk.c`)
The dispatcher routes calls based on the drive index.
```c
int storage_read_sectors(int drive, ...) {
    if (drive == 0)
        return ata_driver.read_sectors(...);
    else
        return mmc_driver.read_sectors(...);
}
```

---

## 9. Sector Caching Strategies (`disk_cache.c`)
Rockbox employs a unified LRU cache for disk sectors to reduce I/O ops.

### 9.1 The Cache Line
```c
struct cache_entry {
    sector_t sector;
    int drive;
    bool dirty;
    bool locked; /* Cannot be evicted */
    uint8_t data[512];
};
```

### 9.2 Write-Back Logic
Writes are cached in RAM (`dirty = true`) and only flushed to disk when:
1.  The cache is full and the entry is evicted.
2.  `storage_flush()` is called explicitly (e.g., before shutdown).
3.  The disk is about to spin down (to prevent spin-up just for a small write).

---

## 10. Serial Protocols: UART and Debugging
Rockbox uses UART primarily for kernel debugging.

### 10.1 The Serial Driver (`serial.c`)
*   **FIFO:** Hardware FIFOs (usually 16 bytes) are enabled.
*   **Interrupts:** RX interrupt enabled. TX interrupt enabled only when buffer has data.
*   **Baud Rate:** Calculated based on the system clock `PCLK`.
    ```c
    /* Divisor calculation for 115200 */
    int div = PCLK / (16 * 115200);
    UART_IBRD = div;
    UART_FBRD = ((PCLK % (16 * 115200)) * 64 + ...);
    ```

### 10.2 The Debug Menu
Rockbox has a hidden debug menu accessible by holding specific keys.
*   **`debug_menu()`:** Displays raw memory, I2C registers, and thread stacks on the LCD.
*   **Integration:** This menu bypasses the high-level GUI and draws directly to the framebuffer for safety during crashes.

---

## 11. I2C Bus Architecture (`i2c-as3525.c`)
I2C is the control plane for peripherals (PMIC, Codec, Tuner).

### 11.1 Master Mode Implementation
The AS3525 I2C controller is complex. Rockbox implements a blocking driver with timeouts.
1.  **Start Condition:** Set `I2C_CTR_START`.
2.  **Address:** Write Slave Address to `I2C_TXR`.
3.  **Wait:** Poll `I2C_SR` for ACK or NACK.
4.  **Data:** Write bytes to `I2C_TXR`.
5.  **Stop:** Set `I2C_CTR_STOP`.

### 11.2 Error Recovery
If a slave holds SDA low (bus hang), Rockbox attempts to toggle SCL manually (bit-banging) to clock out the stuck bit and free the bus.

### 11.3 Bit-Banging I2C
Many targets lack a hardware I2C controller or use GPIOs for I2C.
*   **`i2c-bitbang.c`:** Implements software I2C.
*   **Timing:** Uses `udelay()` to ensure setup/hold times.
*   **Flexibility:** Can run on any two GPIO pins.

---

## 12. SPI Bus Architecture (`spi.c`)
SPI is used for high-speed peripherals like LCDs and Flash chips.

### 12.1 The Shared Bus Problem
Often, the LCD and the Flash memory share the same SPI bus (MOSI/MISO/SCK), distinguished only by the Chip Select (CS) pin.
*   **Problem:** If the LCD driver writes to the bus while the Flash driver is reading, data corruption occurs.
*   **Solution:** `spi_lock()` mutex ensures atomic transactions.

### 12.2 Hardware SPI vs Bit-Bang
*   **Hardware:** Uses the SoC's SPI controller (FIFO, DMA). Used for SD cards.
*   **Bit-Bang:** Used for write-only LCDs where speed is less critical or pins are non-standard.

---

## 13. USB Mass Storage (The Target Side)
Rockbox also acts as a USB Device (Mass Storage Class).

### 13.1 The SCSI Transparent Command Set
The USB driver implements a subset of SCSI commands.
*   **READ(10) / WRITE(10):** Translates logical block addresses to physical sectors.
*   **INQUIRY:** Returns "Rockbox Media Player".

### 13.2 The Bridge
When USB is connected:
1.  Rockbox unmounts the filesystem (to prevent corruption).
2.  It enters `usb_screen()`.
3.  The USB thread loops, receiving SCSI packets and calling `storage_read_sectors()`.
4.  The Host PC (Windows/Linux) sees a raw block device.

### 13.3 The USB Stack Layers
The USB stack (`firmware/usb/`) is layered:
1.  **USB HAL (`usb-drv-*.c`):** Handles Endpoint interrupts and register access.
2.  **USB Core (`usb_core.c`):** Handles Standard Requests (GET_DESCRIPTOR, SET_ADDRESS).
3.  **USB Class (`usb_storage.c`):** Handles Bulk-Only Transport (BOT) and SCSI.

---

## 14. Conclusion
Rockbox's storage stack is a lesson in bare-metal efficiency. It avoids the overhead of generic OS block layers, implementing just enough protocol logic to read sectors fast. The explicit management of DMA descriptors and FIFO watermarks allows it to sustain high throughput with minimal CPU usage, essential for battery life. For the ESP32 port, we will map this entire stack to `esp_vfs_fat`, but understanding the underlying mechanics is crucial for performance tuning.

## 15. Appendix: ATA Register Details
For completeness, here is a detailed breakdown of the ATA registers used in the `ata.c` driver.

| Register | Read Function | Write Function |
| :--- | :--- | :--- |
| `0x1F0` | Data Register | Data Register |
| `0x1F1` | Error Register | Feature Register |
| `0x1F2` | Sector Count | Sector Count |
| `0x1F3` | Sector Number (LBA 0-7) | Sector Number (LBA 0-7) |
| `0x1F4` | Cylinder Low (LBA 8-15) | Cylinder Low (LBA 8-15) |
| `0x1F5` | Cylinder High (LBA 16-23) | Cylinder High (LBA 16-23) |
| `0x1F6` | Drive/Head (LBA 24-27) | Drive/Head (LBA 24-27) |
| `0x1F7` | Status Register | Command Register |

### 15.1 The Status Register Bits
*   **Bit 7 (BSY):** Busy. Drive is executing a command.
*   **Bit 6 (DRDY):** Drive Ready.
*   **Bit 3 (DRQ):** Data Request. Ready to transfer data.
*   **Bit 0 (ERR):** Error. Check Error Register.

---

## 16. The PL081 DMA Linked List Structure
The Linked List Items (LLI) are the key to scatter-gather DMA.

### 16.1 LLI Definition
```c
typedef struct {
    uint32_t src_addr;
    uint32_t dst_addr;
    uint32_t next_lli;
    uint32_t control;
} dma_lli_t;
```

### 16.2 Chaining Logic
To transfer a 100KB file into non-contiguous RAM buffers:
1.  Rockbox allocates an array of LLIs in RAM.
2.  **LLI[0].src_addr** = `&MCI_FIFO`.
3.  **LLI[0].dst_addr** = `Buffer_A`.
4.  **LLI[0].next_lli** = `&LLI[1]`.
5.  **LLI[1].src_addr** = `&MCI_FIFO`.
6.  **LLI[1].dst_addr** = `Buffer_B`.
7.  **LLI[1].next_lli** = `0` (End of chain).
8.  The driver writes `&LLI[0]` to `DMAC_CH0_LLI` and enables the channel.
