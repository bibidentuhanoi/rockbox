# 06_The_Audio_Engine_Codecs_and_DSP.md

## Abstract
This document explores the high-level audio playback engine, which orchestrates file buffering, codec decoding, and digital signal processing. It details the aggressive RAM buffering strategy that minimizes disk spin-up, the fixed-point arithmetic optimizations (`FRAC_MUL`) essential for real-time decoding on FPU-less cores, and the modular DSP chain that implements 5-band parametric EQ, crossfeed, and resampling in software.

## 1. The Audio Engine (`apps/playback.c`)
The Rockbox audio engine is a masterpiece of embedded engineering. It manages decoding, buffering, DSP processing, and playback seamlessly on resource-constrained devices. It runs as a high-priority thread (`PRIORITY_PLAYBACK`).

### 1.1 The Buffering Strategy
Rockbox buffers aggressively. It loads the entire song into RAM (`audiobuf`) if possible, allowing the disk to spin down completely. This saves huge amounts of power.
*   **Struct:** `struct audio_buffer_t`
*   **Logic:**
    1.  Check free space in `audiobuf`.
    2.  Fill from disk until full.
    3.  Codec reads from `audiobuf`.
    4.  PCM data is written to the *PCM Buffer* (a smaller ring buffer for output).

**Source Analysis: `apps/playback.c` (Conceptual):**
```c
void audio_thread(void)
{
    while (1) {
        /* Check Messages */
        if (queue_receive(&audio_queue, &ev)) {
             handle_event(ev);
        }

        /* Fill Buffer */
        if (buffer_needs_filling()) {
             buffer_fill_from_disk();
        }

        /* Check Playback State */
        if (pcm_is_playing()) {
             update_metadata();
        }

        yield();
    }
}
```

---

## 2. Fixed-Point Codecs (`lib/rbcodec/codecs/`)
Rockbox runs on CPUs without Floating Point Units (FPU). All audio decoding (MP3, FLAC, Vorbis, AAC) is implemented using fixed-point integer arithmetic.

**Source Analysis: `lib/rbcodec/codecs/hes.c` (Game Music Emu Wrapper)**
The codec is loaded dynamically as a plugin overlay. It exports a standardized API via `struct codec_api`.

```c
/* The Codec Entry Point */
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Configure DSP Output Format */
        ci->configure(DSP_SET_SAMPLE_DEPTH, 16);
        ci->configure(DSP_SET_FREQUENCY, 44100);
        ci->configure(DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);

        /* Initialize Library */
        Hes_init(&hes_emu);
    }
    return CODEC_OK;
}

/* The Decoding Loop */
enum codec_status codec_run(void)
{
    while (1) {
        /* Check for Stop/Pause/Seek Commands */
        long action = ci->get_command(&param);
        if (action == CODEC_ACTION_HALT) break;

        /* Decode a Chunk */
        err = Hes_play(&hes_emu, CHUNK_SIZE, samples);

        /* Push PCM to Output Buffer */
        ci->pcmbuf_insert(samples, NULL, CHUNK_SIZE >> 1);
    }
    return CODEC_OK;
}
```

### 2.1 Fixed-Point Math Macros
To achieve performance, Rockbox uses macros to implement fractional multiplication.
*   **QFormat:** Q.31 (1 sign bit, 31 fractional bits) is common.
*   **`FRACBITS`:** Typically 31.

```c
#define FRACBITS 31
#define FRAC_ONE (1 << FRACBITS)

/* Multiply two fixed-point numbers (A * B) >> FRACBITS */
#define FRAC_MUL(a, b) \
    ((long long)(a) * (b) >> FRACBITS)

/* Multiply and Accumulate */
#define FRAC_MAC(acc, a, b) \
    ((acc) + FRAC_MUL(a, b))
```
These macros often map to specific assembly instructions (e.g., `SMLAL` on ARM) for single-cycle execution.

---

## 3. The DSP Pipeline (`lib/rbcodec/dsp/`)
Between the decoder and the DAC lies the DSP chain. It processes PCM samples in blocks.
*   **Chain:** Resampler -> Crossfeed -> Equalizer -> Compressor -> Volume -> Dither.

### 3.1 The Equalizer (EQ)
A 5-band fully parametric equalizer implemented with Biquad filters.
*   **Filter Type:** Peaking EQ, Low Shelf, High Shelf.
*   **Coefficients:** Calculated on the fly (using integer approximations of `sin`/`cos`) when settings change.

