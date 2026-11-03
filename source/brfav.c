/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "brfav.h"
#include "app.h"
#include "util.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define FAV_PATH  APP_DIR "/navegador_favoritos.txt"
#define HIST_PATH APP_DIR "/navegador_historial.txt"

static BmFav FAV[BM_FAV_MAX];
static BmHist HI[BM_HIST_MAX];
static int NFAV, NHI;
static bool LOADED;

// sin tabuladores ni saltos de línea (separan los campos y las páginas en el archivo)
static void clean(char *dst, const char *src, size_t n)
{
	size_t j = 0;
	while (*src) {
		unsigned char c = (unsigned char)*src;
		size_t len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
		if (j + len >= n) // sin cortar una letra de UTF-8 por la mitad
			break;
		for (size_t k = 0; k < len && *src; k++, src++)
			dst[j++] = (*src == '\t' || *src == '\n' || *src == '\r') ? ' ' : *src;
	}
	dst[j] = 0;
}

static void save_fav(void)
{
	FILE *f = fopen(FAV_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < NFAV; i++)
		fprintf(f, "%s\t%s\n", FAV[i].title, FAV[i].url);
	fclose(f);
}

static void save_hist(void)
{
	FILE *f = fopen(HIST_PATH, "w");
	if (!f)
		return;
	for (int i = 0; i < NHI; i++)
		fprintf(f, "%lld\t%s\t%s\n", (long long)HI[i].t, HI[i].title, HI[i].url);
	fclose(f);
}

static char *field(char **p)
{
	char *s = *p, *t = strchr(s, '\t');
	if (t) {
		*t = 0;
		*p = t + 1;
	} else
		*p = s + strlen(s);
	return s;
}

void bm_load(void)
{
	if (LOADED)
		return;
	LOADED = true;
	char *line = malloc(1024);
	if (!line)
		return;
	FILE *f = fopen(FAV_PATH, "r");
	if (f) {
		while (NFAV < BM_FAV_MAX && fgets(line, 1024, f)) {
			line[strcspn(line, "\r\n")] = 0;
			char *p = line, *t = field(&p), *u = field(&p);
			if (!*u)
				continue;
			clean(FAV[NFAV].title, t, sizeof FAV[0].title);
			clean(FAV[NFAV].url, u, sizeof FAV[0].url);
			NFAV++;
		}
		fclose(f);
	} else { // la primera vez: unos cuantos de serie
		static const char *const DEF[][2] = {{"Google", "https://www.google.com/"},
		                                     {"Wikipedia", "https://es.wikipedia.org/"},
		                                     {"3DBrew", "https://www.3dbrew.org/"}};
		for (size_t i = 0; i < sizeof DEF / sizeof DEF[0]; i++) {
			util_strlcpy(FAV[NFAV].title, DEF[i][0], sizeof FAV[0].title);
			util_strlcpy(FAV[NFAV].url, DEF[i][1], sizeof FAV[0].url);
			NFAV++;
		}
	}
	f = fopen(HIST_PATH, "r");
	if (f) {
		while (NHI < BM_HIST_MAX && fgets(line, 1024, f)) {
			line[strcspn(line, "\r\n")] = 0;
			char *p = line, *ts = field(&p), *t = field(&p), *u = field(&p);
			if (!*u)
				continue;
			HI[NHI].t = strtoll(ts, NULL, 10);
			clean(HI[NHI].title, t, sizeof HI[0].title);
			clean(HI[NHI].url, u, sizeof HI[0].url);
			NHI++;
		}
		fclose(f);
	}
	free(line);
}

int bm_nfav(void)
{
	return NFAV;
}

const BmFav *bm_fav(int i)
{
	return i >= 0 && i < NFAV ? &FAV[i] : NULL;
}

// la misma página aunque cambie la barra final o el #ancla
static bool same_url(const char *a, const char *b)
{
	size_t la = strcspn(a, "#"), lb = strcspn(b, "#");
	while (la > 0 && a[la - 1] == '/')
		la--;
	while (lb > 0 && b[lb - 1] == '/')
		lb--;
	return la == lb && !strncmp(a, b, la);
}

int bm_find_fav(const char *url)
{
	for (int i = 0; i < NFAV; i++)
		if (same_url(FAV[i].url, url))
			return i;
	return -1;
}

