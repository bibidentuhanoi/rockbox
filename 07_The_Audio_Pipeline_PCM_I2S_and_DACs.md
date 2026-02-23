# 07_The_Audio_Pipeline_PCM_I2S_and_DACs.md

## 1. The Audio Pipeline Overview
The audio pipeline is the heart of Rockbox. It ensures glitch-free playback by meticulously managing data flow from disk to DAC.

### 1.1 The Chain
1.  **Disk (FAT):** Compressed data (MP3) is read into `audiobuf`.
2.  **Codec (Thread):** Reads from `audiobuf`, decodes to PCM (16-bit/44.1kHz), and writes to the **PCM Buffer**.
3.  **DSP (Thread):** Processes PCM (EQ, Volume) in-place in the PCM Buffer.
4.  **PCM Driver (Interrupt):** Reads from PCM Buffer via DMA.
5.  **I2S Controller:** Serializes PCM data to the DAC.
6.  **DAC:** Converts digital samples to analog audio.

---

## 2. PCM Buffering Logic (`firmware/pcm.c`)
The PCM buffer is a circular buffer in SDRAM. The codec writes to the head (`pcm_write_ptr`), and the DMA reads from the tail (`pcm_read_ptr`).

**Source Analysis: `firmware/pcm.c`**
```c
/* PCM Buffer Pointers */
static volatile int pcm_read_ptr;  /* Read by DMA */
static volatile int pcm_write_ptr; /* Written by Codec */
static int pcm_buffer_size;        /* Total Size */
static int16_t *pcm_buffer;        /* Base Address */

/* Insert Samples into PCM Buffer */
void pcmbuf_insert(const void *src, size_t size)
{
    int space = pcmbuf_free();

    if (space < size) {
        /* Wait for DMA to consume data */
        yield();
    }

    /* Copy Data (Handling Wrap-Around) */
    int chunk = MIN(size, pcm_buffer_size - pcm_write_ptr);
    memcpy(pcm_buffer + pcm_write_ptr, src, chunk);

    pcm_write_ptr = (pcm_write_ptr + chunk) % pcm_buffer_size;

    /* Trigger DMA if stopped */
    if (!pcm_is_playing) {
        pcm_play_start();
    }
}
```

### 2.1 Double Buffering & DMA Interrupts
To prevent glitches, Rockbox uses a double-buffering scheme with DMA.
*   **Buffer A:** Being played by DMA.
*   **Buffer B:** Being filled by Codec.
*   **Interrupt (`PCM_DMA_IRQ`):** Fires when Buffer A is done. The ISR immediately points the DMA to Buffer B and signals the codec to refill Buffer A.

```c
/* DMA Completion ISR */
void pcm_dma_interrupt(void)
{
    /* 1. Clear Interrupt Flag */
    DMA_INT_CLEAR = 1;

    /* 2. Advance Read Pointer */
    pcm_read_ptr += DMA_BLOCK_SIZE;
    if (pcm_read_ptr >= pcm_buffer_size)
        pcm_read_ptr = 0;

    /* 3. Setup Next Transfer */
    dma_start(pcm_buffer + pcm_read_ptr, DMA_BLOCK_SIZE);

    /* 4. Wake Codec Thread if low on data */
    if (pcmbuf_free() > THRESHOLD) {
        queue_post(&codec_queue, PCM_NEED_DATA);
    }
}
```

---

## 3. I2S & Hardware Interface
The I2S (Inter-IC Sound) bus is the standard for digital audio transport.
*   **Signals:**
    *   **BCLK (Bit Clock):** 32x or 64x Sample Rate (e.g., 2.8224 MHz for 44.1kHz).
    *   **LRCK (Left/Right Clock):** Sample Rate (44.1kHz).
    *   **SDATA (Serial Data):** The actual audio bits.
    *   **MCLK (Master Clock):** High-frequency reference (e.g., 11.2896 MHz).

### 3.1 Sample Rate Conversion (ASRC)
Some hardware (like AC97 codecs) runs at a fixed rate (48kHz). Rockbox must resample 44.1kHz MP3s to 48kHz in software or using hardware ASRC blocks if available.

### 3.2 DAC Configuration
The DAC (e.g., Wolfson WM8758) is configured via I2C.
*   **Volume Control:** Digital attenuation inside the DAC (better SNR than software volume).
*   **Mute:** Hardware mute during sample rate changes to prevent pops.

```c
/* Set Volume (Hardware) */
void audiohw_set_volume(int vol_l, int vol_r)
{
    /* Map -74dB to +6dB range to register values */
    int reg_l = (vol_l + 74) * 2;
    int reg_r = (vol_r + 74) * 2;

    i2c_write(DAC_ADDR, WM8758_LOUT1_VOL, reg_l | 0x100); /* Update L */
    i2c_write(DAC_ADDR, WM8758_ROUT1_VOL, reg_r | 0x180); /* Update R + Load */
}
```

---

## 4. Playback State Machine
The `playback.c` thread manages high-level states:
*   **STOPPED:** DMA off, Codec thread suspended.
*   **PLAYING:** DMA running, Codec filling.
*   **PAUSED:** DMA stopped, but buffers retained.
*   **SEEKING:** Codec thread flushes buffers, seeks file pointer, and refills.

### 4.1 Crossfading
Rockbox supports crossfading between tracks.
*   **Mechanism:** Two codecs run simultaneously (or one codec alternates fast enough).
*   **Mixing:** The PCM buffer receives data from both sources, mixed in software with volume ramping.

### 4.2 Gapless Playback
A signature feature.
*   **Pre-buffering:** While Track A plays, Rockbox decodes the start of Track B into a separate buffer.
*   **Seamless Stitching:** When Track A finishes, the DMA pointer is instantaneously switched to Track B's buffer, resulting in sample-perfect transitions.
