# 06_The_Audio_Engine_Codecs_and_DSP.md

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
