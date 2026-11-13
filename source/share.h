/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Compartir: pasar aplicaciones homebrew de una consola a otra en la misma Wi-Fi.
 * - Las consolas se encuentran con un aviso UDP por difusión (puerto SHARE_PORT) o se añaden por su IP.
 * - Se vinculan una vez: las dos enseñan el mismo código de 4 cifras y la que recibe la petición la acepta. Cada una
 *   le da a la otra un token; con él se ven sus apps y se piden.
 * - Solo se comparte homebrew: los .3dsx de /3ds y los CIA de la SD que sean aplicaciones sin cifrar (NoCrypto).
 *   Nada del sistema, de cartucho ni de eShop: se vuelve a comprobar justo antes de enviar.
 * - Cada pedido lo acepta quien lo da, salvo que tenga a quien lo pide como «de confianza».
 * - Lo recibido se baja con el gestor de descargas: un .3dsx, a /3ds; un CIA, a /Downloads y a instalar. */

#define SHARE_PORT 47331
#define SHARE_PEERS 16
#define SHARE_ITEMS 96

enum { SK_3DSX, SK_CIA };
enum { SHR_NONE, SHR_WAIT, SHR_DOWNLOADING, SHR_DONE, SHR_REFUSED, SHR_FAIL }; /* lo pedido a otra consola */

typedef struct {
	char id[17], name[48];
	u32  ip;          /* en orden de red */
	int  port;
	bool paired, trusted, online;
	char tok_out[33]; /* para entrar en ella */
	char tok_in[33];  /* para que entre ella aquí */
} SharePeer;

typedef struct {
	int  id;
	char name[64], author[64];
	int  kind;        /* SK_* */
	u64  size;
	bool has_icon;
	u16  icon[48 * 48];
	char path[256];   /* (solo en las propias) */
	bool hidden;      /* (propias) no se comparte */
	int  st;          /* (de otra consola) SR_*: lo pedido */
} ShareItem;

/* algo que esta consola tiene que contestar */
enum { SP_PAIR = 1, SP_SEND };
typedef struct {
	int  rid, kind;
	char who[48], code[8], item[64];
	u64  size;
} SharePrompt;

void share_init(void);
void share_exit(void);
const char *share_my_name(void);

/* ---- pantalla */
void share_discover(void);                         /* buscar consolas (y comprobar las vinculadas) */
bool share_add_ip(const char *ip);                 /* añadir una a mano */
int  share_peers(SharePeer *out, int max);
u32  share_rev(void);
bool share_pair(int peer, char *code, size_t cn);  /* empieza a vincular: el código que hay que comprobar */
int  share_pair_state(int *peer, char *code, size_t cn); /* 0 nada, 1 esperando, 2 vinculada, 3 rechazada, 4 fallo */
void share_pair_cancel(void);
void share_set_trusted(int peer, bool on);
void share_unpair(int peer);
void share_open_catalog(int peer);                 /* lo que ofrece (se baja en su hilo) */
int  share_catalog(ShareItem *out, int max, int *peer, bool *loading, bool *failed);
void share_request(int peer, int item);            /* pedir una app (y bajarla cuando la acepten) */
int  share_mine(ShareItem *out, int max);          /* lo que ofrece esta consola */
void share_hide(const char *path, bool hide);
/* lo que hay que contestar (el primero); share_answer lo contesta */
bool share_prompt(SharePrompt *out);
void share_answer(int rid, bool yes);

/* ---- servidor (server.c, en su hilo). ip: de quién viene la petición */
void share_srv_hello(char *out, size_t n);
int  share_srv_pair(const char *from, const char *name, const char *code, const char *tok, u32 ip, int port);
int  share_srv_pair_st(int rid, char *tok, size_t tn);   /* 0 esperando, 1 aceptada (tok), 2 rechazada, -1 no existe */
int  share_srv_auth(const char *tok, u32 ip);             /* la consola vinculada con ese token; -1 si no */
char *share_srv_catalog(int peer);                        /* JSON (malloc) */
bool share_srv_icon(int peer, int item, u16 *px);
int  share_srv_req(int peer, int item);                   /* el número del pedido; <0 no se puede */
int  share_srv_req_st(int peer, int rid);                 /* 0 esperando, 1 aceptado, 2 rechazado, -1 no existe */
bool share_srv_file(int peer, int rid, char *path, size_t n, char *who, size_t wn); /* aceptado y aún homebrew */

/* ¿un CIA es una aplicación homebrew (sin cifrar)? */
bool share_cia_homebrew(const char *path);