**Biquad Implementation (Fixed Point):**
```c
/* y[n] = b0*x[n] + b1*x[n-1] + b2*x[n-2] - a1*y[n-1] - a2*y[n-2] */
int32_t process_biquad(int32_t sample, struct biquad_coeffs *c, int32_t *history)
{
    int64_t acc;

    acc = (int64_t)sample * c->b0;
    acc += (int64_t)history[0] * c->b1;
    acc += (int64_t)history[1] * c->b2;
    acc -= (int64_t)history[2] * c->a1;
    acc -= (int64_t)history[3] * c->a2;

    /* Shift back to Q.31 range */
    acc >>= c->shift;

    /* Update History */
    history[1] = history[0];
    history[0] = sample;
    history[3] = history[2];
    history[2] = (int32_t)acc;

    return (int32_t)acc;
}
```

### 3.2 Resampler
Used for playback speed control (pitch shifting) or sample rate conversion (44.1kHz -> 48kHz for hardware compatibility). It uses polyphase filtering for high quality.

---

## 4. Crossfeed & Spatialization
Rockbox includes a Meier Crossfeed filter to reduce listening fatigue with headphones. It mixes a delayed, low-pass filtered signal from the left channel into the right channel (and vice-versa) to simulate speaker listening.
*   **Implementation:** Simple delay line + First-order Low Pass Filter.

---

## 5. The Kernel Linkage: Threads and Priorities
The Audio Engine is not a monolithic loop; it spans multiple kernel threads interacting via IPC.

### 5.1 The Audio Thread (`apps/audio_thread.c`)
This is the conductor. It sits in `apps/` (userland) but manages the `codec_thread` and `pcm_driver`.
*   **Priority:** `MIN(PRIORITY_BUFFERING, PRIORITY_USER_INTERFACE)`. It runs higher than background tasks (Database scan) but lower than the Realtime Mixer.
*   **Responsibility:** It does **not** decode audio. It manages the File I/O (`buffer_fill`) and Playlist logic.
*   **Message Loop:**
    ```c
    while(1) {
        queue_wait(&audio_queue, &ev);
        switch(ev.id) {
            case Q_AUDIO_PLAY:
                codec_load(track); // Loads ELF
                codec_run();       // Starts Codec Thread
                break;
        }
    }
    ```

### 5.2 The Codec Thread (`apps/codec_thread.c`)
This thread is spawned by the Audio Thread. It executes the actual decoder loop (e.g., `libmad` logic).
*   **Priority:** Dynamic. It starts low but can be boosted via `trigger_cpu_boost()` if the PCM buffer runs low (`PRIORITY_PLAYBACK` -> `PRIORITY_REALTIME`).
*   **Boosting:**
    ```c
    /* Inside codec loop */
    if (pcmbuf_free() > THRESHOLD) {
        trigger_cpu_boost(); /* Raise CPU clock to Max */
    } else {
        cancel_cpu_boost();  /* Drop to idle clock to save power */
    }
    ```

---

## 6. The HAL Linkage (The PCM Barrier)
The boundary between "Software Audio" (Codecs/DSP) and "Hardware Audio" (DAC/I2S) is the PCM Buffer.

### 6.1 `pcmbuf_insert` vs `pcm_play_data`
*   **Codec Output:** Codecs call `ci->pcmbuf_insert()`. This writes to the **Software Ring Buffer** (in SDRAM).
*   **DSP Processing:** Data sits here waiting for the DSP chain.
*   **HAL Input:** The PCM Driver (`firmware/pcm.c`) calls `pcm_play_data()` to initiate DMA transfers.
    *   **The Glue:** When the DMA interrupt fires (Buffer A done), the ISR calls the registered callback `get_more()`.
    *   **The Callback:** `pcmbuf_callback()` (in `apps/pcmbuf.c`) runs the DSP chain *on demand* to fill Buffer A with fresh processed samples.

### 6.2 The Mixer (`pcm_mixer.c`)
Rockbox supports mixing voice prompts (menus) over music.
*   **Mechanism:** The `pcmbuf_callback` doesn't just copy music; it calls `mixer_process()`.
*   **Mixing:** It adds Voice PCM data to Music PCM data (with saturation protection) before writing to the DMA buffer.

---

## 7. Codec Overlay & Memory Management
Codecs are not linked into the main firmware binary (`rockbox.elf`). They are dynamic overlays.

