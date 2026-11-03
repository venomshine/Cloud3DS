/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

/* pinta un SVG para que quepa en maxw × maxh; *rgba (RGBA, se libera con free) */
bool svg_render(const char *data, size_t n, int maxw, int maxh, unsigned char **rgba, int *w, int *h);
