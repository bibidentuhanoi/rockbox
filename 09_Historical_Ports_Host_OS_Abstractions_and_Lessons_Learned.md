# 09_Historical_Ports_Host_OS_Abstractions_and_Lessons_Learned.md

## Abstract
This document analyzes how Rockbox evolved from a bare-metal firmware into a "Hosted" guest OS that can run atop Linux, Android, and Windows. It examines the critical abstraction layers in `firmware/target/hosted/` that map Rockbox's cooperative scheduler to POSIX pthreads (`thread-sdl.c`), emulate the framebuffer in an SDL window, and route audio through host OS APIs (ALSA/AudioTrack). These historical lessons provide the blueprint for the ESP32 port.

## 1. The "Hosted" Architecture Paradigm
Rockbox was originally designed for bare-metal systems (sh-1, ColdFire). However, the developers soon realized the value of running Rockbox *inside* another operating system for development, simulation, and later, porting to Android.

### 1.1 `firmware/target/hosted/`
This directory is the "Grand Unifier." It allows Rockbox to compile against standard POSIX or Windows APIs instead of hardware registers.
*   **Threads:** Mapped to OS threads (pthreads, Win32 Threads).
*   **Display:** Mapped to a window (SDL, X11, Android Surface).
*   **Audio:** Mapped to an audio API (ALSA, PulseAudio, OpenSL ES).
*   **Storage:** Mapped to a directory on the host filesystem (`.rockbox/`).

### 1.2 The "Guest OS" Illusion
When running as an application (e.g., on Android), Rockbox still *thinks* it is the OS. It manages its own "threads" (cooperative scheduler simulation), "interrupts" (timers), and "hardware" (virtualized). This is the key to porting to ESP32: we must maintain this illusion while running as a FreeRTOS task.

---

## 2. The SDL / UISimulator Port (`uisimulator/`)
The `uisimulator` is the primary development tool. It compiles the full firmware but links it against SDL (Simple DirectMedia Layer). It is a perfect case study in how to wrap a cooperative kernel inside a preemptive OS.

### 2.1 Simulating the Scheduler (`firmware/target/hosted/sdl/thread-sdl.c`)
Standard OS threads (pthreads) are preemptive and scheduled by the kernel (Linux/Windows). Rockbox threads expect to run cooperatively (mostly) and share global state without aggressive locking. To solve this, the hosted port implements a **Global Scheduler Lock**.

**The Global Mutex (`m`)**
Rockbox uses a single global mutex (`m`) to ensure that *only one Rockbox thread executes at a time*. This mimics the single-core behavior of the target hardware.

```c
/* firmware/target/hosted/sdl/thread-sdl.c */
static SDL_mutex *m;
static jmp_buf thread_jmpbufs[MAXTHREADS];

/*
 * The 'create_thread' function spawns a real OS thread, but immediately
 * blocks it on a semaphore until the scheduler allows it to run.
 */
int runthread(void *data)
{
    /* 1. Acquire Global Lock immediately */
    SDL_LockMutex(m);

    struct thread_entry *current = (struct thread_entry *)data;
    __running_self_entry() = current;

    /* 2. Setup longjmp point for thread_exit */
    if (setjmp(*current_jmpbuf) == 0)
    {
        /* 3. Run the actual thread function */
        current->context.start();
        thread_exit();
    }

    SDL_UnlockMutex(m);
    return 0;
}
```

### 2.2 The Context Switch
When Rockbox calls `switch_thread()` (usually via `yield()` or `sleep()`), the hosted port doesn't save CPU registers (the OS does that). Instead, it manipulates the Global Lock.

```c
void switch_thread(void)
{
    struct thread_entry *current = __running_self_entry();

    /* 1. Release the Global Lock */
    /* This allows another waiting OS thread to acquire it and run */
    SDL_UnlockMutex(m);

    /* 2. Block Self */
    /* Wait on our private semaphore until someone wakes us */
    SDL_SemWait(current->context.s);

    /* 3. Re-acquire Global Lock */
    /* We are now running again, and we own the "CPU" */
    SDL_LockMutex(m);
}
```
**Lesson for ESP32:** We don't need this heavy locking on FreeRTOS if we map priorities correctly, but we *do* need to respect that Rockbox code assumes no preemption from other Rockbox threads unless it yields.

