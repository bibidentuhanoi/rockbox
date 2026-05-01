/*
 * Codec/plugin loading for ESP32 via Espressif ELF Loader.
 * Uses dlopen/dlsym/dlclose from the elf_loader component.
 */
#include "config.h"
#include "debug.h"
#include "load_code.h"
#include "esp_dlfcn.h"
#include <string.h>

extern void plugin_release_buffer_for_codec(void);
extern void plugin_reclaim_buffer_after_codec(void);


void *lc_open(const char *filename, unsigned char *buf, size_t buf_size)
{
    (void)buf;
    (void)buf_size;

    if (!filename) return NULL;

    /* esp_elf_open() prepends "/" to the name, so strip leading slash
       to avoid double-slash paths (e.g. "//rockbox/...") */
    const char *path = filename;
    if (path[0] == '/')
        path++;

    plugin_release_buffer_for_codec();

    void *handle = dlopen(path, RTLD_NOW);

    plugin_reclaim_buffer_after_codec();

    if (!handle) {
        DEBUGF("LC: dlopen failed: %s\n", dlerror());
        return NULL;
    }

    return handle;
}

void *lc_get_header(void *handle)
{
    if (!handle) return NULL;

    void *hdr = dlsym(handle, "__header");
    if (!hdr)
        hdr = dlsym(handle, "___header");

    if (!hdr)
        DEBUGF("LC: lc_get_header: __header symbol not found\n");

    return hdr;
}

void lc_close(void *handle)
{
    if (handle)
        dlclose(handle);
}
