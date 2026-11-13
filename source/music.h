/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Reproductor de música: MP3, OGG (Vorbis) y WAV, en su propio hilo, así
 * sigue sonando mientras se usa el resto (y con la tapa cerrada). La carpeta
 * es la lista. El audio usa el firmware DSP de la consola si está en
 * /3ds/dspfirm.cdc y, si no, OpenDSP (libre), que va dentro de la app. */

enum { MU_OFF, MU_PLAY, MU_PAUSE, MU_ERROR };
enum { MR_OFF, MR_ALL, MR_ONE };

typedef struct {
	int  st;
	char dir[512], name[256];
	char title[128], artist[128];
	int  idx, n;          /* cuál de la lista y cuántas hay */
	u32  pos_ms, dur_ms;
	bool shuffle;
	int  repeat;          /* MR_* */
	float level;          /* 0-1: lo fuerte que suena ahora (para la onda) */
	u32  track;           /* cambia con cada canción (la carátula) */
	char err[32];         /* «no_dsp», «bad_file»… */
} MusicState;

bool music_is_audio(const char *name);
/* empieza por start, con names (de dir) como lista. Con dir "", names son rutas enteras (de carpetas distintas) */
bool music_play(const char *dir, char (*names)[256], int n, int start);
void music_toggle(void);
void music_next(void);
void music_prev(void);
void music_stop(void);
void music_shuffle(void);
void music_set_shuffle(bool on); /* para la próxima lista (music_play): sin reordenar la que suena */
void music_repeat(void);
void music_get(MusicState *s);
bool music_playing(void);  /* sonando (no en pausa) */
/* la carátula de la canción actual, si la trae (RGBA, malloc); *track dice de cuál */
bool music_cover(u8 **rgba, int *w, int *h, u32 *track);
void music_exit(void);