---

## 3. The Android Port Deep-Dive (`android/`)
The Android port (`org.rockbox`) is a JNI (Java Native Interface) wrapper around the `hosted` target. It demonstrates how to interface Rockbox with a managed runtime.

### 3.1 `pcm-android.c`: The Audio Pump
In a bare-metal target, Rockbox feeds a DMA buffer via interrupts. In Android, the flow is inverted: Android requests data via a callback.

**The Write Callback:**
This function is called from Java (via JNI) when the `AudioTrack` needs data.

```c
/* firmware/target/hosted/android/pcm-android.c */
JNIEXPORT jint JNICALL
Java_org_rockbox_RockboxPCM_nativeWrite(JNIEnv *env, jobject this,
                                        jbyteArray temp_array, jint max_size)
{
    lock_audio();

    /* 1. Check if we have data in the "DMA" buffer */
    if (!pcm_data_size) {
        /* Ask Rockbox for more data (Simulation of DMA Interrupt) */
        new_buffer = pcm_play_dma_complete_callback(PCM_DMAST_OK,
                            &pcm_data_start, &pcm_data_size);
    }

    /* 2. Copy data to Java array */
    (*env)->SetByteArrayRegion(env, temp_array, 0, transfer_size, pcm_data_start);

    /* 3. Notify status */
    if (new_buffer)
        pcm_play_dma_status_callback(PCM_DMAST_STARTED);

    unlock_audio();
    return bytes_written;
}
```
**Inversion of Control:**
*   **Bare Metal:** ISR fires -> `pcm_dma_complete_callback` called.
*   **Android:** Java thread calls C -> `nativeWrite` calls `pcm_dma_complete_callback`.
*   **ESP32:** We will likely use a dedicated FreeRTOS task to feed the I2S DMA, acting similarly to the Android thread.

### 3.2 JNI Bridge and Lifecycle
The `RockboxActivity.java` manages the lifecycle.
*   **Init:** Loads `librockbox.so`.
*   **Main:** Calls `main()` in a separate thread.
*   **Events:** Touch events are passed to C via `sendTouchEvent(x, y, action)`.

```c
/* firmware/target/hosted/android/button-android.c */
void android_button_event(int button, int data)
{
    /* Post to Rockbox queue */
    queue_post(&button_queue, button, data);
}
```

---

## 4. Linux/Posix Port (`linux/`)
The Linux port runs as a user-space process.

### 4.1 Signal Handling as Interrupts
To simulate the 100Hz kernel tick, the hosted port uses POSIX signals.
*   **Setup:** `setitimer(ITIMER_REAL, ...)` configures a periodic timer.
*   **Handler:** `SIGALRM` handler increments `current_tick` and calls `call_callouts()`.

```c
/* firmware/target/hosted/linux/system-linux.c */
void signal_handler(int sig)
{
    if (sig == SIGALRM) {
        current_tick++;
        /* Handle timers, poll buttons */
        poll_input();
    }
}
```
**Risk:** Signal handlers are asynchronous and limited in what they can do (async-signal-safe). Rockbox's handler often does too much, which can cause deadlocks on Linux if not careful.

### 4.2 Memory Mapping
Rockbox expects contiguous physical RAM. Hosted ports simulate this by `malloc`ing a huge block (e.g., 32MB) at startup and giving it to `core_alloc.c` to manage.
```c
/* firmware/target/hosted/system-hosted.c */
void system_init(void)
{
    /* Allocate the "Physical RAM" */
    static char main_ram[MEMORYSIZE * 1024 * 1024];

    /* Tell core_alloc where it is */
    core_allocator_init(main_ram, sizeof(main_ram));
}
```

---

## 5. Comparative Architecture Matrix
This table summarizes how different subsystems are abstracted across targets.

