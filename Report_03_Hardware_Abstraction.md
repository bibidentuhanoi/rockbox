# Report 03: Hardware Abstraction Layer

Rockbox abstracts storage hardware behind a unified File System interface, allowing applications to read/write files without needing to understand whether the underlying storage is an ATA hard drive, an SD card, eMMC, or a RAM disk.

## Storage Hierarchy and HAL Mapping

The HAL layers for storage are organized as follows:

1.  **Application Layer:** Calls standard POSIX-like functions: `open()`, `read()`, `write()`, `close()`.
2.  **File Abstraction (`firmware/common/file.c`)**: Manages file descriptors, caching, and dispatches requests to the appropriate file system driver (e.g., FAT32).
3.  **File System Layer (`firmware/common/fat.c`)**: Translates logical file offsets into physical disk sectors via the File Allocation Table.
4.  **Disk Layer (`firmware/common/disk.c`)**: Manages partitions and logical drives.
5.  **Storage Abstraction (`firmware/storage.c` / `firmware/export/storage.h`)**: The HAL boundary (`storage_read_sectors()`).
6.  **Hardware Drivers**: `ata.c`, `sd.c`, `mmc.c`, etc.

## Tracing a Storage Read Request

When the application requests to read data from a file, the execution path maps through the abstraction layers:

1.  **`read(int fd, void *buf, size_t count)`** in `firmware/common/file.c`:
    *   Validates the file descriptor `fd`.
    *   Calculates the required disk sectors based on the current file pointer and `count`.
    *   Dispatches the request down to the FAT subsystem.

2.  **`fat_readwrite()`** in `firmware/common/fat.c` (called via the caching subsystem or directly for large reads):
    *   Consults the FAT table for the specific file (`struct fat_file`) to determine the cluster and block addresses.
    *   Issues requests to the disk abstraction layer.

3.  **`storage_read_sectors(sector_t start, int count, void* buf)`** in `firmware/storage.c`:
    *   This is the concrete HAL router for block devices.
    *   It inspects the target drive type and routes the request to the specific hardware driver.

    ```c
    int storage_read_sectors(IF_MD(int drive,) sector_t start, int count, void* buf)
    {
        /* ... */
        if (type == STORAGE_ATA)
            return ata_read_sectors(IF_MD(ldrive,) start,count,buf);
        if (type == STORAGE_MMC)
            return mmc_read_sectors(IF_MD(ldrive,) start,count,buf);
        if (type == STORAGE_SD)
            return sd_read_sectors(IF_MD(ldrive,) start,count,buf);
        /* ... */
        return STORAGE_FUNCTION(read_sectors)(IF_MD(drive,)start,count,buf);
    }
    ```

4.  **Hardware Driver (e.g., `ata_read_sectors()` or `sd_read_sectors()`)**:
    *   These drivers (`firmware/drivers/ata.c`, `firmware/target/.../sdmmc.c`) configure the actual hardware controller (e.g., PL081 DMA, or bit-banging GPIO) to transfer the requested sectors directly from the disk bus into the provided `buf` memory location.

This structured approach allows the `apps/` layer to remain entirely unaware of the physical storage media, utilizing the exact same `open()` and `read()` API on an iPod Video (ATA HDD) as it does on a Sansa Clip+ (MicroSD).