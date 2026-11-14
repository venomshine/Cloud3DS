/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "schls.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------- m3u8 */

static char *dupn(const char *s, size_t n)
{
	char *o = malloc(n + 1);
	if (o)
		memcpy(o, s, n), o[n] = 0;
	return o;
}

// una URI de la lista, absoluta: tal cual, desde la raíz del servidor o junto a la lista
static char *absolute(const char *base, const char *s, size_t n)
{
	if ((n > 7 && !strncmp(s, "http://", 7)) || (n > 8 && !strncmp(s, "https://", 8)))
		return dupn(s, n);
	const char *host = strstr(base, "://");
	if (!host)
		return dupn(s, n);
	size_t keep;
	if (s[0] == '/') {
		const char *p = strchr(host + 3, '/');
		keep = p ? (size_t)(p - base) : strlen(base);
	} else {
		const char *q = strchr(base, '?');
		size_t bl = q ? (size_t)(q - base) : strlen(base);
		const char *sl = base + bl;
		while (sl > host + 3 && sl[-1] != '/')
			sl--;
		keep = (size_t)(sl - base);
	}
	char *o = malloc(keep + n + 1);
	if (o) {
		memcpy(o, base, keep);
		memcpy(o + keep, s, n);
		o[keep + n] = 0;
	}
	return o;
}

bool hls_parse(const char *text, const char *base, HlsList *out)
{
	memset(out, 0, sizeof *out);
	if (strncmp(text, "#EXTM3U", 7))
		return false;
	int cap = 0;
	uint32_t dur = 0;
	for (const char *p = text; *p;) {
		const char *e = p;
		while (*e && *e != '\n' && *e != '\r')
			e++;
		size_t n = (size_t)(e - p);
		if (n) {
			if (!strncmp(p, "#EXT-X-KEY:", 11) && !strstr(p, "METHOD=NONE")) { // cifrada: no se puede
				hls_free(out);
				return false;
			} else if (!strncmp(p, "#EXT-X-MAP:", 11)) {
				const char *u = strstr(p, "URI=\"");
				if (u && u < e) {
					u += 5;
					const char *q = memchr(u, '"', (size_t)(e - u));
					if (q)
						out->init = absolute(base, u, (size_t)(q - u));
				}
			} else if (!strncmp(p, "#EXT-X-STREAM-INF", 17)) {
				out->master = true;
			} else if (!strncmp(p, "#EXTINF:", 8)) {
				dur = (uint32_t)(atof(p + 8) * 1000 + 0.5);
			} else if (p[0] != '#') {
				if (out->master) {
					if (!out->variant)
						out->variant = absolute(base, p, n);
				} else {
					if (out->n == cap) {
						cap = cap ? cap * 2 : 64;
						HlsSeg *s = realloc(out->seg, sizeof *s * cap);
						if (!s) {
							hls_free(out);
							return false;
						}
						out->seg = s;
					}
					out->seg[out->n].uri = absolute(base, p, n);
					out->seg[out->n].ms = dur;
					out->total_ms += dur;
					out->n++;
					dur = 0;
				}
			}
		}
		p = e;
		while (*p == '\n' || *p == '\r')
			p++;
	}
	return out->master ? out->variant != NULL : out->n > 0;
}

void hls_free(HlsList *l)
{
	for (int i = 0; i < l->n; i++)
		free(l->seg[i].uri);
	free(l->seg);
	free(l->init);
	free(l->variant);
	memset(l, 0, sizeof *l);
}

/* ---------------------------------------------------------------- fMP4 (ISO BMFF) */

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

typedef struct {
	const uint8_t *body; // contenido (tras la cabecera)
	size_t len;
	uint32_t type;
} Box;

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

// la caja en d+*off; avanza *off. false al acabar o si no cabe
static bool next_box(const uint8_t *d, size_t n, size_t *off, Box *b)
{
	if (*off + 8 > n)
		return false;
	const uint8_t *p = d + *off;
	uint64_t sz = be32(p);
	size_t hdr = 8;
	if (sz == 1) {
		if (*off + 16 > n)
			return false;
		sz = (uint64_t)be32(p + 8) << 32 | be32(p + 12);
		hdr = 16;
	} else if (sz == 0)
		sz = n - *off;
	if (sz < hdr || *off + sz > n)
		return false;
	b->type = be32(p + 4);
	b->body = p + hdr;
	b->len = (size_t)sz - hdr;
	*off += (size_t)sz;
	return true;
}

static bool child(const uint8_t *d, size_t n, uint32_t type, Box *out)
{
	size_t off = 0;
	Box b;
	while (next_box(d, n, &off, &b))
		if (b.type == type) {
			*out = b;
			return true;
		}
	return false;
}

// un descriptor del esds (etiqueta y tamaño de longitud variable)
static bool desc(const uint8_t **p, const uint8_t *end, uint8_t *tag, size_t *len)
{
	if (*p >= end)
		return false;
	*tag = *(*p)++;
	size_t l = 0;
	for (int i = 0; i < 4 && *p < end; i++) {
		uint8_t c = *(*p)++;
		l = l << 7 | (c & 0x7F);
		if (!(c & 0x80))
			break;
	}
	*len = l;
	return *p + l <= end;
}

