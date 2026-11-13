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

#define CLOUD_CACHE "/3ds/Cloud3DS/nube" /* donde se baja lo que se abre (fotos, textos, música…); se vacía al empezar */

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
	/* capturas en la nube */
	bool caps;              /* subir las capturas nuevas */
	int  caps_queue;        /* capturas nuevas esperando */
	/* abrir un archivo (se baja entero y la consola lo abre) */
	bool opening;
	char open_name[160];
	u64  open_size, open_got;
	u32  open_rev;          /* cambia al terminar; open_ok y open_path dicen cómo */
	bool open_ok;
	char open_path[700];
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
bool cloud_upload(const char *sd_path, const char *parent_id); /* archivo o carpeta. "" en parent: la carpeta Cloud3DS; "root": la raíz */
void cloud_delete(const CloudItem *it);             /* a la papelera de OneDrive */
/* gestionar, como en Archivos. parent_id: el de cloud_folder_id(); "" es la raíz. Si el nombre ya existe, se usa otro */
void cloud_mkdir(const char *parent_id, const char *name);
void cloud_rename(const CloudItem *it, const char *name);
void cloud_move(const CloudItem *it, const char *parent_id);  /* cortar y pegar */
void cloud_copy(const CloudItem *it, const char *parent_id);  /* la copia la hace OneDrive (tarda un poco en verse) */
void cloud_download_to(const CloudItem *it, const char *sd_dir); /* a esa carpeta de la SD; una carpeta, entera */
void cloud_upload_replace(const char *sd_path, const char *parent_id); /* sustituye el que se llame igual (guardar) */
bool cloud_open(const CloudItem *it, const char *sd_dir); /* bajarlo a sd_dir para abrirlo; false si ya hay uno */
void cloud_open_cancel(void);
const char *cloud_app_folder_id(void);              /* la carpeta Cloud3DS ("" si aún no se sabe) */

/* capturas en la nube: las nuevas de Luma se suben solas a /Cloud3DS/Capturas */
void cloud_set_caps(bool on);
void cloud_backup_now(void); /* subir ya las capturas pendientes */