| Feature | Bare Metal (ARM) | Hosted (SDL/Linux) | Android (JNI) | Linux Native (Maemo) | ESP32 (Proposed) |
| :--- | :--- | :--- | :--- | :--- | :--- |
| **Scheduler** | ASM Context Switch | Pthreads + Global Lock | Pthreads + Global Lock | Pthreads + Global Lock | FreeRTOS Tasks |
| **Tick Source** | Hardware Timer IRQ | `SIGALRM` / `SDL_Delay` | `Thread.sleep()` | `SIGALRM` / `timerfd` | FreeRTOS Tick |
| **Display** | LCD Controller Registers | SDL Window / X11 | Android Surface / Bitmap | `/dev/fb0` `mmap` | SPI DMA (esp_lcd) |
| **Audio** | I2S DMA Interrupts | ALSA / PulseAudio | `AudioTrack.write()` | ALSA Async Callback | I2S DMA (driver) |
| **Storage** | ATA/SD Registers | POSIX `fopen`/`fread` | POSIX (via VFS) | POSIX `open`/`read` | POSIX (via VFS) |
| **Input** | GPIO Scanning | Keyboard/Mouse Events | Touch Events | `/dev/input/eventX` | GPIO ISR / ADC |
| **Panic** | Screen Dump + loop | `abort()` / `gdb` | Logcat stacktrace | Terminal dump | ESP Panic / Backtrace |

---

## 6. Golden Maxims for ESP32 Porting
From the analysis of these historical ports, we derive the following "Golden Maxims" for the ESP32 port.

### 6.1 "Don't Fight the Host OS"
The Android port succeeded because it didn't try to access audio hardware directly; it used `AudioTrack`.
*   **ESP32 Application:** Do not bang I2S registers manually. Use the ESP-IDF `driver/i2s` API. It handles the DMA interrupts and ring buffers for us, acting like the Android AudioTrack.

### 6.2 "The Big Malloc"
All hosted ports allocate a single large chunk of memory for Rockbox's internal allocator.
*   **ESP32 Application:** We must allocate the 4MB/8MB PSRAM chunk immediately at boot and hand it to `core_alloc`. Attempting to use system `malloc` for small Rockbox objects is inefficient and fragmenting.

### 6.3 "The Event Loop Bridge"
Hosted ports translate OS events (Keypress, Touch) into Rockbox Queue events.
*   **ESP32 Application:** We need a FreeRTOS task (or ISR) that monitors GPIOs/Touch and simply pushes `BUTTON_HOME` or `BUTTON_POWER` into the `button_queue`. Do not put logic in the ISR.

### 6.4 "Filesystem Transparency"
The Hosted logic uses standard `open/read` calls.
*   **ESP32 Application:** By mounting the SD card via ESP-IDF's VFS at `/sdcard`, we can reuse 99% of Rockbox's file handling code (`firmware/common/file.c`) without modification, as long as we shim `open()` to prepend the mount point.

### 6.5 "Cooperative Simulation"
We do not need the heavy "Global Lock" of the SDL port because ESP32 has 2 cores and FreeRTOS handles preemption gracefully. However, we must ensure that the **Main Thread** (Core 1) is not starved by high-priority interrupts (WiFi on Core 0) or the Audio Feeder task.

---

## 7. Deep Dive: The `uisimulator` Input Stack
The input handling in the simulator differs significantly from the hardware. It uses the host OS event loop to pump messages.

### 7.1 SDL Event Loop
The `sim_do_exit` function loop in `bootloader/sim_main.c` (or equivalent) handles this.
```c
void gui_input_loop(void)
{
    SDL_Event event;
    while(SDL_WaitEvent(&event)) {
        switch(event.type) {
            case SDL_KEYDOWN:
                handle_key(event.key.keysym.sym, true);
                break;
            case SDL_KEYUP:
                handle_key(event.key.keysym.sym, false);
                break;
            case SDL_MOUSEBUTTONDOWN:
                /* Simulate Touch */
                button_post(BUTTON_TOUCHSCREEN);
                break;
        }
    }
}
```
**Mechanism:**
1.  **Mapping:** `handle_key` maps PC keys (Arrow Left) to Rockbox keys (`BUTTON_LEFT`).
2.  **Posting:** It calls `queue_post` to send the event to the kernel thread.
3.  **No Polling:** Unlike `button.c` which polls GPIOs, this is purely event-driven.

---

## 8. Deep Dive: Android Storage Scoping
Android versions > 10 restrict file access. Rockbox on Android had to adapt.

