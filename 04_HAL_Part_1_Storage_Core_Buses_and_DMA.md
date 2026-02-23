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
