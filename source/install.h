/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Instalar CIA de la SD pedidos desde la web. La consola siempre confirma:
 * cada petición queda pendiente hasta que se elige Instalar, Solo guardar o
 * Rechazar. Las aceptadas se instalan de una en una en un hilo propio (AM). */

#define MAX_INS 8

enum { INS_FREE, INS_PENDING, INS_QUEUED, INS_RUNNING, INS_DONE, INS_FAILED, INS_KEPT, INS_REJECTED };
enum { INS_ACCEPT, INS_KEEP, INS_REJECT };

typedef struct {
	int  st;
	int  id;
	char path[256];     /* en la SD: "/cias/juego.cia" */
	char file[128];     /* nombre del archivo */
	char title[64];     /* nombre del juego (SMDH), o el del archivo */
	char publisher[64];
	char device[48];    /* quién lo pidió */
	u64  tid;
	u16  ver;           /* versión del CIA */
	int  inst_ver;      /* versión instalada, -1 si no está */
	bool self;          /* es Cloud3DS: se actualiza desde Ajustes */
	bool n3ds_only;     /* solo para New 3DS */
	u64  need;          /* espacio que pide AM para instalarlo */
	bool has_icon;
	u16  icon[48 * 48]; /* RGB565, ya sin teselar */
	u64  size, done;
	char err[24];
	u64  t_end;         /* ms */
} InsReq;

void ins_init(void);
void ins_exit(void);
/* desde el servidor (con el cerrojo de estado NO tomado): devuelve el id o <0 */
int  ins_request(const char *path, const char *device, char *err, size_t errn);
void ins_decide(int id, int what);   /* id < 0: todas las pendientes */
int  ins_pending(void);
/* copia de la cola para dibujar o para la web */
int  ins_snapshot(InsReq *out, int max);
bool ins_busy(void);
const char *ins_ver_text(u16 v, char *buf, size_t n); /* «1.2.3» */
