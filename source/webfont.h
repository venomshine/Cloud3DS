/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

/* Glifos de fuentes web (los iconos de las fuentes propias de una página): TrueType, OpenType y WOFF
 * (WOFF2 no). El glifo de cp, en un cuadrado de px × px, del color rgb (0xRRGGBB) y transparente
 * alrededor (RGBA, malloc). */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool webfont_glyph(const uint8_t *font, size_t n, uint32_t cp, int px, uint32_t rgb, uint8_t **rgba, int *w, int *h);
