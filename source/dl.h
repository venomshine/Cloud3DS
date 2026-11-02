/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Descargas de Internet: una cola que la consola baja sola, de una en una,
 * aunque se cierre el navegador. Se pueden pausar y reanudar (si el servidor
 * lo permite) y comprobar con un MD5 o SHA-256. Al terminar, un .cia puede
 * pasar a la tarjeta de instalar y un .zip descomprimirse. Un enlace a un
 * repositorio de GitHub (o a sus releases) baja el .cia de la última versión. */

#define DL_MAX 16

enum { DL_QUEUED, DL_RUN, DL_PAUSED, DL_DONE, DL_FAIL, DL_CANCELED };
enum { DLA_NONE = 0, DLA_INSTALL = 1, DLA_UNZIP = 2, DLA_STREAM = 4 }; /* se pueden juntar: lo que toque según sea .cia o .zip.
 * DLA_STREAM (solo pedido desde la propia consola): un .cia se instala mientras baja, sin guardarlo */
enum { DL_PAUSE, DL_RESUME, DL_CANCEL, DL_REMOVE, DL_INSECURE };

typedef struct {
	u32  id;
	int  st;
	char url[1024];
	char host[64];
	char dir[512];     /* dónde se guarda */
	char name[256];    /* nombre del archivo (en cuanto se sabe) */
	char path[700];    /* dónde quedó, al terminar */
	char hash[65];     /* MD5 o SHA-256 esperado (vacío: no se comprueba) */
	int  hash_ok;      /* -1 sin comprobar, 0 no coincide, 1 coincide */
	int  after;        /* DLA_* juntos: qué hacer al terminar */
	bool after_done;   /* ya se pasó a instalar o a descomprimir */
	char device[48];
	char err[24];      /* «offline», «tls», «http», «not_found», «no_space», «no_cia», «io»… */
	u64  done, total, speed;
	bool resumable;
	bool insecure;     /* el usuario pidió bajarla sin comprobar el certificado (tras un error «tls») */
	bool installing;   /* se está instalando mientras baja (no se puede pausar) */
	bool installed;    /* se instaló mientras bajaba: no hay archivo en la SD */
} DlItem;

void dl_init(void);
void dl_exit(void);
/* 0 bien (*id el número); <0 si la cola está llena o el enlace no vale */
int  dl_add(const char *url, const char *dir, const char *hash, int after, const char *device, u32 *id);
int  dl_list(DlItem *out, int max); /* en el orden de la cola */
int  dl_op(u32 id, int op);
u32  dl_rev(void);                  /* cambia con cada novedad */
int  dl_active(void);               /* en cola o bajando */
bool dl_url_ok(const char *url);
