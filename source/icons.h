/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once
#include <3ds.h>

typedef enum {
    IC_MENU, IC_PLUS, IC_SEND, IC_MIC, IC_CAMERA, IC_GLOBE, IC_IMAGE, IC_GEAR, IC_TRASH,
    IC_PIN, IC_EDIT, IC_CHAT, IC_STOP, IC_CHEV_DOWN, IC_CHEV_RIGHT, IC_CHEV_LEFT, IC_CHECK,
    IC_BULB, IC_CLOCK, IC_CLOSE, IC_REFRESH, IC_QR, IC_SPEAKER, IC_SEARCH, IC_GEM, IC_USER,
    IC_KEY, IC_BOLT, IC_PEN, IC_BOOK, IC_MAP, IC_CODE, IC_INFO, IC_LOGOUT, IC_PALETTE,
    IC_TEXTSIZE, IC_NOTE, IC_UP, IC_DOWN, IC_WIFI, IC_SPARK, IC_FLIP, IC_STAR, IC_LINK,
    IC_CLOUD, IC_FOLDER, IC_UPLOAD, IC_DOWNLOAD, IC_PHONE, IC_LAPTOP, IC_TABLET, IC_DEVICES,
    IC_LOCK, IC_WIFI_OFF, IC_POWER, IC_SHIELD, IC_HEART, IC_FILE, IC_ARCHIVE, IC_GAMEPAD, IC_FONT, IC_SORT, IC_GRID, IC_LIST
} IconId;

// dibuja un ícono vectorial centrado en (cx,cy) dentro de un cuadrado de lado s
void draw_icon(IconId id, float cx, float cy, float s, u32 c);
