# 05_The_Audio_Engine_Codecs_and_DSP.md

## 1. The Audio Engine Architecture

Rockbox's audio engine is a masterpiece of embedded engineering, capable of decoding FLAC, MP3, Vorbis, and AAC on 80MHz CPUs without Floating Point Units (FPUs).

### 1.1 The Codec API (`struct codec_api`)

Codecs in Rockbox are **not** compiled into the kernel. They are separate binaries loaded dynamically into RAM when a file type changes. The `codec_api` structure (from `lib/rbcodec/codecs/codecs.h`) acts as the bridge between the kernel and the codec.

```c
struct codec_api {
    /* File I/O */
    size_t (*read_filebuf)(void *ptr, size_t size);
    void (*advance_buffer)(size_t amount);
    bool (*seek_buffer)(size_t newpos);

    /* Output */
    void (*pcmbuf_insert)(const void *ch1, const void *ch2, int count);

    /* Synchronization */
    void (*yield)(void);
    unsigned (*sleep)(unsigned ticks);

    /* Metadata */
    struct mp3entry *id3;
    off_t filesize;
};
```

### 1.2 Fixed-Point Arithmetic

Since legacy targets (ARM7TDMI, ColdFire) lack FPUs, all codecs are heavily optimized using fixed-point math.
*   **libmad:** The MPEG audio decoder uses 32-bit fixed-point representation.
*   **Tremor:** The integer-only Vorbis decoder.
*   **Assembly Optimizations:** Critical paths (IMDCT, Synthesis Window) are often hand-written in ARM or ColdFire assembly (`dct32_arm.S`, `synth_full_arm.S`).

## 2. The DSP Pipeline

Once audio is decoded to PCM, it passes through the DSP chain before reaching the DMA buffers.

### 2.1 The Chain

1.  **Resampling:** If the content is 48kHz and the hardware only supports 44.1kHz (rare now, but historical), or for pitch correction.
2.  **Crossfeed:** Blends stereo channels to reduce "super-stereo" fatigue on headphones.
3.  **Equalizer:** A fully parametric, multi-band equalizer.
4.  **ReplayGain:** Applying track/album gain.
5.  **Dithering:** Converting internal 32-bit audio to 16-bit for the DAC.

### 2.2 `rbcodec` Library

The code resides in `lib/rbcodec/`. This library is linked both into the main firmware (for the DSP) and into the individual codec binaries (for shared math functions).

## 3. Codec Loading Mechanism

When the user selects a file:
1.  **Identification:** `apps/codecs.c` determines the file type.
2.  **Loader:** The kernel allocates memory in the **Codec Buffer** (part of the audio buffer).
3.  **Relocation:** The codec binary (`.rock` which is an ELF) is loaded.
4.  **Execution:** The system jumps to the codec's entry point (`codec_start`).

The codec runs in its own thread, decoding audio frames and pushing them into the `pcmbuf_insert` sink. It yields to the scheduler regularly to allow the UI to update.
