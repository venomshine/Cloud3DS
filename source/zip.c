/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "zip.h"
#include "fsx.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <zlib.h>

#define BUF (64 * 1024)
#define MAX_ENTRIES 65535 // al leer; al crear, hasta 20000
#define MAX_PLAN 20000

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }
static void wr16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void wr32(uint8_t *p, uint32_t v) { wr16(p, v), wr16(p + 2, v >> 16); }

static FILE *zopen(const char *path, const char *mode)
{
	char p[1100];
	snprintf(p, sizeof p, ZIP_ROOT "%s", path);
	return fopen(p, mode);
}

static void join(char *out, size_t n, const char *dir, const char *name)
{
	snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", name);
}

uint32_t zip_crc(uint32_t crc, const void *p, size_t n)
{
	return (uint32_t)crc32(crc, p, (uInt)n);
}

/* ---------------------------------------------------------------- nombres */

/* CP437 (los ZIP sin la marca de UTF-8, como los del Explorador de Windows antiguo) */
static const uint16_t CP437[128] = {
	0xC7, 0xFC, 0xE9, 0xE2, 0xE4, 0xE0, 0xE5, 0xE7, 0xEA, 0xEB, 0xE8, 0xEF, 0xEE, 0xEC, 0xC4, 0xC5,
	0xC9, 0xE6, 0xC6, 0xF4, 0xF6, 0xF2, 0xFB, 0xF9, 0xFF, 0xD6, 0xDC, 0xA2, 0xA3, 0xA5, 0x20A7, 0x192,
	0xE1, 0xED, 0xF3, 0xFA, 0xF1, 0xD1, 0xAA, 0xBA, 0xBF, 0x2310, 0xAC, 0xBD, 0xBC, 0xA1, 0xAB, 0xBB,
	0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557, 0x255D, 0x255C, 0x255B, 0x2510,
	0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F, 0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567,
	0x2568, 0x2564, 0x2565, 0x2559, 0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
	0x3B1, 0xDF, 0x393, 0x3C0, 0x3A3, 0x3C3, 0xB5, 0x3C4, 0x3A6, 0x398, 0x3A9, 0x3B4, 0x221E, 0x3C6, 0x3B5, 0x2229,
	0x2261, 0xB1, 0x2265, 0x2264, 0x2320, 0x2321, 0xF7, 0x2248, 0xB0, 0x2219, 0xB7, 0x221A, 0x207F, 0xB2, 0x25A0, 0xA0};

static char *name_dup(const uint8_t *p, size_t n, bool utf8)
{
	char *s = malloc(n * 3 + 1), *o = s;
	if (!s)
		return NULL;
	for (size_t i = 0; i < n; i++) {
		uint32_t c = p[i];
		if (c < 0x80 || utf8) {
			*o++ = (char)c;
			continue;
		}
		c = CP437[c - 0x80];
		if (c < 0x800)
			*o++ = (char)(0xC0 | c >> 6), *o++ = (char)(0x80 | (c & 63));
		else
			*o++ = (char)(0xE0 | c >> 12), *o++ = (char)(0x80 | ((c >> 6) & 63)), *o++ = (char)(0x80 | (c & 63));
	}
	*o = 0;
	return s;
}

bool zip_safe_name(const char *in, char *out, size_t n)
{
	size_t o = 0;
	const char *p = in;
	while (*p) {
		while (*p == '/' || *p == '\\')
			p++;
		const char *s = p;
		while (*p && *p != '/' && *p != '\\')
			p++;
		size_t l = (size_t)(p - s);
		if (!l || (l == 1 && s[0] == '.'))
			continue;
		if (l == 2 && s[0] == '.' && s[1] == '.') // nada de salir de la carpeta
			continue;
		if (o && o + 1 < n)
			out[o++] = '/';
		size_t start = o;
		for (size_t i = 0; i < l && o + 1 < n; i++) {
			char c = s[i];
			out[o++] = (unsigned char)c < 32 || strchr(":*?\"<>|", c) ? '_' : c;
		}
		if (o > start && (out[o - 1] == '.' || out[o - 1] == ' ')) // FAT no admite que acaben así
			out[o - 1] = '_';
	}
	out[o < n ? o : n - 1] = 0;
	return o > 0;
}

