#pragma once

#include <3ds.h>
#include <stdbool.h>
#include <stdint.h>

#define APP_NAME     "Cloud3DS"
#define APP_VERSION  "1.0.0"
#define APP_DIR      "sdmc:/3ds/Cloud3DS"
#define CONFIG_PATH  APP_DIR "/config.json"
#define LOG_PATH     APP_DIR "/net.log"

#define LINK_GITHUB  "https://github.com/venomshine"
#define LINK_DISCORD "https://discord.com/invite/yVKp8JwV4e"
#define LINK_KOFI    "https://ko-fi.com/venomshine"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))
