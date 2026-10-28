/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_NAME     "Cloud3DS"
#define APP_VERSION  "1.1.0"
#define APP_DIR      "sdmc:/3ds/Cloud3DS"
#define CONFIG_PATH  APP_DIR "/config.json"
#define LOG_PATH     APP_DIR "/net.log"

#define LINK_GITHUB  "https://github.com/venomshine"
#define LINK_DISCORD "https://discord.com/invite/yVKp8JwV4e"
#define LINK_KOFI    "https://ko-fi.com/venomshine"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