/* ---------------------------------------------------------------- leer */

void zip_dir_free(ZipDir *z)
{
	for (int i = 0; i < z->n; i++)
		free(z->e[i].name);
	free(z->e);
	memset(z, 0, sizeof *z);
}

static int fseek64(FILE *f, uint64_t off)
{
	return fseeko(f, (off_t)off, SEEK_SET);
}

int zip_read_dir(const char *zip, ZipDir *z)
{
	memset(z, 0, sizeof *z);
	FILE *f = zopen(zip, "rb");
	if (!f)
		return ZIP_IO;
	fseeko(f, 0, SEEK_END);
	uint64_t fsz = (uint64_t)ftello(f);
	size_t tail = fsz < 65557 ? (size_t)fsz : 65557;
	uint8_t *t = malloc(tail ? tail : 1);
	int r = ZIP_BAD;
	if (!t || tail < 22 || fseek64(f, fsz - tail) || fread(t, 1, tail, f) != tail)
		goto out;
	long e = -1;
	for (long i = (long)tail - 22; i >= 0; i--)
		if (rd32(t + i) == 0x06054b50) {
			e = i;
			break;
		}
	if (e < 0)
		goto out;
	uint64_t count = rd16(t + e + 10), cd_size = rd32(t + e + 12), cd_off = rd32(t + e + 16);
	if ((count == 0xFFFF || cd_size == 0xFFFFFFFF || cd_off == 0xFFFFFFFF) && e >= 20 && rd32(t + e - 20) == 0x07064b50) {
		uint8_t z64[56];
		uint64_t at = rd64(t + e - 20 + 8);
		if (fseek64(f, at) || fread(z64, 1, 56, f) != 56 || rd32(z64) != 0x06064b50)
			goto out;
		count = rd64(z64 + 32), cd_size = rd64(z64 + 40), cd_off = rd64(z64 + 48);
	}
	if (count > MAX_ENTRIES || cd_size > 32 * 1024 * 1024 || cd_off + cd_size > fsz)
		goto out;
	free(t);
	t = malloc(cd_size ? cd_size : 1);
	z->e = calloc(count ? count : 1, sizeof *z->e);
	if (!t || !z->e || fseek64(f, cd_off) || fread(t, 1, cd_size, f) != cd_size)
		goto out;
	size_t p = 0;
	for (uint64_t i = 0; i < count; i++) {
		if (p + 46 > cd_size || rd32(t + p) != 0x02014b50)
			goto out;
		const uint8_t *h = t + p;
		ZipEnt *ze = &z->e[z->n];
		ze->flags = rd16(h + 8), ze->method = rd16(h + 10);
		ze->dostime = rd16(h + 12) | (uint32_t)rd16(h + 14) << 16;
		ze->crc = rd32(h + 16), ze->csize = rd32(h + 20), ze->usize = rd32(h + 24);
		size_t nl = rd16(h + 28), xl = rd16(h + 30), cl = rd16(h + 32);
		ze->off = rd32(h + 42);
		if (p + 46 + nl + xl + cl > cd_size)
			goto out;
		// ZIP64: los que no caben en 32 bits van en el extra 0x0001, en este orden
		for (size_t x = 0; x + 4 <= xl;) {
			const uint8_t *xp = h + 46 + nl + x;
			size_t id = rd16(xp), sz = rd16(xp + 2), k = 4;
			if (x + 4 + sz > xl)
				break;
			if (id == 1) {
				if (ze->usize == 0xFFFFFFFF && k + 8 <= sz + 4)
					ze->usize = rd64(xp + k), k += 8;
				if (ze->csize == 0xFFFFFFFF && k + 8 <= sz + 4)
					ze->csize = rd64(xp + k), k += 8;
				if (ze->off == 0xFFFFFFFF && k + 8 <= sz + 4)
					ze->off = rd64(xp + k), k += 8;
			}
			x += 4 + sz;
		}
		ze->name = name_dup(h + 46, nl, ze->flags & 0x800);
		if (!ze->name)
			goto out;
		size_t ln = strlen(ze->name);
		ze->dir = ln && (ze->name[ln - 1] == '/' || ze->name[ln - 1] == '\\');
		z->n++;
		if (ze->dir)
			z->ndirs++;
		else {
			z->nfiles++;
			z->usize += ze->usize, z->csize += ze->csize;
			if (ze->flags & 1)
				z->encrypted = true;
			else if (ze->method != 0 && ze->method != 8)
				z->unsupported = true;
		}
		p += 46 + nl + xl + cl;
	}
	r = ZIP_OK;
out:
	free(t);
	fclose(f);
	if (r != ZIP_OK)
		zip_dir_free(z);
	return r;
}

