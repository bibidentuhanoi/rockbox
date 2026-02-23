# 08_The_Plugin_Paradox_and_Dynamic_Execution.md

## 1. The Plugin Architecture

Rockbox achieves a "Plugin" system (dynamic loading of code) on bare-metal microcontrollers without an OS-level dynamic linker (ld.so) or virtual memory. It does this through a custom ELF loader and a massive function pointer table.

### 1.1 The `.rock` File

A Rockbox plugin is a standard **ELF (Executable and Linkable Format)** binary, cross-compiled for the target architecture.
*   **Position Independent Code (PIC):** Plugins are compiled with `-fPIC` or `-fPIE` because they are loaded into a variable address in the `pluginbuf`.
*   **Single Entry Point:** The entry point is defined in the ELF header.

### 1.2 The Plugin API Trampoline (`struct plugin_api`)

To allow the plugin to call firmware functions (like `printf` or `lcd_puts`) without knowing their absolute addresses at build time, the firmware passes a pointer to a massive structure of function pointers.

From `apps/plugin.c`:
```c
static const struct plugin_api rockbox_api = {
    rbversion,
    &global_settings,
    lcd_puts,
    lcd_clear_display,
    /* ... hundreds of functions ... */
};
```

When a plugin starts, it receives this pointer. The plugin macros (in `firmware/export/plugin.h`) map function calls to this table.

```c
/* Inside a plugin */
#define rb         (*rb_api)
#define lcd_puts   rb->lcd_puts
```

## 2. The Custom ELF Loader (`firmware/elf_loader.c`)

Rockbox includes a lightweight ELF parser embedded in the kernel.

### 2.1 The Loading Process

1.  **Read Header:** The loader reads the ELF header to verify the architecture (`EM_ARM`, `EM_MIPS`, etc.).
2.  **Memory Allocation:** It allocates space in the `pluginbuf` (the "Plugin/Codec Buffer") for the `PT_LOAD` segments.
3.  **Relocation:** This is the tricky part. Since the code is PIC, the loader performs relocation fixups if the architecture requires it (e.g., GOT/PLT fixups), though often Rockbox relies on the compiler generating PC-relative addressing.
4.  **BSS Clearing:** It zeroes out the uninitialized data section.
5.  **Execution:** It casts the entry point address to a function pointer and calls it.

```c
int plugin_load(const char* plugin, const void* parameter)
{
    /* ... open file ... */
    current_plugin_handle = lc_open(plugin, pluginbuf, PLUGIN_BUFFER_SIZE);

    /* ... get header ... */
    struct plugin_header *p_hdr = lc_get_header(current_plugin_handle);

    /* ... execute ... */
    int rc = p_hdr->entry_point(parameter);
}
```

## 3. Memory Management

Plugins execute in a precarious memory environment.

### 3.1 Buffer Stealing

Plugins reside in the **Audio Buffer**. When a plugin loads, audio playback must stop (usually), or the buffer is partitioned.
*   **TSR (Terminate and Stay Resident):** Some plugins (like "battery benchmark") return but keep code in memory.
*   **Overlays:** Codecs and Plugins share the same physical RAM region. You cannot run a complex plugin and play audio simultaneously if they both demand the full buffer.

### 3.2 The Stack

Plugins typically share the main thread's stack or are given a small dedicated stack within the plugin buffer. Stack overflow in a plugin is fatal and will crash the whole firmware (Panic).

## 4. Architectural Constraints

*   **Harvard Architecture Issues:** On standard ARM7/9, code runs from RAM seamlessly. On architectures with separate Instruction/Data buses or strict cache coherency (like ESP32), loading code into data RAM and executing it requires special handling (IRAM allocation or cache flushing).
*   **Cache Coherency:** After loading the ELF into RAM, the loader must flush the D-Cache and invalidate the I-Cache to ensure the CPU sees the new instructions.
