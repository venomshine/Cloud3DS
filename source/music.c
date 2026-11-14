/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "music.h"
#include "img.h"
#include "sc.h"

#include <neaacdec.h>
#include "util.h"

#include <mpg123.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <tremor/ivorbisfile.h>

#define CH 0
#define NBUF 4
#define FRAMES 4096 /* por búfer, estéreo de 16 bits */
#define LIST_MAX 500

enum { C_NONE, C_TOGGLE, C_NEXT, C_PREV, C_STOP, C_PLAY };

static struct {
	LightLock lock;
	LightEvent ev;
	Thread thr;
	volatile bool quit;
	bool dsp, dsp_tried;
	u8 *opendsp;
	volatile int cmd;
	MusicState s;
	char (*list)[256];
	int n, *order; /* order: el orden en que suenan (aleatorio o no) */
	int pos;       /* posición en order */
	u8 *cover;
	int cw, ch;
	bool cover_ok;
} M;

static const char *const EXTS[] = {"mp3", "ogg", "wav", "aac"};

bool music_is_audio(const char *name)
{
	const char *d = strrchr(name, '.');
	if (!d)
		return false;
	for (size_t i = 0; i < sizeof EXTS / sizeof EXTS[0]; i++)
		if (!strcasecmp(d + 1, EXTS[i]))
			return true;
	return false;
}

/* audio: el firmware de la consola (/3ds/dspfirm.cdc) o, si no está, OpenDSP de romfs */
static bool dsp_up(void)
{
	if (M.dsp_tried)
		return M.dsp;
	M.dsp_tried = true;
	FILE *f = fopen("sdmc:/3ds/dspfirm.cdc", "rb");
	if (f) {
		fclose(f);
		if (R_SUCCEEDED(ndspInit()))
			return M.dsp = true;
	}
	f = fopen("romfs:/dsp/opendsp.cdc", "rb");
	if (f) {
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		fseek(f, 0, SEEK_SET);
		if (sz > 0x300 && sz < 1024 * 1024 && (M.opendsp = malloc(sz)) && fread(M.opendsp, 1, sz, f) == (size_t)sz &&
		    !memcmp(M.opendsp + 0x100, "DSP1", 4)) {
			ndspUseComponent(M.opendsp, (u32)sz, 0xFF, 0xFF); // debe seguir en memoria: ndsp lo recarga al despertar
			if (R_SUCCEEDED(ndspInit()))
				M.dsp = true;
		}
		fclose(f);
	}
	util_log("música: audio %s", M.dsp ? "listo" : "sin firmware DSP");
	return M.dsp;
}

/* ---------------------------------------------------------------- decodificadores */

typedef struct {
	int kind; /* 0 mp3, 1 ogg, 2 wav, 3 SoundCloud, 4 AAC (ADTS) */
	NeAACDecHandle aac;
	u8 *ab;          /* AAC: lo leído del archivo */
	u32 an, aat;     /* cuánto hay y por dónde va */
	bool aeof;
	ScStream *sc;
	int cancel; /* SoundCloud: la orden que llegó mientras cargaba */
	char err[32];
	mpg123_handle *mh;
	OggVorbis_File vf;
	FILE *f;
	int rate, chans;
	u32 left; /* wav: bytes que quedan */
	bool wav8;
} Dec;

static void copy_tag(char *out, size_t n, const mpg123_string *s)
{
	if (s && s->p && s->fill)
		snprintf(out, n, "%.*s", (int)s->fill, s->p);
}

