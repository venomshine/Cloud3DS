/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Fotos para el explorador de la consola: PNG, JPEG, BMP y GIF (primer
 * fotograma). Se reducen para que quepan en maxw × maxh. */

typedef enum { IMG_OK, IMG_BAD, IMG_TOO_BIG } ImgResult;

/* *rgba se libera con free(). ow/oh: tamaño original. */
ImgResult img_load_fit(const char *path, int maxw, int maxh, u8 **rgba, int *w, int *h, int *ow, int *oh);
bool img_size(const char *path, int *w, int *h);