### 7.1 The Overlay Mechanism
*   **Location:** The `audiobuf` (MP3 data) and the Codec Binary (`.codec`) share the same RAM region (`BUFLIB_CONTEXT_MAIN`).
*   **Allocation:** When a track starts:
    1.  `audio_thread` requests a block for the codec via `core_alloc()`.
    2.  `buflib` compacts existing audio data to make room at the *end* of the buffer.
    3.  The codec ELF is loaded into this high-memory block.
*   **Implication:** Larger codecs (WMA, Vorbis) leave less room for audio buffering, reducing battery life (disk spins up more often).

### 7.2 The API Trampoline
Since codecs are separate binaries, they cannot call kernel functions (`sleep`, `read`) directly.
*   **`struct codec_api`:** A table of function pointers passed to `codec_start()`.
*   **Usage:**
    ```c
    /* Inside Codec */
    ci->read_filebuf(ptr, size); // Calls back into firmware
    ```

---

## 8. Vertical Slice Diagram: The Audio Stack
This diagram traces the complete path of an audio byte from Storage to Speaker.

```text
LAYER               COMPONENT                  FUNCTION
=====               =========                  ========
[ APP ]             Audio Thread               Orchestrates File I/O
                         |
                         v (loads)
                    Codec Thread               Decodes MP3 -> PCM
                         |
[ LIB ]                  v (ci->pcmbuf_insert)
                    Software Ring Buffer       Holds raw PCM (SDRAM)
                         |
                         v (callback)
                    DSP Chain                  EQ, Crossfeed, Volume
                         |
                         v (mixed)
                    Mixer Buffer               Final Audio Frame
                         |
[ FIRMWARE ]             v (dma_start)
                    PCM Driver                 Manages DMA transfers
                         |
                         v (AHB Bus)
[ HARDWARE ]        DMA Controller             Push to I2S FIFO
                         |
                         v (I2S Bus)
                    DAC Chip                   Digital -> Analog
                         |
                    Headphones                 Sound
```

## 9. Latency vs. Throughput
The system is tuned for **Throughput** (Battery Life), not Latency.
*   **High Latency:** The DSP chain runs ahead of the DMA by several frames to ensure the CPU can sleep.
*   **Synchronization:** The UI (Spectrum Analyzer) must account for this latency. The `pcm_get_realtime()` function subtracts the DMA buffer depth to guess the actual sound being heard.

---

## 10. Source Code Dump: The Codec-to-PCM Bridge
The following code snippet (reconstructed from `pcmbuf.c` and `dsp.c`) illustrates the critical "Pull" mechanism where the DMA interrupt drives the DSP chain.

```c
/* apps/pcmbuf.c */
static void pcmbuf_callback(const void **start, size_t *size)
{
    /* 1. Calculate how much space the DMA needs */
    size_t needed = *size;

    /* 2. Check if we have enough decoded data in the Ring Buffer */
    if (pcmbuf_read_level() < needed) {
        /* UNDERRUN! */
        /* Wake codec thread immediately */
        trigger_cpu_boost();
        queue_post(&codec_queue, CODEC_DECODE);

        /* Feed silence to avoid buzzing */
        memset(dma_buffer, 0, needed);
        return;
    }

    /* 3. Run the DSP Chain */
    /* This processes data from Ring Buffer -> DMA Buffer */
    dsp_process(dma_buffer, needed);

    /* 4. Update Ring Buffer Pointers */
    pcmbuf_advance(needed);

    /* 5. Return the filled buffer to the Driver */
    *start = dma_buffer;
}

/* lib/rbcodec/dsp/dsp.c */
void dsp_process(int16_t *dest, size_t count)
{
    int32_t sample[2];

    for (int i = 0; i < count/4; i++) { /* Stereo 16-bit = 4 bytes */
        /* Read Source */
        sample[0] = *ring_ptr_l++;
        sample[1] = *ring_ptr_r++;

        /* Apply EQ */
        sample[0] = eq_process(sample[0]);
        sample[1] = eq_process(sample[1]);

        /* Apply Volume */
        sample[0] = (sample[0] * global_volume) >> 16;
        sample[1] = (sample[1] * global_volume) >> 16;

        /* Write Dest */
        *dest++ = clip(sample[0]);
        *dest++ = clip(sample[1]);
    }
}
```

This interaction confirms that **Hardware Interrupts drive the Software Logic**. The Codec thread is a "Producer" that fills the ring buffer, and the DMA ISR is a "Consumer" that drains it via the DSP chain.

---

## 11. Detailed Codec Architecture: The `libmad` Wrapper
To illustrate how a specific codec integrates, we analyze the MP3 codec (`lib/rbcodec/codecs/mpga.c`) which wraps `libmad`.