int zip_top_level(const ZipDir *z, char (*names)[256], int *counts, uint64_t *sizes, bool *dirs, int max)
{
	int n = 0;
	for (int i = 0; i < z->n; i++) {
		char clean[512];
		if (!zip_safe_name(z->e[i].name, clean, sizeof clean))
			continue;
		char *sl = strchr(clean, '/');
		bool d = sl || z->e[i].dir;
		if (sl)
			*sl = 0;
		int k;
		for (k = 0; k < n && strcmp(names[k], clean); k++)
			;
		if (k == n) {
			if (n == max)
				continue;
			snprintf(names[n], 256, "%s", clean);
			counts[n] = 0, sizes[n] = 0, dirs[n] = d;
			n++;
		}
		if (!z->e[i].dir)
			counts[k] += d ? 1 : 0, sizes[k] += z->e[i].usize;
		dirs[k] |= d;
	}
	return n;
}

/* ---------------------------------------------------------------- extraer */

static bool exists(const char *p)
{
	bool d;
	uint64_t s;
	return fsx_stat(p, &d, &s) == 0;
}

/* «nombre (2).ext» libre en esa carpeta */
static void free_name(char *out, size_t n, const char *dir, const char *name, bool isdir)
{
	join(out, n, dir, name);
	if (!exists(out))
		return;
	const char *dot = isdir ? NULL : strrchr(name, '.');
	int bl = dot && dot != name ? (int)(dot - name) : (int)strlen(name);
	for (int k = 2; k < 1000; k++) {
		char nm[300];
		snprintf(nm, sizeof nm, "%.*s (%d)%s", bl, name, k, dot && dot != name ? dot : "");
		join(out, n, dir, nm);
		if (!exists(out))
			return;
	}
}

static int write_entry(FILE *zf, const ZipEnt *e, const char *dst, uint8_t *in, uint8_t *outb, ZipProgress *pr)
{
	uint8_t lh[30];
	if (fseek64(zf, e->off) || fread(lh, 1, 30, zf) != 30 || rd32(lh) != 0x04034b50)
		return ZIP_BAD;
	if (fseek64(zf, e->off + 30 + rd16(lh + 26) + rd16(lh + 28)))
		return ZIP_BAD;
	FILE *o = zopen(dst, "wb");
	if (!o)
		return ZIP_IO;
	setvbuf(o, NULL, _IOFBF, BUF);
	int r = ZIP_OK;
	uint32_t crc = 0;
	uint64_t left = e->csize, outn = 0;
	z_stream s;
	memset(&s, 0, sizeof s);
	if (e->method == 8 && inflateInit2(&s, -MAX_WBITS) != Z_OK) {
		fclose(o);
		return ZIP_IO;
	}
	bool end = false;
	while (r == ZIP_OK && !end) {
		if (pr && pr->cancel && *pr->cancel) {
			r = ZIP_CANCELED;
			break;
		}
		size_t want = left > BUF ? BUF : (size_t)left;
		size_t got = want ? fread(in, 1, want, zf) : 0;
		if (got != want) {
			r = ZIP_BAD;
			break;
		}
		left -= got;
		if (e->method == 0) {
			if (got && fwrite(in, 1, got, o) != got)
				r = ZIP_IO;
			crc = zip_crc(crc, in, got);
			outn += got;
			if (pr && pr->done)
				*pr->done += got;
			end = left == 0;
			continue;
		}
		s.next_in = in, s.avail_in = (uInt)got;
		do {
			s.next_out = outb, s.avail_out = BUF;
			int zr = inflate(&s, Z_NO_FLUSH);
			if (zr != Z_OK && zr != Z_STREAM_END && !(zr == Z_BUF_ERROR && !got)) {
				r = ZIP_BAD;
				break;
			}
			size_t k = BUF - s.avail_out;
			if (k && fwrite(outb, 1, k, o) != k)
				r = ZIP_IO;
			crc = zip_crc(crc, outb, k);
			outn += k;
			if (pr && pr->done)
				*pr->done += k;
			if (zr == Z_STREAM_END)
				end = true;
		} while (r == ZIP_OK && !end && s.avail_out == 0);
		if (!end && !left && r == ZIP_OK && s.avail_in == 0 && !got)
			r = ZIP_BAD; // se acabó lo comprimido sin llegar al final
	}
	if (e->method == 8)
		inflateEnd(&s);
	if (fclose(o) != 0 && r == ZIP_OK)
		r = ZIP_IO;
	if (r == ZIP_OK && (crc != e->crc || outn != e->usize))
		r = ZIP_CRC;
	if (r != ZIP_OK) {
		char p[1100];
		snprintf(p, sizeof p, ZIP_ROOT "%s", dst);
		remove(p);
	}
	return r;
}

