/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "titles.h"
#include "fsx.h"
#include "i18n.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define HI_APP 0x00040000u
#define HI_UPD 0x0004000Eu
#define HI_DLC 0x0004008Cu

/* SMDH de un título instalado (su «icon» en ExeFS), como hace FBI */
static bool read_smdh(u64 tid, u8 *smdh)
{
	static const u32 fpath[5] = {0x0, 0x0, 0x2, 0x6E6F6369 /* "icon" */, 0x0};
	u32 apath[4] = {(u32)tid, (u32)(tid >> 32), MEDIATYPE_SD, 0};
	FS_Path ap = {PATH_BINARY, sizeof apath, apath}, fp = {PATH_BINARY, sizeof fpath, fpath};
	Handle h;
	if (R_FAILED(FSUSER_OpenFileDirectly(&h, ARCHIVE_SAVEDATA_AND_CONTENT, ap, fp, FS_OPEN_READ, 0)))
		return false;
	u32 rd = 0;
	Result r = FSFILE_Read(h, &rd, 0, smdh, 0x36C0);
	FSFILE_Close(h);
	return R_SUCCEEDED(r) && rd == 0x36C0 && !memcmp(smdh, "SMDH", 4);
}

static void smdh_text(const u8 *smdh, int lang, int off, char *out, size_t n)
{
	const u8 *t = smdh + 8 + lang * 0x200 + off;
	if (!t[0] && !t[1]) // sin ese idioma: el inglés
		t = smdh + 8 + 0x200 + off;
	u16 tmp[0x81] = {0};
	int max = off == 0 ? 0x40 : 0x40;
	for (int i = 0; i < max; i++)
		tmp[i] = t[i * 2] | t[i * 2 + 1] << 8;
	util_utf16_to_utf8(out, n, tmp, max);
}

static int cmp_title(const void *a, const void *b)
{
	return strcasecmp(((const TitleInfo *)a)->title, ((const TitleInfo *)b)->title);
}

int titles_list(TitleInfo **out)
{
	*out = NULL;
	if (R_FAILED(amInit()))
		return -1;
	u32 n = 0;
	u64 *ids = NULL;
	AM_TitleEntry *info = NULL;
	int cnt = 0;
	if (R_SUCCEEDED(AM_GetTitleCount(MEDIATYPE_SD, &n)) && n && (ids = malloc(n * sizeof *ids)) && (info = malloc(n * sizeof *info)) &&
	    R_SUCCEEDED(AM_GetTitleList(&n, MEDIATYPE_SD, n, ids)) && R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, n, ids, info))) {
		TitleInfo *t = calloc(n, sizeof *t);
		u8 *smdh = malloc(0x36C0);
		if (t && smdh) {
			for (u32 i = 0; i < n; i++) // primero los juegos y apps (nada del sistema)
				if ((u32)(ids[i] >> 32) == HI_APP) {
					TitleInfo *x = &t[cnt++];
					x->tid = ids[i], x->version = info[i].version, x->size = info[i].size;
					int lang = g_lang == LANG_JA ? 0 : g_lang == LANG_ES ? 5 : 1;
					if ((x->smdh = read_smdh(x->tid, smdh))) {
						smdh_text(smdh, lang, 0x80, x->title, sizeof x->title); // el nombre largo
						if (!x->title[0])
							smdh_text(smdh, lang, 0, x->title, sizeof x->title);
						smdh_text(smdh, 1, 0, x->short_en, sizeof x->short_en);
						smdh_text(smdh, lang, 0x180, x->pub, sizeof x->pub);
					}
					if (!x->title[0])
						snprintf(x->title, sizeof x->title, "%016llX", (unsigned long long)x->tid);
				}
			for (u32 i = 0; i < n; i++) { // y a cada uno, su actualización y sus DLC
				u32 hi = (u32)(ids[i] >> 32), lo = (u32)ids[i];
				if (hi != HI_UPD && hi != HI_DLC)
					continue;
				for (int k = 0; k < cnt; k++)
					if ((u32)t[k].tid == lo) {
						if (hi == HI_UPD)
							t[k].has_upd = true, t[k].upd_version = info[i].version, t[k].upd_size = info[i].size;
						else
							t[k].dlc++, t[k].dlc_size += info[i].size;
					}
			}
			qsort(t, cnt, sizeof *t, cmp_title);
			*out = t;
		} else
			free(t);
		free(smdh);
	}
	free(ids);
	free(info);
	amExit();
	return cnt;
}

