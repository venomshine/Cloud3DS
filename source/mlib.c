/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "mlib.h"
#include "cJSON.h"
#include "fsx.h"
#include "music.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define ML_FILE APP_DIR "/musica.json"

static struct {
	LightLock lock;
	Thread thr;
	volatile bool quit, busy, again;
	MSong *s;       // la lista que se ve
	int n;
	u32 rev;
	int where;
	volatile int found;
} L;

/* ---------------------------------------------------------------- etiquetas */

// texto de una etiqueta a UTF-8: enc 0 Latin-1, 1 UTF-16 con BOM, 2 UTF-16BE, 3 UTF-8
static void tag_text(const u8 *p, int n, int enc, char *out, size_t on)
{
	size_t o = 0;
	if (!on)
		return;
	if (enc == 1 || enc == 2) {
		bool be = enc == 2;
		int i = 0;
		if (enc == 1 && n >= 2) {
			be = p[0] == 0xFE && p[1] == 0xFF;
			i = 2;
		}
		for (; i + 1 < n; i += 2) {
			u32 c = be ? (u32)(p[i] << 8 | p[i + 1]) : (u32)(p[i + 1] << 8 | p[i]);
			if (!c)
				break;
			if (c >= 0xD800 && c < 0xDC00 && i + 3 < n) { // par sustituto
				u32 lo = be ? (u32)(p[i + 2] << 8 | p[i + 3]) : (u32)(p[i + 3] << 8 | p[i + 2]);
				c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00);
				i += 2;
			}
			char b[4];
			int k = c < 0x80 ? (b[0] = (char)c, 1)
			        : c < 0x800 ? (b[0] = (char)(0xC0 | c >> 6), b[1] = (char)(0x80 | (c & 63)), 2)
			        : c < 0x10000 ? (b[0] = (char)(0xE0 | c >> 12), b[1] = (char)(0x80 | (c >> 6 & 63)), b[2] = (char)(0x80 | (c & 63)), 3)
			                      : (b[0] = (char)(0xF0 | c >> 18), b[1] = (char)(0x80 | (c >> 12 & 63)), b[2] = (char)(0x80 | (c >> 6 & 63)),
			                         b[3] = (char)(0x80 | (c & 63)), 4);
			if (o + k >= on)
				break;
			memcpy(out + o, b, k), o += k;
		}
	} else if (enc == 3) {
		for (int i = 0; i < n && p[i] && o + 1 < on; i++)
			out[o++] = (char)p[i];
	} else {
		for (int i = 0; i < n && p[i]; i++) {
			u8 c = p[i];
			if (c < 0x80) {
				if (o + 1 >= on)
					break;
				out[o++] = (char)c;
			} else {
				if (o + 2 >= on)
					break;
				out[o++] = (char)(0xC0 | c >> 6), out[o++] = (char)(0x80 | (c & 63));
			}
		}
	}
	out[o] = 0;
	while (o && (out[o - 1] == ' ' || out[o - 1] == '\n' || out[o - 1] == '\r'))
		out[--o] = 0;
	if (!util_utf8_valid(out))
		out[0] = 0;
}

static u32 syncsafe(const u8 *b)
{
	return (u32)(b[0] & 127) << 21 | (u32)(b[1] & 127) << 14 | (u32)(b[2] & 127) << 7 | (b[3] & 127);
}