int zip_extract(const char *zip, const ZipDir *z, const char *dest, ZipProgress *pr)
{
	if (z->encrypted || z->unsupported)
		return ZIP_UNSUP;
	uint64_t fr = 0, tot = 0;
	if (fsx_space(&fr, &tot) && fr < z->usize + 1024 * 1024)
		return ZIP_NO_SPACE;
	if (fsx_mkdirs(dest) < 0)
		return ZIP_IO;
	FILE *zf = zopen(zip, "rb");
	uint8_t *in = malloc(BUF), *outb = malloc(BUF);
	int r = zf && in && outb ? ZIP_OK : ZIP_IO;
	// las carpetas de primer nivel que ya existían se combinan sin pisar nada: sus archivos repetidos van como «x (2)»
	for (int i = 0; i < z->n && r == ZIP_OK; i++) {
		const ZipEnt *e = &z->e[i];
		char rel[512], full[1100];
		if (!zip_safe_name(e->name, rel, sizeof rel))
			continue;
		join(full, sizeof full, dest, rel);
		if (e->dir) {
			if (fsx_mkdirs(full) < 0)
				r = ZIP_IO;
			continue;
		}
		char *sl = strrchr(full, '/');
		char dir[1100];
		snprintf(dir, sizeof dir, "%.*s", (int)(sl - full), full);
		if (!dir[0])
			strcpy(dir, "/");
		if (fsx_mkdirs(dir) < 0) {
			r = ZIP_IO;
			break;
		}
		char dst[1100];
		free_name(dst, sizeof dst, dir, sl + 1, false);
		r = write_entry(zf, e, dst, in, outb, pr);
	}
	if (zf)
		fclose(zf);
	free(in), free(outb);
	return r;
}

/* ---------------------------------------------------------------- cabeceras */

uint32_t zip_dostime(uint64_t unix_s)
{
	time_t t = (time_t)unix_s;
	struct tm tm;
	if (!unix_s || !gmtime_r(&t, &tm) || tm.tm_year < 80)
		return (0 << 25) | (1 << 21) | (1 << 16); // 1980-01-01
	return (uint32_t)(tm.tm_year - 80) << 25 | (uint32_t)(tm.tm_mon + 1) << 21 | (uint32_t)tm.tm_mday << 16 |
	       (uint32_t)tm.tm_hour << 11 | (uint32_t)tm.tm_min << 5 | (uint32_t)(tm.tm_sec / 2);
}

/* stream: se manda sin saber el CRC (va detrás, en el descriptor) */
size_t zip_local_header(uint8_t *o, const char *name, uint32_t dostime, uint64_t size, bool dir, bool stream)
{
	size_t nl = strlen(name);
	wr32(o, 0x04034b50);
	wr16(o + 4, 20);
	wr16(o + 6, 0x800 | (stream && !dir ? 8 : 0)); // UTF-8 (y descriptor detrás)
	wr16(o + 8, 0);
	wr32(o + 10, dostime);
	wr32(o + 14, 0);
	wr32(o + 18, (uint32_t)size);
	wr32(o + 22, (uint32_t)size);
	wr16(o + 26, (uint32_t)nl);
	wr16(o + 28, 0);
	memcpy(o + 30, name, nl);
	return 30 + nl;
}

