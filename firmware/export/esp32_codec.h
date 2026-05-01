/*
 * Audio codec settings for ESP32-S3 + MAX98357A
 *
 * MAX98357A is a fixed-gain I2S amp. At the highest GAIN pin setting,
 * full-scale (0 dB) digital input causes brownout on USB power.
 * Max safe volume is -20 dB (~0.1x linear), verified empirically.
 *
 * Volume range: -80 dB (near-silence) to -20 dB (max safe)
 * Default: -20 dB (loudest without brownout)
 *
 * TODO: Retest volume limit with better hardware (proper speaker,
 * headphone amp, or external DAC). The current -20 dB cap is based on
 * the dev board's USB power + MAX98357A brownout threshold. A board with
 * better power supply or a different amp may support higher max volume.
 * Also retest EQ bass bands (60-400 Hz) — barely audible on the tiny
 * dev board speaker.
 */
#ifndef _ESP32_CODEC_H
#define _ESP32_CODEC_H

AUDIOHW_SETTING(VOLUME, "dB", 0, 1, -80, -20, -20)

#endif /* _ESP32_CODEC_H */
