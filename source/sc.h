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

#define SC_MAX 100
#define SC_ARTISTS 20
#define SC_IDS_LEN (SC_MAX * 22)

typedef struct {
	char id[24];
	char title[128], artist[96];
	char uid[16];        /* el autor (para «Ir al artista») */
	char created[24];    /* cuándo se subió (ISO 8601: se ordena como texto) */
	u32  dur_ms;
	char art[256];       /* carátula (o el avatar del autor) */
	char tc[400];        /* la transcodificación AAC elegida (api-v2) */
	char auth[700];      /* track_authorization */
	bool preview;        /* solo 30 s (la canción completa es de pago) */
	bool downloadable;   /* el autor deja descargarla */
	u32  plays, likes;   /* reproducciones y «me gusta» en SoundCloud */
	char genre[32];
	char wave[96];       /* la onda (wave.sndcdn.com/…json) */
} ScTrack;

typedef struct {
	char id[80];         /* el número o, las de SoundCloud («Tendencias»), su urn («soundcloud:system-playlists:…») */
	char title[128], user[96];
	int  count;
	char art[256];       /* su carátula */
	bool album;
} ScList;

typedef struct {
	char id[16];
	char name[96];
	int  tracks;
	u32  followers;
	char art[256];       /* su foto */
} ScArtist;

enum { SCQ_IDLE, SCQ_BUSY, SCQ_DONE, SCQ_ERR };

void sc_init(void);
void sc_exit(void);

/* buscar (canciones y playlists) o abrir una playlist (sus canciones), en su hilo */
void sc_search(const char *q);
void sc_open_list(const char *id, const char *title);
void sc_open_ids(const char *ids);  /* canciones por su número («1,2,3», hasta SC_MAX), en ese orden (las guardadas) */
void sc_open_artist(const char *id); /* las canciones de un artista */
/* lo último encontrado (copia); st: SCQ_*, err: por qué falló. Devuelve el número de versión (cambia al llegar) */
/* novedades de los artistas que se siguen: ids y, de cada uno, desde cuándo se miró (vacío: la primera vez, solo se
 * apunta lo último, sin avisar). En su hilo, sin tocar lo que se está buscando */
#define SC_NEW_MAX 30
#ifndef SCL_ARTISTS_MAX
#define SCL_ARTISTS_MAX 40 /* artistas seguidos, como mucho */
#endif
void sc_check_new(const char (*ids)[16], const char (*since)[24], int n);
/* lo encontrado (las nuevas, la más reciente primero) y, de cada artista, lo más reciente visto; cambia el número de
 * versión al terminar. busy: aún buscando */
u32  sc_new_results(ScTrack *t, int *nt, char (*latest)[24], int *nl, bool *busy);
u32  sc_results(ScTrack *t, int *nt, ScList *l, int *nl, ScArtist *a, int *na, int *st, char *err, size_t en);
bool sc_track(const char *id, ScTrack *out); /* una canción ya vista (para el reproductor) */
/* la página del artista abierto (sc_open_artist): sus datos y cuántas de las canciones son las populares (van primero);
 * sus listas y álbumes llegan como playlists */
bool sc_artist_page(ScArtist *a, int *ntop);

/* ---- aparte de las búsquedas (en el mismo hilo, cuando no hay otra cosa) */
/* Descubrir: las selecciones de SoundCloud (tendencias por género, recomendadas…) */
#define SC_SECTIONS 6
#define SC_SEC_ITEMS 12
typedef struct {
	char title[64];
	int  n;
	ScList it[SC_SEC_ITEMS];
} ScSection;
void sc_discover(void);
u32  sc_discover_results(ScSection *s, int *n, bool *busy);

/* lo que suena: su onda (SC_WAVE barras, 0-255) y los comentarios con su momento (por orden) */
#define SC_WAVE 64
#define SC_COMMENTS 120
typedef struct {
	u32  ms;
	char user[40];
	char body[120];
} ScComment;
typedef struct {
	char id[24];
	bool has_wave;
	u8   wave[SC_WAVE];
	int  nc;
	ScComment c[SC_COMMENTS];
} ScInfo;
void sc_info(const char *id);
u32  sc_info_results(ScInfo *out); /* (cambia el número al llegar) */

/* la radio: canciones parecidas a una (para seguir cuando se acaba la lista) */
#define SC_RADIO 20
void sc_radio(const char *id);
u32  sc_radio_results(ScTrack *t, int *n, char *seed, size_t sn);

/* carátulas pequeñas (64×64 como mucho), en su propio hilo: se piden y se recogen cuando llegan */
void sc_thumb_want(const char *url);
bool sc_thumb_take(char *url, size_t un, u8 **rgba, int *w, int *h);
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