/* ¿empieza como un MP3? (etiqueta ID3 o dos cabeceras de trama seguidas): con otra cosa, mpg123 se pone a buscar sin fin */
static bool looks_mp3(const char *sp)
{
	FILE *f = fopen(sp, "rb");
	if (!f)
		return false;
	u8 b[4096 + 4];
	size_t n = fread(b, 1, 10, f);
	long start = 0;
	if (n == 10 && !memcmp(b, "ID3", 3))
		start = 10 + ((b[6] & 127) << 21 | (b[7] & 127) << 14 | (b[8] & 127) << 7 | (b[9] & 127)) + ((b[5] & 0x10) ? 10 : 0);
	bool ok = false;
	if (!fseek(f, start, SEEK_SET) && (n = fread(b, 1, 4096, f)) >= 4) {
		static const int BR[2][16] = {{0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0},  // MPEG-1 capa III
		                              {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0}}; // MPEG-2/2.5
		static const int SR[4][3] = {{11025, 12000, 8000}, {0, 0, 0}, {22050, 24000, 16000}, {44100, 48000, 32000}};
		for (size_t i = 0; i + 4 <= n && !ok; i++) {
			u8 *h = b + i;
			if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0)
				continue;
			int ver = (h[1] >> 3) & 3, layer = (h[1] >> 1) & 3, bri = h[2] >> 4, sri = (h[2] >> 2) & 3, pad = (h[2] >> 1) & 1;
			if (ver == 1 || layer != 1 || !bri || bri == 15 || sri == 3)
				continue;
			int br = BR[ver == 3 ? 0 : 1][bri], sr = SR[ver][sri];
			int len = (ver == 3 ? 144 : 72) * br * 1000 / sr + pad;
			if (i + len + 2 <= n)
				ok = b[i + len] == 0xFF && (b[i + len + 1] & 0xE0) == 0xE0; // y la siguiente trama donde toca
			else
				ok = true; // la siguiente queda fuera de lo leído: vale con esta
		}
	}
	fclose(f);
	return ok;
}

static void set_loading(bool on)
{
	LightLock_Lock(&M.lock);
	M.s.loading = on;
	LightLock_Unlock(&M.lock);
}

// SoundCloud: abre el streaming y espera a que esté listo (sin dejar de atender las órdenes)
static bool dec_open_sc(Dec *d, const char *id, MusicState *st)
{
	d->kind = 3;
	ScTrack t;
	if (sc_track(id, &t)) { // (lo que se carga, ya a la vista)
		util_strlcpy(st->title, t.title, sizeof st->title), util_strlcpy(st->artist, t.artist, sizeof st->artist);
		LightLock_Lock(&M.lock);
		snprintf(M.s.title, sizeof M.s.title, "%s", t.title);
		snprintf(M.s.artist, sizeof M.s.artist, "%s", t.artist);
		M.s.pos_ms = 0, M.s.dur_ms = t.dur_ms;
		LightLock_Unlock(&M.lock);
	}
	if (!(d->sc = sc_stream_open(id)))
		return false;
	set_loading(true);
	char err[32] = "";
	u32 dur = 0;
	int r;
	while (!(r = sc_stream_ready(d->sc, &d->rate, &d->chans, &dur, err, sizeof err))) {
		int c = M.cmd;
		if (M.quit || (c != C_NONE && c != C_TOGGLE)) {
			M.cmd = C_NONE;
			d->cancel = c;
			break;
		}
		svcSleepThread(20 * 1000000ll);
	}
	set_loading(false);
	st->dur_ms = dur;
	if (r < 0)
		snprintf(d->err, sizeof d->err, "sc_%s", err);
	return r == 1;
}

/* AAC: la etiqueta ID3v2 del principio (la que pone la descarga de SoundCloud): título, artista, duración y carátula.
 * Devuelve dónde empieza el audio */