static void read_id3(FILE *f, MSong *m)
{
	u8 h[10];
	if (fread(h, 1, 10, f) == 10 && !memcmp(h, "ID3", 3) && h[3] >= 2 && h[3] <= 4) {
		int ver = h[3];
		u32 end = 10 + syncsafe(h + 6);
		long pos = 10;
		if (ver >= 3 && (h[5] & 0x40)) { // cabecera extendida
			u8 e[4];
			if (fread(e, 1, 4, f) == 4)
				pos += ver == 4 ? syncsafe(e) : (u32)(e[0] << 24 | e[1] << 16 | e[2] << 8 | e[3]) + 4;
		}
		int frames = 0;
		while (pos + (ver == 2 ? 6 : 10) < (long)end && frames++ < 200) {
			if (fseek(f, pos, SEEK_SET) != 0)
				break;
			u8 fh[10];
			int hl = ver == 2 ? 6 : 10;
			if (fread(fh, 1, hl, f) != (size_t)hl || !fh[0])
				break;
			char id[5] = {0};
			memcpy(id, fh, ver == 2 ? 3 : 4);
			u32 sz = ver == 2 ? (u32)(fh[3] << 16 | fh[4] << 8 | fh[5]) : ver == 4 ? syncsafe(fh + 4) : (u32)(fh[4] << 24 | fh[5] << 16 | fh[6] << 8 | fh[7]);
			if (!sz || pos + hl + (long)sz > (long)end)
				break;
			char *dst = NULL;
			size_t dn = 0;
			bool trk = false;
			if (!strcmp(id, "TIT2") || !strcmp(id, "TT2"))
				dst = m->title, dn = sizeof m->title;
			else if (!strcmp(id, "TPE1") || !strcmp(id, "TP1") || ((!strcmp(id, "TPE2") || !strcmp(id, "TP2")) && !m->artist[0]))
				dst = m->artist, dn = sizeof m->artist;
			else if (!strcmp(id, "TALB") || !strcmp(id, "TAL"))
				dst = m->album, dn = sizeof m->album;
			else if (!strcmp(id, "TRCK") || !strcmp(id, "TRK"))
				trk = true;
			if ((dst || trk) && sz < 1024) {
				u8 b[1024];
				if (fread(b, 1, sz, f) == sz) {
					char t[192];
					tag_text(b + 1, (int)sz - 1, b[0], dst ? dst : t, dst ? dn : sizeof t);
					if (trk)
						m->track = (u16)atoi(t);
				}
			}
			pos += hl + (long)sz;
		}
	}
	// ID3v1, al final: lo que falte
	if (!m->title[0] || !m->artist[0] || !m->album[0]) {
		u8 t[128];
		if (fseek(f, -128, SEEK_END) == 0 && fread(t, 1, 128, f) == 128 && !memcmp(t, "TAG", 3)) {
			if (!m->title[0])
				tag_text(t + 3, 30, 0, m->title, sizeof m->title);
			if (!m->artist[0])
				tag_text(t + 33, 30, 0, m->artist, sizeof m->artist);
			if (!m->album[0])
				tag_text(t + 63, 30, 0, m->album, sizeof m->album);
			if (!m->track && !t[125] && t[126])
				m->track = t[126];
		}
	}
}

// OGG: el paquete de comentarios («\3vorbis»), en las primeras páginas
static void read_vorbis(FILE *f, MSong *m)
{
	enum { N = 65536 };
	u8 *b = malloc(N);
	if (!b)
		return;
	size_t n = fread(b, 1, N, f);
	for (size_t i = 0; i + 7 < n; i++) {
		if (b[i] != 3 || memcmp(b + i + 1, "vorbis", 6))
			continue;
		size_t p = i + 7;
		if (p + 4 > n)
			break;
		u32 vl = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (u32)b[p + 3] << 24;
		p += 4 + vl;
		if (p + 4 > n)
			break;
		u32 cnt = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (u32)b[p + 3] << 24;
		p += 4;
		for (u32 k = 0; k < cnt && k < 200 && p + 4 <= n; k++) {
			u32 l = b[p] | b[p + 1] << 8 | b[p + 2] << 16 | (u32)b[p + 3] << 24;
			p += 4;
			if (l > n - p)
				break;
			const char *c = (const char *)b + p;
			struct { const char *k; char *d; size_t dn; } T[] = {{"TITLE=", m->title, sizeof m->title}, {"ARTIST=", m->artist, sizeof m->artist},
			                                                      {"ALBUM=", m->album, sizeof m->album}};
			for (int t = 0; t < 3; t++) {
				size_t kl = strlen(T[t].k);
				if (l > kl && !strncasecmp(c, T[t].k, kl) && !T[t].d[0])
					tag_text((const u8 *)c + kl, (int)(l - kl), 3, T[t].d, T[t].dn);
			}
			if (l > 12 && !strncasecmp(c, "TRACKNUMBER=", 12)) {
				char t[16];
				tag_text((const u8 *)c + 12, (int)(l - 12) < 15 ? (int)(l - 12) : 15, 3, t, sizeof t);
				m->track = (u16)atoi(t);
			}
			p += l;
		}
		break;
	}
	free(b);
}

