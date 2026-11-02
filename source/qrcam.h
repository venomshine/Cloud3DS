/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Cámara para leer códigos QR (como en FBI): un hilo captura 400×240 y busca
 * un QR en cada imagen con quirc. La imagen se puede enseñar arriba. */

#define QRCAM_W 400
#define QRCAM_H 240

enum { QRC_OFF, QRC_STARTING, QRC_RUN, QRC_FAIL };

bool qrcam_start(bool inner);   /* inner: la cámara de dentro */
void qrcam_stop(void);
int  qrcam_state(Result *err);
/* copia la última imagen (RGB565) si hay una nueva desde *seen; true si se copió */
bool qrcam_frame(u16 *out, u32 *seen);
/* el texto del último QR leído (y se olvida); false si aún no hay */
bool qrcam_take(char *out, size_t n);
