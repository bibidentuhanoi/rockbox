#ifndef _FILESYSTEM_ESP32_H_
#define _FILESYSTEM_ESP32_H_

#if defined(PLUGIN) || defined(CODEC)
#define FILEFUNCTIONS_DECLARED
#define FILEFUNCTIONS_DEFINED
#define DIRFUNCTIONS_DECLARED
#define DIRFUNCTIONS_DEFINED
#define OSFUNCTIONS_DECLARED
#endif /* PLUGIN || CODEC */

#ifndef OSFUNCTIONS_DECLARED
#define FS_PREFIX(_x_) esp32_ ## _x_

void paths_init(void);
#endif /* !OSFUNCTIONS_DECLARED */

#endif /* _FILESYSTEM_ESP32_H_ */

#ifdef _FILE_H_
#include <unistd.h>
#ifndef _FILESYSTEM_ESP32__FILE_H_
#define _FILESYSTEM_ESP32__FILE_H_

#ifdef RB_FILESYSTEM_OS
#define FILEFUNCTIONS_DEFINED
#endif

#ifndef FILEFUNCTIONS_DECLARED
#define __OPEN_MODE_ARG
#define __CREAT_MODE_ARG \
    , mode

#include <time.h>

int     esp32_open(const char *name, int oflag, ...);
int     esp32_creat(const char *name, mode_t mode);
int     esp32_close(int fildes);
int     esp32_ftruncate(int fildes, off_t length);
int     esp32_fsync(int fildes);
ssize_t esp32_read(int fildes, void *buf, size_t nbyte);
ssize_t esp32_write(int fildes, const void *buf, size_t nbyte);
off_t   esp32_lseek(int fildes, off_t offset, int whence);
int     esp32_remove(const char *path);
int     esp32_rename(const char *old, const char *new);
int     esp32_modtime(const char *path, time_t modtime);
off_t   esp32_filesize(int fildes);
int     esp32_fsamefile(int fildes1, int fildes2);
int     esp32_relate(const char *path1, const char *path2);
bool    esp32_file_exists(const char *path);
ssize_t esp32_readlink(const char *path, char *buf, size_t bufsiz);
#endif /* !FILEFUNCTIONS_DECLARED */

#endif /* _FILESYSTEM_ESP32__FILE_H_ */
#endif /* _FILE_H_ */

#ifdef _DIR_H_
#ifndef _FILESYSTEM_ESP32__DIR_H_
#define _FILESYSTEM_ESP32__DIR_H_

#include <dirent.h>

#define DIRENT dirent
#define DIRENT_DEFINED

struct dirinfo_native {
    unsigned int attr;
    off_t        size;
    uint16_t     wrtdate;
    uint16_t     wrttime;
};

#ifndef DIRFUNCTIONS_DECLARED
#define __MKDIR_MODE_ARG \
    , 0777

#ifdef RB_FILESYSTEM_OS
#define DIRFUNCTIONS_DEFINED
#endif

DIR           *esp32_opendir(const char *dirname);
struct dirent *esp32_readdir(DIR *dirp);
int            esp32_readdir_r(DIR *dirp, struct dirent *entry, struct dirent **result);
void           esp32_rewinddir(DIR *dirp);
int            esp32_closedir(DIR *dirp);
int            esp32_mkdir(const char *path);
int            esp32_rmdir(const char *path);
int            esp32_samedir(DIR *dirp1, DIR *dirp2);
bool           esp32_dir_exists(const char *dirname);
#endif /* !DIRFUNCTIONS_DECLARED */

#endif /* _FILESYSTEM_ESP32__DIR_H_ */
#endif /* _DIR_H_ */