static void read_tags(MSong *m)
{
	m->title[0] = m->artist[0] = m->album[0] = 0;
	m->track = 0;
	char sp[300];
	snprintf(sp, sizeof sp, "sdmc:%s", m->path);
	FILE *f = fopen(sp, "rb");
	if (f) {
		const char *x = strrchr(m->path, '.');
		if (x && (!strcasecmp(x, ".mp3") || !strcasecmp(x, ".aac"))) // (.aac: las descargas de SoundCloud llevan ID3)
			read_id3(f, m);
		else if (x && !strcasecmp(x, ".ogg"))
			read_vorbis(f, m);
		fclose(f);
	}
	m->notag = !m->title[0] && !m->artist[0] && !m->album[0];
}

/* ---------------------------------------------------------------- buscar */

typedef struct {
	MSong *out;
	int n;
	const MSong *old;
	int on;
	int depth;
	char dir[256];
} Walk;

static const MSong *find_old(const Walk *w, const char *path)
{
	for (int i = 0; i < w->on; i++)
		if (!strcmp(w->old[i].path, path))
			return &w->old[i];
	return NULL;
}

static void walk(Walk *w, const char *dir);

typedef struct {
	char (*names)[256];
	bool *dirs;
	u64 *sizes, *mtimes;
	int n, cap;
} Ents;

static bool ents_cb(const FsxEntry *e, void *u)
{
	Ents *l = u;
	if (e->name[0] == '.')
		return true;
	if (!e->dir && !music_is_audio(e->name))
		return true;
	if (l->n == l->cap) {
		int nc = l->cap ? l->cap * 2 : 64;
		void *a = realloc(l->names, nc * sizeof *l->names), *b = realloc(l->dirs, nc * sizeof *l->dirs), *c = realloc(l->sizes, nc * sizeof *l->sizes),
		     *d = realloc(l->mtimes, nc * sizeof *l->mtimes);
		if (a)
			l->names = a;
		if (b)
			l->dirs = b;
		if (c)
			l->sizes = c;
		if (d)
			l->mtimes = d;
		if (!a || !b || !c || !d)
			return false;
		l->cap = nc;
	}
	util_strlcpy(l->names[l->n], e->name, sizeof l->names[0]);
	l->dirs[l->n] = e->dir, l->sizes[l->n] = e->size, l->mtimes[l->n] = e->mtime;
	l->n++;
	return true;
}

static bool skip_dir(const char *path)
{
	static const char *const SKIP[] = {"/Nintendo 3DS", "/Nintendo DSiWare", "/luma", "/gm9", "/3ds/Cloud3DS", "/private", "/DCIM"};
	for (size_t i = 0; i < sizeof SKIP / sizeof SKIP[0]; i++)
		if (!strcasecmp(path, SKIP[i]))
			return true;
	return false;
}