size_t zip_data_desc(uint8_t *o, uint32_t crc, uint64_t size)
{
	wr32(o, 0x08074b50);
	wr32(o + 4, crc);
	wr32(o + 8, (uint32_t)size);
	wr32(o + 12, (uint32_t)size);
	return 16;
}

size_t zip_central(uint8_t *o, const char *name, uint32_t dostime, uint32_t crc, uint64_t csize, uint64_t usize, uint32_t off,
                   bool dir, uint16_t method, bool stream)
{
	size_t nl = strlen(name);
	wr32(o, 0x02014b50);
	wr16(o + 4, 0x0314); // hecho en «Unix» 2.0: así se respetan los atributos de abajo
	wr16(o + 6, 20);
	wr16(o + 8, 0x800 | (stream && !dir ? 8 : 0));
	wr16(o + 10, method);
	wr32(o + 12, dostime);
	wr32(o + 16, crc);
	wr32(o + 20, (uint32_t)csize);
	wr32(o + 24, (uint32_t)usize);
	wr16(o + 28, (uint32_t)nl);
	wr16(o + 30, 0);
	wr16(o + 32, 0);
	wr16(o + 34, 0);
	wr16(o + 36, 0);
	wr32(o + 38, dir ? (0040755u << 16) | 0x10 : (0100644u << 16));
	wr32(o + 42, off);
	memcpy(o + 46, name, nl);
	return 46 + nl;
}

size_t zip_end(uint8_t *o, int n, uint32_t cd_size, uint32_t cd_off)
{
	wr32(o, 0x06054b50);
	wr16(o + 4, 0);
	wr16(o + 6, 0);
	wr16(o + 8, (uint32_t)n);
	wr16(o + 10, (uint32_t)n);
	wr32(o + 12, cd_size);
	wr32(o + 16, cd_off);
	wr16(o + 20, 0);
	return 22;
}

/* ---------------------------------------------------------------- crear */

typedef ZipItem Item;

typedef struct {
	Item *it;
	int n, cap;
	uint64_t total;
	bool full;
} Plan;

static void plan_add(Plan *p, const char *rel, bool dir, uint64_t size)
{
	if (p->n == MAX_PLAN) {
		p->full = true;
		return;
	}
	if (p->n == p->cap) {
		int cap = p->cap ? p->cap * 2 : 256;
		Item *ni = realloc(p->it, cap * sizeof *ni);
		if (!ni) {
			p->full = true;
			return;
		}
		p->it = ni, p->cap = cap;
	}
	size_t l = strlen(rel);
	char *r = malloc(l + 2);
	if (!r) {
		p->full = true;
		return;
	}
	memcpy(r, rel, l);
	r[l] = dir ? '/' : 0, r[l + 1] = 0;
	Item *i = &p->it[p->n++];
	i->rel = r, i->dir = dir, i->size = size;
	p->total += size;
}

typedef struct {
	Plan *p;
	const char *abs, *rel;
	char (*sub)[256];
	int nsub, cap;
} WalkCtx;

static bool walk_cb(const FsxEntry *e, void *u)
{
	WalkCtx *w = u;
	char r[600];
	snprintf(r, sizeof r, "%s/%s", w->rel, e->name);
	size_t l = strlen(e->name);
	if (!e->dir && !strncmp(e->name, ".c3d_", 5) && l > 5 && !strcmp(e->name + l - 5, ".part"))
		return true; // temporales de subidas y copias a medias
	if (e->dir) {
		if (w->nsub == w->cap) {
			int cap = w->cap ? w->cap * 2 : 32;
			char (*ns)[256] = realloc(w->sub, cap * sizeof *ns);
			if (!ns)
				return true;
			w->sub = ns, w->cap = cap;
		}
		snprintf(w->sub[w->nsub++], 256, "%s", e->name);
	} else
		plan_add(w->p, r, false, e->size);
	return !w->p->full;
}

