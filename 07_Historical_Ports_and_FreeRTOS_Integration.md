# 07_Historical_Ports_and_FreeRTOS_Integration.md

## 1. Lessons from the Past

Rockbox isn't just for iPods. It runs on Linux, Android, and Windows (Simulator).

### The SDL Port (`firmware/target/hosted/sdl`)
*   **Abstraction Level:** High.
*   **Threading:** Uses `pthread` (POSIX Threads) or `SDL_Thread`.
*   **Key takeaway:** `system-sdl.c` shows how to wrap a "Rockbox main loop" inside a foreign event loop.

### The Android Port (`firmware/target/hosted/android`)
*   **Architecture:** JNI (Java Native Interface).
*   **Audio:** Uses OpenSL ES or AudioTrack (via JNI).
*   **Key takeaway:** Rockbox can live as a library (`librockbox.so`) invoked by a host.

## 2. ESP32 OS Collision (FreeRTOS vs Rockbox)

The ESP32 runs FreeRTOS. We cannot replace it. We must coexist.

### The "Task Wrapping" Strategy
Rockbox expects to create threads (`create_thread`). We will map these to FreeRTOS tasks.

#### The Mapping Table

| Rockbox Primitive | FreeRTOS Equivalent | Implementation Notes |
| :--- | :--- | :--- |
| `create_thread` | `xTaskCreatePinnedToCore` | Map priorities. Rockbox stack size -> FreeRTOS stack size. |
| `thread_exit` | `vTaskDelete(NULL)` | |
| `switch_thread` | `taskYIELD()` | Cooperative yield. |
| `sleep(ticks)` | `vTaskDelay(ticks)` | 1 tick = 10ms (usually). Rockbox assumes HZ=100. |
| `mutex_lock` | `xSemaphoreTake` | Use Recursive Mutexes. |
| `queue_wait` | `xQueueReceive` | |
| `queue_post` | `xQueueSend` | |

### `firmware/target/hosted/esp32/kernel-freertos.c`
We will create this file. It will implement the Rockbox `thread.h` API using FreeRTOS calls.

```c
/* Concept Code */
unsigned int create_thread(void (*function)(void), void* stack, ...)
{
    TaskHandle_t handle;
    xTaskCreate(function, name, stack_size, NULL, PRIORITY, &handle);
    return (unsigned int)handle;
}
```

## 3. Core Pinning Strategy

The ESP32 has two cores: PRO_CPU (0) and APP_CPU (1).

*   **PRO_CPU (Core 0):** Handles WiFi, Bluetooth, and TCP/IP stack (LwIP).
*   **APP_CPU (Core 1):** Typically runs the user application.

**Recommendation:**
*   **Rockbox Main (UI):** Pin to **Core 1**.
*   **Audio Decoder:** Pin to **Core 1** (Preemptive).
*   **Buffering/Disk:** Pin to **Core 1** (or let it float).
*   **Reason:** Keep Core 0 free for RF interrupts. If the MP3 decoder hogs Core 0, WiFi packets drop.

## 4. ESP32 Memory Mapping

### The SPIRAM Mandate
The ESP32-S3 has ~384KB internal SRAM and up to 8MB PSRAM (SPIRAM).
Rockbox needs buffers larger than 384KB.

*   **Audio Buffer:** Must go to SPIRAM.
    *   `audiobuf = heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);`
*   **Codecs:** If statically linked, they live in Flash (XIP). If dynamically loaded (Phase 2), they go to SPIRAM (data) or IRAM (code).
*   **Stacks:** FreeRTOS stacks should generally be in Internal SRAM for speed, but can go to SPIRAM if necessary (slower context switches).

### The ISR Constraint
Code running in an Interrupt Service Routine (ISR) **must** reside in Internal RAM (`IRAM_ATTR`) and access only Internal DRAM.
*   **Implication:** The I2S DMA callback in `pcm-esp32.c` must be small, fast, and in IRAM. It cannot touch the main `audiobuf` in SPIRAM if the cache is disabled (e.g., during Flash write).
*   **Solution:** Use a small "Ping-Pong" buffer in Internal RAM between the Codec (SPIRAM) and the I2S Driver.
