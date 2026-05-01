/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * Copyright (c) 2025 Rockbox ESP32 port
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This software is distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY
 * KIND, either express or implied.
 *
 ****************************************************************************/

#include "config.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

#define TAG "Rockbox"

void debug_init(void) {}

static void log_buf(const char *buf)
{
    if (strncmp(buf, "WARNING:", 8) == 0)
        ESP_LOGW(TAG, "%s", buf + 9);
    else if (strncmp(buf, "ERROR:", 6) == 0)
        ESP_LOGE(TAG, "%s", buf + 7);
    else
        ESP_LOGI(TAG, "%s", buf);
}

static char debugmembuf[256];

void debugf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(debugmembuf, sizeof(debugmembuf), fmt, ap);
    va_end(ap);
    log_buf(debugmembuf);
}

void ldebugf(const char* file, int line, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(debugmembuf, sizeof(debugmembuf), "%s:%d ", file, line);
    vsnprintf(debugmembuf + n, sizeof(debugmembuf) - n, fmt, ap);
    va_end(ap);
    log_buf(debugmembuf);
}
