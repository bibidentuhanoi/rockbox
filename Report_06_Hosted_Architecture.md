# Report 06: Hosted Architecture vs. Bare-Metal

Rockbox's architecture is unique in that the entire OS—including its RTOS threading, message queues, and memory management—can be compiled as a user-space application running on top of a "Host OS" (like Linux, Android, or an SDL-based emulator). This is known as the "Hosted Port."

## Bypassing the HAL

In a bare-metal build (e.g., an iPod), Rockbox drivers talk directly to hardware registers. In a hosted build, the hardware abstraction layer (HAL) is re-routed to use the host OS's APIs.

This code is primarily located in `firmware/target/hosted/`.

### 1. Threading and Concurrency (`thread-sdl.c`)
On bare-metal, `create_thread()` allocates a stack and `switch_thread()` swaps CPU registers.
In a hosted build using SDL (`firmware/target/hosted/sdl/thread-sdl.c`), Rockbox maps its internal threading API directly to `SDL_Thread` and `SDL_mutex` primitives:
*   `create_thread()` calls `SDL_CreateThread()`.
*   Rockbox mutexes (`mutex_lock()`) map to `SDL_LockMutex(m)`.
*   The bare-metal `yield()` function, which normally evaluates the thread scheduler queue, simply maps to `SDL_Delay()` or `SDL_SemWaitTimeout()`, delegating the scheduling back to the Host OS kernel.

### 2. Audio Output (`pcm-alsa.c`)
On bare-metal, `pcm_play_data()` configures a PL081 DMA controller or an I2S bus.
In a Linux hosted port (`firmware/target/hosted/pcm-alsa.c`), Rockbox acts as an ALSA client.
*   The Rockbox software mixer still processes the audio data into `pcmbuf`.
*   Instead of configuring DMA registers, the ALSA driver calls `snd_pcm_writei()` to push the Rockbox PCM frames into the Linux sound server.
*   The driver manages ALSA states (e.g., `SND_PCM_STATE_XRUN`, `snd_pcm_recover`) to keep the Rockbox audio engine synced with the host OS.

### 3. Display and Input (`lcd-sdl.c` / `button-sdl.c`)
On bare-metal, Rockbox draws its `frame_buffer_t` directly to a memory-mapped LCD controller (DBOP).
*   In the SDL port, `lcd_update()` copies the Rockbox framebuffer pixels into an `SDL_Surface` and calls `SDL_Flip()`.
*   Input is gathered via `SDL_PollEvent()`. When a user presses an arrow key on their PC keyboard, `button-sdl.c` translates the `SDL_KEYDOWN` event into a native Rockbox button ID (e.g., `BUTTON_RIGHT`) and pushes it onto the standard Rockbox message queue using `button_queue_post()`.

## Android Port (`android/`)
The Android port operates similarly. It wraps the Rockbox core in a JNI (Java Native Interface) layer.
*   Java code (`android/src/`) provides the standard Android Activity and SurfaceView.
*   Touch events from Android are passed down via JNI to Rockbox's internal queue.
*   Audio is output using OpenSL ES natively, allowing Rockbox to bypass the Java audio latency and act as a high-performance native media player application on the phone, while retaining 100% of its core application logic, codecs, and database system.