static void walk(Walk *w, const char *dir)
{
	if (L.quit || w->depth > 12 || w->n >= MLIB_MAX)
		return;
	Ents l = {0};
	fsx_list(dir, true, ents_cb, &l);
	for (int i = 0; i < l.n && !L.quit && w->n < MLIB_MAX; i++) {
		char p[256];
		snprintf(p, sizeof p, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", l.names[i]);
		if (l.dirs[i]) {
			if (!skip_dir(p)) {
				w->depth++;
				walk(w, p);
				w->depth--;
			}
			continue;
		}
		bool dup = false; // (la misma canción con otra forma de la ruta: la SD no distingue mayúsculas)
		for (int k = 0; k < w->n && !dup; k++)
			dup = !strcasecmp(w->out[k].path, p);
		if (dup)
			continue;
		MSong *m = &w->out[w->n];
		const MSong *o = find_old(w, p);
		if (o && o->size == l.sizes[i] && o->mtime == l.mtimes[i])
			*m = *o; // no cambió: lo que ya se sabía (también si estaba oculta)
		else {
			memset(m, 0, sizeof *m);
			util_strlcpy(m->path, p, sizeof m->path);
			m->size = l.sizes[i], m->mtime = l.mtimes[i];
			m->hidden = o && o->hidden;
			read_tags(m);
		}
		w->n++;
		L.found = w->n;
	}
	free(l.names), free(l.dirs), free(l.sizes), free(l.mtimes);
}

static void save(void)
{
	cJSON *j = cJSON_CreateObject(), *a = cJSON_CreateArray();
	cJSON_AddNumberToObject(j, "v", 1);
	cJSON_AddNumberToObject(j, "where", L.where);
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++) {
		const MSong *m = &L.s[i];
		cJSON *e = cJSON_CreateObject();
		cJSON_AddStringToObject(e, "p", m->path);
		if (m->title[0])
			cJSON_AddStringToObject(e, "t", m->title);
		if (m->artist[0])
			cJSON_AddStringToObject(e, "a", m->artist);
		if (m->album[0])
			cJSON_AddStringToObject(e, "b", m->album);
		if (m->track)
			cJSON_AddNumberToObject(e, "k", m->track);
		if (m->hidden)
			cJSON_AddBoolToObject(e, "h", true);
		cJSON_AddNumberToObject(e, "s", (double)m->size);
		cJSON_AddNumberToObject(e, "m", (double)m->mtime);
		cJSON_AddItemToArray(a, e);
	}
	LightLock_Unlock(&L.lock);
	cJSON_AddItemToObject(j, "songs", a);
	char *s = cJSON_PrintUnformatted(j);
	cJSON_Delete(j);
	if (!s)
		return;
	FILE *f = fopen(ML_FILE ".tmp", "wb");
	if (f) {
		bool ok = fwrite(s, 1, strlen(s), f) == strlen(s);
		ok = fclose(f) == 0 && ok;
		if (ok) {
			remove(ML_FILE);
			rename(ML_FILE ".tmp", ML_FILE);
		}
	}
	free(s);
}

static bool load(void)
{
	FILE *f = fopen(ML_FILE, "rb");
	if (!f)
		return false;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *b = sz > 0 && sz < 4 * 1024 * 1024 ? malloc(sz + 1) : NULL;
	size_t n = b ? fread(b, 1, sz, f) : 0;
	fclose(f);
	if (!b)
		return false;
	b[n] = 0;
	cJSON *j = cJSON_Parse(b);
	free(b);
	const cJSON *a = cJSON_GetObjectItemCaseSensitive(j, "songs"), *w = cJSON_GetObjectItemCaseSensitive(j, "where"), *e;
	L.where = cJSON_IsNumber(w) ? w->valueint : MLS_ALL;
	int k = 0;
	cJSON_ArrayForEach(e, a)
	{
		if (k >= MLIB_MAX)
			break;
		const cJSON *p = cJSON_GetObjectItemCaseSensitive(e, "p");
		if (!cJSON_IsString(p))
			continue;
		MSong *m = &L.s[k++];
		memset(m, 0, sizeof *m);
		util_strlcpy(m->path, p->valuestring, sizeof m->path);
		const cJSON *t = cJSON_GetObjectItemCaseSensitive(e, "t"), *ar = cJSON_GetObjectItemCaseSensitive(e, "a"), *al = cJSON_GetObjectItemCaseSensitive(e, "b"),
		            *tk = cJSON_GetObjectItemCaseSensitive(e, "k"), *h = cJSON_GetObjectItemCaseSensitive(e, "h"), *s = cJSON_GetObjectItemCaseSensitive(e, "s"),
		            *mt = cJSON_GetObjectItemCaseSensitive(e, "m");
		if (cJSON_IsString(t))
			util_strlcpy(m->title, t->valuestring, sizeof m->title);
		if (cJSON_IsString(ar))
			util_strlcpy(m->artist, ar->valuestring, sizeof m->artist);
		if (cJSON_IsString(al))
			util_strlcpy(m->album, al->valuestring, sizeof m->album);
		m->track = cJSON_IsNumber(tk) ? (u16)tk->valueint : 0;
		m->hidden = cJSON_IsTrue(h);
		m->size = cJSON_IsNumber(s) ? (u64)s->valuedouble : 0;
		m->mtime = cJSON_IsNumber(mt) ? (u64)mt->valuedouble : 0;
		m->notag = !m->title[0] && !m->artist[0] && !m->album[0];
	}
	L.n = k;
	cJSON_Delete(j);
	return true;
}

