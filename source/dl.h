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
	char url[2048];    /* las de descarga de OneDrive son largas */
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
	bool fixed_name;   /* el nombre lo puso quien la pidió (la nube): manda sobre el del servidor */
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

/* GET con el servicio HTTP del sistema (comprobando el certificado), para el actualizador.
 * DLF_NOCONN: no conectó (se puede probar con libcurl); con DLF_OK, *code es el estado HTTP */
enum { DLF_OK, DLF_NOCONN, DLF_FAIL };
/* para el hilo que llama: User-Agent propio (NULL, el de Cloud3DS) y la dirección final de su último dl_fetch */
void dl_set_thread_ua(const char *ua);
/* las peticiones de este hilo se cortan (en ~1 s) en cuanto *a o *b sea true (NULL: no se mira) */
void dl_set_thread_stop(volatile bool *a, volatile bool *b);
/* para el hilo que llama: de dónde sacar las cookies que mandar y dónde guardar las que lleguen (NULL, ninguna) */
void dl_set_thread_cookies(size_t (*get)(const char *url, char *out, size_t n), void (*set)(const char *url, const char *set_cookie));
const char *dl_last_url(void);
/* el Content-Type de la última respuesta (de este hilo), con su charset si lo dice; "" si no */
const char *dl_last_ctype(void);
/* lo mismo con libcurl (TLS moderno): para webs que no aceptan a httpc */
int dl_fetch_curl(const char *url, const char *accept, size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code);
int dl_fetch(const char *url, const char *accept, size_t (*wr)(char *, size_t, size_t, void *), void (*tot)(void *, u64), void *u, long *code);
/* petición general (método HTTPC_METHOD_*, cabeceras «Nombre: valor» acabadas en NULL, cuerpo opcional) */
int dl_request(int method, const char *url, const char *const *headers, const void *body, size_t blen,
               size_t (*wr)(char *, size_t, size_t, void *), void *u, long *code);
void dl_set_name(u32 id, const char *name); /* nombre fijo para una descarga (la nube) */
