/*
 * Filesystem stubs for ESP32 — delegates to POSIX layer provided by ESP-IDF
 * (VFS + FATFS on SD card).  Real implementations go here after VFS is wired.
 */
#define RB_FILESYSTEM_OS
#include "config.h"
#include "debug.h"
#include "filesystem-esp32.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <stdbool.h>
#include "sdkconfig.h"

#include "dir.h"    /* struct dirinfo, ATTR_DIRECTORY */

#ifndef ATTR_DIRECTORY
#define ATTR_DIRECTORY 0x10
#endif

#ifndef MAX_PATH
#define MAX_PATH 260
#endif

#ifndef MAX_OPEN_DIRS
#define MAX_OPEN_DIRS 8
#endif
static struct {
    DIR *dirp;
    char path[MAX_PATH];
} open_dirs[MAX_OPEN_DIRS];

static int open_dirs_find(DIR *dirp)
{
    for (int i = 0; i < MAX_OPEN_DIRS; i++)
        if (open_dirs[i].dirp == dirp)
            return i;
    return -1;
}

static int open_dirs_alloc(void)
{
    for (int i = 0; i < MAX_OPEN_DIRS; i++)
        if (open_dirs[i].dirp == NULL)
            return i;
    return -1;
}

void paths_init(void)
{
    /* stub — mount points are set up by ESP-IDF VFS */
}

/* ---- File ops — thin wrappers around POSIX ---- */

int esp32_open(const char *name, int oflag, ...)
{
    return open(name, oflag, 0666);
}

int esp32_creat(const char *name, mode_t mode)
{
    return open(name, O_CREAT | O_WRONLY | O_TRUNC, mode);
}

int esp32_close(int fildes)       { return close(fildes); }
int esp32_ftruncate(int fildes, off_t length) { return ftruncate(fildes, length); }
int esp32_fsync(int fildes)       { return fsync(fildes); }

ssize_t esp32_read(int fildes, void *buf, size_t nbyte)
{ return read(fildes, buf, nbyte); }

ssize_t esp32_write(int fildes, const void *buf, size_t nbyte)
{ return write(fildes, buf, nbyte); }

off_t esp32_lseek(int fildes, off_t offset, int whence)
{ return lseek(fildes, offset, whence); }

int esp32_remove(const char *path) { return remove(path); }
int esp32_rename(const char *old, const char *newp) { return rename(old, newp); }

int esp32_modtime(const char *path, time_t modtime)
{
    (void)path; (void)modtime;
    return 0; /* stub */
}

off_t esp32_filesize(int fildes)
{
    struct stat st;
    if (fstat(fildes, &st) < 0) return -1;
    return st.st_size;
}

int esp32_fsamefile(int fildes1, int fildes2)
{
    struct stat s1, s2;
    if (fstat(fildes1, &s1) < 0 || fstat(fildes2, &s2) < 0) return -1;
    return (s1.st_ino == s2.st_ino && s1.st_dev == s2.st_dev) ? 1 : 0;
}

int esp32_relate(const char *path1, const char *path2)
{
    return strcmp(path1, path2) == 0 ? 1 : 0;
}

bool esp32_file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

ssize_t esp32_readlink(const char *path, char *buf, size_t bufsiz)
{
    (void)path; (void)buf; (void)bufsiz;
    return -1; /* no symlinks on FAT */
}

/* ---- Directory ops ---- */

DIR *esp32_opendir(const char *dirname)
{
    /* ESP-IDF VFS can't list "/", redirect to our only mount point */
    if (dirname && strcmp(dirname, "/") == 0)
        dirname = TREE_ROOT;

    DIR *dirp = opendir(dirname);
    if (!dirp)
        DEBUGF("FS: esp32_opendir FAILED: '%s' errno=%d\n", dirname, errno);
    if (dirp)
    {
        int slot = open_dirs_alloc();
        if (slot >= 0)
        {
            open_dirs[slot].dirp = dirp;
            strncpy(open_dirs[slot].path, dirname, MAX_PATH - 1);
            open_dirs[slot].path[MAX_PATH - 1] = '\0';
        }
    }
    return dirp;
}
struct dirent *esp32_readdir(DIR *dirp)   { return readdir(dirp); }

int esp32_readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result)
{ return readdir_r(dirp, entry, result); }

void esp32_rewinddir(DIR *dirp)  { rewinddir(dirp); }
int esp32_closedir(DIR *dirp)
{
    int slot = open_dirs_find(dirp);
    if (slot >= 0)
    {
        open_dirs[slot].dirp = NULL;
        open_dirs[slot].path[0] = '\0';
    }
    return closedir(dirp);
}

/* HTOLS: speex byte-swap macro — on ESP32 (BYTES_PER_CHAR==1) it's a no-op.
 * Provided as a function so rockbox.esp32 can resolve it via the symbol table
 * when the speex bits.o is stale and has an unresolved HTOLS reference. */
int HTOLS(int a) { return a; }

int esp32_mkdir(const char *path)
{
    return mkdir(path, 0777);
}

int esp32_rmdir(const char *path)  { return rmdir(path); }

int esp32_samedir(DIR *dirp1, DIR *dirp2)
{
    (void)dirp1; (void)dirp2;
    return 0;
}

bool esp32_dir_exists(const char *dirname)
{
    struct stat st;
    return stat(dirname, &st) == 0 && S_ISDIR(st.st_mode);
}

struct dirinfo dir_get_info(DIR *dirp, struct dirent *entry)
{
    struct dirinfo info = { .attribute = 0, .size = 0, .mtime = 0 };

    char fullpath[MAX_PATH * 2];
    int slot = open_dirs_find(dirp);

    if (slot >= 0)
        snprintf(fullpath, sizeof(fullpath), "%s/%s",
                 open_dirs[slot].path, entry->d_name);
    else
    {
        strncpy(fullpath, entry->d_name, sizeof(fullpath) - 1);
        fullpath[sizeof(fullpath) - 1] = '\0';
    }

    struct stat st;
    if (stat(fullpath, &st) == 0)
    {
        info.attribute = S_ISDIR(st.st_mode) ? ATTR_DIRECTORY : 0;
        info.size = st.st_size;
        info.mtime = st.st_mtime;
    }

    return info;
}

const char *esp32_root_realpath(void)
{
    return CONFIG_ROCKBOX_MOUNT_POINT;
}

bool os_file_exists(const char *ospath)
{
    struct stat st;
    return stat(ospath, &st) == 0;
}

/* ---- Volume / storage info ---- */
#include "mv.h"    /* sector_t, IF_MV */
#include <sys/statvfs.h>

void volume_size(IF_MV(int volume,) sector_t *sizep, sector_t *freep)
{
    IF_MV((void)volume;)
    struct statvfs fs;
    if (statvfs(CONFIG_ROCKBOX_MOUNT_POINT, &fs) == 0) {
        if (sizep) *sizep = (sector_t)(fs.f_blocks * (fs.f_frsize / 512));
        if (freep) *freep = (sector_t)(fs.f_bfree  * (fs.f_frsize / 512));
    } else {
        if (sizep) *sizep = 0;
        if (freep) *freep = 0;
    }
}