### 8.1 The Storage Abstraction Layer
Rockbox uses `storage.c` to abstract block devices. On Android, it doesn't use `storage.c` for reading music files directly; it uses the file system.
*   **Issue:** Android's Storage Access Framework (SAF) returns `content://` URIs, not paths.
*   **Workaround:** Rockbox typically asks for "All Files Access" permission or targets legacy storage to get `/sdcard/Music` paths.
*   **ESP32 Parallel:** We face a similar issue with VFS. We don't have block access to the SD card (unless we unmount it), so we must rely on the FATFS middleware entirely.

---

## 9. The Maemo D-Bus Integration
On Nokia N900 (Maemo), Rockbox integrates with the Linux desktop bus (D-Bus).
*   **Purpose:** Allows the media keys on the lock screen to control Rockbox.
*   **Implementation:** A separate thread listens on the D-Bus socket. When `org.rockbox.pause` is received, it posts `BUTTON_PLAY | BUTTON_REL` to the Rockbox queue.
*   **ESP32 Parallel:** This is analogous to handling **Bluetooth AVRCP** (Audio Video Remote Control Profile). The ESP32 Bluetooth stack receives a "Pause" command and must inject a button press into the Rockbox queue.

---

## 10. Hosted Graphics: Scaling and Rotation
The simulator can run at the native resolution (e.g., 128x64) or scaled up.
*   **Logic:** The framebuffer is kept at native resolution.
*   **Blitting:** `lcd-sdl.c` performs a nearest-neighbor scaling when copying the framebuffer to the SDL Texture.
*   **Rotation:** Rockbox supports screen rotation in software (`lcd_set_drawmode`). On Hosted ports, rotation can be handled by the host (SDL) or by Rockbox. Usually, Rockbox handles it to test the internal rotation algorithms.

```c
/* apps/gui/lcd-sdl.c */
void lcd_update(void) {
    /* Copy framebuffer to SDL surface */
    for(y = 0; y < LCD_HEIGHT; y++) {
        for(x = 0; x < LCD_WIDTH; x++) {
            pixel = framebuffer[y][x];
            sdl_pixel = map_color(pixel);
            /* Scale 1x pixel to 2x2 on screen */
            draw_rect(x*2, y*2, 2, 2, sdl_pixel);
        }
    }
    SDL_Flip(screen);
}
```
**ESP32 Application:** We will use `esp_lcd`'s hardware rotation capabilities if available, or Rockbox's software rotation if the display controller is dumb.

## 11. Code Dump: `thread-sdl.c` Analysis
This section analyzes the verbatim code of the hosted thread implementation to understand the locking strategy deeply.

```c
/* firmware/target/hosted/sdl/thread-sdl.c */

/*
 * This creates a new thread in the Rockbox scheduler.
 * Note that it creates an SDL thread AND a semaphore.
 */
unsigned int create_thread(void (*function)(void), ...) {
    /* Allocate the Rockbox internal struct */
    struct thread_entry *thread = thread_alloc();

    /* Create a binary semaphore, initialized to 0 (Blocked) */
    SDL_sem *s = SDL_CreateSemaphore(0);

    /* Spawn the OS thread */
    SDL_Thread *t = SDL_CreateThread(runthread, thread);

    /* Store the handles */
    thread->context.s = s;
    thread->context.t = t;

    return thread->id;
}
```

**Key Takeaway:** The Semaphore `s` is the "Gatekeeper". Even though the thread is created by the OS immediately, `runthread` (see Section 2.1) calls `SDL_LockMutex(m)` immediately. If the Main Thread holds `m`, the new thread blocks instantly. This preserves the single-core illusion. It only runs when the Main Thread (or current thread) calls `switch_thread` and releases `m`.

## 12. Audio Latency in Hosted Ports
One of the biggest challenges in hosted ports is audio latency.
*   **Rockbox Native:** Buffer -> DAC (< 10ms latency).
*   **SDL (Win32):** Buffer -> SDL -> DirectSound -> Driver -> DAC (> 50ms latency).
*   **Android:** Buffer -> AudioTrack -> AudioFlinger -> HAL -> DAC (> 80ms latency).

**Impact on UI:**
Rockbox's spectrum analyzer and VU meters read from the *current playback position* in the decoding buffer. On native hardware, this matches the sound in your ears. On hosted ports, the visualizer is often 100ms *ahead* of the sound because the sound is stuck in the OS buffer.
*   **Mitigation:** Hosted ports implement a "Latency Correction" delay in the visualizer drawing code to delay the FFT visualization by the estimated audio path latency.
*   **ESP32 Strategy:** We must measure the I2S DMA buffer depth exactly and report it to `pcm_get_buffer_count()` so the visualizers stay synced.

