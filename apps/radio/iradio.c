/***************************************************************************
 *   Internet Radio for ESP32 Rockbox port
 *
 *   Station list parsed from .rockbox/iradio.ifmr (URL|Name format).
 *   Stream lifecycle managed via launcher functions resolved at ELF load.
 ***************************************************************************/
#include "config.h"

#ifdef HAVE_IRADIO

#include <string.h>
#include <stdio.h>
#include "iradio.h"
#include "lcd.h"
#include "font.h"
#include "action.h"
#include "kernel.h"
#include "splash.h"
#include "playlist.h"
#include "audio.h"
#include "viewport.h"

/* ── Launcher functions (resolved via ELF symbol table) ────────── */
extern int  wifi_manager_connect(void);
extern void wifi_manager_disconnect(void);
extern int  wifi_manager_is_connected(void);
extern void wifi_manager_power_save(bool enable);
extern int  http_stream_open(const char *url, char *name_out, size_t name_len);
extern int  http_stream_start(int (*write_fn)(const void *, size_t));
extern void http_stream_close(void);
extern void http_stream_destroy(void);
extern void stream_vfs_set_extension(const char *ext);
extern int  stream_vfs_write(const void *data, size_t len);
extern void stream_vfs_cancel(void);
extern void stream_vfs_signal_eof(void);
extern void stream_vfs_reset(void);
extern void stream_vfs_reconnect(void);
extern int  stream_vfs_wait_data(size_t min_bytes, int timeout_ms);
extern const char *stream_vfs_get_path(void);

typedef void (*icy_metadata_cb_t)(const char *title);
typedef void (*stream_event_cb_t)(int event);
#define STREAM_EVENT_DEAD    1
#define STREAM_EVENT_STARTED 2
typedef struct {
    icy_metadata_cb_t on_icy_metadata;
    stream_event_cb_t on_stream_event;
} http_stream_callbacks_t;
extern void http_stream_set_callbacks(const http_stream_callbacks_t *cb);

#define WIFI_MGR_OK       0
#define STREAM_FMT_UNKNOWN  0
#define STREAM_FMT_MP3      1
#define STREAM_FMT_OGG      2
#define STREAM_FMT_AAC      3

#define IRADIO_CFG_FILE ROCKBOX_DIR "/iradio.ifmr"
#define DEBOUNCE_TICKS  (HZ * 3 / 2)
#define MAX_RETRIES     3
#define RETRY_DELAY     (HZ * 2)

/* ── Station list ──────────────────────────────────────────────── */

static struct {
    char url[IRADIO_MAX_URL_LEN];
    char name[IRADIO_MAX_NAME_LEN];
} stations[IRADIO_MAX_STATIONS];

static int  station_count    = 0;
static int  selected         = 0;
static int  playing          = -1;
static bool streaming        = false;
static long debounce_tick    = 0;
static bool debounce_active  = false;
static int  retry_count      = 0;
static volatile bool stream_died = false;
/* Set once we observe AUDIO_STATUS_PLAY after start_stream(), cleared on stop.
 * Guards against false-restart during the brief pre-play window. */
static bool playing_confirmed = false;

static void on_stream_event(int event)
{
    if (event == STREAM_EVENT_DEAD && streaming) {
        /* Don't signal EOF — let the codec stall in vfs_read (ring empty)
           so we can reconnect transparently without restarting the codec. */
        stream_died = true;
    }
}

static int load_stations(void)
{
    FILE *f = fopen(IRADIO_CFG_FILE, "r");
    if (!f)
        return 0;

    char line[IRADIO_MAX_URL_LEN + IRADIO_MAX_NAME_LEN + 4];
    station_count = 0;

    while (fgets(line, sizeof(line), f) &&
           station_count < IRADIO_MAX_STATIONS)
    {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        char *cr = strchr(line, '\r');
        if (cr) *cr = '\0';

        if (line[0] == '#' || line[0] == '\0')
            continue;

        char *sep = strchr(line, '|');
        if (!sep) continue;

        *sep = '\0';
        strlcpy(stations[station_count].url,  line,    IRADIO_MAX_URL_LEN);
        strlcpy(stations[station_count].name, sep + 1, IRADIO_MAX_NAME_LEN);
        station_count++;
    }

    fclose(f);
    return station_count;
}

/* ── Stream control ────────────────────────────────────────────── */

static const char *stream_fmt_ext(int fmt)
{
    switch (fmt) {
        case STREAM_FMT_AAC:  return "aac";
        case STREAM_FMT_OGG:  return "ogg";
        default:              return "mp3";
    }
}

static void stop_stream(void)
{
    if (!streaming)
        return;
    stream_vfs_cancel();
    audio_stop();
    http_stream_close();
    stream_vfs_reset();
    streaming        = false;
    playing          = -1;
    playing_confirmed = false;
}

static bool resume_stream(int idx)
{
    if (idx < 0 || idx >= station_count)
        return false;

    http_stream_close();
    stream_vfs_reconnect();

    int fmt = http_stream_open(stations[idx].url, NULL, 0);
    if (fmt == STREAM_FMT_UNKNOWN)
        return false;

    http_stream_start(stream_vfs_write);
    streaming = true;
    return true;
}