### 11.1 The Entry Point
```c
enum codec_status codec_main(enum codec_entry_call_reason reason)
{
    if (reason == CODEC_LOAD) {
        /* Initialize MAD Decoder */
        mad_stream_init(&stream);
        mad_frame_init(&frame);
        mad_synth_init(&synth);

        /* Configure DSP for 44.1kHz Stereo */
        ci->configure(DSP_SET_FREQUENCY, 44100);
        ci->configure(DSP_SET_STEREO_MODE, STEREO_INTERLEAVED);
    }
    return CODEC_OK;
}
```

### 11.2 The Buffer Request Loop
The codec acts as a stream processor. It requests "File Chunks" and outputs "PCM Chunks".
```c
enum codec_status codec_run(void)
{
    while (1) {
        /* 1. Request Input Data */
        /* ci->request_buffer asks the kernel for a pointer to the file buffer */
        /* It handles the sector cache logic transparently */
        unsigned char *input_buffer = ci->request_buffer(&size, MIN_FRAME_SIZE);

        if (size == 0) break; /* EOF */

        /* 2. Decode One Frame */
        mad_stream_buffer(&stream, input_buffer, size);
        mad_header_decode(&frame.header, &stream);
        mad_frame_decode(&frame, &stream);

        /* 3. Synthesis (Fixed Point -> PCM) */
        mad_synth_frame(&synth, &frame);

        /* 4. Output to Kernel */
        /* dithered_pcm is a temporary buffer in codec RAM */
        ci->pcmbuf_insert(dithered_pcm, NULL, synth.pcm.length);

        /* 5. Advance File Pointer */
        ci->advance_buffer(stream.next_frame - input_buffer);

        /* 6. Yield to allow UI/Disk threads to run */
        ci->yield();
    }
    return CODEC_OK;
}
```

## 12. DSP Profiling and Optimization
Rockbox developers spend immense effort shaving cycles off the DSP.
*   **Inline Assembly:** Critical loops (biquad, volume) are written in ARM/ColdFire assembly.
*   **Zero-Copy:** The DSP chain modifies data *in place* in the DMA buffer to avoid `memcpy`.
*   **Branch Prediction:** `likely()`/`unlikely()` macros steer the compiler for the "Music Playing" path.

### 12.1 The Cycle Counter
Rockbox has a built-in profiler that uses hardware timers to measure CPU usage per thread.
*   `cpu_idle`: Time spent in the Idle thread (WFI).
*   `audio_thread`: Time spent managing the playlist.
*   `codec_thread`: Time spent decoding.
*   **Target:** On an ARM926EJ-S at 200MHz, MP3 decoding should consume < 15MHz (7.5% CPU), leaving 92.5% for idle (battery saving).

## 13. ReplayGain Analysis
ReplayGain normalizes audio volume to a standard loudness. Rockbox applies this in the DSP chain.

### 13.1 ID3 Tag Parsing
The metadata parser reads `TXXX:replaygain_track_gain` tags.
*   **Values:** Stored in dB (e.g., "-8.54 dB").
*   **Conversion:** Converted to a fixed-point scaling factor.

### 13.2 The Gain Stage
The DSP chain applies the gain.
```c
/* lib/rbcodec/dsp/dsp_misc.c */
void apply_replaygain(int32_t *sample, int32_t gain)
{
    /* gain is Q.24 fixed point */
    int64_t val = (int64_t)*sample * gain;
    *sample = val >> 24;
}
```
**Clipping:** ReplayGain can cause clipping. Rockbox implements a "hard limiter" or "soft clipper" to prevent digital distortion.

---

## 14. Cuesheet Parsing
Rockbox supports embedded or external cuesheets for single-file album rips.

### 14.1 The Virtual Track Logic
A Cuesheet splits one physical file into multiple logical tracks.
*   **Struct:** `struct cuesheet` contains an array of track offsets.
*   **Seek:** "Next Track" calculates the offset of Track N+1 and seeks the file pointer, rather than opening a new file.
*   **Gapless:** Since it's one file, gapless playback is implicit.

## 15. Conclusion
The Audio Engine is a high-wire act of balancing **Buffer Depth** (for disk power saving) against **RAM Usage** (for the codec overlay) and **CPU Cycles** (for DSP complexity). The use of fixed-point arithmetic is a hard constraint that permeates the entire architecture, from the `libmad` source to the custom `FRAC_MUL` macros in the DSP chain.