---

## 13. The Architecture Web (Inter-File Connections)
This section explicitly maps how the "Hosted" abstractions relate to the architectural components analyzed in Files 1-8.

### 13.1 Scheduler Emulation (`thread-sdl.c` <-> File 3)
*   **Concept:** File 3 (`03_OS_Core_Kernel...`) detailed `struct thread_entry` and the cooperative `switch_thread` assembly logic.
*   **Connection:** `thread-sdl.c` replaces the ASM context switch with `SDL_LockMutex(m)`. It reuses the exact same `struct thread_entry` defined in `firmware/kernel/thread-internal.h`, but replaces the CPU registers (`r0-r15`) in `context` with OS handles (`pthread_t`, `sem_t`).

### 13.2 HAL Mapping (`lcd-linuxfb.c` <-> File 5)
*   **Concept:** File 5 (`05_HAL_Part_2...`) detailed the `lcd_update()` function driving physical controller pins (DBOP/SPI).
*   **Connection:** `lcd-linuxfb.c` implements the exact same `lcd_update()` API signature. However, instead of writing to `0xC8120000` (DBOP_BASE), it `memcpy`s the buffer to the pointer returned by `mmap("/dev/fb0")`. The upper layers of Rockbox (UI, Plugins) are unaware of this difference.

### 13.3 Audio Pumping (`pcm-alsa.c` <-> File 7)
*   **Concept:** File 7 (`07_The_Audio_Pipeline...`) detailed the I2S DMA interrupt logic.
*   **Connection:** `pcm-alsa.c` replaces the hardware ISR with an ALSA callback (`snd_async_handler_t`).
    *   **File 7 Flow:** Codec -> `audiobuf` -> `pcmbuf` -> DMA (ISR triggers next chunk).
    *   **Hosted Flow:** Codec -> `audiobuf` -> `pcmbuf` -> ALSA (Callback triggers next chunk).
    *   **Key Insight:** The `pcmbuf` ring buffer logic remains identical. The "Pump" mechanism changes from a Hardware Interrupt to a Software Callback.

### 13.4 Plugin Linking (`elf_loader.c` <-> File 8)
*   **Concept:** File 8 (`08_Applications_GUI...`) described dynamic loading of `.rock` ELF files.
*   **Connection:** On many hosted platforms (especially Windows), dynamic loading of ELF files into a running executable is blocked by Data Execution Prevention (DEP) or OS architecture.
    *   **Strategy:** Hosted builds often disable plugins or compile them as shared libraries (`.so`/`.dll`).
    *   **Simulator:** The simulator compiles plugins as native shared objects and uses `dlopen()` instead of the custom `elf_loader`. This proves that the plugin logic is modular enough to survive the transition to ESP32's static linking model (File 10).

---

## 14. Cross-Architecture Data Flow Diagram
This ASCII diagram visualizes the flow of a single MP3 frame from Disk to Speaker across the three major architectures analyzed.

```text
STEP          BARE METAL (ARM)        HOSTED (ANDROID)       ESP32 (PROPOSED)
----          ----------------        ----------------       ----------------
1. Read       ATA Driver (PIO/DMA)    Java InputStream       ESP-IDF VFS (FATFS)
              |                       |                      |
              v                       v                      v
2. Buffer     SDRAM `audiobuf`        malloc() heap          PSRAM `audiobuf`
              |                       |                      |
              v                       v                      v
3. Decode     MAD (Fixed Point)       MAD (Fixed Point)      MAD (Fixed Point)
              |                       |                      |
              v                       v                      v
4. PCM Ring   SDRAM `pcmbuf`          malloc() `pcmbuf`      PSRAM/SRAM `pcmbuf`
              |                       |                      |
              v                       v                      v
5. Transfer   DMA Controller          JNI Array Copy         I2S DMA Controller
              (Interrupt Driven)      (Threaded Copy)        (Interrupt Driven)
              |                       |                      |
              v                       v                      v
6. Output     I2S -> DAC Chip         AudioFlinger -> HAL    I2S -> DAC Chip
              |                       |                      |
              v                       v                      v
7. Sound      Headphones              Speaker/Bluetooth      Headphones
```