static void plan_tree(Plan *p, const char *abs, const char *rel, int depth)
{
	plan_add(p, rel, true, 0);
	if (depth > 24 || p->full)
		return;
	WalkCtx w = {p, abs, rel, NULL, 0, 0};
	fsx_list(abs, false, walk_cb, &w);
	for (int i = 0; i < w.nsub && !p->full; i++) {
		char a[1100], r[600];
		join(a, sizeof a, abs, w.sub[i]);
		snprintf(r, sizeof r, "%s/%s", rel, w.sub[i]);
		plan_tree(p, a, r, depth + 1);
	}
	free(w.sub);
}

static int plan_make(Plan *p, const char *base, char (*names)[256], int n)
{
	memset(p, 0, sizeof *p);
	for (int i = 0; i < n && !p->full; i++) {
		char a[1100];
		bool d;
		uint64_t s;
		join(a, sizeof a, base, names[i]);
		if (fsx_stat(a, &d, &s) < 0)
			continue;
		if (d)
			plan_tree(p, a, names[i], 0);
		else
			plan_add(p, names[i], false, s);
	}
	return p->n ? ZIP_OK : ZIP_EMPTY;
}

void zip_plan_free(ZipItem *items, int count)
{
	for (int i = 0; i < count; i++)
		free(items[i].rel);
	free(items);
}

void zip_item_path(const char *base, const ZipItem *it, char *out, size_t n)
{
	size_t l = strlen(it->rel);
	char rel[600];
	snprintf(rel, sizeof rel, "%.*s", (int)(it->dir && l ? l - 1 : l), it->rel);
	join(out, n, base, rel);
}

int zip_plan(const char *base, char (*names)[256], int n, ZipItem **items, int *count, uint64_t *total)
{
	Plan p;
	int r = plan_make(&p, base, names, n);
	if (r == ZIP_OK && p.full)
		r = ZIP_TOO_BIG;
	if (r != ZIP_OK) {
		zip_plan_free(p.it, p.n);
		p.it = NULL, p.n = 0;
	}
	*items = p.it, *count = p.n, *total = p.total;
	return r;
}

int zip_plan_size(const char *base, char (*names)[256], int n, uint64_t *total, int *count)
{
	Plan p;
	int r = plan_make(&p, base, names, n);
	*total = p.total;
	*count = p.n;
	zip_plan_free(p.it, p.n);
	return r;
}

/* lo que ya va comprimido no se vuelve a comprimir: solo tardaría más */
static bool stored_ext(const char *name)
{
	static const char *const X[] = {"jpg", "jpeg", "png", "gif", "webp", "mp3", "mp4", "m4a", "aac", "ogg", "opus", "flac",
	                                "mkv", "webm", "moflex", "zip", "7z", "rar", "gz", "bz2", "xz", "lz4", "cia", "3ds",
	                                "cxi", "cci", "nds", "z64", "chd", "cso"};
	const char *d = strrchr(name, '.');
	if (!d)
		return false;
	for (size_t i = 0; i < sizeof X / sizeof X[0]; i++)
		if (!strcasecmp(d + 1, X[i]))
			return true;
	return false;
}

typedef struct {
	uint32_t crc, off;
	uint64_t csize, usize;
	uint16_t method;
	uint32_t dostime;
} Written;

