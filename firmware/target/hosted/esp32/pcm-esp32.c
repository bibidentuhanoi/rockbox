#include "config.h"
#include "debug.h"
#include "system.h"
#include "sound.h"
#include "pcm.h"
#include "pcm-internal.h"
#include "pcm_sink.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

extern void *esp32_pcm_get_handle(void);
extern int   esp32_pcm_enable(void);
extern int   esp32_pcm_write(const void *data, unsigned int size,
                              unsigned int *bytes_written, int timeout_ms);

static volatile bool pcm_running = false;
static volatile bool pcm_paused  = false;
static volatile bool channel_enabled = false;

static SemaphoreHandle_t pcm_mutex = NULL;

static void esp32_pcm_lock(void)
{
    if (pcm_mutex)
        xSemaphoreTakeRecursive(pcm_mutex, portMAX_DELAY);
}

static void esp32_pcm_unlock(void)
{
    if (pcm_mutex)
        xSemaphoreGiveRecursive(pcm_mutex);
}

static const void *pcm_data = NULL;
static size_t      pcm_size = 0;

static void pcm_playback_task(void *arg)
{
    (void)arg;

    const uint8_t *cur_ptr  = NULL;
    size_t         cur_left = 0;

    for (;;) {
        if (!pcm_running || pcm_paused) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (cur_ptr == NULL || cur_left == 0) {
            esp32_pcm_lock();
            if (pcm_data != NULL && pcm_size != 0) {
                cur_ptr  = (const uint8_t *)pcm_data;
                cur_left = pcm_size;
                pcm_data = NULL;
                pcm_size = 0;
                esp32_pcm_unlock();
            } else {
                esp32_pcm_unlock();
                vTaskDelay(pdMS_TO_TICKS(5));
                continue;
            }
        }

        size_t chunk = cur_left;
        if (chunk > 2048) chunk = 2048;
        int16_t scaled_buf[1024];

        pcm_copy_buffer(scaled_buf, cur_ptr, chunk);

        unsigned int bytes_written = 0;
        int err = esp32_pcm_write(scaled_buf, chunk, &bytes_written, 500);

        if (bytes_written > 0) {
            cur_ptr  += bytes_written;
            cur_left -= bytes_written;
        } else if (err != 0) {
            DEBUGF("PCM: esp32_pcm_write error\n");
            vTaskDelay(pdMS_TO_TICKS(10));
        }

        if (cur_left == 0) {
            const void *next_data;
            size_t next_size;
            esp32_pcm_lock();
            bool got_more = pcm_play_dma_complete_callback(PCM_DMAST_OK,
                                                           &next_data, &next_size);
            if (got_more) {
                cur_ptr  = (const uint8_t *)next_data;
                cur_left = next_size;
                pcm_play_dma_status_callback(PCM_DMAST_STARTED);
            }
            esp32_pcm_unlock();
            if (!got_more) {
                cur_ptr  = NULL;
                cur_left = 0;
            }
        }
    }
}

void pcm_play_dma_init(void)
{
    pcm_mutex = xSemaphoreCreateRecursiveMutex();
    if (pcm_mutex == NULL) {
        DEBUGF("PCM: pcm_play_dma_init: failed to create mutex\n");
        return;
    }

    if (!esp32_pcm_get_handle()) {
        DEBUGF("PCM: pcm_play_dma_init: no I2S handle from platform\n");
        return;
    }

    xTaskCreatePinnedToCore(pcm_playback_task, "pcm_play", 8192,
                            NULL, configMAX_PRIORITIES - 1, NULL, 0);
}

void pcm_play_dma_postinit(void)
{
}

void pcm_play_dma_start(const void *addr, size_t size)
{
    if (!channel_enabled) {
        if (esp32_pcm_enable() != 0) {
            DEBUGF("PCM: esp32_pcm_enable failed\n");
            return;
        }
        channel_enabled = true;
    }

    esp32_pcm_lock();
    pcm_data    = addr;
    pcm_size    = size;
    pcm_paused  = false;
    pcm_running = true;
    esp32_pcm_unlock();
}

void pcm_play_dma_stop(void)
{
    esp32_pcm_lock();
    pcm_running = false;
    pcm_data    = NULL;
    pcm_size    = 0;
    esp32_pcm_unlock();
}

void pcm_play_dma_pause(bool pause)
{
    pcm_paused = pause;
}

size_t pcm_get_bytes_waiting(void)
{
    return pcm_size;
}

const void *pcm_play_dma_get_peak_buffer(int *count)
{
    esp32_pcm_lock();
    *count = pcm_size / 4;
    const void *data = pcm_data;
    esp32_pcm_unlock();
    return data;
}

void pcm_dma_apply_settings(void)
{
}

static const unsigned long esp32_sampr[] = { 44100 };
static void esp32_pcm_set_freq(uint16_t freq) { (void)freq; }

struct pcm_sink builtin_pcm_sink = {
    .caps = {
        .samprs      = esp32_sampr,
        .num_samprs  = 1,
        .default_freq = 0,
    },
    .ops = {
        .init     = pcm_play_dma_init,
        .postinit = pcm_play_dma_postinit,
        .set_freq = esp32_pcm_set_freq,
        .lock     = esp32_pcm_lock,
        .unlock   = esp32_pcm_unlock,
        .play     = pcm_play_dma_start,
        .stop     = pcm_play_dma_stop,
    },
};
