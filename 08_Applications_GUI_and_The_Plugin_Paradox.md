# 08_Applications_GUI_and_The_Plugin_Paradox.md

## 1. The GUI Framework (`apps/gui/`)
The Graphical User Interface (GUI) is built on top of the HAL display drivers. It manages windows, viewports, bitmaps, fonts, and the main event loop.

### 1.1 The Logical Framebuffer (`struct frame_buffer_t`)
Rockbox abstracts the physical display into a logical framebuffer (`frame_buffer_t`). This allows complex drawing operations (lines, rectangles, bitmaps) to happen in RAM before being pushed to the LCD.

**Source Analysis: `apps/screen_access.c`**
The `screens[]` array holds the state for each physical display (Main LCD + Remote LCD).

```c
struct screen screens[NB_SCREENS] = {
    {
        .screen_type = SCREEN_MAIN,
        .lcdwidth = LCD_WIDTH,
        .lcdheight = LCD_HEIGHT,
        .depth = LCD_DEPTH,
        .pixel_format = LCD_PIXELFORMAT,
        .init_viewport = &lcd_init_viewport,
        .set_viewport = &lcd_set_viewport,
        /* Function pointers for hardware ops */
        .update = &lcd_update,
        .set_contrast = &lcd_set_contrast
    }
};
```

### 1.2 Viewports (`apps/gui/viewport.c`)
Viewports define a rectangular region of the screen where drawing commands are clipped.
*   **Struct:** `struct viewport`
*   **Usage:** The Status Bar is one viewport; the File Browser is another.
*   **Clipping:** Drawing outside the viewport boundaries is ignored.

### 1.3 The Main Event Loop (`apps/action.c`)
The heart of the application logic is a loop that waits for events (buttons, timers, USB insertion) and dispatches them.

```c
/* Main Event Loop (Conceptual) */
void app_main_loop(void)
{
    while (1) {
        /* Wait for Event */
        long action = get_action(CONTEXT_MAINMENU, HZ);

        switch (action) {
            case ACTION_STD_PREV:
                /* Handle Up/Left */
                break;
            case ACTION_STD_NEXT:
                /* Handle Down/Right */
                break;
            case ACTION_WPS_PLAY:
                /* Toggle Play/Pause */
                break;
        }
    }
}
```

---

## 2. The Dynamic Plugin System (`firmware/elf_loader.c`)
Rockbox supports dynamic loading of plugins (`.rock` files) at runtime. This is remarkable because Rockbox runs on bare metal without an MMU (on most targets) or a standard OS loader (like `ld-linux.so`).

### 2.1 Position Independent Code (PIC)
Plugins are compiled as Position Independent Code (`-fPIC`). This means all jumps and data accesses are relative to the Program Counter (PC), allowing the code to run at any address in RAM.

### 2.2 The Plugin API Trampoline (`plugin.h`)
Since plugins cannot link against the main firmware symbols directly (their addresses change with every build), Rockbox uses a massive table of function pointers called the **Plugin API**.
*   **Struct:** `struct plugin_api`
*   **Content:** Pointers to kernel functions (`thread_create`, `lcd_puts`, `open`, `read`, etc.).

**The API Table:**
```c
/* firmware/export/plugin.h */
struct plugin_api {
    /* Threading */
    int (*sleep)(int ticks);
    void (*yield)(void);

    /* LCD */
    void (*lcd_update)(void);
    void (*lcd_puts)(int x, int y, const char *str);

    /* File I/O */
    int (*open)(const char *name, int flags);
    ssize_t (*read)(int fd, void *buf, size_t count);

    /* ... 1000+ more functions ... */
};
```

### 2.3 The ELF Loader Logic
When a user launches a plugin (e.g., `doom.rock`), the firmware:
1.  **Allocates RAM:** Reserves space in `pluginbuf`.
2.  **Parses ELF Header:** Checks magic bytes (`0x7F 'E' 'L' 'F'`).
3.  **Relocates:** Adjusts internal pointers if necessary (though PIC minimizes this).
4.  **Injects API:** Passes the address of `struct plugin_api` to the plugin's entry point.
5.  **Jumps:** Transfers control to `plugin_start()`.

**Source Analysis: `firmware/elf_loader.c`**
```c
/* Load ELF Plugin */
int elf_load(const char* filename, struct plugin_api* api)
{
    int fd = open(filename, O_RDONLY);

    /* Read Header */
    read(fd, &ehdr, sizeof(ehdr));

    /* Validate (ARM, Little Endian, Executable) */
    if (ehdr.e_machine != EM_ARM) return -1;

    /* Load Segments (Text, Data) to Buffer */
    for (i=0; i<ehdr.e_phnum; i++) {
        read(fd, &phdr, sizeof(phdr));
        if (phdr.p_type == PT_LOAD) {
             read_to_ram(fd, plugin_buffer + phdr.p_vaddr, phdr.p_filesz);
        }
    }

    /* Call Entry Point with API Table */
    int (*entry)(struct plugin_api*) = (void*)(plugin_buffer + ehdr.e_entry);
    return entry(api);
}
```

### 2.4 The "Plugin Paradox"
The paradox is that plugins are compiled separately but must act as part of the firmware.
*   **Versioning:** The API struct is versioned. If the firmware API version differs from the plugin's expectation, it refuses to load (to prevent crashing on mismatched function pointers).
*   **Reentrancy:** Plugins run in the main thread (usually). If a plugin crashes, it often takes down the whole OS because there is no memory protection.