static long aac_id3(FILE *f, MusicState *st)
{
	u8 h[10];
	if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) || h[3] < 3) {
		fseek(f, 0, SEEK_SET);
		return 0;
	}
	u32 n = (u32)(h[6] & 127) << 21 | (u32)(h[7] & 127) << 14 | (u32)(h[8] & 127) << 7 | (h[9] & 127);
	u8 *b = n <= 2u << 20 ? malloc(n) : NULL;
	if (b && fread(b, 1, n, f) == n) {
		for (u32 p = 0; p + 10 <= n && b[p];) {
			u32 fs = h[3] == 4 ? (u32)(b[p + 4] & 127) << 21 | (u32)(b[p + 5] & 127) << 14 | (u32)(b[p + 6] & 127) << 7 | (b[p + 7] & 127)
			                   : (u32)b[p + 4] << 24 | (u32)b[p + 5] << 16 | (u32)b[p + 6] << 8 | b[p + 7];
			if (!fs || p + 10 + fs > n)
				break;
			const u8 *fb = b + p + 10;
			char *dst = !memcmp(b + p, "TIT2", 4) ? st->title : !memcmp(b + p, "TPE1", 4) ? st->artist : NULL;
			if (dst && (fb[0] == 0 || fb[0] == 3)) // (latín-1 o UTF-8)
				snprintf(dst, sizeof st->title, "%.*s", (int)fs - 1, (const char *)fb + 1);
			else if (!memcmp(b + p, "TLEN", 4) && fs < 16) {
				char t[16];
				snprintf(t, sizeof t, "%.*s", (int)fs - 1, (const char *)fb + 1);
				st->dur_ms = (u32)strtoul(t, NULL, 10);
			} else if (!memcmp(b + p, "APIC", 4) && fs > 4 && fb[0] == 0) { // codificación, MIME, tipo, descripción y la imagen
				const u8 *q = memchr(fb + 1, 0, fs - 1);
				if (q && q + 2 < fb + fs) {
					q += 2; // (el tipo)
					const u8 *e = memchr(q, 0, (size_t)(fb + fs - q));
					u8 *rgba;
					int w, hh;
					if (e && ++e < fb + fs && img_mem_fit(e, (size_t)(fb + fs - e), 160, 160, &rgba, &w, &hh) == IMG_OK) {
						LightLock_Lock(&M.lock);
						free(M.cover);
						M.cover = rgba, M.cw = w, M.ch = hh, M.cover_ok = true;
						LightLock_Unlock(&M.lock);
					}
				}
			}
			p += 10 + fs;
		}
	}
	free(b);
	fseek(f, 10 + (long)n, SEEK_SET);
	return 10 + (long)n;
}

#define AAC_BUF (32 * 1024)

static void aac_fill(Dec *d)
{
	if (d->aat) { // lo que queda, al principio
		memmove(d->ab, d->ab + d->aat, d->an - d->aat);
		d->an -= d->aat, d->aat = 0;
	}
	if (!d->aeof && d->an < AAC_BUF) {
		size_t r = fread(d->ab + d->an, 1, AAC_BUF - d->an, d->f);
		d->an += (u32)r;
		if (!r)
			d->aeof = true;
	}
}