**Key Insight:** The middle steps (Buffer, Decode, PCM Ring) are identical across all three. The divergence only happens at the edges (Step 1 and Step 5). This confirms that porting Rockbox to ESP32 is primarily a driver-level task (Files 4, 5, 7), preserving the kernel core (File 3) and application logic (Files 6, 8).

---

## 15. Deep Comparative Analysis: Hosted vs Bare Metal
This section deconstructs how `apps/` code remains agnostic despite the massive underlying differences.

### 15.1 Threading: `thread.c` vs `thread-sdl.c`
*   **The Agnostic View:** `apps/main.c` calls `create_thread(..., "audio")`. It expects a thread ID back.
*   **Bare Metal (`thread.c`):**
    1.  Allocates `struct thread_entry` from static array `__threads`.
    2.  Sets `context.pc` to function pointer.
    3.  Modifies the run queue `rtr`.
    4.  Scheduler (IRQ driven) picks it up.
*   **Hosted (`thread-sdl.c`):**
    1.  Allocates `struct thread_entry` (same struct!).
    2.  Calls `SDL_CreateThread()`.
    3.  New OS thread immediately hits `SDL_LockMutex(m)` and sleeps.
    4.  Scheduler (Mutex driven) unlocks it.
*   **Result:** `apps/` sees threads running. It doesn't know one is a hardware context switch and the other is a Pthread wake-up.

### 15.2 Kernel: `kernel.c` vs `system-hosted.c`
*   **The Agnostic View:** `apps/` calls `sleep(HZ)`.
*   **Bare Metal:**
    1.  Thread added to `tmo` queue.
    2.  `switch_thread()` called.
    3.  Hardware Timer IRQ decrements tick count.
    4.  When 0, thread moved to `rtr` queue.
*   **Hosted:**
    1.  `sleep()` calls `SDL_Delay()`.
    2.  OS puts the thread to sleep.
    3.  OS Scheduler wakes it up.
*   **Conflict:** Rockbox assumes `sleep(1)` is exactly 1 tick (10ms). OS `sleep()` is "at least" X ms. Hosted ports use `gettimeofday()` to drift-correct the ticks.

### 15.3 HAL: `lcd-16bit.c` vs `lcd-sdl.c`
*   **The Agnostic View:** `apps/gui/` writes `0xF800` (Red) to `framebuffer[0][0]`.
*   **Bare Metal:**
    1.  Memory write to SDRAM.
    2.  `lcd_update()` sends SPI command to controller to read that RAM.
*   **Hosted:**
    1.  Memory write to `malloc`'d array.
    2.  `lcd_update()` copies array to `SDL_Surface`.
    3.  `SDL_Flip()` presents it.
*   **Result:** The "Logical Framebuffer" is the key interface contract.

### 15.4 UI: `button.c` vs `button-sdl.c`
*   **The Agnostic View:** `apps/action.c` calls `get_action()`, which waits on `button_queue`.
*   **Bare Metal:**
    1.  Timer IRQ -> `button_tick()` -> `button_read()` (GPIO).
    2.  Debounce logic runs.
    3.  `queue_post()` to `button_queue`.
*   **Hosted:**
    1.  SDL Event Loop (Main Thread) captures KeyDown.
    2.  Maps KeySym to Rockbox Button ID.
    3.  `queue_post()` to `button_queue`.
*   **Result:** The application receives `BUTTON_HOME` regardless of whether it came from a 3.3V GPIO pull-down or a USB Keyboard scan code.

---

## 16. The `apps/` Interface Contract
The `apps/` directory is strictly forbidden from accessing hardware directly. It MUST use the APIs defined in `firmware/export/`.

### 16.1 The "Export" Boundary
*   `firmware/export/button.h`: Defines `BUTTON_HOME`, `button_get_w_tmo()`.
*   `firmware/export/lcd.h`: Defines `LCD_WIDTH`, `lcd_update()`.
*   `firmware/export/pcm.h`: Defines `pcm_play_data()`.
*   `firmware/export/storage.h`: Defines `storage_read_sectors()`.