static bool parse_esds(const uint8_t *p, size_t n, Fmp4Cfg *c)
{
	const uint8_t *end = p + n;
	p += 4; // versión y banderas
	uint8_t tag;
	size_t len;
	if (!desc(&p, end, &tag, &len) || tag != 3) // ES_Descriptor
		return false;
	p += 2; // ES_ID
	uint8_t fl = *p++;
	if (fl & 0x80)
		p += 2;
	if (fl & 0x40)
		p += 1 + *p;
	if (fl & 0x20)
		p += 2;
	if (!desc(&p, end, &tag, &len) || tag != 4) // DecoderConfigDescriptor
		return false;
	const uint8_t *dc_end = p + len;
	p += 13;
	if (p > dc_end || !desc(&p, dc_end, &tag, &len) || tag != 5 || len > sizeof c->asc) // DecoderSpecificInfo
		return false;
	memcpy(c->asc, p, len);
	c->asc_len = (int)len;
	return true;
}

bool fmp4_init(const uint8_t *d, size_t n, Fmp4Cfg *c)
{
	memset(c, 0, sizeof *c);
	Box moov, trak, mdia, mdhd, minf, stbl, stsd, mp4a, esds, mvex, trex;
	if (!child(d, n, FOURCC('m', 'o', 'o', 'v'), &moov) || !child(moov.body, moov.len, FOURCC('t', 'r', 'a', 'k'), &trak) ||
	    !child(trak.body, trak.len, FOURCC('m', 'd', 'i', 'a'), &mdia))
		return false;
	if (child(mdia.body, mdia.len, FOURCC('m', 'd', 'h', 'd'), &mdhd) && mdhd.len >= 24)
		c->timescale = mdhd.body[0] == 1 ? be32(mdhd.body + 20) : be32(mdhd.body + 12);
	if (!child(mdia.body, mdia.len, FOURCC('m', 'i', 'n', 'f'), &minf) || !child(minf.body, minf.len, FOURCC('s', 't', 'b', 'l'), &stbl) ||
	    !child(stbl.body, stbl.len, FOURCC('s', 't', 's', 'd'), &stsd) || stsd.len < 8)
		return false;
	// stsd: versión, cuántas y la primera entrada (mp4a: 28 bytes de la entrada de audio y luego sus cajas)
	if (!child(stsd.body + 8, stsd.len - 8, FOURCC('m', 'p', '4', 'a'), &mp4a) || mp4a.len < 28)
		return false;
	c->chans = be16(mp4a.body + 16);
	c->rate = be16(mp4a.body + 24);
	if (!child(mp4a.body + 28, mp4a.len - 28, FOURCC('e', 's', 'd', 's'), &esds) || !parse_esds(esds.body, esds.len, c))
		return false;
	if (child(moov.body, moov.len, FOURCC('m', 'v', 'e', 'x'), &mvex) && child(mvex.body, mvex.len, FOURCC('t', 'r', 'e', 'x'), &trex) && trex.len >= 24) {
		c->def_dur = be32(trex.body + 12);
		c->def_size = be32(trex.body + 16);
	}
	return c->asc_len > 0;
}

int fmp4_frames(const uint8_t *d, size_t n, const Fmp4Cfg *c, bool (*cb)(const uint8_t *, size_t, void *), void *u)
{
	size_t off = 0, moof_off = 0;
	Box b;
	int total = 0;
	while (true) {
		size_t at = off;
		if (!next_box(d, n, &off, &b))
			break;
		if (b.type != FOURCC('m', 'o', 'o', 'f'))
			continue;
		moof_off = at;
		Box traf, tfhd, trun;
		if (!child(b.body, b.len, FOURCC('t', 'r', 'a', 'f'), &traf) || !child(traf.body, traf.len, FOURCC('t', 'f', 'h', 'd'), &tfhd) ||
		    !child(traf.body, traf.len, FOURCC('t', 'r', 'u', 'n'), &trun) || tfhd.len < 8 || trun.len < 8)
			return -1;
		uint32_t tf = be32(tfhd.body) & 0xFFFFFF, def_size = c->def_size;
		const uint8_t *q = tfhd.body + 8;
		size_t base = moof_off; // (default-base-is-moof, o sin base explícita: el principio del moof)
		if (tf & 0x01) {
			base = (size_t)((uint64_t)be32(q) << 32 | be32(q + 4));
			q += 8;
		}
		if (tf & 0x02)
			q += 4;
		if (tf & 0x08)
			q += 4;
		if (tf & 0x10)
			def_size = be32(q), q += 4;
		uint32_t rf = be32(trun.body) & 0xFFFFFF, cnt = be32(trun.body + 4);
		const uint8_t *r = trun.body + 8, *rend = trun.body + trun.len;
		int32_t data_off = 0;
		if (rf & 0x01)
			data_off = (int32_t)be32(r), r += 4;
		if (rf & 0x04)
			r += 4;
		size_t pos = base + (size_t)data_off;
		for (uint32_t i = 0; i < cnt; i++) {
			uint32_t sz = def_size;
			if (rf & 0x100)
				r += 4;
			if (rf & 0x200) {
				if (r + 4 > rend)
					return -1;
				sz = be32(r), r += 4;
			}
			if (rf & 0x400)
				r += 4;
			if (rf & 0x800)
				r += 4;
			if (!sz || pos + sz > n)
				return -1;
			total++;
			if (!cb(d + pos, sz, u))
				return total;
			pos += sz;
		}
	}
	return total;
}
