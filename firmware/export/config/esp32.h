/*
 * This config file is for the ESP32-S3 hosted application
 * Based on firmware/export/config/ctru.h
 */

/* We don't run on hardware directly */
#define CONFIG_PLATFORM (PLATFORM_HOSTED|PLATFORM_ESP32)
#define HAVE_FPU

/* Model identification */
#define MODEL_NUMBER  101
#define MODEL_NAME    "ESP32"

#define USB_NONE

#define CONFIG_CPU    ESP32S3

#define CPU_FREQ      240000000

/* define this if you have a colour LCD */
#define HAVE_LCD_COLOR

/* define this if you want album art for this target */
#define HAVE_ALBUMART

/* define this to enable bitmap scaling */
#define HAVE_BMP_SCALING

/* define this to enable JPEG decoding */
#define HAVE_JPEG

/* define this if you have access to the quickscreen */
#define HAVE_QUICKSCREEN

/* define this if you would like tagcache to build on this target */
#define HAVE_TAGCACHE

/* LCD dimensions — ILI9341 native portrait (240×320).
 * MADCTL is set by esp_lcd_panel_init() via rgb_ele_order=RGB in lcd-bitmap.c. */
#define LCD_WIDTH  240
#define LCD_HEIGHT 320

#define LCD_DEPTH  16
#define LCD_PIXELFORMAT RGB565

#define LCD_OPTIMIZED_UPDATE
#define LCD_OPTIMIZED_UPDATE_RECT
#define LCD_OPTIMIZED_BLIT_YUV

/* define this to indicate your device's keypad */
#define CONFIG_KEYPAD ESP32_PAD

#define CONFIG_RTC RTC_HOSTED

/* Power management */
#define CONFIG_BATTERY_MEASURE PERCENTAGE_MEASURE
#define CONFIG_CHARGING        CHARGING_MONITOR
#define HAVE_SW_POWEROFF

/* The number of bytes reserved for loadable codecs */
#define CODEC_SIZE 0x100000

/* The number of bytes reserved for loadable plugins */
#define PLUGIN_BUFFER_SIZE 0x80000

#define AB_REPEAT_ENABLE

/* Define this for LCD backlight available */
#define HAVE_BACKLIGHT
#define HAVE_BACKLIGHT_BRIGHTNESS

/* Main LCD backlight brightness range and defaults */
#define MIN_BRIGHTNESS_SETTING      0
#define MAX_BRIGHTNESS_SETTING      100
#define BRIGHTNESS_STEP             5
#define DEFAULT_BRIGHTNESS_SETTING  80
#define CONFIG_BACKLIGHT_FADING     BACKLIGHT_FADING_SW_SETTING

#define CONFIG_LCD LCD_COWOND2

/* Define this if a programmable hotkey is mapped */
#define HAVE_HOTKEY

#define BOOTDIR "/rockbox/.rockbox"

/* No special storage */
#define CONFIG_STORAGE STORAGE_HOSTFS
#define HAVE_STORAGE_FLUSH

/* Audio — I2S output via MAX98357A */
#define HAVE_ESP32_AUDIO
#define CONFIG_CODEC    SWCODEC
#define HW_SAMPR_CAPS   (SAMPR_CAP_44)
/* No hardware codec: all audio processing is software */
#define AUDIOHW_CAPS    0
/* Software volume control — Rockbox's pcm_sw_volume system handles
   dB-to-linear conversion and fixed-point scaling in pcm_copy_buffer().
   UNBUFFERED because our PCM driver does its own buffering (task loop).
   FRACBITS 16 gives -79..+0 dB range with no large-integer math. */
#define HAVE_SW_VOLUME_CONTROL
#define PCM_SW_VOLUME_UNBUFFERED
#define PCM_SW_VOLUME_FRACBITS  16

/* Phase 3.2 — Software DSP pipeline: EQ, tone controls, resampler */
#define HAVE_SW_TONE_CONTROLS   /* Bass/treble sliders in Sound Settings */
#define HAVE_PITCHCONTROL       /* Activates resampler path; also enables pitch UI */
#define HAVE_EQ                 /* 10-band parametric EQ in Sound Settings */

/* Phase 3.3 — Extended DSP effects (each independently toggleable in UI) */
#define HAVE_CROSSFEED      /* BS2B headphone crossfeed imaging (~2% CPU) */
#define HAVE_STEREO_WIDTH   /* Mid-side stereo width adjustment (<1% CPU) */
#define HAVE_COMPRESSOR     /* Single-band RMS compressor/limiter (~3% CPU) */

/* Memory */
#define MEMORYSIZE 4  /* 3 MB audio buffer — must be large enough to buffer full FLAC
                         tracks, because a Rockbox threading deadlock between the codec
                         and audio threads prevents the buffering thread from refilling
                         mid-track when old handles haven't been freed yet. */

/* ESP32 hosted port needs extra thread slots beyond BASETHREADS(16) */
#define TARGET_EXTRA_THREADS 8

/* Internet radio via HTTP streaming + VFS shim */
#define HAVE_IRADIO

/* Safety: ESP32 is Xtensa, not ARM — prevent false CPU_ARM detection */
#undef CPU_ARM
#undef HAVE_RB_BACKTRACE

/* System font dimensions (normally from generated sysfont.h) */
#ifndef SYSFONT_WIDTH
#define SYSFONT_WIDTH  6
#define SYSFONT_HEIGHT 8
#endif

/* Filesystem root — ESP-IDF VFS has no listable "/" so point browser here */
#define TREE_ROOT           "/rockbox"
#define ROCKBOX_DIR         "/rockbox/.rockbox"
#define ROCKBOX_SHARE_PATH  "/rockbox/.rockbox"
#define ROCKBOX_BINARY_PATH "/rockbox/.rockbox"
#define ROCKBOX_LIBRARY_PATH "/rockbox/.rockbox/lib"