bool bm_add_fav(const char *title, const char *url)
{
	if (bm_find_fav(url) >= 0)
		return true;
	if (NFAV >= BM_FAV_MAX)
		return false;
	char host[96];
	bm_host(url, host, sizeof host);
	clean(FAV[NFAV].title, title && *title ? title : host, sizeof FAV[0].title);
	clean(FAV[NFAV].url, url, sizeof FAV[0].url);
	NFAV++;
	save_fav();
	return true;
}

void bm_del_fav(int i)
{
	if (i < 0 || i >= NFAV)
		return;
	memmove(&FAV[i], &FAV[i + 1], sizeof FAV[0] * (NFAV - i - 1));
	NFAV--;
	save_fav();
}

void bm_rename_fav(int i, const char *title)
{
	if (i < 0 || i >= NFAV || !title || !*title)
		return;
	clean(FAV[i].title, title, sizeof FAV[0].title);
	save_fav();
}

void bm_move_fav(int i, int to)
{
	if (i < 0 || i >= NFAV || to < 0 || to >= NFAV || i == to)
		return;
	BmFav tmp = FAV[i];
	if (to < i)
		memmove(&FAV[to + 1], &FAV[to], sizeof FAV[0] * (i - to));
	else
		memmove(&FAV[i], &FAV[i + 1], sizeof FAV[0] * (to - i));
	FAV[to] = tmp;
	save_fav();
}

int bm_nhist(void)
{
	return NHI;
}

const BmHist *bm_hist(int i)
{
	return i >= 0 && i < NHI ? &HI[i] : NULL;
}

void bm_visit(const char *title, const char *url)
{
	if (!url || !*url)
		return;
	// si ya estaba, se sube arriba (con la hora nueva)
	for (int i = 0; i < NHI; i++)
		if (same_url(HI[i].url, url)) {
			memmove(&HI[i], &HI[i + 1], sizeof HI[0] * (NHI - i - 1));
			NHI--;
			break;
		}
	if (NHI == BM_HIST_MAX)
		NHI--;
	memmove(&HI[1], &HI[0], sizeof HI[0] * NHI);
	char host[96];
	bm_host(url, host, sizeof host);
	HI[0].t = (int64_t)time(NULL);
	clean(HI[0].title, title && *title ? title : host, sizeof HI[0].title);
	clean(HI[0].url, url, sizeof HI[0].url);
	NHI++;
	save_hist();
}

void bm_del_hist(int i)
{
	if (i < 0 || i >= NHI)
		return;
	memmove(&HI[i], &HI[i + 1], sizeof HI[0] * (NHI - i - 1));
	NHI--;
	save_hist();
}

void bm_clear_hist(void)
{
	NHI = 0;
	remove(HIST_PATH);
}

void bm_host(const char *url, char *out, int n)
{
	const char *h = strstr(url, "://");
	if (!h) { // de la SD: el nombre del archivo
		const char *sl = strrchr(url, '/');
		snprintf(out, n, "%s", sl ? sl + 1 : url);
		return;
	}
	h += 3;
	if (!strncasecmp(h, "www.", 4))
		h += 4;
	snprintf(out, n, "%.*s", (int)strcspn(h, "/?#:"), h);
}

char bm_letter(const char *title, const char *url)
{
	for (const char *p = title; p && *p; p++)
		if (isalnum((unsigned char)*p))
			return (char)toupper((unsigned char)*p);
	char host[96];
	bm_host(url, host, sizeof host);
	return host[0] ? (char)toupper((unsigned char)host[0]) : '?';
}

uint32_t bm_color(const char *url)
{
	static const uint32_t PAL[] = {0xFFFF9C3B, 0xFF7AC42E, 0xFF3E8AF0, 0xFFFF5C7A, 0xFF4B53E5, 0xFF3DB8F5, 0xFFCCDD29, 0xFFA855F7};
	char host[96];
	bm_host(url, host, sizeof host);
	uint32_t h = 2166136261u;
	for (const char *p = host; *p; p++)
		h = (h ^ (unsigned char)*p) * 16777619u;
	return PAL[h % (sizeof PAL / sizeof PAL[0])];
}
