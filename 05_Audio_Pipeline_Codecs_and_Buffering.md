# 05_Audio_Pipeline_Codecs_and_Buffering.md

## 1. The Audio Pipeline Map

The audio pipeline is the core function of Rockbox. It is designed to be power-efficient (disk spins down) and gapless.

```mermaid
graph TD
    Disk[Storage (SD/HDD)] -->|Read| Buffer[Audio Buffer (RAM)]
    Buffer -->|Demux/Parse| Codec[Codec (MP3/FLAC)]
    Codec -->|PCM Data| DSP[DSP Chain (EQ, Crossfeed)]
    DSP -->|Processed PCM| FIFO[PCM Ring Buffer]
    FIFO -->|DMA Transfer| I2S[I2S / AC97 Controller]
    I2S -->|Analog Signal| DAC[DAC / Amp]
```

### Key Components
1.  **`apps/playback.c`**: The conductor. It decides *what* to play.
2.  **`apps/buffering.c`**: The memory manager. It fills the RAM buffer from disk.
3.  **`apps/codec_thread.c`**: The worker. It takes data from the RAM buffer, runs the codec, and outputs PCM.
4.  **`firmware/pcm.c`**: The driver. It manages the DMA transfer to the hardware.

## 2. Buffering & Ring Buffers

Rockbox assumes the storage is slow and power-hungry.
*   **The Big Buffer:** Most of the device's RAM (e.g., 32MB - 64MB) is allocated to `audiobuf`.
*   **Behavior:**
    1.  User selects a track.
    2.  `buffering_thread` wakes up, spins up the disk.
    3.  It fills the `audiobuf` with as many future tracks as possible.
    4.  Disk spins down.
    5.  Codec plays from RAM.

### `struct track_info`
Located in `playback.c`. Tracks metadata handles within the buffer.
```c
struct track_info
{
    int id3_hid;        /* Metadata handle */
    int audio_hid;      /* Audio data handle */
    int codec_hid;      /* Codec overlay handle */
};
```

## 3. Codec Integration (`lib/rbcodec`)

Codecs are dynamically loaded.
*   **The Interface:** `struct codec_api`.
    *   `init()`
    *   `run()`
    *   `seek()`
*   **Execution:** The `codec_thread` loads the codec binary into a reserved slot in RAM (IRAM or DRAM depending on architecture).
*   **Interaction:** The codec reads raw file data using `ci->read_filebuf` (which reads from RAM) and writes PCM samples using `ci->pcmbuf_insert`.

## 4. The PCM Driver (`firmware/pcm.c`)

This acts as the final stage ring buffer.
*   **Double Buffering / DMA:** It maintains a DMA buffer. When one half is finished, an interrupt fires (`PCM_DMA_IRQ`).
*   **The ISR:**
    1.  Acknowledges the interrupt.
    2.  Refills the finished buffer half from the software PCM ring buffer.
    3.  Updates the position counters (for the progress bar).

## 5. Playback State Machine

`apps/playback.c` manages the complex state of a music player.

### States
*   `PLAY_STOPPED`
*   `PLAY_PLAYING`
*   `PLAY_PAUSED`

### Thread Synchronization
*   **Queue Events:**
    *   `Q_AUDIO_PLAY`: Start playing.
    *   `Q_AUDIO_STOP`: Stop.
    *   `Q_AUDIO_SKIP`: Next/Prev track.
*   **Handling:** `audio_playback_handler()` in `playback.c` processes these events. It interacts with the `buffering_thread` (to load data) and the `codec_thread` (to decode it).

## 6. ESP32 Implementation Details

### The I2S Bridge
*   **Rockbox `pcm_play_data()`**: We will map this to `i2s_channel_write()` (ESP-IDF V5.x) or `i2s_write()` (V4.x).
*   **DMA:** ESP32 I2S uses DMA automatically. We just need to feed it.
*   **Buffer Size:** We must ensure the Rockbox PCM ring buffer is large enough to prevent underruns if the codec task is preempted by WiFi.

### Codec Execution
*   As noted in File 4, dynamic loading on ESP32 is hard.
*   **Strategy:** Compile `libmad`, `libflac`, etc., as static libraries linked into the main image.
*   **`codec_load()` Shim:** Implement a shim that, instead of loading a file, simply returns a pointer to the statically linked codec function table.
    ```c
    // Pseudo-code shim
    bool codec_load(int codec_id) {
        switch(codec_id) {
            case CODEC_MP3: ci->entry = &mp3_entry_point; break;
            case CODEC_FLAC: ci->entry = &flac_entry_point; break;
        }
        return true;
    }
    ```