bool title_icon(u64 tid, u16 *px)
{
	u8 *smdh = malloc(0x36C0);
	bool ok = smdh && read_smdh(tid, smdh);
	if (ok) {
		const u16 *ic = (const u16 *)(smdh + 0x24C0);
		static const u8 tbl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
		for (int y = 0; y < 48; y++)
			for (int x = 0; x < 48; x++)
				px[y * 48 + x] = ic[((y >> 3) * 6 + (x >> 3)) * 64 + (tbl[x & 7] | tbl[y & 7] << 1)];
	}
	free(smdh);
	return ok;
}

bool title_is_self(u64 tid)
{
	u64 me = 0;
	return R_SUCCEEDED(APT_GetProgramID(&me)) && me == tid;
}

int title_uninstall(u64 tid)
{
	if ((u32)(tid >> 32) != HI_APP || title_is_self(tid))
		return -1;
	if (R_FAILED(amInit()))
		return -2;
	u32 lo = (u32)tid, n = 0;
	u64 *ids = NULL;
	Result r = AM_DeleteTitle(MEDIATYPE_SD, tid);
	// su actualización y sus DLC, que sin el juego no sirven
	if (R_SUCCEEDED(r) && R_SUCCEEDED(AM_GetTitleCount(MEDIATYPE_SD, &n)) && n && (ids = malloc(n * sizeof *ids)) &&
	    R_SUCCEEDED(AM_GetTitleList(&n, MEDIATYPE_SD, n, ids)))
		for (u32 i = 0; i < n; i++)
			if ((u32)ids[i] == lo && ((u32)(ids[i] >> 32) == HI_UPD || (u32)(ids[i] >> 32) == HI_DLC))
				AM_DeleteTitle(MEDIATYPE_SD, ids[i]);
	free(ids);
	amExit();
	util_log("desinstalar %016llX: %08lX", (unsigned long long)tid, (unsigned long)r);
	return R_SUCCEEDED(r) ? 0 : -2;
}

/* ---------------------------------------------------------------- copias de partidas */

/* la carpeta de Checkpoint para ese juego: «0xXXXXX Nombre»; si ya hay una con ese número, esa */
typedef struct {
	char prefix[16];
	char found[256];
} FindCtx;

static bool find_cb(const FsxEntry *e, void *u)
{
	FindCtx *c = u;
	if (e->dir && !strncasecmp(e->name, c->prefix, strlen(c->prefix))) {
		util_strlcpy(c->found, e->name, sizeof c->found);
		return false;
	}
	return true;
}

void save_dir(const TitleInfo *t, char *out, size_t n)
{
	FindCtx c = {0};
	snprintf(c.prefix, sizeof c.prefix, "0x%05lX ", (unsigned long)(((u32)t->tid >> 8) & 0xFFFFF));
	fsx_list(CHECKPOINT_DIR, false, find_cb, &c);
	if (c.found[0]) {
		snprintf(out, n, CHECKPOINT_DIR "/%s", c.found);
		return;
	}
	// como Checkpoint: el nombre corto en inglés, sin los caracteres que no deja
	char nm[64];
	util_strlcpy(nm, t->short_en[0] ? t->short_en : t->title, sizeof nm);
	for (char *p = nm; *p; p++)
		if (strchr(".,!\\/:?*\"<>|", *p) || (unsigned char)*p < 32)
			*p = ' ';
	snprintf(out, n, CHECKPOINT_DIR "/%s%s", c.prefix, nm);
}

typedef struct {
	FS_Archive a;
	int files;
	bool err;
} CopyCtx;

