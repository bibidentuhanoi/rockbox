# 04_HAL_Storage_FAT_and_Peripherals.md

## 1. The Custom FAT Filesystem Stack

Rockbox does not use a standard library like FatFs. Instead, it implements a highly optimized, custom FAT12/16/32 driver located in `firmware/common/fat.c`. This implementation is designed for:
*   **Low Memory Footprint:** Minimal RAM usage for file handles.
*   **High Performance:** Optimized for sequential reading of large audio files.
*   **DirCaching:** Accelerating folder browsing on slow disks (HDDs).

### 1.1 The BIOS Parameter Block (`struct bpb`)

The driver parses the BPB to understand the volume layout.

```c
struct bpb {
    unsigned long bpb_bytspersec; /* Bytes per sector */
    unsigned long bpb_secperclus; /* Sectors per cluster */
    unsigned long bpb_rsvdseccnt; /* Reserved sectors */
    uint8_t       bpb_numfats;    /* Number of FATs */
    unsigned long bpb_totsec32;   /* Total sectors (FAT32) */
    unsigned long bpb_rootclus;   /* Root directory cluster */

    /* Internal tracking */
    unsigned long fatsize;
    unsigned long firstdatasector;
    unsigned long dataclusters;
    uint8_t       mounted;
};
```

### 1.2 File Handles (`struct fat_file`)

Rockbox uses a lightweight structure to track open files, avoiding the overhead of full POSIX descriptors at the low level.

```c
struct fat_file {
    unsigned long firstcluster; /* Start of the file */
    unsigned long dircluster;   /* Directory containing the entry */
    struct dir_entry_index e;   /* Index in the directory */
    /* Multi-volume support */
    uint8_t volume;
};
```

### 1.3 The Directory Cache (`dircache`)

To avoid spinning up the hard drive just to scroll through a file list, Rockbox implements a **Directory Cache**. This RAM-based cache stores a condensed representation of the directory structure, allowing instant browsing without I/O.

## 2. The Storage Abstraction Layer (`storage.c`)

Beneath the FAT layer lies the Block Device abstraction.

### 2.1 The Interface

The `storage` API provides a unified way to access sectors, regardless of the underlying medium (IDE, SD, MMC).

```c
int storage_read_sectors(int drive, sector_t start, int count, void *buf);
int storage_write_sectors(int drive, sector_t start, int count, const void *buf);
```

### 2.2 Physical Drivers (`firmware/drivers/`)

*   **ATA/IDE (`ata.c`):** For HDD-based players (iPod Video, iRiver H300). Implements PIO and UDMA modes. Handles disk spin-up/spin-down logic to save battery.
*   **SD/MMC (`sd.c`):** For flash-based players (Sansa Clip, SD mods). Implements SPI or native SD bus protocols.

## 3. Peripheral Buses and Interrupts

Rockbox runs on "Bare Metal," meaning it talks directly to hardware registers.

### 3.1 GPIO Abstraction

GPIOs are typically mapped via macros in `target.h` for efficiency.

```c
#define GPIO_POWER_OFF  (1 << 4)
#define POWER_OFF_SET   GPLR |= GPIO_POWER_OFF
```

### 3.2 Interrupt Handling (`system-target.h`)

Interrupts are routed through a Vector Interrupt Controller (VIC) or similar. The system distinguishes between:
*   **IRQ (Interrupt Request):** Standard priority (Buttons, Timers, USB).
*   **FIQ (Fast Interrupt Request):** High priority, used almost exclusively for **PCM DMA** interrupts to prevent audio skipping.

```c
/* Example IRQ Handler Registration */
void int_enable(int irq) {
    VICIntEnable = (1 << irq);
}
```

## 4. The Disk Cache (`disk_cache.c`)

Between the FAT layer and the Storage layer sits the Disk Cache. This is critical for HDD-based players.
*   **Read-Ahead:** When playing audio, Rockbox reads megabytes of data ahead of the playback cursor into the `audiobuf`.
*   **Spin Down:** Once the buffer is full, the HDD is spun down to sleep mode to maximize battery life. The system runs from RAM until the buffer nears empty.