static void scan_thread(void *arg)
{
	(void)arg;
	do {
		L.again = false;
		L.found = 0;
		MSong *out = malloc(sizeof(MSong) * MLIB_MAX), *old = malloc(sizeof(MSong) * MLIB_MAX);
		if (!out || !old) {
			free(out), free(old);
			break;
		}
		LightLock_Lock(&L.lock);
		int on = L.n;
		memcpy(old, L.s, sizeof(MSong) * on);
		LightLock_Unlock(&L.lock);
		Walk w = {out, 0, old, on, 0, ""};
		if (L.where == MLS_MUSIC) {
			// (en la SD, /Music y /music son la misma carpeta: cada nombre, una vez; las acentuadas, por si acaso, se
			// descartan abajo si ya se recorrieron con otra forma)
			static const char *const DIRS[] = {"/Music", "/Música", "/MP3", "/Musica"};
			for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++)
				walk(&w, DIRS[i]);
		} else
			walk(&w, "/");
		if (!L.quit) {
			LightLock_Lock(&L.lock);
			memcpy(L.s, out, sizeof(MSong) * w.n);
			L.n = w.n;
			L.rev++;
			LightLock_Unlock(&L.lock);
			save();
			util_log("música: %d canciones", w.n);
		}
		free(out), free(old);
	} while (L.again && !L.quit);
	L.busy = false;
	L.rev++;
}

static void start(void)
{
	if (L.busy) {
		L.again = true;
		return;
	}
	if (L.thr) {
		threadJoin(L.thr, U64_MAX);
		threadFree(L.thr);
		L.thr = NULL;
	}
	L.busy = true;
	L.rev++;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	L.thr = threadCreate(scan_thread, NULL, 32 * 1024, prio + 2, -2, false);
	if (!L.thr)
		L.busy = false;
}

void mlib_init(void)
{
	if (L.s)
		return;
	LightLock_Init(&L.lock);
	L.s = malloc(sizeof(MSong) * MLIB_MAX);
	if (!L.s)
		return;
	load();
	start(); // (lo nuevo o lo cambiado desde la última vez; lo demás sale de la caché)
}

void mlib_exit(void)
{
	L.quit = true;
	if (L.thr) {
		threadJoin(L.thr, 3000000000ull);
		threadFree(L.thr);
		L.thr = NULL;
	}
}

void mlib_scan(void)
{
	if (L.s)
		start();
}

bool mlib_scanning(int *found)
{
	if (found)
		*found = L.found;
	return L.busy;
}

u32 mlib_rev(void)
{
	return L.rev;
}

int mlib_lock(const MSong **out)
{
	if (!L.s) {
		*out = NULL;
		return 0;
	}
	LightLock_Lock(&L.lock);
	*out = L.s;
	return L.n;
}

void mlib_unlock(void)
{
	if (L.s)
		LightLock_Unlock(&L.lock);
}

void mlib_hide(const char *path, bool hide)
{
	if (!L.s)
		return;
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++)
		if (!strcmp(L.s[i].path, path))
			L.s[i].hidden = hide;
	L.rev++;
	LightLock_Unlock(&L.lock);
	save();
}

void mlib_forget(const char *path)
{
	if (!L.s)
		return;
	LightLock_Lock(&L.lock);
	for (int i = 0; i < L.n; i++)
		if (!strcmp(L.s[i].path, path)) {
			memmove(&L.s[i], &L.s[i + 1], (L.n - i - 1) * sizeof L.s[0]);
			L.n--;
			break;
		}
	L.rev++;
	LightLock_Unlock(&L.lock);
	save();
}

int mlib_where(void)
{
	return L.where;
}

void mlib_set_where(int w)
{
	L.where = w;
	save();
	mlib_scan();
}