static bool dec_open(Dec *d, const char *path, MusicState *st)
{
	memset(d, 0, sizeof *d);
	if (!strncmp(path, "sc:", 3))
		return dec_open_sc(d, path + 3, st);
	char sp[720];
	snprintf(sp, sizeof sp, "sdmc:%s", path);
	const char *x = strrchr(path, '.');
	x = x ? x + 1 : "";
	if (!strcasecmp(x, "mp3")) {
		int err;
		d->kind = 0;
		if (!looks_mp3(sp) || !(d->mh = mpg123_new(NULL, &err)))
			return false;
		mpg123_param(d->mh, MPG123_FLAGS, MPG123_FORCE_STEREO | MPG123_QUIET, 0);
		mpg123_param(d->mh, MPG123_ADD_FLAGS, MPG123_PICTURE, 0);
		mpg123_param(d->mh, MPG123_RESYNC_LIMIT, 64 * 1024, 0); // si no es un MP3, que no lo busque por todo el archivo
		if (mpg123_open(d->mh, sp) != MPG123_OK) {
			mpg123_delete(d->mh);
			return false;
		}
		long rate = 0;
		int ch, enc;
		if (mpg123_getformat(d->mh, &rate, &ch, &enc) != MPG123_OK || rate <= 0) {
			mpg123_close(d->mh);
			mpg123_delete(d->mh);
			d->mh = NULL;
			return false;
		}
		mpg123_format_none(d->mh);
		mpg123_format(d->mh, rate, MPG123_STEREO, MPG123_ENC_SIGNED_16);
		d->rate = (int)rate, d->chans = 2;
		off_t len = mpg123_length(d->mh);
		st->dur_ms = len > 0 && rate > 0 ? (u32)((u64)len * 1000 / rate) : 0;
		mpg123_id3v1 *v1;
		mpg123_id3v2 *v2;
		if (mpg123_id3(d->mh, &v1, &v2) == MPG123_OK) {
			if (v2) {
				copy_tag(st->title, sizeof st->title, v2->title);
				copy_tag(st->artist, sizeof st->artist, v2->artist);
				for (size_t i = 0; i < v2->pictures && !M.cover_ok; i++) { // la carátula (la portada, si hay varias)
					mpg123_picture *p = &v2->picture[i];
					if (p->data && p->size && (p->type == 3 || v2->pictures == 1)) {
						u8 *rgba;
						int w, h;
						if (img_mem_fit(p->data, p->size, 160, 160, &rgba, &w, &h) == IMG_OK) {
							LightLock_Lock(&M.lock);
							free(M.cover);
							M.cover = rgba, M.cw = w, M.ch = h, M.cover_ok = true;
							LightLock_Unlock(&M.lock);
						}
					}
				}
			}
			if (v1 && !st->title[0])
				snprintf(st->title, sizeof st->title, "%.30s", v1->title);
			if (v1 && !st->artist[0])
				snprintf(st->artist, sizeof st->artist, "%.30s", v1->artist);
		}
		return d->rate > 0;
	}
	if (!strcasecmp(x, "ogg")) {
		d->kind = 1;
		if (!(d->f = fopen(sp, "rb")))
			return false;
		if (ov_open(d->f, &d->vf, NULL, 0) < 0) {
			fclose(d->f);
			return false;
		}
		vorbis_info *vi = ov_info(&d->vf, -1);
		d->rate = vi ? (int)vi->rate : 0, d->chans = vi ? vi->channels : 0;
		ogg_int64_t ms = ov_time_total(&d->vf, -1); // tremor: en milisegundos
		st->dur_ms = ms > 0 ? (u32)ms : 0;
		vorbis_comment *vc = ov_comment(&d->vf, -1);
		for (int i = 0; vc && i < vc->comments; i++) {
			const char *c = vc->user_comments[i];
			if (!strncasecmp(c, "TITLE=", 6))
				snprintf(st->title, sizeof st->title, "%s", c + 6);
			else if (!strncasecmp(c, "ARTIST=", 7))
				snprintf(st->artist, sizeof st->artist, "%s", c + 7);
		}
		return d->rate > 0 && (d->chans == 1 || d->chans == 2);
	}
	if (!strcasecmp(x, "aac")) { // AAC en ADTS (las descargas de SoundCloud)
		d->kind = 4;
		if (!(d->f = fopen(sp, "rb")) || !(d->ab = malloc(AAC_BUF)))
			return false;
		aac_id3(d->f, st);
		aac_fill(d);
		unsigned long rate;
		unsigned char ch;
		d->aac = NeAACDecOpen();
		NeAACDecConfigurationPtr c = NeAACDecGetCurrentConfiguration(d->aac);
		c->outputFormat = FAAD_FMT_16BIT;
		c->downMatrix = 1;
		NeAACDecSetConfiguration(d->aac, c);
		long used = NeAACDecInit(d->aac, d->ab, d->an, &rate, &ch);
		if (used < 0)
			return false;
		d->aat = (u32)used;
		d->rate = (int)rate, d->chans = 2;
		return rate > 0;
	}
	if (!strcasecmp(x, "wav")) { // PCM de 8 o 16 bits, mono o estéreo
		d->kind = 2;
		if (!(d->f = fopen(sp, "rb")))
			return false;
		u8 h[12];
		if (fread(h, 1, 12, d->f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4))
			return false;
		int bits = 0;
		for (;;) {
			u8 c[8];
			if (fread(c, 1, 8, d->f) != 8)
				return false;
			u32 sz = c[4] | c[5] << 8 | c[6] << 16 | (u32)c[7] << 24;
			if (!memcmp(c, "fmt ", 4)) {
				u8 fm[16];
				if (sz < 16 || fread(fm, 1, 16, d->f) != 16)
					return false;
				if ((fm[0] | fm[1] << 8) != 1) // solo PCM
					return false;
				d->chans = fm[2] | fm[3] << 8;
				d->rate = fm[4] | fm[5] << 8 | fm[6] << 16 | (u32)fm[7] << 24;
				bits = fm[14] | fm[15] << 8;
				fseek(d->f, sz - 16 + (sz & 1), SEEK_CUR);
			} else if (!memcmp(c, "data", 4)) {
				d->left = sz;
				break;
			} else
				fseek(d->f, sz + (sz & 1), SEEK_CUR);
		}
		d->wav8 = bits == 8;
		if ((bits != 8 && bits != 16) || (d->chans != 1 && d->chans != 2) || d->rate <= 0)
			return false;
		u32 bps = (u32)d->rate * d->chans * (bits / 8);
		st->dur_ms = bps ? (u32)((u64)d->left * 1000 / bps) : 0;
		return true;
	}
	return false;
}

