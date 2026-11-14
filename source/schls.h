/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Streaming de audio por HLS con fMP4 (lo que entrega SoundCloud): la lista m3u8 y sus trozos. Portable (sin nada de
 * la 3DS) para poder probarlo en el PC.
 * - hls_parse: la lista (de medios) → la URI del init (EXT-X-MAP) y las de cada trozo con su duración.
 * - fmp4_init: el init.mp4 → la configuración del AAC (AudioSpecificConfig del esds), la escala de tiempo y lo que
 *   valga por defecto (trex).
 * - fmp4_frames: un trozo (moof + mdat) → cada trama AAC, en orden, a una función. */

typedef struct {
	char *uri;
	uint32_t ms; /* duración */
} HlsSeg;

typedef struct {
	char *init;  /* EXT-X-MAP (NULL si no hay) */
	HlsSeg *seg;
	int n;
	uint32_t total_ms;
	bool master; /* era una lista maestra: variant tiene la de medios */
	char *variant;
} HlsList;

bool hls_parse(const char *text, const char *base, HlsList *out);
void hls_free(HlsList *l);

typedef struct {
	uint8_t asc[64];
	int asc_len;
	uint32_t timescale, def_size, def_dur;
	int rate, chans;
} Fmp4Cfg;

bool fmp4_init(const uint8_t *d, size_t n, Fmp4Cfg *cfg);
/* llama a cb con cada trama; devuelve cuántas (o -1 si el trozo no se entiende). cb devuelve false para parar */
int fmp4_frames(const uint8_t *d, size_t n, const Fmp4Cfg *cfg, bool (*cb)(const uint8_t *f, size_t len, void *u), void *u);