### 16.2 Violation Consequences
If an App file includes `#include "as3525.h"`:
1.  **Compilation Error:** On Hosted builds (Simulator), that file doesn't exist.
2.  **Portability Failure:** The feature won't work on iPods.
3.  **Review Rejection:** Rockbox code review strictly polices this boundary.

**Conclusion:** This rigid interface contract is why Rockbox is portable. The ESP32 port simply needs to implement the `firmware/export` API contract, and the 500,000+ lines of code in `apps/` will function immediately.

---

## 17. API Adaptation Layer Analysis
This section explicitly maps how historical ports rewrote or extended the core APIs to function in their environments.

### 17.1 The Hosted Plugin API Shim
The simulator must run plugins that were compiled for the host architecture (x86), not the target.
*   **The Shim:** `apps/plugins/bitmaps/plugin_bitmap.c` is replaced by a host-native version.
*   **API Table:** The `plugin_api` table is populated with pointers to functions in the simulator executable (`rockboxui.exe`).
*   **Loading:** Instead of parsing ELF headers, the simulator uses `dlopen()` on `.dll` or `.so` files which are compiled from the plugin source during the build process.

### 17.2 The Android Audio Adaptation
Android 4.0+ requires a strict buffer size for `AudioTrack`.
*   **Constraint:** Rockbox prefers arbitrary buffer sizes (e.g., 4096 samples).
*   **Adaptation:** The `pcm-android.c` layer implements a **Double Buffer**.
    1.  Rockbox writes small chunks to a ring buffer.
    2.  The JNI callback waits until `minBufferSize` is available.
    3.  A burst write is sent to Android.
    4.  **Result:** Rockbox sees a continuous DMA, Android sees burst writes.

### 17.3 The Linux Input Subsystem Adaptation
Linux provides input via `/dev/input/eventX`.
*   **The Adaptation:** `firmware/target/hosted/linux/button-linux.c` opens these device nodes.
*   **Translation:** It uses an `ioctl(EVIOCGKEY)` to read the keymap.
*   **Mapping:** `KEY_ENTER` -> `BUTTON_SELECT`, `KEY_ESC` -> `BUTTON_HOME`.
*   **Event Injection:** It pushes these translated events into the Rockbox `button_queue`.

### 17.4 The SDL Framebuffer Emulation
Rockbox assumes a simple array of pixels. SDL assumes a "Surface" or "Texture".
*   **The Emulation:** `lcd-sdl.c` allocates a `uint16_t` array for Rockbox.
*   **The Blit:** On `lcd_update()`, it iterates over this array.
*   **Pixel Format Conversion:** Rockbox uses RGB565. SDL might use RGB888. The loop performs bit-shifting:
    ```c
    r = (pixel >> 11) & 0x1F;
    g = (pixel >> 5) & 0x3F;
    b = pixel & 0x1F;
    sdl_pixel = SDL_MapRGB(fmt, r<<3, g<<2, b<<3);
    ```
*   **Optimization:** This conversion happens only on "dirty" rectangles to maintain 60 FPS on the simulator.

---

## 18. The PalmOS Port Analysis
One of the earliest "Hosted" attempts was for PalmOS 5 devices (Tungsten T3, Tapwave Zodiac). This port is architecturally significant because it bridged two CPU architectures.

### 18.1 The "PACE" Emulator
PalmOS 5 runs on ARM processors but executes legacy 68k applications via the PACE (Palm Application Compatibility Environment) emulator.
*   **The Problem:** Rockbox needed to run native ARM code for speed, but interface with 68k OS calls.
*   **The Solution:** The "PNO" (Palm Native Object) format. Rockbox was compiled as a small 68k launcher that loaded a massive ARM binary blob.

### 18.2 The "Armlet" Architecture
The core firmware ran as an "Armlet" (native ARM subroutine).
*   **Context Switch:** When Rockbox needed to call a PalmOS API (e.g., `SndPlayResource`), it had to exit the ARM context, return to 68k mode, make the syscall, and then re-enter ARM mode.
*   **Relevance to ESP32:** This is eerily similar to the interaction between the ESP32 ULP (Ultra Low Power) coprocessor and the main CPU. We can apply the "Armlet" pattern to offload button scanning to the ULP while the main cores sleep.

---

