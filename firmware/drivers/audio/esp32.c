/*
 * Audio hardware driver for ESP32-S3 (MAX98357A I2S amp)
 *
 * Uses Rockbox's pcm_sw_volume system for software volume control.
 * audiohw_set_volume() delegates to pcm_set_master_volume(), which stores
 * fixed-point scaling factors applied by pcm_copy_buffer() in the PCM task.
 *
 * Same approach as iBasso DX50/DX90, Erosq Native, and SDL simulator.
 *
 * Hardware volume limit:
 *   The MAX98357A is a fixed-gain I2S class-D amp (GAIN pin set to highest).
 *   At 0 dB (unity) software volume, the amp draws enough current to trigger
 *   the ESP32-S3 brownout detector on USB power. Empirically, -20 dB (~0.1x
 *   linear) is the maximum safe volume. The volume range is capped in
 *   esp32_codec.h: AUDIOHW_SETTING(VOLUME, ..., -80, -20, -20).
 */
#include "config.h"
#include "audiohw.h"
#include "pcm_sw_volume.h"

void audiohw_init(void)
{
    /* pcm_sw_volume factors default to 0 (MUTE). Set -20 dB so audio
       is audible before Rockbox calls audiohw_set_volume().
       -200 centibels = -20 dB ≈ 0.1x, same as old PCM_VOLUME_SCALE. */
    pcm_set_master_volume(-200, -200);
}

void audiohw_close(void)
{
    /* nothing */
}

void audiohw_set_frequency(int fsel)
{
    (void)fsel;
}

/* Called by sound.c set_prescaled_volume().
 * vol_l and vol_r are centibels (tenths of dB).
 * pcm_set_master_volume converts to fixed-point scaling factors. */
void audiohw_set_volume(int vol_l, int vol_r)
{
    pcm_set_master_volume(vol_l, vol_r);
}

void audiohw_preinit(void)
{
    /* nothing */
}

void audiohw_postinit(void)
{
    /* nothing */
}
