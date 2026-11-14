/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* SoundCloud: buscar canciones y playlists y escucharlas en streaming.
 * - Habla con la API que usa la web de SoundCloud (api-v2) con el identificador de cliente que lleva su propia web:
 *   se saca de ella y se guarda (APP_DIR/soundcloud.json); si deja de valer, se vuelve a sacar.
 * - Las búsquedas van en su hilo; las canciones que se han visto quedan apuntadas (sc_track) para el reproductor.
 * - El audio llega por HLS en trozos fMP4 con AAC (schls.c) y lo decodifica FAAD2: el reproductor de música (music.c)
 *   lo usa como un archivo más, con la ruta «sc:<id>». */

#define SC_MAX 60

typedef struct {
	char id[24];
	char title[128], artist[96];
	u32  dur_ms;
	char art[256];       /* carátula (o el avatar del autor) */
	char tc[400];        /* la transcodificación AAC elegida (api-v2) */
	char auth[700];      /* track_authorization */
	bool preview;        /* solo 30 s (la canción completa es de pago) */
	bool downloadable;   /* el autor deja descargarla */
} ScTrack;

typedef struct {
	char id[24];
	char title[128], user[96];
	int  count;
} ScList;

enum { SCQ_IDLE, SCQ_BUSY, SCQ_DONE, SCQ_ERR };

void sc_init(void);
void sc_exit(void);

/* buscar (canciones y playlists) o abrir una playlist (sus canciones), en su hilo */
void sc_search(const char *q);
void sc_open_list(const char *id, const char *title);
/* lo último encontrado (copia); st: SCQ_*, err: por qué falló. Devuelve el número de versión (cambia al llegar) */
u32  sc_results(ScTrack *t, int *nt, ScList *l, int *nl, int *st, char *err, size_t en);
bool sc_track(const char *id, ScTrack *out); /* una canción ya vista (para el reproductor) */
/* descargar una canción que el autor deja descargar: su audio (AAC, con título, artista y carátula) a SC_DL_DIR, en su
 * hilo y de una en una. 0 empezó; 1 ya hay otra en curso; 2 no se puede (no la deja o es un fragmento); 3 error */
#define SC_DL_DIR "/Music/SoundCloud"
enum { SCD_IDLE, SCD_RUN, SCD_DONE, SCD_FAIL };
int  sc_download(const char *id);
int  sc_download_state(char *title, size_t n, int *pct); /* SCD_*; DONE y FAIL se devuelven una vez */
bool sc_download_busy(char *title, size_t n, int *pct);  /* en curso (sin consumir el final) */

/* ---- streaming (desde el hilo de la música) */
typedef struct ScStream ScStream;
ScStream *sc_stream_open(const char *id);
/* 1 listo (rate, chans y duración), 0 aún no, -1 no se pudo (err) */
int  sc_stream_ready(ScStream *s, int *rate, int *chans, u32 *dur_ms, char *err, size_t en);
/* hasta frames muestras estéreo en out: cuántas; 0 se acabó; -1 aún no hay (cargando) */
int  sc_stream_read(ScStream *s, s16 *out, int frames);
bool sc_stream_cover(ScStream *s, u8 **rgba, int *w, int *h); /* la carátula, cuando llega (una vez) */
void sc_stream_close(ScStream *s);