## 19. Windows Hosted Port: Simulating Interrupts
On Windows, Rockbox uses multimedia timers (`timeSetEvent`) to simulate the 100Hz hardware tick.

### 19.1 The Win32 Event Loop
The Windows messaging loop (`GetMessage`/`DispatchMessage`) replaces the `while(1)` loop in `kernel.c`.
```c
/* firmware/target/hosted/win32/system-win32.c */
void CALLBACK TickTimerProc(UINT uID, UINT uMsg, DWORD_PTR dwUser, ...)
{
    /* Post a message to the main thread to run the tick */
    PostMessage(hwnd, WM_ROCKBOX_TICK, 0, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ROCKBOX_TICK) {
        current_tick++;
        /* Run Rockbox scheduler */
        call_callouts();
    }
}
```
**Constraint:** Windows message queues are low priority. If the window is being dragged, `WM_TIMER` messages get delayed.
**Fix:** The port creates a separate high-priority thread just for the tick timer to ensure audio stability, using `SetThreadPriority(THREAD_PRIORITY_TIME_CRITICAL)`.

---

## 20. The "UiSimulator" Input Stack
The simulator allows developers to click buttons on a JPEG skin of the device.

### 20.1 Skin Parsing (`apps/gui/skin_engine/`)
The simulator parses a `.bmp` file and a `.fms` (FaceMap Script) file.
*   **Hit Testing:** When `SDL_MOUSEBUTTONDOWN` occurs, the code iterates through defined "Touch Regions" in the skin file.
*   **Mapping:** If the click coordinates match a region defined as `BUTTON_HOME`, that keycode is injected.

### 20.2 Key Injection Logic
```c
/* bootloader/simulator_main.c */
void button_post(int button) {
    /* 1. Lock Queue */
    mutex_lock(&button_queue_lock);

    /* 2. Add to Ring Buffer */
    if (queue_count < MAX_QUEUE) {
        queue[head] = button;
        head = (head + 1) % MAX_QUEUE;
        queue_count++;
    }

    /* 3. Wake Kernel */
    semaphore_release(&button_queue_wait);
    mutex_unlock(&button_queue_lock);
}
```
This demonstrates that the input subsystem is entirely decoupled from the hardware driver. As long as *something* calls `button_post`, Rockbox works.

---

## 21. Final Synthesis: The "Hybrid" Model for ESP32
After analyzing the Bare Metal (Files 1-7) and Hosted (Files 9) architectures, we arrive at a definitive conclusion for the ESP32 port. It cannot be purely one or the other. It must be a **Hybrid**.

### 21.1 The "Bare Metal" Aspects
*   **Memory:** We must use `core_alloc` with a static heap (PSRAM) like a bare-metal target. Using `malloc` for everything (Hosted style) will fragment the heap too much for long-running playback on a device with limited virtual address space.
*   **Display:** We must drive the display via SPI directly (Bare Metal style), not via a windowing system. `lcd_update` will flush directly to hardware.

### 21.2 The "Hosted" Aspects
*   **Threading:** We must use the OS (FreeRTOS) scheduler (Hosted style). Implementing a custom context switch on top of FreeRTOS is redundant and dangerous.
*   **Storage:** We must use the OS filesystem (VFS/FATFS) like the Android port. Writing a raw SDMMC driver is unnecessary when ESP-IDF provides a robust, thread-safe one.
*   **Audio:** We must use the OS Audio API (I2S Driver) like the Android `AudioTrack`. We feed a buffer, and the OS handles the DMA interrupts.

### 21.3 The "Shim" Layer
The success of the port hinges on the quality of the **Shim Layer** that translates Rockbox's cooperative assumptions into FreeRTOS's preemptive reality.
*   **The Yield Shim:** `yield()` must map to `taskYIELD()` or `vTaskDelay(1)` to ensure the Idle task runs (feeding the Watchdog).
*   **The ISR Shim:** Rockbox ISRs (which run in interrupt context) must be converted to High Priority Tasks or strictly obey FreeRTOS `FromISR` semantics.

## 22. References
*   `firmware/target/hosted/android/` - The reference for JNI and AudioTrack integration.
*   `firmware/target/hosted/sdl/` - The reference for Threading and Input simulation.
*   `firmware/target/hosted/linux/` - The reference for Framebuffer mapping.
