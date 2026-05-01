# Rockbox ESP32-S3 Port

Fork of [Rockbox](https://www.rockbox.org/) with an ESP32-S3 hosted target.

## What this is

Rockbox running on ESP32-S3 via ESP-IDF. The firmware loads Rockbox as an ELF binary through dlopen, with the platform (LCD, buttons, I2S audio, power) handled by the main app.

## Hardware

- ESP32-S3 (PSRAM required)
- ILI9341 240x320 LCD over SPI
- MAX98357A I2S DAC
- LittleFS on flash for storage
- UART input for buttons (dev setup)

## Features

- Full Rockbox UI and playback engine
- Internet radio (HTTP/ICY streaming with AAC, MP3, Ogg)
- Software volume, 10-band EQ, crossfeed, compressor
- Multi-app boot menu with carousel selector
- Deep sleep power off

## Building

This is an ESP-IDF component. See the parent project for build instructions.

## Upstream

Based on upstream Rockbox, synced periodically. ESP32-specific changes are kept minimal and guarded with `#ifdef ESP32`.