static void copy_dir(CopyCtx *c, const u16 *src, int srclen, const char *dst)
{
	if (fsx_mkdirs(dst) < 0) {
		c->err = true;
		return;
	}
	u16 path[512];
	memcpy(path, src, srclen * 2);
	path[srclen] = 0;
	Handle d;
	if (R_FAILED(FSUSER_OpenDirectory(&d, c->a, (FS_Path){PATH_UTF16, (srclen + 1) * 2, path}))) {
		c->err = true;
		return;
	}
	FS_DirectoryEntry *e = malloc(sizeof *e);
	u32 got;
	while (e && !c->err && R_SUCCEEDED(FSDIR_Read(d, &got, 1, e)) && got) {
		int nl = 0;
		while (nl < 0x105 && e->name[nl])
			nl++;
		if (srclen + 1 + nl >= 500)
			continue;
		u16 child[512];
		memcpy(child, path, srclen * 2);
		int cl = srclen;
		if (cl == 0 || child[cl - 1] != '/')
			child[cl++] = '/';
		memcpy(child + cl, e->name, nl * 2);
		cl += nl;
		child[cl] = 0;
		char nm[300], out[800];
		util_utf16_to_utf8(nm, sizeof nm, e->name, nl);
		snprintf(out, sizeof out, "%s/%s", dst, nm);
		if (e->attributes & FS_ATTRIBUTE_DIRECTORY) {
			copy_dir(c, child, cl, out);
			continue;
		}
		Handle f;
		u64 sz = 0;
		if (R_FAILED(FSUSER_OpenFile(&f, c->a, (FS_Path){PATH_UTF16, (cl + 1) * 2, child}, FS_OPEN_READ, 0)) ||
		    R_FAILED(FSFILE_GetSize(f, &sz))) {
			c->err = true;
			break;
		}
		char sp[820];
		snprintf(sp, sizeof sp, "sdmc:%s", out);
		FILE *o = fopen(sp, "wb");
		u8 *buf = malloc(64 * 1024);
		u64 off = 0;
		while (o && buf && off < sz) {
			u32 rd = 0, want = sz - off > 64 * 1024 ? 64 * 1024 : (u32)(sz - off);
			if (R_FAILED(FSFILE_Read(f, &rd, off, buf, want)) || !rd || fwrite(buf, 1, rd, o) != rd)
				break;
			off += rd;
		}
		if (!o || !buf || off != sz)
			c->err = true;
		if (o && fclose(o) != 0)
			c->err = true;
		free(buf);
		FSFILE_Close(f);
		c->files++;
	}
	free(e);
	FSDIR_Close(d);
}

int save_backup(const TitleInfo *t, char *out, size_t n)
{
	u32 path[3] = {MEDIATYPE_SD, (u32)t->tid, (u32)(t->tid >> 32)};
	CopyCtx c = {0};
	if (R_FAILED(FSUSER_OpenArchive(&c.a, ARCHIVE_USER_SAVEDATA, (FS_Path){PATH_BINARY, sizeof path, path})))
		return -1; // el juego aún no tiene partida (o no se puede abrir)
	char gd[512];
	save_dir(t, gd, sizeof gd);
	time_t now = time(NULL);
	struct tm tm;
	gmtime_r(&now, &tm); // la hora de la consola ya es la local
	snprintf(out, n, "%s/%04d%02d%02d-%02d%02d%02d", gd, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
	static const u16 root[1] = {'/'};
	copy_dir(&c, root, 1, out);
	FSUSER_CloseArchive(c.a);
	util_log("copia de la partida %016llX en %s: %d archivos%s", (unsigned long long)t->tid, out, c.files, c.err ? " (error)" : "");
	if (c.err) {
		fsx_rmrf(out); // a medias no sirve
		return -2;
	}
	return 0;
}

typedef struct {
	int n;
	u64 last;
	const char *dir;
} CountCtx;

static bool count_cb(const FsxEntry *e, void *u)
{
	CountCtx *c = u;
	if (e->dir) {
		c->n++;
		// la fecha va en el nombre (AAAAMMDD-HHMMSS, como Checkpoint): la SD no la guarda en las carpetas
		struct tm tm = {0};
		if (sscanf(e->name, "%4d%2d%2d-%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday, &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
			tm.tm_year -= 1900, tm.tm_mon -= 1;
			u64 t = (u64)mktime(&tm); // en la consola no hay zona horaria: es como timegm
			if (t > c->last)
				c->last = t;
		}
	}
	return true;
}

int save_backups_count(const TitleInfo *t, u64 *last)
{
	char gd[512];
	save_dir(t, gd, sizeof gd);
	CountCtx c = {0, 0, gd};
	fsx_list(gd, false, count_cb, &c);
	if (last)
		*last = c.last;
	return c.n;
}
