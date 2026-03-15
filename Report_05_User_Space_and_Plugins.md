# Report 05: User Space and Plugins

Rockbox separates its core operating system logic from its user interface and applications. The "user space" resides in the `apps/` directory and interacts with the firmware via message queues and an explicit API.

## The Entry Point (`apps/main.c`)

The system life-cycle and user space entry point are defined in `apps/main.c`.

After the bootloader and RTOS initialization, execution passes to `int main(void)` (around line 173). This monolithic initialization function:
1. Calls `system_init()` to set up interrupts, DMA, and the bare-metal environment.
2. Initializes the storage sub-systems (`storage_init()`, `disk_init()`, `fat_init()`).
3. Sets up the GUI layers (`font_init()`, `lcd_init()`, `viewport_init()`).
4. Seeds the memory subsystem (`buflib_init()`, `core_allocator_init()`).
5. After all core tasks are running, it launches the main application interface by calling `app_main()`.

## The UI Event Loop (`apps/action.c`)

The core event loop for Rockbox applications is built around the `get_action()` function located in `apps/action.c`.

1. **Hardware Driver (`firmware/drivers/button.c`)**: A tick-based state machine runs at a hardware interrupt/timer level, scanning physical GPIO matrix states to detect button presses, releases, and holds.
2. **Message Queue (`firmware/drivers/button_queue.c`)**: Validated button events are posted to a kernel queue using `button_queue_post()`.
3. **Action Mapping (`apps/action.c`)**: The `get_action(int context, int timeout)` function calls `button_get()` to pull raw hardware button IDs off the queue.
   * `get_action_worker()` takes the raw button ID (e.g., `BUTTON_PLAY`) and looks up the current UI context (e.g., `CONTEXT_MAINMENU` vs `CONTEXT_WPS`).
   * It translates the hardware button into a semantic "Action" (e.g., `ACTION_STD_OK` or `ACTION_WPS_PLAY`).
   * This indirection allows standard application logic (like scrolling menus) to work on dozens of devices with completely different physical button layouts.

## The Plugin Interface (`apps/plugin.h`)

Rockbox supports third-party applications and games (like Doom, Sudoku, and metadata viewers) compiled as dynamically loaded plugins. Because plugins are built separately and loaded at runtime into RAM via `buflib`, they cannot be statically linked against firmware functions.

To solve this, Rockbox utilizes a massive jump table defined in `apps/plugin.h`.

```c
struct plugin_api {
    /* ... 1000+ function pointers ... */

    /* Memory */
    int (*buflib_alloc)(struct buflib_context *ctx, size_t size);

    /* UI / Display */
    void (*lcd_update)(void);
    void (*lcd_puts)(int x, int y, const char *string);

    /* System */
    void (*yield)(void);
    long (*button_get)(bool block);

    /* Storage */
    int (*open)(const char *pathname, int flags);
};
```

When a plugin starts, the core OS populates `struct plugin_api` with pointers to the real kernel, HAL, and application functions, and passes this struct to the plugin's entry point (`plugin_start`).

### Terminate-and-Stay-Resident (TSR) Plugins
Plugins can run in the background. By returning `PLUGIN_TSR_CONTINUE` from their main function and hooking into system events (like audio playback callbacks via the `plugin_api`), plugins can draw over the UI or process audio asynchronously, demonstrating the deep coupling possible via this explicit struct-based dependency injection.