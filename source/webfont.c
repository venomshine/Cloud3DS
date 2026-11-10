/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "webfont.h"

#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb/stb_truetype.h"

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

// WOFF (1): las tablas de una TrueType/OpenType, cada una comprimida con zlib. Se rehace la fuente
static uint8_t *woff_to_sfnt(const uint8_t *w, size_t n, size_t *out_n)
{
	if (n < 44 || memcmp(w, "wOFF", 4))
		return NULL;
	uint16_t nt = be16(w + 12);
	uint32_t total = be32(w + 16);
	if (!nt || nt > 64 || n < 44 + (size_t)nt * 20 || total > 4u << 20)
		return NULL;
	size_t hdr = 12 + (size_t)nt * 16, cap = hdr + total + (size_t)nt * 4;
	uint8_t *s = calloc(1, cap);
	if (!s)
		return NULL;
	put32(s, be32(w + 4)); // flavor
	put16(s + 4, nt);
	uint16_t es = 0, p2 = 1;
	while ((uint16_t)(p2 * 2) <= nt)
		p2 *= 2, es++;
	put16(s + 6, (uint16_t)(p2 * 16)), put16(s + 8, es), put16(s + 10, (uint16_t)(nt * 16 - p2 * 16));
	size_t off = hdr;
	for (int i = 0; i < nt; i++) {
		const uint8_t *e = w + 44 + i * 20;
		uint32_t to = be32(e + 4), cl = be32(e + 8), ol = be32(e + 12);
		if ((size_t)to + cl > n || off + ol > cap) {
			free(s);
			return NULL;
		}
		if (cl < ol) {
			uLongf dl = ol;
			if (uncompress(s + off, &dl, w + to, cl) != Z_OK || dl != ol) {
				free(s);
				return NULL;
			}
		} else
			memcpy(s + off, w + to, ol);
		uint8_t *d = s + 12 + i * 16;
		memcpy(d, e, 4);
		put32(d + 4, be32(e + 16));
		put32(d + 8, (uint32_t)off);
		put32(d + 12, ol);
		off += (ol + 3) & ~3u;
	}
	*out_n = off;
	return s;
}

bool webfont_glyph(const uint8_t *font, size_t n, uint32_t cp, int px, uint32_t rgb, uint8_t **rgba, int *ow, int *oh)
{
	*rgba = NULL;
	if (!font || n < 12 || px < 4 || px > 256)
		return false;
	uint8_t *sf = NULL;
	size_t sn = 0;
	const uint8_t *f = font;
	if (!memcmp(font, "wOFF", 4)) {
		sf = woff_to_sfnt(font, n, &sn);
		if (!sf)
			return false;
		f = sf;
	} else if (!memcmp(font, "wOF2", 4)) // (WOFF2: otra compresión, no se sabe leer)
		return false;
	stbtt_fontinfo fi;
	bool ok = false;
	if (stbtt_InitFont(&fi, f, stbtt_GetFontOffsetForIndex(f, 0))) {
		int g = stbtt_FindGlyphIndex(&fi, (int)cp);
		if (g > 0) {
			float sc = stbtt_ScaleForMappingEmToPixels(&fi, (float)px);
			int x0, y0, x1, y1;
			stbtt_GetGlyphBitmapBox(&fi, g, sc, sc, &x0, &y0, &x1, &y1);
			int gw = x1 - x0, gh = y1 - y0;
			uint8_t *a = gw > 0 && gh > 0 && gw <= 512 && gh <= 512 ? calloc(1, (size_t)gw * gh) : NULL;
			uint8_t *o = calloc(1, (size_t)px * px * 4);
			if (a && o) {
				stbtt_MakeGlyphBitmap(&fi, a, gw, gh, gw, sc, sc, g);
				// en un cuadrado de 1em, con la línea de base donde la pone la fuente (como el texto)
				int asc, desc, gap;
				stbtt_GetFontVMetrics(&fi, &asc, &desc, &gap);
				float em = (float)(asc - desc);
				int base = em > 0 ? (int)(px * (float)asc / em + 0.5f) : px * 4 / 5;
				int adv, lsb;
				stbtt_GetGlyphHMetrics(&fi, g, &adv, &lsb);
				int ox = (px - gw) / 2, oy = base + y0;
				if (oy < 0)
					oy = 0;
				if (oy + gh > px)
					oy = px - gh > 0 ? px - gh : 0;
				for (int y = 0; y < gh && oy + y < px; y++)
					for (int x = 0; x < gw && ox + x < px; x++) {
						if (ox + x < 0)
							continue;
						uint8_t *d = o + ((size_t)(oy + y) * px + ox + x) * 4;
						d[0] = (uint8_t)(rgb >> 16), d[1] = (uint8_t)(rgb >> 8), d[2] = (uint8_t)rgb, d[3] = a[y * gw + x];
					}
				*rgba = o, *ow = px, *oh = px;
				ok = true;
				o = NULL;
			}
			free(a), free(o);
		}
	}
	free(sf);
	return ok;
}
