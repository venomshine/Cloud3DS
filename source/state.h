/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Estado compartido entre el hilo del servidor y la interfaz.
 * Todo acceso va con state_lock()/state_unlock(). */

#define MAX_XFERS    6
#define MAX_HISTORY  60
#define MAX_DEVICES  16
#define MAX_REVOKED  8

typedef enum { XF_UPLOAD, XF_DOWNLOAD } XferKind;
typedef enum { XS_FREE, XS_RUNNING, XS_DONE, XS_FAILED, XS_CANCELED } XferState;

typedef struct {
	XferState st;
	XferKind kind;
	int id;
	char name[128];
	char device[48];
	u64 total, done;
	u64 t_start, t_end;  /* ms */
	u64 speed;           /* bytes/s suavizado */
	u64 spd_t, spd_done; /* última muestra */
} Xfer;

typedef enum {
	EV_UPLOAD,
	EV_DOWNLOAD,
	EV_UPLOAD_FAIL,
	EV_MKDIR,
	EV_RENAME,
	EV_DELETE,
	EV_PAIRED,
	EV_REVOKED,
	EV_PIN_LOCK,   /* bloqueo por intentos fallidos: device = IP, size = segundos */
} EventKind;

typedef struct {
	EventKind kind;
	u64 when; /* unix */
	u64 size;
	char name[128];
	char device[48];
} Event;

typedef struct {
	bool used;
	char token[33];
	char name[48];   /* "iPhone · Safari" */
	int  kind;       /* 0 móvil, 1 ordenador, 2 tableta */
	char ip[16];
	u64  created;    /* unix */
	u64  last_seen;  /* unix */
	u64  last_ms;    /* ms, para «conectado ahora» */
} Device;

typedef struct {
	/* ajustes */
	int  lang;        /* 0 sistema, 1 es, 2 en, 3 ja */
	bool light;       /* tema claro */
	bool readonly;
	bool protect_sys; /* pedir confirmación en carpetas del sistema */

	/* servidor */
	char console[48]; /* «3DS de Mario» */
	u32  ip;
	int  port;
	bool running;
	char srv_error[96];
	char pin[5];
	char qr_key[9];
	int  pin_fails;
	u64  stat_up, stat_down; /* bytes totales en esta sesión */

	Xfer   xfers[MAX_XFERS];
	Event  hist[MAX_HISTORY];
	int    hist_n, hist_head; /* anillo: hist_head es el más reciente */
	Device dev[MAX_DEVICES];
	u32    rev; /* cambia en cada modificación visible */
	bool   dirty_cfg;
	char   revoked[MAX_REVOKED][33]; /* expulsados recientes, para avisarles */
	int    revoked_next;
} AppState;

extern AppState G;

void state_init(void);
void state_lock(void);
void state_unlock(void);

/* Llamar con el cerrojo tomado */
int    xfer_begin(XferKind k, const char *name, const char *device, u64 total);
void   xfer_progress(int id, u64 done);
void   xfer_end(int id, XferState st);
Xfer  *xfer_get(int id);
int    xfer_active(void);
void   event_add(EventKind k, const char *name, const char *device, u64 size);
const Event *event_at(int i); /* 0 = más reciente */
void   pin_regen(void);           /* PIN y QR nuevos al azar */
bool   pin_set(const char *pin);   /* PIN elegido (4 cifras) y QR nuevo */
void   device_revoke(int i); /* expulsa y recuerda el token */

/* Configuración en la SD (config.json) */
void config_load(void);
void config_save(void);
