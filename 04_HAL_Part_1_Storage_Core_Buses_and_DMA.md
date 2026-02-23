# 04_HAL_Part_1_Storage_Core_Buses_and_DMA.md

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

## 2. Storage & Block Devices (`firmware/drivers/ata.c`)
For HDD-based players (iPod Classic, iRiver H300), Rockbox implements a full ATA/IDE driver.

**Source Analysis: `firmware/drivers/ata.c`**
The driver manages the ATA command set directly via MMIO registers.

```c
#define CMD_READ_SECTORS           0x20
#define CMD_WRITE_SECTORS          0x30
#define CMD_IDENTIFY               0xEC
#define STATUS_BSY                 0x80
#define STATUS_RDY                 0x40

static int ata_perform_wakeup(int state)
{
    /* 1. Wait for BSY to clear */
    if (wait_for_bsy())
        return -1;

    /* 2. Issue Soft Reset */
    ATA_OUT8(ATA_CONTROL, CONTROL_SRST);
    sleep(HZ/20); /* Wait 50ms */
    ATA_OUT8(ATA_CONTROL, 0);

    /* 3. Wait for RDY */
    if (wait_for_rdy())
        return -1;

    return 0;
}
```

### 2.1 PIO vs. DMA
*   **PIO (Programmed I/O):** The CPU reads data byte-by-byte from the ATA data register. Slow, CPU intensive. Used for small transfers (partition table, boot sector).
*   **DMA (Direct Memory Access):** The ATA controller writes directly to SDRAM. Rockbox uses this for bulk audio reads (`audiobuf`) to maximize throughput and allow the CPU to sleep or decode simultaneously.
    *   **Struct:** `struct ata_dma_descriptor` (Hardware specific).
    *   **Interrupt:** `ATA_IRQ` fires on transfer complete.

---

## 3. The Custom FAT Implementation (`firmware/common/fat.c`)
Rockbox bypasses standard libraries (like FatFs) for a highly optimized, RAM-efficient implementation tailored for read-mostly media workloads.

**Source Analysis: `firmware/common/fat.c`**
The core structure is the `bpb` (BIOS Parameter Block), which represents a mounted volume.

```c
static struct bpb
{
    unsigned long bpb_bytspersec; /* Bytes per sector (512) */
    unsigned long bpb_secperclus; /* Sectors per cluster */
    unsigned long bpb_rsvdseccnt; /* Reserved sectors */
    uint8_t       bpb_numfats;    /* Number of FATs */
    unsigned long bpb_rootclus;   /* Root directory cluster */

    /* FAT32 Specifics */
    unsigned long bpb_fatsz32;    /* Size of FAT in sectors */
    unsigned long bpb_fsinfo;     /* FSInfo sector */

    /* Runtime State */
    unsigned long fatsize;        /* Cached FAT size */
    unsigned long totalsectors;   /* Total volume sectors */
    unsigned long firstdatasector;/* Start of data region */
    uint8_t mounted;              /* Is mounted? */
} fat_bpbs[NUM_VOLUMES];
```

### 3.1 FAT Cache & Optimization
*   **FAT Cache:** Rockbox caches a small window of the FAT table in RAM to avoid seeking back to the FAT area constantly.
*   **Cluster Chaining:** It reads the FAT table linearly to determine contiguous cluster runs, optimizing DMA transfers.
*   **Directory Parsing:** Optimized for fast scrolling. It reads directory entries in bulk and parses filenames on the fly.

### 3.2 Long File Names (LFN)
Rockbox supports VFAT LFNs. It parses the multiple 32-byte directory entries required for long names, reassembling the UCS-2 (Unicode) characters into UTF-8 for display.

---

## 4. Peripheral Buses
Rockbox drivers manually bit-bang or use hardware controllers for I2C and SPI.

### 4.1 I2C (Inter-Integrated Circuit)
Used for:
*   **PMIC:** Power management (battery level, charging).
*   **Audio Codec:** DAC configuration (volume, sample rate).
*   **FM Tuner:** Radio control.

**Driver:** `firmware/drivers/i2c.c`
```c
int i2c_write(int bus, int addr, const unsigned char* buf, int count)
{
    /* 1. Send START condition */
    i2c_start(bus);

    /* 2. Send Address + Write Bit */
    if (i2c_send_byte(bus, (addr << 1) | 0) < 0)
        goto error;

    /* 3. Send Data Bytes */
    for (int i = 0; i < count; i++)
        if (i2c_send_byte(bus, buf[i]) < 0)
            goto error;

    /* 4. Send STOP condition */
    i2c_stop(bus);
    return 0;
}
```

### 4.2 SPI (Serial Peripheral Interface)
Used for:
*   **LCD:** Command/Data transfer to display controllers.
*   **Flash:** SPI NOR Flash (bootloader storage).
*   **SD Card:** In SPI mode (fallback if SDIO fails).

**Optimization:**
Rockbox often uses **Async SPI with DMA** for LCD updates to avoid stalling the UI thread while pushing pixels.

---

## 5. Direct Memory Access (DMA)
DMA is the lifeblood of Rockbox.
*   **Audio DMA:** `pcm_play_dma_start()` sets up a circular buffer transfer from SDRAM to the I2S FIFO.
*   **Storage DMA:** ATA/SD reads fill the `audiobuf` directly.
*   **Memcpy DMA:** Some targets (like PP502x) use a "memcpy engine" to move large memory blocks faster than the CPU loop.

**Sync Logic:**
DMA transfers are asynchronous. Drivers use semaphores (`semaphore_wait()`) to block the calling thread until the DMA completion interrupt fires (`dma_interrupt_handler()`), which posts to the semaphore.