/* llena out con hasta FRAMES muestras estéreo; cuántas (0: se acabó) */
static int dec_read(Dec *d, s16 *out)
{
	int got = 0;
	if (d->kind == 3) { // -1: aún no ha llegado (se sigue esperando)
		got = sc_stream_read(d->sc, out, FRAMES);
		u8 *rgba;
		int w, h;
		if (sc_stream_cover(d->sc, &rgba, &w, &h)) {
			LightLock_Lock(&M.lock);
			free(M.cover);
			M.cover = rgba, M.cw = w, M.ch = h, M.cover_ok = true;
			M.s.cover_rev++;
			LightLock_Unlock(&M.lock);
		}
		return got;
	}
	if (d->kind == 0) {
		size_t done = 0;
		int r = mpg123_read(d->mh, (u8 *)out, FRAMES * 4, &done);
		got = (int)(done / 4);
		if (r != MPG123_OK && r != MPG123_NEW_FORMAT && !got)
			return 0;
		return got;
	}
	if (d->kind == 4) { // tramas de 1024 muestras hasta llenar
		while (got + 2048 <= FRAMES) {
			aac_fill(d);
			if (d->an - d->aat < 8)
				break;
			NeAACDecFrameInfo fi;
			s16 *pcm = NeAACDecDecode(d->aac, &fi, d->ab + d->aat, d->an - d->aat);
			if (fi.error) { // (una trama dañada: se busca la siguiente)
				d->aat++;
				continue;
			}
			d->aat += (u32)fi.bytesconsumed;
			if (!fi.bytesconsumed)
				break;
			int ch = fi.channels ? fi.channels : 2, n = (int)(fi.samples / ch);
			for (int i = 0; pcm && i < n; i++) {
				out[(got + i) * 2] = pcm[i * ch];
				out[(got + i) * 2 + 1] = ch > 1 ? pcm[i * ch + 1] : pcm[i * ch];
			}
			got += pcm ? n : 0;
		}
		return got;
	}
	static s16 tmp[FRAMES * 2];
	if (d->kind == 1) {
		int want = FRAMES * d->chans * 2, bs;
		while (got < want) {
			long r = ov_read(&d->vf, (char *)tmp + got, want - got, &bs);
			if (r <= 0)
				break;
			got += (int)r;
		}
		got /= 2 * d->chans;
	} else {
		int bytes_per = d->chans * (d->wav8 ? 1 : 2);
		u32 want = (u32)FRAMES * bytes_per;
		if (want > d->left)
			want = d->left;
		size_t r = want ? fread(tmp, 1, want, d->f) : 0;
		d->left -= (u32)r;
		got = (int)(r / bytes_per);
		if (d->wav8) { // de 8 bits sin signo a 16 con signo (desde el final, en el mismo búfer)
			u8 *b = (u8 *)tmp;
			for (int i = got * d->chans - 1; i >= 0; i--)
				tmp[i] = (s16)((b[i] - 128) << 8);
		}
	}
	for (int i = 0; i < got; i++) { // a estéreo
		s16 l = tmp[i * d->chans], r = d->chans == 2 ? tmp[i * 2 + 1] : l;
		out[i * 2] = l, out[i * 2 + 1] = r;
	}
	return got;
}

static void dec_close(Dec *d)
{
	if (d->kind == 3)
		sc_stream_close(d->sc);
	else if (d->kind == 4) {
		if (d->aac)
			NeAACDecClose(d->aac);
		free(d->ab);
		if (d->f)
			fclose(d->f);
	}
	else if (d->kind == 0 && d->mh) {
		mpg123_close(d->mh);
		mpg123_delete(d->mh);
	} else if (d->kind == 1 && d->f)
		ov_clear(&d->vf); // cierra también el archivo
	else if (d->f)
		fclose(d->f);
	memset(d, 0, sizeof *d);
}

/* ---------------------------------------------------------------- lista */

