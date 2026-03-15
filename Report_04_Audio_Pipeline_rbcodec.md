# Report 04: Audio Pipeline and rbcodec

The Rockbox audio engine is completely abstracted from the underlying OS to enable a seamless cross-platform DSP and decoding pipeline. The entire decoder library (`lib/rbcodec/`) is built as a standalone suite.

## The Codec API Boundary

Codecs in Rockbox (e.g., MP3, FLAC, Opus) are not statically linked into the main firmware. Instead, they are dynamically loaded plugins that execute using a massive struct of function pointers passed down from the firmware, defined in `lib/rbcodec/codecs/codecs.h` as `struct codec_api`.

This is a "reverse API" approach: the core firmware calls the codec's entry point, passing the `ci` (codec interface) struct. The codec then uses this struct to call *back* into the firmware for file I/O, memory, and PCM buffer insertion.

```c
// Example from lib/rbcodec/codecs/codecs.h
struct codec_api {
    // ... file io ...
    int (*read_filebuf)(void *ptr, size_t size);

    // ... output ...
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);

    // ... threading and system ...
    void (*yield)(void);
};
```

## Step-by-Step Playback Trace

1. **Loading the Codec**:
   When the user selects a track, `apps/playback.c` determines the file type and calls the loader in `lib/rbcodec/codec_thread.c`. The system uses `codec_load_file()` or `codec_load_buf()` to pull the compiled `.codec` binary off the disk (or RAM cache) into the `audiobuf` utilizing `core_alloc()`. Once loaded, it resolves the codec's entry point and passes the `struct codec_api *api`.

2. **Reading the File Buffer**:
   Inside a codec plugin (e.g., `lib/rbcodec/codecs/flac.c`), the codec calls `ci->read_filebuf()`. This pulls chunks of the compressed audio file from the disk into the remaining `audiobuf` space.

3. **Decoding and fixed-point Math**:
   Because many target devices lack a hardware Floating Point Unit (FPU), `rbcodec` relies entirely on a custom fixed-point math library (`lib/fixedpoint/`) to perform operations like IMDCT (Inverse Modified Discrete Cosine Transform) in software using macros like `FRAC_MUL`.

4. **PCM Buffer Insertion (`pcmbuf_insert`)**:
   Once a frame of audio is decoded into raw PCM integers, the codec pushes it to the core system via `ci->pcmbuf_insert(ch1, ch2, count)`.
   *   For FLAC: `ci->pcmbuf_insert(&fc.decoded[0][fc.sample_skip], &fc.decoded[1][fc.sample_skip], sample_count);`

5. **DSP Processing and Mixer (`pcm_mixer.c`)**:
   The data pushed by `pcmbuf_insert` goes into the `pcmbuf`. Before it hits the hardware, it is processed by the software mixer (`firmware/pcm_mixer.c`).
   *   The mixer handles voice UI overlays, crossfading, software volume control (`firmware/pcm_sw_volume.c`), and sample rate conversion (`firmware/pcm_sampr.c`).
   *   The mixer logic requests more data from `pcmbuf` via a callback.

6. **DMA Hardware Output (`pcm_play_data`)**:
   Once the mixer has prepared a contiguous block of PCM data, it is passed to the core HAL DMA logic.
   *   `firmware/pcm_mixer.c` calls `pcm_play_data(mixer_pcm_callback, mixer_buffer_callback, ...)` which is defined in `firmware/pcm.c`.
   *   `pcm.c` manages the DMA ring buffers (`pcm_read_ptr`, `pcm_write_ptr`). It configures the target-specific DMA engine (e.g., I2S, PL081) to continuously blast the PCM ring buffer to the onboard DAC hardware, generating sound without further CPU intervention until an interrupt fires requesting more data.