int zip_create(const char *dst, const char *base, char (*names)[256], int n, ZipProgress *pr)
{
	Plan p;
	int r = plan_make(&p, base, names, n);
	if (r != ZIP_OK) {
		zip_plan_free(p.it, p.n);
		return r;
	}
	if (p.total >= 0xF0000000ull || p.full) { // sin ZIP64: hasta ~4 GB
		zip_plan_free(p.it, p.n);
		return ZIP_TOO_BIG;
	}
	uint64_t fr = 0, tot = 0;
	if (fsx_space(&fr, &tot) && fr < p.total + 1024 * 1024) {
		zip_plan_free(p.it, p.n);
		return ZIP_NO_SPACE;
	}
	FILE *o = zopen(dst, "wb+");
	Written *w = calloc(p.n, sizeof *w);
	uint8_t *in = malloc(BUF), *ob = malloc(BUF), hdr[700];
	r = o && w && in && ob ? ZIP_OK : ZIP_IO;
	uint64_t pos = 0;
	for (int i = 0; i < p.n && r == ZIP_OK; i++) {
		Item *it = &p.it[i];
		Written *wi = &w[i];
		wi->off = (uint32_t)pos;
		char ap[1100];
		zip_item_path(base, it, ap, sizeof ap);
		wi->dostime = zip_dostime(fsx_mtime(ap));
		wi->method = it->dir || stored_ext(it->rel) ? 0 : 8;
		size_t hl = zip_local_header(hdr, it->rel, wi->dostime, 0, it->dir, false);
		wr16(hdr + 8, wi->method);
		if (fwrite(hdr, 1, hl, o) != hl) {
			r = ZIP_IO;
			break;
		}
		pos += hl;
		if (it->dir)
			continue;
		char abs[1100];
		zip_item_path(base, it, abs, sizeof abs);
		FILE *f = zopen(abs, "rb");
		if (!f) {
			r = ZIP_IO;
			break;
		}
		z_stream s;
		memset(&s, 0, sizeof s);
		if (wi->method == 8 && deflateInit2(&s, 3, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
			fclose(f);
			r = ZIP_IO;
			break;
		}
		for (;;) {
			if (pr && pr->cancel && *pr->cancel) {
				r = ZIP_CANCELED;
				break;
			}
			size_t got = fread(in, 1, BUF, f);
			if (!got && ferror(f)) {
				r = ZIP_IO;
				break;
			}
			wi->crc = zip_crc(wi->crc, in, got);
			wi->usize += got;
			if (pr && pr->done)
				*pr->done += got;
			if (wi->method == 0) {
				if (got && fwrite(in, 1, got, o) != got)
					r = ZIP_IO;
				wi->csize += got;
			} else {
				s.next_in = in, s.avail_in = (uInt)got;
				int fl = got ? Z_NO_FLUSH : Z_FINISH, zr;
				do {
					s.next_out = ob, s.avail_out = BUF;
					zr = deflate(&s, fl);
					size_t k = BUF - s.avail_out;
					if (k && fwrite(ob, 1, k, o) != k)
						r = ZIP_IO;
					wi->csize += k;
				} while (r == ZIP_OK && s.avail_out == 0 && zr != Z_STREAM_END);
			}
			if (!got || r != ZIP_OK)
				break;
		}
		if (wi->method == 8)
			deflateEnd(&s);
		fclose(f);
		if (r != ZIP_OK)
			break;
		// la cabecera, ya con el CRC y los tamaños
		uint64_t end = pos + wi->csize;
		uint8_t fix[12];
		wr32(fix, wi->crc), wr32(fix + 4, (uint32_t)wi->csize), wr32(fix + 8, (uint32_t)wi->usize);
		if (fseek64(o, wi->off + 14) || fwrite(fix, 1, 12, o) != 12 || fseek64(o, end))
			r = ZIP_IO;
		pos = end;
	}
	uint32_t cd_off = (uint32_t)pos, cd_size = 0;
	for (int i = 0; i < p.n && r == ZIP_OK; i++) {
		size_t hl = zip_central(hdr, p.it[i].rel, w[i].dostime, w[i].crc, w[i].csize, w[i].usize, w[i].off, p.it[i].dir,
		                        w[i].method, false);
		if (fwrite(hdr, 1, hl, o) != hl)
			r = ZIP_IO;
		cd_size += (uint32_t)hl;
	}
	if (r == ZIP_OK) {
		size_t hl = zip_end(hdr, p.n, cd_size, cd_off);
		if (fwrite(hdr, 1, hl, o) != hl)
			r = ZIP_IO;
	}
	if (o && fclose(o) != 0 && r == ZIP_OK)
		r = ZIP_IO;
	if (r != ZIP_OK) {
		char pp[1100];
		snprintf(pp, sizeof pp, ZIP_ROOT "%s", dst);
		remove(pp);
	}
	free(w), free(in), free(ob);
	zip_plan_free(p.it, p.n);
	return r;
}
