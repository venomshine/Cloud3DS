/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Nube: OneDrive con Microsoft Graph. Se conecta con el inicio de sesión por código (la consola
 * enseña un QR y un código; se acepta en el móvil). El permiso (refresh token) se guarda en
 * APP_DIR/onedrive.json y se renueva solo. Todo lo que habla con Microsoft va en un hilo. */

enum { CLS_NOCFG, CLS_OFF, CLS_CODE, CLS_ON };

typedef struct {
	int  st;
	u32  rev;               /* cambia con cada novedad */
	bool busy;              /* esperando a Microsoft */
	char err[96];           /* el último error, para enseñarlo */
	/* conectando */
	char user_code[24], verify[128];
	u64  code_until;        /* ms (util_ms) en que caduca el código */
	/* conectado */
	char name[96], email[128];
	u64  used, total;
} CloudInfo;

typedef struct {
	char id[64];
	char name[160];
	u64  size;
	bool dir;
	int  children;          /* en carpetas */
	char mtime[24];         /* «2026-11-03T18:42:10Z» */
} CloudItem;

void cloud_init(void);
void cloud_exit(void);
void cloud_get(CloudInfo *out);
bool cl_connected(void);

void cloud_connect(void);      /* empieza el inicio de sesión por código */
void cloud_cancel(void);       /* deja de esperar al código */
void cloud_disconnect(void);   /* borra el permiso de la SD */

/* la carpeta que se ve: id de OneDrive ("" = raíz) */
void cloud_list(const char *id, const char *name);
void cloud_up(void);           /* a la carpeta de encima; false si ya se está en la raíz */
int  cloud_depth(void);
const char *cloud_folder_name(void);
const char *cloud_folder_id(void);
/* copia de lo que hay en la carpeta (se renueva cuando cambia rev) */
int  cloud_items(CloudItem *out, int max, bool *loading);

void cloud_download(const CloudItem *it);           /* a la cola de descargas (un .cia se instala al bajar) */
void cloud_upload(const char *sd_path, const char *parent_id); /* "" en parent: la carpeta Cloud3DS */
void cloud_delete(const CloudItem *it);             /* a la papelera de OneDrive */
