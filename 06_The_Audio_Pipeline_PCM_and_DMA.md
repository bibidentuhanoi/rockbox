# 06_The_Audio_Pipeline_PCM_and_DMA.md

## 1. The Data Path: From Disk to DAC

The journey of an audio sample in Rockbox is a long one, traversing multiple buffers and threads.

```mermaid
graph TD
    Disk[Storage (HDD/SD)] -->|Read Ahead| AudioBuf[Audio Buffer (RAM)]
    AudioBuf -->|Decode| CodecThread[Codec Thread]
    CodecThread -->|pcmbuf_insert| PCMQueue[PCM Ring Buffer]
    PCMQueue -->|Mix/Fade| Mixer[Software Mixer]
    Mixer -->|DMA Transfer| I2S[I2S Controller]
    I2S -->|Digital| DAC[DAC / Codec Chip]
    DAC -->|Analog| Headphones
```

## 2. The PCM Buffer (`pcmbuf.c`)

The codec decodes compressed audio into raw PCM samples (usually 16-bit signed stereo, 44.1kHz). It pushes these samples into the **PCM Buffer**.

*   **Circular Buffer:** The PCM buffer acts as a FIFO queue between the codec (producer) and the DMA interrupt (consumer).
*   **High Watermark:** The codec thread runs until the PCM buffer is full, then sleeps.
*   **Low Watermark:** When the DMA drains the buffer below a threshold, the codec thread wakes up to decode more frames.

## 3. The PCM Mixer (`firmware/pcm_mixer.c`)

Rockbox includes a software mixer to handle:
*   **Cross-fading:** Blending the end of one track into the start of another.
*   **Voice Over:** Mixing the "Talkbox" (voice menu) audio over the music.
*   **Beeps:** System notifications.

The mixer runs in the context of the PCM interrupt or a high-priority thread, processing samples just before they hit the hardware.

## 4. The DMA Engine (`firmware/pcm.c`)

The interface to the hardware is managed by `firmware/pcm.c` and its target-specific backends.

### 4.1 Double Buffering (Ping-Pong)

To ensure skip-free audio, Rockbox uses DMA double buffering.
1.  **Buffer A:** The hardware transmits this buffer to the DAC.
2.  **Buffer B:** The CPU fills this buffer with new samples from the mixer.
3.  **Interrupt:** When Buffer A is finished, the DMA controller triggers an interrupt (`FIQ` or `IRQ`).
4.  **Swap:** The interrupt handler points the DMA to Buffer B and signals the CPU to start filling Buffer A.

### 4.2 Target-Specific Implementation

For an ARM target (e.g., Sansa), the low-level driver (`target/arm/as3525/pcm-as3525.c`) manipulates the SoC's DMA registers.

```c
/* Pseudocode for DMA Interrupt Handler */
void DMA_Handler(void) {
    /* Acknowledge Interrupt */
    Clear_DMA_Status();

    /* Request more data from the PCM middle-layer */
    if (pcm_play_dma_complete_callback(status, &next_addr, &next_size)) {
        /* Configure DMA for the next transfer */
        DMA_SetAddress(next_addr);
        DMA_SetCount(next_size);
        DMA_Enable();
    } else {
        /* Underrun or Stop */
        DMA_Stop();
    }
}
```

## 5. Clocking and Sample Rates

Rockbox supports multiple sample rates (44.1, 48, 88.2, 96 kHz). Changing sample rates requires:
1.  **PLL Reconfiguration:** Adjusting the I2S master clock.
2.  **Codec Reconfiguration:** Sending I2C commands to the DAC chip to update its internal filters.
3.  **Resampling:** If the hardware doesn't support the source rate, the `dsp` layer handles software resampling.
