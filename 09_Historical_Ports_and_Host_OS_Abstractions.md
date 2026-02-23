# 09_Historical_Ports_and_Host_OS_Abstractions.md

## 1. The Hosted Architecture

Rockbox isn't just for MP3 players. The `hosted` target allows the Rockbox kernel and application layer to run as a user-space application on top of another Operating System (Linux, Windows, Android, macOS). This was originally designed for the **UI Simulator**, but evolved into a full port (Rockbox as an App).

### 1.1 The Abstraction Layer (`firmware/target/hosted/`)

Instead of writing to registers, the hosted drivers wrap OS APIs.

*   **Threading (`kernel-unix.c`):** Maps Rockbox `create_thread` to POSIX `pthread_create`.
*   **Storage (`filesystem-unix.c`):** Maps `storage_read_sectors` to `read()` on a disk image file or a directory tree.
*   **Display (`lcd-sdl.c`):** Maps the Rockbox framebuffer to an SDL surface or texture.

## 2. The UI Simulator (`uisimulator/`)

The simulator is a critical development tool. It compiles the full Rockbox firmware but links it against the hosted HAL.

### 2.1 SDL Integration

The Simple DirectMedia Layer (SDL) is the backbone of the simulator.
*   **Input:** Keypresses on the PC keyboard are mapped to Rockbox button events.
*   **Audio:** The `pcm-sdl.c` or `pcm-alsa.c` driver feeds the decoded audio samples to the host's sound card.

### 2.2 Debugging

The simulator allows developers to use GDB, Valgrind, and other host tools to debug Rockbox logic without needing physical hardware.

## 3. The Android Port (`android/`)

Rockbox was ported to Android (as an APK) using the JNI (Java Native Interface).

### 3.1 Architecture

1.  **Java Front-end:** Handles the Activity lifecycle, touch input, and screen drawing (blitting the framebuffer to a Bitmap).
2.  **Native Library (`librockbox.so`):** The entire Rockbox firmware compiled as a shared library.
3.  **Audio Output:** Uses the Android NDK Audio APIs (OpenSL ES or AudioTrack).

### 3.2 Challenges

*   **Touch:** Adapting the button-driven UI to a touchscreen (this led to the "Point to Point" and "PictureFlow" plugins).
*   **Backgrounding:** Keeping the Rockbox thread alive when the Android Activity is paused.

## 4. Mapping the Flat Memory Model

On a hosted target, `buflib` and the flat memory map are simulated using a large `malloc()` block.
*   **Virtual RAM:** Rockbox allocates, say, 32MB of heap memory on startup.
*   **Emulation:** The internal allocators (`core_alloc`) carve up this block just as they would physical RAM.
*   **Exceptions:** `setjmp`/`longjmp` or signal handlers (SIGSEGV) are sometimes used to simulate hardware exceptions, though less common in the standard build.

## 5. Lessons for Modern Ports

The hosted ports prove that Rockbox is highly portable. The strict separation between the **Kernel/Driver** layer and the **Application** layer allows the upper 90% of the code to run unmodified on almost anything that can provide a frame buffer and a PCM stream.
