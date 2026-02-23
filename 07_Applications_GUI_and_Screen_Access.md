# 07_Applications_GUI_and_Screen_Access.md

## 1. The Application Layer (`apps/`)

The `apps/` directory contains the high-level logic that the user interacts with. It sits atop the kernel and drivers.

### 1.1 Core Components

*   **`main.c`:** The application entry point. Initializes the GUI, mounts the disk, and enters the event loop.
*   **`playback.c`:** The playback engine controller. Manages the playlist, track skipping, and codec loading.
*   **`tree.c`:** The file browser. Handles directory navigation and database queries.
*   **`menus/`:** Defines the menu structure (Settings, Sound, etc.).

### 1.2 The Event Loop

Rockbox uses a message-queue based event loop (`apps/action.c`).

```c
/* Simplified Main Loop */
while (true) {
    /* Wait for event (Button, Timer, USB insertion) */
    int event = get_event(&data);

    /* Dispatch to the current context */
    switch (global_status.activity) {
        case ACTIVITY_WPS:
            wps_handle_event(event, data);
            break;
        case ACTIVITY_TREE:
            tree_handle_event(event, data);
            break;
        case ACTIVITY_MENU:
            menu_handle_event(event, data);
            break;
    }
}
```

## 2. The GUI Framework (`apps/gui/`)

The Rockbox GUI is a custom toolkit designed for small, low-resolution screens (from 128x64 monochrome to 320x240 color).

### 2.1 Viewports (`viewport.c`)

The screen is divided into **Viewports**. A viewport is a rectangular clipping region.
*   **Status Bar:** Top viewport.
*   **Main Content:** Middle viewport (List or WPS).
*   **Scroll Bar:** Side viewport.

### 2.2 The While Playing Screen (WPS)

The WPS is the most complex screen. It uses a custom script language (`.wps` files) to define the layout of album art, progress bars, and text.
*   **Parser:** `apps/gui/wps_parser.c` parses the skin file.
*   **Engine:** `apps/gui/wps.c` draws the dynamic elements.

### 2.3 Lists and Menus

The `Synclist` (`apps/gui/list.c`) is the universal list widget. It handles:
*   Scrolling logic (kinetic scrolling on some targets).
*   Selection highlighting.
*   Speaking titles (for accessibility).

## 3. Screen Access Abstraction (`screen_access.c`)

Rockbox supports dual screens (Main LCD and Remote LCD). The `screen_access` layer abstracts this.

### 3.1 The Framebuffer

Most targets use a shadow framebuffer in RAM.
*   **Monochrome:** 1 bit per pixel (packed).
*   **Grayscale:** 2 bits per pixel.
*   **Color:** 16-bit RGB565 (most common) or RGB555.

### 3.2 Drawing Primitives (`firmware/drivers/lcd-*.c`)

The low-level drawing routines are highly optimized:
*   `lcd_clear_display()`: Memsets the framebuffer.
*   `lcd_puts()`: Draws text using bitmap fonts (`.fnt`).
*   `lcd_bitmap()`: Blits a bitmap icon.
*   `lcd_update()`: Flushes the shadow framebuffer to the LCD controller via parallel bus or SPI.

### 3.3 Multiple Screen Depths

The build system defines `LCD_DEPTH`.
*   If `LCD_DEPTH == 1`: The code compiles optimized bit-banging routines for monochrome.
*   If `LCD_DEPTH == 16`: It compiles the RGB565 routines.

This compile-time polymorphism ensures no runtime overhead for checking color depth on devices that don't support it.