static bool start_stream(int idx)
{
    if (idx < 0 || idx >= station_count)
        return false;

    stop_stream();
    /* Ensure codec/audio are stopped even when stop_stream was a no-op
       (streaming==false after stream_died).  Without this, the codec
       is still reading from the ring when stream_vfs_reset zeroes it. */
    audio_stop();
    splashf(0, "Tuning: %s", stations[idx].name);

    int fmt = http_stream_open(stations[idx].url, NULL, 0);
    if (fmt == STREAM_FMT_UNKNOWN) {
        splash(HZ * 2, "Connection failed");
        return false;
    }

    stream_vfs_reset();
    stream_vfs_set_extension(stream_fmt_ext(fmt));
    http_stream_start(stream_vfs_write);

    splashf(0, "Buffering: %s", stations[idx].name);
    stream_vfs_wait_data(84 * 1024, 30000);

    playlist_create("/", NULL);
    playlist_insert_track(NULL, stream_vfs_get_path(),
                          PLAYLIST_INSERT_LAST, false, true);
    playlist_start(0, 0, 0);

    playing   = idx;
    streaming = true;
    return true;
}

/* ── Screen drawing ────────────────────────────────────────────── */

static void draw_screen(void)
{
    struct viewport vp;
    viewport_set_defaults(&vp, SCREEN_MAIN);
    lcd_set_viewport(&vp);

    int font_h = font_get(lcd_getfont())->height;

    lcd_clear_viewport();

    int y = 1;
    lcd_putsxy(2, y, "Internet Radio");
    y += font_h + 4;

    if (station_count == 0) {
        lcd_putsxy(2, y, "No stations found");
        y += font_h;
        lcd_putsxy(2, y, "Edit iradio.ifmr");
    } else {
        int max_visible = (vp.height - y - font_h * 2) / font_h;
        if (max_visible < 1) max_visible = 1;
        if (max_visible > station_count) max_visible = station_count;

        int top = selected - max_visible / 2;
        if (top < 0) top = 0;
        if (top + max_visible > station_count)
            top = station_count - max_visible;
        if (top < 0) top = 0;

        for (int i = top; i < top + max_visible; i++) {
            char buf[IRADIO_MAX_NAME_LEN + 4];
            const char *mark = "  ";
            if (i == selected && i == playing)
                mark = ">#";
            else if (i == selected)
                mark = "> ";
            else if (i == playing)
                mark = " #";

            snprintf(buf, sizeof(buf), "%s%s", mark, stations[i].name);
            lcd_putsxy(0, y, buf);
            y += font_h;
        }
    }

    y = vp.height - font_h - 2;
    if (streaming && playing >= 0) {
        char buf[IRADIO_MAX_NAME_LEN + 12];
        snprintf(buf, sizeof(buf), "Playing: %s", stations[playing].name);
        lcd_putsxy(2, y, buf);
    } else if (debounce_active) {
        lcd_putsxy(2, y, "Tuning...");
    } else {
        lcd_putsxy(2, y, "OK=Play  Back=Exit");
    }

    lcd_update_viewport();
    lcd_set_viewport(NULL);
}

/* ── Main screen ───────────────────────────────────────────────── */

int iradio_screen(void)
{
    if (!wifi_manager_is_connected()) {
        splash(0, "Connecting WiFi...");
        if (wifi_manager_connect() != WIFI_MGR_OK) {
            splash(HZ * 2, "WiFi connection failed");
            return 0;
        }
    }
    wifi_manager_power_save(0);

    if (station_count == 0)
        load_stations();

    if (station_count == 0) {
        splash(HZ * 2, "No stations in iradio.ifmr");
        return 0;
    }

    debounce_active = false;
    stream_died = false;
    retry_count = 0;

    http_stream_callbacks_t cb = { .on_stream_event = on_stream_event };
    http_stream_set_callbacks(&cb);

    bool done = false;

    while (!done) {
        if (stream_died) {
            stream_died = false;
            if (retry_count < MAX_RETRIES && playing >= 0) {
                retry_count++;
                if (!resume_stream(playing)) {
                    /* Reconnect failed — fall back to full restart */
                    stop_stream();
                    start_stream(playing);
                }
            } else {
                stop_stream();
                splash(HZ * 2, "Connection lost");
                playing = -1;
                retry_count = 0;
            }
        }

        /* Detect codec exit (e.g. audiobuf cycling stall) while HTTP is alive.
         * playing_confirmed prevents false restart in the pre-play startup window. */
        if (streaming && playing >= 0 && !debounce_active) {
            if (audio_status() & AUDIO_STATUS_PLAY) {
                playing_confirmed = true;
                if (retry_count > 0)
                    retry_count = 0;
            } else if (playing_confirmed) {
                playing_confirmed = false;
                streaming = false;
                retry_count = 0;
                debounce_active = true;
                debounce_tick = current_tick + RETRY_DELAY;
            }
        }

        draw_screen();

        int timeout = debounce_active ? HZ / 10 : HZ / 2;
        int action  = get_action(CONTEXT_STD, timeout);

        switch (action) {
        case ACTION_STD_PREV:
            selected = (selected - 1 + station_count) % station_count;
            debounce_active = true;
            debounce_tick   = current_tick + DEBOUNCE_TICKS;
            break;

        case ACTION_STD_NEXT:
            selected = (selected + 1) % station_count;
            debounce_active = true;
            debounce_tick   = current_tick + DEBOUNCE_TICKS;
            break;

        case ACTION_STD_OK:
            debounce_active = false;
            retry_count = 0;
            if (playing != selected)
                start_stream(selected);
            break;

        case ACTION_STD_CANCEL:
            if (streaming) {
                stop_stream();
                debounce_active = false;
            } else if (debounce_active) {
                debounce_active = false;
                retry_count = 0;
            } else {
                done = true;
            }
            break;

        default:
            break;
        }

        if (debounce_active && TIME_AFTER(current_tick, debounce_tick)) {
            debounce_active = false;
            if (selected != playing || !streaming)
                start_stream(selected);
        }
    }

    stop_stream();
    http_stream_set_callbacks(NULL);
    wifi_manager_disconnect();
    return 0;
}

#endif /* HAVE_IRADIO */