static void shuffle_order(void)
{
	for (int i = 0; i < M.n; i++)
		M.order[i] = i;
	if (!M.s.shuffle)
		return;
	for (int i = M.n - 1; i > 0; i--) {
		u32 r;
		util_rand_bytes(&r, sizeof r);
		int j = (int)(r % (u32)(i + 1)), t = M.order[i];
		M.order[i] = M.order[j], M.order[j] = t;
	}
}

/* ---------------------------------------------------------------- el hilo */

static void set_state(int st, const char *err)
{
	LightLock_Lock(&M.lock);
	M.s.st = st;
	snprintf(M.s.err, sizeof M.s.err, "%s", err ? err : "");
	LightLock_Unlock(&M.lock);
}

/* toca la canción de order[pos]; devuelve el siguiente comando que la cortó (o C_NONE si acabó sola) */
static int play_one(s16 **buf, ndspWaveBuf *wb)
{
	int idx = M.order[M.pos];
	char path[800];
	MusicState st = {0};
	LightLock_Lock(&M.lock);
	if (!M.s.dir[0]) // (la biblioteca: cada una con su ruta entera)
		snprintf(path, sizeof path, "%s", M.list[idx]);
	else
		snprintf(path, sizeof path, "%s%s%s", M.s.dir, strcmp(M.s.dir, "/") ? "/" : "", M.list[idx]);
	free(M.cover);
	M.cover = NULL, M.cover_ok = false;
	M.s.cover_rev++, M.s.idx = idx; // (la de la anterior, fuera ya)
	LightLock_Unlock(&M.lock);
	Dec d;
	bool ok = dec_open(&d, path, &st);
	LightLock_Lock(&M.lock);
	snprintf(M.s.name, sizeof M.s.name, "%s", M.list[idx]);
	if (!st.title[0]) { // sin etiquetas: el nombre del archivo, sin extensión
		const char *bn = strrchr(M.list[idx], '/');
		snprintf(st.title, sizeof st.title, "%s", bn ? bn + 1 : M.list[idx]);
		char *dot = strrchr(st.title, '.');
		if (dot)
			*dot = 0;
	}
	snprintf(M.s.title, sizeof M.s.title, "%s", st.title);
	snprintf(M.s.artist, sizeof M.s.artist, "%s", st.artist);
	M.s.idx = idx, M.s.dur_ms = st.dur_ms, M.s.pos_ms = 0, M.s.track++;
	M.s.st = ok ? MU_PLAY : MU_ERROR;
	snprintf(M.s.err, sizeof M.s.err, "%s", ok ? "" : d.err[0] ? d.err : "bad_file");
	LightLock_Unlock(&M.lock);
	if (!ok) {
		int c = d.cancel;
		dec_close(&d);
		if (c) { // (una orden mientras cargaba: no es un fallo)
			set_state(MU_PLAY, NULL);
			return c;
		}
		return C_NONE; // a la siguiente
	}
	ndspChnReset(CH);
	ndspChnSetInterp(CH, NDSP_INTERP_POLYPHASE);
	ndspChnSetFormat(CH, NDSP_FORMAT_STEREO_PCM16);
	ndspChnSetRate(CH, (float)d.rate);
	float mix[12] = {1, 1};
	ndspChnSetMix(CH, mix);
	u64 played = 0; // muestras ya sonadas (búferes terminados)
	int queued_samples[NBUF] = {0};
	bool eof = false, paused = false;
	int cmd = C_NONE;
	for (int i = 0; i < NBUF; i++)
		wb[i].status = NDSP_WBUF_DONE;
	while (!M.quit) {
		int c = M.cmd;
		if (c != C_NONE) {
			M.cmd = C_NONE;
			if (c == C_TOGGLE) {
				paused = !paused;
				ndspChnSetPaused(CH, paused);
				set_state(paused ? MU_PAUSE : MU_PLAY, NULL);
			} else {
				cmd = c;
				break;
			}
		}
		bool busy = false, starved = false;
		for (int i = 0; i < NBUF; i++) {
			if (wb[i].status == NDSP_WBUF_QUEUED || wb[i].status == NDSP_WBUF_PLAYING) {
				busy = true;
				continue;
			}
			if (queued_samples[i]) { // este terminó de sonar
				played += (u64)queued_samples[i];
				queued_samples[i] = 0;
			}
			if (eof || paused)
				continue;
			int got = dec_read(&d, buf[i]);
			if (got < 0) { // (SoundCloud: aún no ha llegado)
				starved = true;
				break;
			}
			if (got == 0) {
				eof = true;
				continue;
			}
			if (d.kind == 3 && M.s.loading)
				set_loading(false);
			// lo fuerte que suena (para la onda de arriba)
			u64 acc = 0;
			for (int k = 0; k < got * 2; k += 8)
				acc += (u64)(buf[i][k] < 0 ? -buf[i][k] : buf[i][k]);
			float lv = (float)acc / (got / 4 + 1) / 32768.0f * 3;
			DSP_FlushDataCache(buf[i], got * 4);
			wb[i].data_vaddr = buf[i];
			wb[i].nsamples = got;
			ndspChnWaveBufAdd(CH, &wb[i]);
			queued_samples[i] = got;
			busy = true;
			LightLock_Lock(&M.lock);
			M.s.level = lv > 1 ? 1 : lv;
			LightLock_Unlock(&M.lock);
		}
		if (starved && !busy && !M.s.loading)
			set_loading(true);
		LightLock_Lock(&M.lock);
		M.s.pos_ms = (u32)((played + (busy && !paused ? ndspChnGetSamplePos(CH) : 0)) * 1000 / (u64)d.rate);
		LightLock_Unlock(&M.lock);
		if (eof && !busy)
			break;
		LightEvent_WaitTimeout(&M.ev, 10 * 1000000ll);
	}
	ndspChnWaveBufClear(CH);
	svcSleepThread(30 * 1000000ll); // que el DSP suelte los búferes
	dec_close(&d);
	return cmd;
}

static void music_thread(void *arg)
{
	(void)arg;
	s16 *buf[NBUF];
	static ndspWaveBuf wb[NBUF];
	for (int i = 0; i < NBUF; i++) {
		buf[i] = linearAlloc(FRAMES * 4);
		memset(&wb[i], 0, sizeof wb[i]);
	}
	while (!M.quit) {
		int c = M.cmd;
		if (c != C_PLAY) {
			LightEvent_WaitTimeout(&M.ev, 100 * 1000000ll);
			continue;
		}
		M.cmd = C_NONE;
		int fails = 0;
		while (!M.quit && M.n > 0) {
			int r = play_one(buf, wb);
			if (M.s.st == MU_ERROR && ++fails >= M.n) // no se puede abrir ninguna
				break;
			if (M.s.st != MU_ERROR)
				fails = 0;
			if (r == C_STOP || r == C_PLAY) {
				if (r == C_PLAY)
					M.cmd = C_PLAY; // otra lista: se vuelve a empezar
				break;
			}
			LightLock_Lock(&M.lock);
			int rep = M.s.repeat;
			LightLock_Unlock(&M.lock);
			if (r == C_PREV)
				M.pos = (M.pos + M.n - 1) % M.n;
			else if (r == C_NEXT || rep != MR_ONE) {
				M.pos++;
				if (M.pos >= M.n) {
					if (rep == MR_OFF && r != C_NEXT) // se acabó la carpeta
						break;
					M.pos = 0;
				}
			}
		}
		if (M.cmd != C_PLAY && M.s.st != MU_ERROR)
			set_state(MU_OFF, NULL);
	}
	for (int i = 0; i < NBUF; i++)
		linearFree(buf[i]);
}

static void mu_send(int c)
{
	M.cmd = c;
	LightEvent_Signal(&M.ev);
}

bool music_play(const char *dir, char (*names)[256], int n, int start)
{
	if (!M.thr) {
		LightLock_Init(&M.lock);
		LightEvent_Init(&M.ev, RESET_ONESHOT);
		mpg123_init();
	}
	if (!dsp_up()) {
		LightLock_Lock(&M.lock);
		M.s.st = MU_ERROR;
		snprintf(M.s.err, sizeof M.s.err, "no_dsp");
		LightLock_Unlock(&M.lock);
		return false;
	}
	if (M.thr && M.s.st != MU_OFF) { // ya suena algo: se para y se cambia la lista
		mu_send(C_STOP);
		for (int i = 0; i < 100 && M.s.st != MU_OFF; i++)
			svcSleepThread(10 * 1000000ll);
	}
	if (n > LIST_MAX)
		n = LIST_MAX;
	LightLock_Lock(&M.lock);
	free(M.list);
	free(M.order);
	M.list = malloc(n * sizeof *M.list);
	M.order = malloc(n * sizeof *M.order);
	if (!M.list || !M.order) {
		LightLock_Unlock(&M.lock);
		return false;
	}
	memcpy(M.list, names, n * sizeof *M.list);
	M.n = n;
	snprintf(M.s.dir, sizeof M.s.dir, "%s", dir);
	M.s.n = n;
	shuffle_order();
	M.pos = 0;
	for (int i = 0; i < n; i++) // la elegida suena primero
		if (M.order[i] == start) {
			M.pos = i;
			break;
		}
	if (M.s.shuffle && M.pos) { // en aleatorio: la elegida, y luego las demás
		int t = M.order[0];
		M.order[0] = M.order[M.pos], M.order[M.pos] = t, M.pos = 0;
	}
	M.s.st = MU_PLAY;
	LightLock_Unlock(&M.lock);
	if (!M.thr) {
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		M.thr = threadCreate(music_thread, NULL, 64 * 1024, prio - 2, -2, false); // por delante de la interfaz: que no se corte
	}
	mu_send(C_PLAY);
	return M.thr != NULL;
}

void music_toggle(void) { if (M.thr && M.s.st != MU_OFF) mu_send(C_TOGGLE); }
void music_next(void) { if (M.thr && M.s.st != MU_OFF) mu_send(C_NEXT); }
void music_prev(void) { if (M.thr && M.s.st != MU_OFF) mu_send(C_PREV); }
void music_stop(void) { if (M.thr && M.s.st != MU_OFF) mu_send(C_STOP); }

void music_set_shuffle(bool on)
{
	if (!M.thr) {
		LightLock_Init(&M.lock);
		M.s.shuffle = on;
		return;
	}
	LightLock_Lock(&M.lock);
	M.s.shuffle = on;
	LightLock_Unlock(&M.lock);
}

void music_shuffle(void)
{
	if (!M.thr)
		return;
	LightLock_Lock(&M.lock);
	M.s.shuffle = !M.s.shuffle;
	int cur = M.n ? M.order[M.pos] : 0;
	shuffle_order();
	for (int i = 0; i < M.n; i++) // sigue por la que suena
		if (M.order[i] == cur)
			M.pos = i;
	LightLock_Unlock(&M.lock);
}

void music_repeat(void)
{
	if (!M.thr)
		return;
	LightLock_Lock(&M.lock);
	M.s.repeat = (M.s.repeat + 1) % 3;
	LightLock_Unlock(&M.lock);
}

void music_get(MusicState *s)
{
	if (!M.thr) {
		memset(s, 0, sizeof *s);
		s->st = M.s.st;
		snprintf(s->err, sizeof s->err, "%s", M.s.err);
		return;
	}
	LightLock_Lock(&M.lock);
	*s = M.s;
	LightLock_Unlock(&M.lock);
}

bool music_playing(void)
{
	return M.thr && M.s.st == MU_PLAY;
}

bool music_cover(u8 **rgba, int *w, int *h, u32 *track)
{
	if (!M.thr)
		return false;
	LightLock_Lock(&M.lock);
	bool ok = M.cover_ok && M.cover;
	if (ok) {
		*rgba = malloc((size_t)M.cw * M.ch * 4);
		if (*rgba)
			memcpy(*rgba, M.cover, (size_t)M.cw * M.ch * 4);
		else
			ok = false;
		*w = M.cw, *h = M.ch;
	}
	*track = M.s.track;
	LightLock_Unlock(&M.lock);
	return ok;
}

void music_exit(void)
{
	if (M.thr) {
		M.quit = true;
		mu_send(C_STOP);
		threadJoin(M.thr, 2000000000ull);
		threadFree(M.thr);
		M.thr = NULL;
	}
	if (M.dsp)
		ndspExit();
	free(M.opendsp);
	free(M.list);
	free(M.order);
	free(M.cover);
}
