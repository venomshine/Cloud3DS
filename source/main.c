/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - bucle principal e interfaz de la consola
#include "app.h"
#include "fsx.h"
#include "browser.h"
#include "cloud.h"
#include "i18n.h"
#include "ops.h"
#include "update.h"
#include "install.h"
#include "img.h"
#include "txt.h"
#include "hl.h"
#include "dl.h"
#include "hash.h"
#include "music.h"
#include "reader.h"
#include "mlib.h"
#include "share.h"
#include "live.h"
#include "titles.h"
#include "remote.h"
#include "qrcam.h"
#include "zipjob.h"
#include "search.h"
#include "trash.h"
#include <ctype.h>
#include <time.h>
#include <stdlib.h>
#include <strings.h>
#include "server.h"
#include "state.h"
#include "ui.h"
#include "util.h"

// pila del hilo principal: el navegador recorre árboles de páginas grandes de forma recursiva
u32 __stacksize__ = 256 * 1024;

#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
#include <sys/stat.h>
#include <string.h>

// escalas de texto: siempre las mismas en toda la interfaz
#define TS_TITLE 0.58f
#define TS_BODY  0.48f
#define TS_SUB   0.40f
#define TS_HDR   0.45f

// pantalla superior
#define HDR_H  22
#define CARD_X 12
#define CARD_Y 28
#define CARD_W (TOP_W - 24)
#define CARD_H 204
#define QR_X   24
#define QR_Y   40
#define QR_S   180
#define COL_X  216
#define COL_W  160

// pantalla inferior
#define TABBAR_H 0 /* (sin barra de pestañas: cada aplicación ocupa su pantalla; el inicio, con su botón) */
#define VIEW_Y   32
#define VIEW_H   (SCR_H - TABBAR_H - VIEW_Y)
#define QR_PAD   8
#define MINI_H   30 // tira de arriba con el QR cerrado
#define SDB_H    44 // tarjeta de la SD, al pie de la pantalla de arriba
static float g_body_y = CARD_Y + MINI_H + 8; // lo de en medio, entre la tira y la tarjeta de la SD (sin la tira, desde arriba)
#define BODY_Y   g_body_y
#define BODY_B   (SCR_H - 8 - SDB_H - 6)
static float g_info_ymax = 1e9f; // info_row: las filas que no caben (las menos importantes van al final) no se dibujan

// hoja del botón central (ver qa_draw)
static struct {
	bool open, just;
	float anim;
	int sel;
	float pos;        // posición del carrusel (se acerca a sel; arrastrando, sigue al dedo)
	bool drag;
	float drag_pos;
} QA = {.sel = 1, .pos = 1};

enum { QA_CLOUD, QA_CONNECT, QA_SCAN, QA_LINK, QA_WEB, QA_N };
/* las aplicaciones del inicio (ver hm_*) */
enum { AP_FILES, AP_TITLES, AP_CONNECT, AP_DL, AP_CLOUD, AP_WEB, AP_MUSIC, AP_READER, AP_SHARE, AP_SETTINGS, AP_N };


// actualizar desde un archivo: los CIA de Cloud3DS que hay en la SD, para elegir uno
static struct {
	bool open, just;
	float anim;
	int sel, n;
	UpdLocal f[6];
} UL;
static void settings_top(float a);
static void settings_sub_top(float a);

#define ABOUT_TEXT "Tu nube local para Nintendo 3DS. Gestiona la tarjeta SD desde el móvil o el PC con solo escanear un código QR: sin apps, sin cables y sin configuración. Tus archivos no salen de tu red local; solo se consulta GitHub para buscar actualizaciones."

enum { TAB_FILES, TAB_ACTIVITY, TAB_DEVICES, TAB_SETTINGS, TAB_COUNT };
enum { PAGE_MAIN, PAGE_LANG, PAGE_ABOUT, PAGE_PIN, PAGE_UPDATE, PAGE_CONN };
enum { PIN_CUSTOM, PIN_RANDOM, PIN_COUNT };
enum { SET_READONLY, SET_PROTECT, SET_PIN, SET_CONN, SET_SHOTS, SET_LANG, SET_THEME, SET_UPDATE, SET_ABOUT, SET_EXIT, SET_COUNT };
enum { AB_APP, AB_GITHUB, AB_DISCORD, AB_KOFI, AB_COUNT };
enum { DLG_REVOKE = 1, DLG_NEWPIN, DLG_EXIT, DLG_CLEAR, DLG_UPDATE, DLG_FDELETE, DLG_FEXISTS, DLG_TXTDISCARD, DLG_TXTOVER, DLG_FDELMULTI, DLG_TRASHEMPTY, DLG_TRASHDEL, DLG_UNINSTALL, DLG_UNINSTALL2, DLG_DLINSECURE, DLG_UPDFILE, DLG_UPDRESTART, DLG_CLDEL, DLG_CLDELN, DLG_CLOUT, DLG_MLDEL, DLG_SHARE, DLG_UNUPD, DLG_UNDLC, DLG_EXTDEL };

typedef struct {
	float off, target;
	int sel;
} List;

// la nube (OneDrive): pantalla propia que se abre desde el carrusel
static struct {
	bool open;
	List l;
	CloudItem *it;
	int n;
	u32 rev;
	bool loading;
	CloudInfo info;
	bool om, om_just; // opciones de un elemento
	float om_anim;
	int om_sel, om_item;
	bool hm, hm_just; // opciones de la cuenta (cabecera)
	float hm_anim;
	int hm_sel;
	bool bk;          // página «Capturas en la nube»
	int bk_sel;
	int want_ren;     // cambiar el nombre del elemento want_ren - 1 (el teclado, fuera del dibujo)
	bool want_mkdir;  // carpeta nueva
	bool ms;          // seleccionando varios (mark: cuáles, en la carpeta ms_folder)
	bool mark[400];
	char ms_folder[64];
	bool mo, mo_just; // opciones de lo seleccionado
	float mo_anim;
	int mo_sel;
} CLU;

// portapapeles de la nube: lo copiado o cortado en OneDrive. Se pega en otra carpeta de la nube (copiar o
// mover) o, copiado, en una carpeta de Archivos (se descarga ahí). Con el de Archivos (FP) es uno solo: lo
// último que se copia, de un sitio o de otro, es lo que se pega
enum { CC_NONE, CC_COPY, CC_CUT };
static struct {
	int mode;
	int n;
	CloudItem it[64];
} CC;
static bool vw_cloud_open(void);           // el visor tiene abierto un archivo de la nube
static void cl_open_item(int i);           // abrir como en Archivos (carpeta, foto, texto, música, CIA, ZIP…)
static int fp_count(void);                 // cuántos hay copiados en Archivos (0: nada)
static void fp_clear(void);
static int fp_upload(const char *parent);  // sube lo copiado en Archivos a esa carpeta de la nube
static void cl_top(void);
static void cl_bottom(void);

static struct {
	int tab, page;
	float tab_anim;
	List list[TAB_COUNT + 5];   // una por pestaña y por subpágina de Ajustes
	bool buttons;                // la última interacción fue con botones (se ve la selección)
	float t_ro, t_prot;          // animación de los interruptores
	int dlg_arg;
	bool quit;
	float splash;                // segundos de presentación (-1 = terminada)
	float fade;                  // fundido de la presentación a la app (1 -> 0)
	bool lid_off;
	bool want_pin;               // abrir el teclado del PIN fuera del dibujo
	int want_rename;             // dispositivo a renombrar con el teclado (-1 = ninguno)
	char dev_tok[33];            // dispositivo elegido en la lista: su información sale arriba
	bool want_frename;           // renombrar el archivo elegido en Archivos
	bool want_fnew;              // crear una carpeta en Archivos
	bool want_tnew;              // crear un documento de texto en Archivos
	bool want_search;            // buscar desde la carpeta abierta (teclado)
	int app;                     // la aplicación abierta (AP_*, del inicio): de ella depende la pantalla de arriba
	char app_root[512];          // en las que son una vista de Archivos (Capturas, Títulos…): su carpeta; ahí, B vuelve al inicio
	bool want_dlurl;             // descargar un enlace escrito en la consola (teclado)
	bool want_web;               // abrir una página en el navegador (teclado)
	bool want_netname;           // cambiar el nombre en la red (teclado)
	bool nav_eat;                // se cambió de carpeta en este frame: lo pulsado ya se usó
	float t_mdns, t_fbi, t_shots; // animación de sus interruptores
	int want_txt;                // editor de texto: 1 editar la línea, 2 añadir una debajo
	float qr_anim;               // 1 = tarjeta del QR abierta, 0 = minimizada
	float t_upd;                 // interruptor «Buscar al abrir»
	UpdInfo upd;                 // estado de las actualizaciones (copia por frame)
	u32 upd_seen;                // para avisar una sola vez de una versión nueva
	InsReq ins[MAX_INS];         // cola de CIA para instalar (copia)
	int ins_n, ins_pend;
	int ins_icon_id;             // de qué petición es el icono cargado
	Texture ins_icon;
	float ins_anim;
} U;

static AppState V;               // copia del estado para dibujar sin el cerrojo
static u32 s_rev = ~0u;
static u64 s_copy_t;
static int s_hist_n, s_hist_head;
static bool s_events_seen;

static const char *const LINKS[3] = {LINK_GITHUB, LINK_DISCORD, LINK_KOFI};

// ------------------------------------------------------------ datos
static bool dev_online(const Device *d)
{
	return d->last_ms && util_ms() - d->last_ms < 30000;
}

static int online_count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_DEVICES; i++)
		n += V.dev[i].used && dev_online(&V.dev[i]);
	return n;
}

static void url_text(char *out, size_t n, bool scheme)
{
	struct in_addr a = {V.ip};
	if (V.port == 80)
		snprintf(out, n, "%s%s", scheme ? "http://" : "", inet_ntoa(a));
	else
		snprintf(out, n, "%s%s:%d", scheme ? "http://" : "", inet_ntoa(a), V.port);
}

// transferencias a la vista: las activas y las que acaban de terminar
static int visible_xfers(const Xfer **out, int max)
{
	int n = 0;
	u64 now = util_ms();
	for (int i = 0; i < MAX_XFERS && n < max; i++)
		if (V.xfers[i].st == XS_RUNNING)
			out[n++] = &V.xfers[i];
	for (int i = 0; i < MAX_XFERS && n < max; i++) {
		const Xfer *x = &V.xfers[i];
		if (x->st != XS_FREE && x->st != XS_RUNNING && now - x->t_end < 2500)
			out[n++] = x;
	}
	return n;
}

static const Event *ev(int i) { return &V.hist[(V.hist_head - i + MAX_HISTORY) % MAX_HISTORY]; }

static void ev_info(const Event *e, char *title, size_t tn, char *sub, size_t sn, IconId *ic, u32 *col)
{
	char sz[24];
	util_fmt_size(sz, sizeof sz, e->size);
	util_strlcpy(title, e->name, tn);
	switch (e->kind) {
	// desde la consola: lo que sube la web es algo que la consola recibe (flecha hacia dentro)
	case EV_UPLOAD:
		*ic = IC_DOWNLOAD; *col = T.ok;
		snprintf(sub, sn, "%s · %s · %s", _("Recibido"), sz, e->device);
		break;
	case EV_DOWNLOAD:
		*ic = IC_UPLOAD; *col = T.accent;
		snprintf(sub, sn, "%s · %s · %s", _("Enviado"), sz, e->device);
		break;
	case EV_UPLOAD_FAIL:
		*ic = IC_CLOSE; *col = T.danger;
		snprintf(sub, sn, "%s · %s", _("Transferencia incompleta"), e->device);
		break;
	case EV_MKDIR:
		*ic = IC_FOLDER; *col = T.warn;
		snprintf(sub, sn, "%s · %s", _("Carpeta creada"), e->device);
		break;
	case EV_RENAME:
		*ic = IC_PEN; *col = T.accent2;
		snprintf(sub, sn, "%s · %s", _("Renombrado"), e->device);
		break;
	case EV_DELETE:
		*ic = IC_TRASH; *col = T.danger;
		snprintf(sub, sn, "%s · %s", _("Enviado a la papelera"), e->device);
		break;
	case EV_PAIRED:
		*ic = IC_LINK; *col = T.accent2;
		util_strlcpy(title, _("Nuevo dispositivo vinculado"), tn);
		util_strlcpy(sub, e->device, sn);
		break;
	case EV_REVOKED:
		*ic = IC_LOGOUT; *col = T.text3;
		util_strlcpy(title, _("Dispositivo desvinculado"), tn);
		util_strlcpy(sub, e->device, sn);
		break;
	case EV_COPY:
		*ic = IC_FOLDER; *col = T.accent;
		snprintf(sub, sn, "%s · %s · %s", _("Copiado"), sz, e->device);
		break;
	case EV_MOVE:
		*ic = IC_FOLDER; *col = T.accent2;
		snprintf(sub, sn, "%s · %s", _("Movido"), e->device);
		break;
	case EV_RESTORE:
		*ic = IC_REFRESH; *col = T.ok;
		snprintf(sub, sn, "%s · %s", _("Recuperado de la papelera"), e->device);
		break;
	case EV_FETCH:
		*ic = IC_GLOBE; *col = T.accent;
		snprintf(sub, sn, "%s · %s · %s", _("Descargado"), sz, e->device);
		break;
	case EV_ZIP:
		*ic = IC_ARCHIVE; *col = T.warn;
		snprintf(sub, sn, "%s · %s · %s", _("ZIP creado"), sz, e->device);
		break;
	case EV_UNZIP:
		*ic = IC_ARCHIVE; *col = T.warn;
		snprintf(sub, sn, "%s · %s · %s", _("Extraído"), sz, e->device);
		break;
	case EV_CREATE:
		*ic = IC_NOTE; *col = T.accent;
		snprintf(sub, sn, "%s · %s", _("Creado"), e->device);
		break;
	case EV_EDIT:
		*ic = IC_NOTE; *col = T.accent2;
		snprintf(sub, sn, "%s · %s", _("Editado"), e->device);
		break;
	case EV_INSTALL:
		*ic = IC_CHECK; *col = T.ok;
		snprintf(sub, sn, "%s · %s · %s", _("Instalado"), sz, e->device);
		break;
	default: {
		char dur[24];
		*ic = IC_LOCK; *col = T.warn;
		util_fmt_eta(dur, sizeof dur, e->size);
		util_strlcpy(title, _("Intentos de PIN fallidos"), tn);
		if (e->size)
			snprintf(sub, sn, _("Bloqueado %s · %s"), dur, e->device);
		else
			util_strlcpy(sub, _("Demasiados intentos fallidos"), sn);
		break;
	}
	}
}

static IconId dev_icon(const Device *d)
{
	return d->kind == 0 ? IC_PHONE : d->kind == 2 ? IC_TABLET : IC_LAPTOP;
}

// lista de dispositivos: los conectados primero
static int device_order(int *idx)
{
	int n = 0;
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < MAX_DEVICES; i++)
			if (V.dev[i].used && dev_online(&V.dev[i]) == (pass == 0))
				idx[n++] = i;
	return n;
}

// ------------------------------------------------------------ piezas comunes
// círculo de color con un icono blanco: el mismo tamaño en todas las listas
static void row_icon(float cx, float cy, IconId ic, u32 col)
{
	draw_circle(cx, cy, 13, col);
	draw_icon(ic, cx, cy, 15, 0xFFFFFFFF);
}

static void toggle(float x, float y, float anim)
{
	float w = 36, h = 20;
	u32 off = col_lerp(T.surface2, T.text3, 0.45f);
	draw_rrect(x, y, w, h, h / 2, col_lerp(off, T.accent, anim));
	draw_circle(x + 10 + (w - 20) * anim, y + h / 2, 7.5f, 0xFFFFFFFF);
}

// el espacio libre de la SD, para dibujar: preguntar a la tarjeta en cada fotograma frena el dibujo (y mientras la
// consola instala o borra, la tarjeta tarda en contestar): se pregunta como mucho cada 2 s, y nunca con la pantalla
// de espera puesta (entonces vale lo último que se supo)
static bool guard_on(void);
static bool sd_space(u64 *fr, u64 *tot)
{
	static u64 f, t, at;
	static bool ok;
	u64 now = util_ms();
	if ((!at || now - at > 2000) && !guard_on()) {
		ok = fsx_space(&f, &t);
		at = now;
	}
	*fr = f, *tot = t;
	return ok;
}

// la tarjeta de la SD, al pie de la pantalla de arriba: lo libre y una barra con lo usado.
// Si algo se está guardando en la SD, una marca más clara muestra lo que ocupará al terminar
static int visible_xfers(const Xfer **out, int max);
static void sd_bar(float a)
{
	static u64 fr, tot;
	static float t = -100;
	if (g_time - t > 3) {
		t = g_time;
		if (!sd_space(&fr, &tot))
			fr = tot = 0;
	}
	float x = CARD_X, y = SCR_H - 8 - SDB_H, w = CARD_W;
	bool low = tot && fr < 512ull * 1024 * 1024, warn = tot && fr < tot / 10;
	u32 c = low ? T.danger : warn ? T.warn : T.accent;
	draw_rrect(x, y + 3, w, SDB_H, 14, col_alpha(T.shadow, a));
	draw_rrect(x, y, w, SDB_H, 14, col_alpha(T.surface, a));
	if (low)
		draw_rrect_outline(x, y, w, SDB_H, 14, 1, col_alpha(T.danger, 0.45f * a));
	// una tarjeta SD: esquina cortada y sus contactos
	float ix = x + 14, iy = y + (SDB_H - 18) / 2;
	draw_rrect(ix, iy, 14, 18, 2.5f, col_alpha(c, a));
	draw_tri(ix + 8, iy - 0.5f, ix + 14.5f, iy - 0.5f, ix + 14.5f, iy + 6, T.surface);
	for (int k = 0; k < 3; k++)
		draw_rect(ix + 3 + k * 3, iy + 3, 1.6f, 4, T.surface);
	float lx = ix + 26, rx = x + w - 14, bw = rx - lx;
	float ty = y + 9;
	if (!tot) {
		text_draw(_("Tarjeta SD"), lx, ty, TS_SUB, col_alpha(T.text, a));
		text_draw_right(_("No se puede leer el espacio disponible"), rx, ty, TS_SUB, col_alpha(T.text3, a));
		return;
	}
	char fs[24], ts[24], r1[48], r2[48];
	util_fmt_size(fs, sizeof fs, fr);
	util_fmt_size(ts, sizeof ts, tot);
	snprintf(r1, sizeof r1, _("%s libres"), fs);
	snprintf(r2, sizeof r2, _("de %s"), ts);
	float w2 = text_width(r2, TS_SUB);
	text_draw_right(r2, rx, ty, TS_SUB, col_alpha(T.text3, a));
	text_draw_right(r1, rx - w2 - 5, ty, TS_SUB, col_alpha(low ? T.danger : T.text, a));
	text_draw(low ? _("Espacio disponible bajo") : _("Tarjeta SD"), lx, ty, TS_SUB, col_alpha(low ? T.danger : T.text, a));
	// la barra: lo usado
	float by = y + SDB_H - 15, bh = 6;
	float used = (float)((double)(tot - fr) / tot);
	// lo que aún falta por guardar de lo que está en curso
	float extra = 0;
	const Xfer *xs[4];
	int nx = visible_xfers(xs, 4);
	for (int i = 0; i < nx; i++)
		if (xs[i]->st == XS_RUNNING && xs[i]->kind != XF_DOWNLOAD && xs[i]->kind != XF_INSTALL && xs[i]->total > xs[i]->done)
			extra += (float)((double)(xs[i]->total - xs[i]->done) / tot);
	draw_rrect(lx, by, bw, bh, bh / 2, col_alpha(T.surface2, a));
	if (extra > 0)
		draw_rrect(lx, by, fmaxf(bh, bw * clampf(used + extra, 0, 1)), bh, bh / 2, col_alpha(c, 0.35f * a));
	float fw = fmaxf(bh, bw * clampf(used, 0, 1));
	if (low || warn)
		draw_rrect(lx, by, fw, bh, bh / 2, col_alpha(c, a));
	else {
		gfx_clip(lx, by, fw, bh);
		draw_circle(lx + bh / 2, by + bh / 2, bh / 2, col_alpha(T.accent, a));
		draw_grad_h(lx + bh / 2, by, bw - bh, bh, col_alpha(T.accent, a), col_alpha(T.accent2, a));
		draw_circle(lx + bw - bh / 2, by + bh / 2, bh / 2, col_alpha(T.accent2, a));
		gfx_clip_off();
		draw_circle(lx + fw - bh / 2, by + bh / 2, bh / 2, col_alpha(col_lerp(T.accent, T.accent2, clampf((fw - bh / 2) / bw, 0, 1)), a));
	}
}

static void brand_bar(float x, float y, float w, float h, float frac)
{
	draw_rrect(x, y, w, h, h / 2, T.surface2);
	frac = clampf(frac, 0, 1);
	if (frac <= 0)
		return;
	float fw = fmaxf(w * frac, h);
	// el degradado es fijo sobre toda la barra; el avance lo va descubriendo
	gfx_clip(x, y, fw, h);
	draw_circle(x + h / 2, y + h / 2, h / 2, T.accent);
	draw_grad_h(x + h / 2, y, w - h, h, T.accent, T.accent2);
	draw_circle(x + w - h / 2, y + h / 2, h / 2, T.accent2);
	gfx_clip_off();
	draw_circle(x + fw - h / 2, y + h / 2, h * 0.28f, 0xFFFFFFFF);
}

// texto centrado línea a línea dentro de un ancho
static void wrap_center(const char *text, float cx, float y, float maxw, float sc, int maxl, u32 c)
{
	const char *p = text;
	for (int l = 0; l < maxl && *p; l++) {
		char line[240];
		int n = 0, last_n = 0;
		float w = 0;
		const char *last_sp = NULL;
		while (*p && *p != '\n') {
			const char *q = p;
			u32 cp = utf8_next(&q);
			float gw = glyph_w(cp, sc);
			if (w + gw > maxw && n)
				break;
			if (cp == ' ') { last_sp = q; last_n = n; }
			memcpy(line + n, p, q - p);
			n += (int)(q - p);
			w += gw;
			p = q;
			if (n > 220)
				break;
		}
		if (*p && *p != '\n' && last_sp) { n = last_n; p = last_sp; }
		line[n] = 0;
		if (*p == '\n') p++;
		while (*p == ' ') p++;
		text_draw_center(line, cx, y + l * text_lineh(sc), sc, c);
	}
}

// ------------------------------------------------------------ actualizaciones
static void card(void);

static const char *upd_err_text(const char *e)
{
	if (!strcmp(e, "offline"))
		return _("Sin conexión a Internet. Comprueba el Wi-Fi e inténtalo de nuevo.");
	if (!strcmp(e, "tls"))
		return _("No se pudo verificar la conexión segura. Comprueba la fecha y la hora de la consola.");
	if (!strcmp(e, "rate"))
		return _("GitHub ha limitado temporalmente las consultas. Inténtalo de nuevo en unos minutos.");
	if (!strcmp(e, "no_release"))
		return _("Todavía no hay versiones publicadas.");
	if (!strcmp(e, "sha"))
		return _("El archivo descargado no coincide con su SHA-256 y no se ha instalado.");
	if (!strcmp(e, "am"))
		return _("La consola no pudo instalar la actualización. Inténtalo de nuevo o instálala con FBI.");
	if (!strcmp(e, "io"))
		return _("No se pudo escribir en la tarjeta SD.");
	if (!strcmp(e, "not_self"))
		return _("Este archivo CIA no corresponde a Cloud3DS.");
	if (!strcmp(e, "older"))
		return _("El archivo corresponde a una versión anterior a la instalada. La consola no permite instalar versiones anteriores.");
	if (!strcmp(e, "bad_cia") || !strcmp(e, "bad_file"))
		return _("El archivo está dañado o no es válido.");
	if (!strcmp(e, "not_found"))
		return _("El archivo ya no se encuentra en la tarjeta SD.");
	return _("No se pudo completar. Inténtalo de nuevo más tarde.");
}

static const char *upd_title(void)
{
	switch (U.upd.st) {
	case UPD_CHECKING: return _("Buscando actualizaciones…");
	case UPD_LATEST: return _("Cloud3DS está al día");
	case UPD_AVAILABLE: return _("Hay una versión nueva");
	case UPD_DOWNLOADING: return _("Descargando la actualización");
	case UPD_INSTALLING: return _("Instalando la actualización");
	case UPD_DONE: return _("Actualización instalada");
	case UPD_ERROR: return _("No se pudo actualizar");
	default: return _("Actualizaciones");
	}
}

// pantalla de arriba en Ajustes → Actualizaciones: versiones, novedades y progreso
static void update_top(void)
{
	card();
	const float X = CARD_X + 18, W = CARD_W - 36;
	bool busy = U.upd.st == UPD_DOWNLOADING || U.upd.st == UPD_INSTALLING || U.upd.st == UPD_CHECKING;
	u32 col = U.upd.st == UPD_ERROR ? T.danger : U.upd.st == UPD_AVAILABLE || U.upd.st == UPD_DONE ? T.ok : T.accent;
	draw_circle(X + 16, CARD_Y + 30, 16, col);
	if (busy)
		draw_arc(X + 16, CARD_Y + 30, 9, 2.2f, g_time * 6, g_time * 6 + 4.2f, 0xFFFFFFFF);
	else
		draw_icon(U.upd.st == UPD_ERROR ? IC_CLOSE : U.upd.st == UPD_LATEST || U.upd.st == UPD_DONE ? IC_CHECK : IC_DOWNLOAD,
		          X + 16, CARD_Y + 30, 18, 0xFFFFFFFF);
	text_draw_clip(upd_title(), X + 42, CARD_Y + 14, W - 42, TS_TITLE, T.text);
	char v[96];
	if (U.upd.latest[0])
		snprintf(v, sizeof v, _("Instalada: v%s · Última: v%s"), APP_VERSION, U.upd.latest);
	else
		snprintf(v, sizeof v, _("Instalada: v%s"), APP_VERSION);
	text_draw_clip(v, X + 42, CARD_Y + 34, W - 42, TS_SUB, T.text3);
	float y = CARD_Y + 60;
	draw_rect(X, y, W, 1, T.divider);
	y += 10;
	if (U.upd.st == UPD_DOWNLOADING || U.upd.st == UPD_INSTALLING) {
		float frac = U.upd.total ? (float)((double)U.upd.done / U.upd.total) : 0;
		brand_bar(X, y + 4, W, 8, frac);
		char a[24], b[24], line[80];
		util_fmt_size(a, sizeof a, U.upd.done);
		util_fmt_size(b, sizeof b, U.upd.total);
		snprintf(line, sizeof line, _("%s de %s"), a, b);
		text_draw_clip(line, X, y + 18, W, TS_SUB, T.text2);
		text_wrap_lines(_("No apagues la consola ni cierres Cloud3DS hasta que termine."), W, TS_SUB, 2, X, y + 40, T.text3, true);
		return;
	}
	if (U.upd.st == UPD_ERROR) {
		text_wrap_lines(upd_err_text(U.upd.err), W, TS_BODY, 4, X, y, T.text2, true);
		return;
	}
	if (U.upd.st == UPD_DONE) {
		text_wrap_lines(envIsHomebrew() ? _("Cierra Cloud3DS y vuelve a abrirlo para usar la nueva versión.")
		                                : _("Reinicia Cloud3DS para usar la nueva versión."),
		                W, TS_BODY, 3, X, y, T.text2, true);
		return;
	}
	if (U.upd.st == UPD_AVAILABLE && !U.upd.has_file) {
		text_wrap_lines(_("Esta versión no incluye el archivo de verificación (Cloud3DS.sha256), por lo que no se puede instalar desde aquí."),
		                W, TS_BODY, 4, X, y, T.text2, true);
		return;
	}
	if (U.upd.st == UPD_AVAILABLE && U.upd.notes[0]) {
		// novedades de la release, sin los símbolos de Markdown
		char notes[1500];
		int o = 0;
		for (const char *p = U.upd.notes; *p && o < (int)sizeof notes - 1; p++) {
			if (*p == '#' || *p == '*' || *p == '`' || *p == '\r' || *p == '_')
				continue;
			notes[o++] = *p;
		}
		notes[o] = 0;
		text_draw_clip(_("Novedades"), X, y, W, TS_SUB, T.text3);
		text_wrap_lines(notes, W, 0.42f, 8, X, y + 16, T.text2, true);
		return;
	}
	text_wrap_lines(_("Cloud3DS busca versiones nuevas en GitHub y, antes de instalarlas, verifica la integridad del archivo descargado (SHA-256)."),
	                W, TS_BODY, 4, X, y, T.text3, true);
}

// ------------------------------------------------------------ instalar CIA (pedido desde la web)
static void text_draw_center_clip(const char *t, float cx, float y, float maxw, float sc, u32 c)
{
	float w = fminf(text_width(t, sc), maxw);
	text_draw_clip(t, cx - w / 2, y, maxw, sc, c);
}

// se puede instalar: ni Cloud3DS (va por Actualizaciones) ni bajar de versión (AM no deja)
static bool ins_can(const InsReq *r)
{
	return !(r->inst_ver >= 0 && r->inst_ver > r->ver);
}

static const InsReq *ins_first_pending(void)
{
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING)
			return &U.ins[i];
	return NULL;
}

// versión del CIA frente a la instalada, con el color que le toca
static void ins_version_line(const InsReq *r, char *out, size_t n, u32 *col)
{
	char a[16], b[16];
	ins_ver_text(r->ver, a, sizeof a);
	*col = T.text3;
	if (r->self) {
		snprintf(out, n, _("Es Cloud3DS %s. Se instalará como actualización y la aplicación se reiniciará."), a);
		*col = T.ok;
	} else if (r->inst_ver < 0) {
		snprintf(out, n, _("Versión %s · No instalado"), a);
	} else if (r->inst_ver == r->ver) {
		snprintf(out, n, _("Versión %s · Ya instalada (se reinstalará)"), a);
	} else if (r->inst_ver > r->ver) {
		snprintf(out, n, _("Versión %s · Hay una versión más reciente instalada (%s). La consola no permite instalar versiones anteriores."), a,
		         ins_ver_text((u16)r->inst_ver, b, sizeof b));
		*col = T.warn;
	} else {
		snprintf(out, n, _("Versión %s · Actualiza la versión %s instalada"), a, ins_ver_text((u16)r->inst_ver, b, sizeof b));
		*col = T.ok;
	}
}

static void ins_refresh(void)
{
	U.ins_n = ins_snapshot(U.ins, MAX_INS);
	U.ins_pend = 0;
	static int failed_seen; // la última que falló y ya se avisó
	for (int i = 0; i < U.ins_n; i++) {
		U.ins_pend += U.ins[i].st == INS_PENDING;
		if (U.ins[i].st == INS_FAILED && U.ins[i].id > failed_seen) {
			failed_seen = U.ins[i].id;
			char msg[120];
			snprintf(msg, sizeof msg, _("No se instaló: %s"), U.ins[i].title);
			toast_icon(msg, IC_CLOSE, T.danger);
		}
	}
	const InsReq *r = ins_first_pending();
	if (r && r->id != U.ins_icon_id) {
		U.ins_icon_id = r->id;
		if (r->has_icon)
			tex_from_rgb565(&U.ins_icon, r->icon, 48, 48);
		else
			tex_free(&U.ins_icon);
	}
}

// con el QR cerrado y CIA esperando: los datos del primero, debajo de la tira
static void install_top(float a)
{
	const InsReq *r = ins_first_pending();
	if (!r)
		return;
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28, IS = h - 20;
	float px = X - 4, py = y0 + 10;
	draw_rrect(px, py, IS, IS, 12, col_alpha(T.surface2, a));
	if (U.ins_icon.ok)
		draw_texture(&U.ins_icon, px + (IS - 72) / 2, py + (IS - 72) / 2, 72, 72, a);
	else
		draw_icon(IC_GEM, px + IS / 2, py + IS / 2, 38, col_alpha(T.text3, a));
	float tx = px + IS + 14, tw = X + W - tx, y = py + 1;
	text_draw_clip(_("Instalar en la consola"), tx, y, tw, TS_SUB, col_alpha(T.accent, a));
	if (U.ins_pend > 1) {
		char more[48];
		snprintf(more, sizeof more, _("y %d más en espera"), U.ins_pend - 1);
		text_draw_right(more, X + W, y, TS_SUB, col_alpha(T.text3, a));
	}
	y += 15;
	text_draw_clip(r->title, tx, y, tw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 2;
	char line[160], sz[24];
	util_fmt_size(sz, sizeof sz, r->size);
	snprintf(line, sizeof line, "%s%s%s", r->publisher, r->publisher[0] ? " · " : "", sz);
	text_draw_clip(line, tx, y, tw, TS_SUB, col_alpha(T.text2, a));
	y += 18;
	u32 col;
	ins_version_line(r, line, sizeof line, &col);
	y += text_wrap_lines(line, tw, TS_SUB, 2, tx, y, col_alpha(col, a), true) * text_lineh(TS_SUB) + 2;
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	if (r->n3ds_only && !n3ds) {
		text_draw_clip(_("Este juego es solo para New 3DS."), tx, y, tw, TS_SUB, col_alpha(T.warn, a));
		y += 15;
	}
	snprintf(line, sizeof line, _("Enviado desde %s"), r->device);
	text_draw_clip(line, tx, y0 + h - 10 - text_lineh(TS_SUB), tw, TS_SUB, col_alpha(T.text3, a));
}

// tarjeta de abajo: Instalar / Solo guardar / Rechazar (con varios, para todos)
#define INS_BX 16.0f
#define INS_BW (BOT_W - 32.0f)
static void ins_button(float y, const char *glyph, const char *label, u32 fg, u32 bg, bool on)
{
	bool p = on && touch_in(INS_BX, y, INS_BW, 36);
	draw_rrect(INS_BX, y, INS_BW, 36, 18, p ? col_lerp(bg, T.text, 0.15f) : on ? bg : T.surface2);
	char t[96];
	snprintf(t, sizeof t, "%s %s", glyph, label);
	text_draw_center(t, BOT_W / 2, y + (36 - text_lineh(0.48f)) / 2, 0.48f, on ? fg : T.text3);
}

// botones de la tarjeta: si lo que espera se acaba de subir, «Solo copiar» y «Cancelar la copia»
// (que lo borra); si ya estaba en la SD, solo «Instalar» o «Cerrar» (se queda en la SD)
typedef struct {
	const char *glyph, *label;
	u32 fg, bg;
	bool on;
	int what;
	u32 key;
} InsBtn;

static int ins_buttons(InsBtn *b)
{
	bool can = false, up = false;
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING) {
			can |= ins_can(&U.ins[i]);
			up |= U.ins[i].uploaded;
		}
	int n = 0;
	const InsReq *f = ins_first_pending();
	b[n++] = (InsBtn){GLYPH_A, U.ins_pend > 1 ? _("Instalar todos") : f && f->self ? _("Actualizar Cloud3DS") : _("Instalar"), 0xFFFFFFFF, T.ok, can, INS_ACCEPT, KEY_A};
	if (up) {
		b[n++] = (InsBtn){GLYPH_X, _("Solo guardar en la SD"), T.text, T.surface2, true, INS_KEEP, KEY_X};
		b[n++] = (InsBtn){GLYPH_B, _("Cancelar la copia"), T.danger, col_alpha(T.danger, 0.16f), true, INS_REJECT, KEY_B};
	} else {
		b[n++] = (InsBtn){GLYPH_B, _("Cerrar"), T.text, T.surface2, true, INS_REJECT, KEY_B};
	}
	return n;
}

// la tarjeta mide lo que sus botones (sin hueco vacío debajo) y va centrada
#define INS_DEL_H 28 // (la casilla «borrar el .cia después», con lo recibido)
static bool ins_up(void)
{
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING && U.ins[i].uploaded && !U.ins[i].self)
			return true;
	return false;
}
static float ins_card_h(int n)
{
	return 80 + n * 44 + (ins_up() ? INS_DEL_H : 0);
}

static float ins_card_y(int n)
{
	return (SCR_H - ins_card_h(n)) / 2;
}

static void install_card(void)
{
	U.ins_anim = approach(U.ins_anim, U.ins_pend ? 1 : 0, 14);
	if (U.ins_anim < 0.01f)
		return;
	const InsReq *r = ins_first_pending();
	float a = ease_out_cubic(U.ins_anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	InsBtn b[3];
	int n = ins_buttons(b);
	float y = ins_card_y(n) + (1 - a) * 30, ch = ins_card_h(n);
	draw_rrect(10, y + 3, BOT_W - 20, ch, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, ch, 16, T.surface);
	if (!r)
		return;
	char title[96];
	if (U.ins_pend > 1)
		snprintf(title, sizeof title, _("%d CIA para instalar"), U.ins_pend);
	else
		util_strlcpy(title, _("¿Instalar este CIA?"), sizeof title);
	text_draw_center(title, BOT_W / 2, y + 12, TS_TITLE, T.text);
	// nombres de los que esperan
	int shown = 0;
	for (int i = 0; i < U.ins_n && shown < 2; i++)
		if (U.ins[i].st == INS_PENDING) {
			text_draw_center_clip(U.ins[i].title, BOT_W / 2, y + 36 + shown * 16, BOT_W - 60, TS_SUB, T.text3);
			shown++;
		}
	float by = y + 76;
	if (ins_up()) { // lo recibido: ¿se borra el .cia al instalarlo? (lo decide quien instala; se recuerda)
		bool on = V.ins_del;
		float cx = INS_BX + 10, cy = by + 4;
		draw_rrect(cx, cy, 16, 16, 4, on ? T.ok : T.surface2);
		if (on)
			draw_icon(IC_CHECK, cx + 8, cy + 8, 11, 0xFFFFFFFF);
		char t[96];
		snprintf(t, sizeof t, "%s %s", GLYPH_Y, _("Eliminar el archivo .cia después de instalarlo"));
		text_draw_clip(t, cx + 24, cy + (16 - text_lineh(TS_SUB)) / 2, INS_BW - 40, TS_SUB, on ? T.text : T.text2);
		by += INS_DEL_H;
	}
	for (int k = 0; k < n; k++)
		ins_button(by + k * 44, b[k].glyph, b[k].label, b[k].fg, b[k].bg, b[k].on);
}

// botones de la tarjeta; devuelve true si se ha decidido algo
static bool install_input(void)
{
	if (!U.ins_pend || g_dialog.open)
		return false;
	InsBtn b[3];
	int n = ins_buttons(b), what = -1;
	float by = ins_card_y(n) + 76;
	if (ins_up()) { // Y o tocarla: borrar (o no) el .cia al instalarlo
		if ((g_kdown & KEY_Y) || tap(INS_BX, by, INS_BW, INS_DEL_H)) {
			state_lock();
			G.ins_del = !G.ins_del, G.dirty_cfg = true;
			V.ins_del = G.ins_del;
			state_unlock();
			g_kdown &= ~KEY_Y;
			return true;
		}
		by += INS_DEL_H;
	}
	for (int k = 0; k < n && what < 0; k++)
		if (b[k].on && ((g_kdown & b[k].key) || tap(INS_BX, by + k * 44, INS_BW, 36)))
			what = b[k].what;
	if (what < 0)
		return true; // la tarjeta tapa lo de debajo: no pasa nada más
	bool del = what == INS_ACCEPT && ins_up() && V.ins_del;
	bool up = false;
	for (int i = 0; i < U.ins_n; i++)
		up |= U.ins[i].st == INS_PENDING && U.ins[i].uploaded;
	// Cloud3DS no se instala por la cola: va al actualizador (y luego se ofrece reiniciar)
	bool self_upd = false;
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING && U.ins[i].self && what == INS_ACCEPT && ins_can(&U.ins[i]) && !upd_busy()) {
			upd_install_file(U.ins[i].path);
			ins_decide(U.ins[i].id, INS_KEEP);
			self_upd = true;
		}
	ins_decide(-1, del ? INS_ACCEPT_DEL : what);
	ins_refresh();
	// el toque que decidió no sigue a lo de debajo (el botón del QR está justo bajo el último)
	g_kdown &= ~(KEY_A | KEY_B | KEY_X);
	g_touch.up = false;
	if (self_upd)
		toast_icon(_("Actualizando Cloud3DS…"), IC_DOWNLOAD, T.ok);
	else if (what == INS_ACCEPT)
		toast_icon(del ? _("Instalando. El archivo .cia se eliminará al terminar.") : _("Instalando…"), IC_DOWNLOAD, T.ok);
	else if (what == INS_KEEP)
		toast_icon(_("Guardado en la SD sin instalar"), IC_FOLDER, T.accent);
	else if (up)
		toast_icon(_("Copia cancelada. El archivo se eliminó de la SD."), IC_TRASH, T.text3);
	return true;
}

// ------------------------------------------------------------ pantalla superior
static void draw_battery(float x, float y, int pct, bool charging)
{
	float w = 19, h = 10;
	draw_rrect_outline(x, y, w, h, 2.5f, 1.2f, T.text3);
	draw_rrect(x + w + 0.5f, y + 3, 2, h - 6, 1, T.text3);
	if (pct >= 0) {
		u32 c = charging ? T.accent : (pct <= 15 ? T.danger : (pct <= 30 ? T.warn : T.ok));
		draw_rrect(x + 2, y + 2, fmaxf(1.5f, (w - 4) * pct / 100.0f), h - 4, 1.2f, c);
	}
	if (charging)
		draw_icon(IC_BOLT, x + w / 2, y + h / 2, 9, T.bg);
}

// señal Wi-Fi: tres arcos encendidos según la intensidad (0-3); -1 = sin conexión
static void draw_wifi(float cx, float by, int bars)
{
	for (int i = 0; i < 3; i++) {
		u32 c = bars > i ? T.text2 : T.divider;
		draw_arc(cx, by, 3.5f + i * 3.5f, 1.6f, (float)(-M_PI * 0.75), (float)(-M_PI * 0.25), c);
	}
	draw_circle(cx, by - 0.5f, 1.4f, bars > 0 ? T.text2 : T.divider);
	if (bars < 0)
		draw_line(cx - 6, by - 11, cx + 6, by + 1, 1.4f, T.danger);
}

static void draw_header(void)
{
	draw_cloud(18, 11, 8, 1);
	text_draw(APP_NAME, 32, 3, 0.5f, T.text);
	// derecha: fecha · hora   [Wi-Fi] [batería] 87 %
	float rx = TOP_W - 10;
	bool chg = false;
	int pct = util_battery(&chg);
	char bp[8];
	snprintf(bp, sizeof bp, "%d%%", pct);
	text_draw_right(bp, rx, 4, TS_HDR, T.text2);
	rx -= text_width(bp, TS_HDR) + 4;
	draw_battery(rx - 22, 6, pct, chg);
	rx -= 22 + 8;
	draw_wifi(rx - 7, 16, V.ip ? osGetWifiStrength() : -1);
	rx -= 14 + 10;
	char clk[8], day[32];
	util_fmt_now(clk, sizeof clk);
	text_draw_right(clk, rx, 4, TS_HDR, T.text2);
	rx -= text_width(clk, TS_HDR) + 5;
	draw_circle(rx - 1.5f, 11, 1.3f, T.text3);
	rx -= 6;
	util_fmt_today(day, sizeof day);
	text_draw_right(day, rx, 4, TS_HDR, T.text3);
	// con un aviso en la cabecera (según su estilo), lo de siempre se aparta
	float ha = toast_header_alpha();
	if (ha < 1)
		draw_rect(0, 0, TOP_W, HDR_H, col_alpha(T.bg, 1 - ha));
	// algo en curso (también con el QR abierto): una línea fina bajo la cabecera con su avance
	const Xfer *xs[1];
	if (visible_xfers(xs, 1) && xs[0]->st == XS_RUNNING) {
		float p = g_time * 1.2f, f = xs[0]->total ? clampf((float)((double)xs[0]->done / xs[0]->total), 0, 1) : 0;
		draw_rect(0, HDR_H, TOP_W, 2, T.surface2);
		draw_grad_h(0, HDR_H, fmaxf(4, TOP_W * f), 2, col_alpha(brand_grad(p), 0.95f), col_alpha(brand_grad(p + 0.5f), 0.95f));
	}
}

static void card(void)
{
	draw_rrect(CARD_X, CARD_Y + 3, CARD_W, CARD_H, 16, T.shadow);
	draw_rrect(CARD_X, CARD_Y, CARD_W, CARD_H, 16, T.surface);
}

// mensaje centrado en la tarjeta (sin Wi-Fi, error)
static void card_message(IconId ic, u32 icc, const char *title, const char *text)
{
	card();
	draw_circle(TOP_W / 2, CARD_Y + 60, 32, T.surface2);
	draw_icon(ic, TOP_W / 2, CARD_Y + 60, 36, icc);
	text_draw_center(title, TOP_W / 2, CARD_Y + 104, TS_TITLE, T.text);
	wrap_center(text, TOP_W / 2, CARD_Y + 134, 300, 0.42f, 3, T.text3);
}

static void draw_status(float x, float y, float w, float a)
{
	int on = online_count();
	if (on) {
		char s[48];
		snprintf(s, sizeof s, on == 1 ? _("%d dispositivo conectado") : _("%d dispositivos conectados"), on);
		draw_circle(x + 4, y + 7, 4, col_alpha(T.ok, a));
		text_draw_clip(s, x + 14, y, w - 14, TS_SUB, col_alpha(T.ok, a));
	} else {
		int dots = (int)(g_time * 2) % 4;
		char t[48];
		snprintf(t, sizeof t, _("Esperando conexión%.*s"), dots, "...");
		text_draw_clip(t, x, y, w, TS_SUB, col_alpha(T.text3, a));
	}
}

static void connect_col(float a, float dy)
{
	float x = COL_X, y = QR_Y + dy;
	text_draw_clip(_("Escanea con tu móvil"), x, y + 2, COL_W, 0.55f, col_alpha(T.text, a));
	int nl = text_wrap_lines(_("Escanéalo con la cámara. El móvil debe estar conectado a la misma red Wi-Fi."), COL_W, TS_SUB, 3, x,
	                         y + 24, col_alpha(T.text3, a), true);
	// en japonés las líneas son más altas: la raya y lo que sigue bajan lo que haga falta (hasta el PIN)
	float dy2 = fmaxf(0, y + 24 + nl * text_lineh(TS_SUB) + 4 - (y + 66));
	draw_rect(x, y + 66 + dy2, COL_W, 1, col_alpha(T.divider, a));
	text_draw_clip(_("O escribe en el navegador:"), x, y + 74 + dy2, COL_W, TS_SUB, col_alpha(T.text3, a));
	char url[48], nm[64];
	url_text(url, sizeof url, false);
	// con el nombre en la red activo: «cloud3ds.local» y, debajo y en pequeño, la IP por si el móvil no lo entiende
	bool named = V.mdns && (V.remote_ok & 1);
	if (named) {
		if (V.port == 80)
			snprintf(nm, sizeof nm, "%s.local", V.net_name);
		else
			snprintf(nm, sizeof nm, "%s.local:%d", V.net_name, V.port);
	}
	const char *main_t = named ? nm : url;
	float s = 0.64f;
	while (s > 0.44f && text_width(main_t, s) > COL_W)
		s -= 0.02f;
	float uy = y + 74 + dy2 + fmaxf(16, text_lineh(TS_SUB) + 2);
	text_draw(main_t, x, uy, s, col_alpha(T.accent, a));
	if (named) {
		char alt[64];
		snprintf(alt, sizeof alt, _("o %s"), url);
		text_draw_clip(alt, x, uy + text_lineh(s) - 1, COL_W, TS_SUB, col_alpha(T.text3, a));
	}
	text_draw(_("PIN"), x, y + 132, TS_SUB, col_alpha(T.text3, a));
	for (int i = 0; i < 4; i++) {
		float bx = x + 30 + i * 31, by = y + 122;
		draw_rrect(bx, by, 27, 32, 8, col_alpha(T.surface2, a));
		char d[2] = {V.pin[i], 0};
		text_draw_center(d, bx + 13.5f, by + (32 - text_lineh(0.62f)) / 2, 0.62f, col_alpha(T.text, a));
	}
}

static void xfer_head(const Xfer *x, const char **head, u32 *col, IconId *ic)
{
	bool up = x->kind == XF_UPLOAD;
	*head = up ? _("Recibiendo") : _("Enviando");
	*col = up ? T.ok : T.accent;
	*ic = up ? IC_DOWNLOAD : IC_UPLOAD; // la consola recibe (↓) o envía (↑)
	if (x->kind == XF_COPY) {
		*head = _("Copiando");
		*col = T.accent2;
		*ic = IC_FOLDER;
	} else if (x->kind == XF_INSTALL) {
		*head = _("Instalando");
		*col = T.ok;
		*ic = IC_DOWNLOAD;
	} else if (x->kind == XF_FETCH) {
		*head = _("Descargando");
		*col = T.accent;
		*ic = IC_GLOBE;
	} else if (x->kind == XF_ZIP || x->kind == XF_UNZIP) {
		*head = x->kind == XF_ZIP ? _("Comprimiendo") : _("Descomprimiendo");
		*col = T.warn;
		*ic = IC_ARCHIVE;
	}
	if (x->st == XS_PAUSED) { // una descarga en pausa: no es un fallo
		*head = _("En pausa");
		*col = T.text3;
		*ic = IC_CLOCK;
	} else if (x->st == XS_DONE) {
		*head = _("Completado");
		*col = T.ok;
		*ic = IC_CHECK;
	} else if (x->st != XS_RUNNING) {
		*head = _("Transferencia incompleta");
		*col = T.danger;
		*ic = IC_CLOSE;
	}
}

// transferencia a lo ancho, para la pantalla con el QR minimizado
static void xfer_wide(float x0, float y, float w, float a)
{
	const Xfer *xs[3];
	int n = visible_xfers(xs, 3);
	if (!n)
		return;
	const Xfer *x = xs[0];
	const char *head;
	u32 col;
	IconId ic;
	xfer_head(x, &head, &col, &ic);
	float frac = x->st == XS_DONE ? 1 : x->total ? (float)((double)x->done / x->total) : 0;
	char pc[12], line[96], d[24], t[24];
	snprintf(pc, sizeof pc, "%d%%", (int)(frac * 100));
	draw_circle(x0 + 13, y + 13, 13, col_alpha(col, a));
	draw_icon(ic, x0 + 13, y + 13, 15, col_alpha(0xFFFFFFFF, a));
	float pw = text_width(pc, 0.62f);
	text_draw_clip(head, x0 + 34, y + 4, w - 34 - pw - 10, 0.55f, col_alpha(T.text, a));
	text_draw_right(pc, x0 + w, y + 2, 0.62f, col_alpha(T.accent, a));
	text_draw_clip(x->name, x0, y + 28, w, TS_BODY, col_alpha(T.text, a));
	snprintf(line, sizeof line, x->kind != XF_DOWNLOAD ? _("de %s") : _("a %s"), x->device);
	text_draw_clip(line, x0, y + 46, w * 0.6f, TS_SUB, col_alpha(T.text3, a));
	brand_bar(x0, y + 64, w, 6, frac);
	util_fmt_size(d, sizeof d, x->done);
	util_fmt_size(t, sizeof t, x->total);
	snprintf(line, sizeof line, _("%s de %s"), d, t);
	text_draw_clip(line, x0, y + 76, w / 2, TS_SUB, col_alpha(T.text2, a));
	if (x->st == XS_RUNNING && x->speed > 0) {
		char sp[24], eta[24];
		util_fmt_speed(sp, sizeof sp, x->speed);
		util_fmt_eta(eta, sizeof eta, (x->total - x->done) / x->speed);
		snprintf(line, sizeof line, _("%s · quedan %s"), sp, eta);
		text_draw_right(line, x0 + w, y + 76, TS_SUB, col_alpha(T.text3, a));
	}
	if (n > 1) {
		snprintf(line, sizeof line, _("y %d más"), n - 1);
		text_draw_right(line, x0 + w, y + 46, TS_SUB, col_alpha(T.text3, a));
	}
}


// tira compacta: dirección, PIN y estado; el resto de la pantalla queda libre
static void xfer_head(const Xfer *x, const char **head, u32 *col, IconId *ic);


// debajo de la tira: las transferencias a lo ancho o una pista para volver a abrir el QR
// debajo de la tira, con el QR cerrado: toda la cola (transferencias, copias e instalaciones)
// ---- reproductor de música: lo que suena (copia por frame), su carátula y su hoja
static struct {
	MusicState s;
	bool open, just;
	float anim;
	int sel;          // botón elegido con la cruceta en la hoja
	Texture cover;
	u32 cover_track;
	float bars[24];   // la onda de arriba
} MU = {.cover_track = 0xFFFFFFFF};

// ▶ o ❚❚ dibujados (sin depender de la fuente)
static void mu_icon(float cx, float cy, float s, bool playing, u32 c)
{
	if (playing) {
		draw_rect(cx - s * 0.32f, cy - s * 0.4f, s * 0.22f, s * 0.8f, c);
		draw_rect(cx + s * 0.1f, cy - s * 0.4f, s * 0.22f, s * 0.8f, c);
	} else
		draw_tri(cx - s * 0.28f, cy - s * 0.42f, cx - s * 0.28f, cy + s * 0.42f, cx + s * 0.42f, cy, c);
}

static bool mu_on(void)
{
	return MU.s.st == MU_PLAY || MU.s.st == MU_PAUSE;
}

static void mu_fmt(u32 ms, char *out, size_t n)
{
	snprintf(out, n, "%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)(ms / 1000 % 60));
}

// arriba: la carátula (o una nota), lo que suena, la onda y el tiempo
static void music_top(float a)
{
	if (MU.cover_track != MU.s.track) {
		MU.cover_track = MU.s.track;
		u8 *rgba;
		int w, h;
		u32 tr;
		if (music_cover(&rgba, &w, &h, &tr)) {
			tex_from_rgba(&MU.cover, rgba, w, h);
			free(rgba);
		} else
			tex_free(&MU.cover);
	}
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28, PS = h - 20;
	float px = X - 4, py0 = y0 + 10;
	draw_rrect(px, py0, PS, PS, 12, col_alpha(T.surface2, a));
	if (MU.cover.ok) {
		float k = fminf(PS / MU.cover.w, PS / MU.cover.h), cw = MU.cover.w * k, ch = MU.cover.h * k;
		draw_texture(&MU.cover, px + (PS - cw) / 2, py0 + (PS - ch) / 2, cw, ch, a);
	} else
		draw_icon(IC_SPEAKER, px + PS / 2, py0 + PS / 2, 38, col_alpha(T.accent2, a));
	float rx = px + PS + 14, rw = X + W - rx, y = py0 + 1;
	const char *d = strrchr(MU.s.dir, '/');
	char head[160];
	snprintf(head, sizeof head, MU.s.st == MU_PAUSE ? _("En pausa · %s") : _("Reproduciendo · %s"), d && d[1] ? d + 1 : "SD");
	text_draw_clip(head, rx, y, rw, TS_SUB, col_alpha(MU.s.st == MU_PAUSE ? T.text3 : T.accent, a));
	y += 15;
	text_draw_clip(MU.s.title, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 2;
	if (MU.s.artist[0])
		text_draw_clip(MU.s.artist, rx, y, rw, TS_SUB, col_alpha(T.text2, a));
	// la onda: barras que siguen lo fuerte que suena
	float wy = y + 18, wh = 22;
	for (int i = 0; i < 24; i++) {
		float target = MU.s.st == MU_PLAY ? MU.s.level * (0.35f + 0.65f * fabsf(sinf(g_time * (2.1f + i * 0.37f) + i))) : 0.04f;
		MU.bars[i] = approach(MU.bars[i], target, 10);
		float bw = rw / 24.0f, bh = fmaxf(2, MU.bars[i] * wh);
		draw_rrect(rx + i * bw + 1, wy + (wh - bh) / 2, bw - 2, bh, 1.5f, col_alpha(T.accent2, a * 0.85f));
	}
	// tiempo
	float py = py0 + PS - 4 - text_lineh(TS_SUB) - 8, f = MU.s.dur_ms ? fminf(1, (float)MU.s.pos_ms / MU.s.dur_ms) : 0;
	char t1[16], t2[16];
	mu_fmt(MU.s.pos_ms, t1, sizeof t1);
	mu_fmt(MU.s.dur_ms, t2, sizeof t2);
	text_draw(t1, rx, py + 8, TS_SUB, col_alpha(T.text3, a));
	if (MU.s.dur_ms)
		text_draw_right(t2, rx + rw, py + 8, TS_SUB, col_alpha(T.text3, a));
	draw_rrect(rx, py, rw, 4, 2, col_alpha(T.surface2, a));
	draw_rrect(rx, py, fmaxf(4, rw * f), 4, 2, col_alpha(T.accent, a));
}

// abajo, sobre las pestañas mientras suena: play/pausa, lo que suena y siguiente; tocarla abre el reproductor
#define MU_BAR_H 30
static bool vw_is_open(void);
static bool mu_bar_shown(void)
{
	return mu_on() && !MU.open && !(U.tab == TAB_FILES && vw_is_open()) && !U.ins_pend;
}

static void music_bar(void)
{
	if (!mu_bar_shown())
		return;
	float y = SCR_H - TABBAR_H - MU_BAR_H - 4, x = 8, w = BOT_W - 16;
	draw_rrect(x, y + 2, w, MU_BAR_H, 12, T.shadow);
	draw_rrect(x, y, w, MU_BAR_H, 12, T.surface2);
	float f = MU.s.dur_ms ? fminf(1, (float)MU.s.pos_ms / MU.s.dur_ms) : 0;
	draw_rect(x + 10, y + MU_BAR_H - 3, (w - 20) * f, 2, T.accent);
	float bx = x + 18, nx = x + w - 18;
	draw_circle(bx, y + MU_BAR_H / 2, 10, touch_in(bx - 14, y, 28, MU_BAR_H) ? T.accent2 : T.accent);
	mu_icon(bx, y + MU_BAR_H / 2, 11, MU.s.st == MU_PLAY, 0xFFFFFFFF);
	draw_icon(IC_CHEV_RIGHT, nx, y + MU_BAR_H / 2, 12, T.text2);
	char t[300];
	snprintf(t, sizeof t, MU.s.artist[0] ? "%s · %s" : "%s", MU.s.title, MU.s.artist);
	text_draw_clip(t, bx + 18, y + MU_BAR_H / 2 - text_lineh(TS_SUB) / 2, nx - bx - 36, TS_SUB, T.text);
	if (tap(bx - 14, y, 28, MU_BAR_H))
		music_toggle(), g_touch.up = false;
	else if (tap(nx - 16, y, 32, MU_BAR_H))
		music_next(), g_touch.up = false;
	else if (tap(x, y, w, MU_BAR_H)) {
		MU.open = MU.just = true, MU.sel = 2;
		g_touch.up = false;
	}
}

static void mini_body(float a)
{
	if (a <= 0.01f)
		return;
	float y = BODY_Y + 2, h = BODY_B - y;
	const Xfer *xs[MAX_XFERS];
	int nx = visible_xfers(xs, MAX_XFERS);
	const InsReq *wait[MAX_INS];
	int nw = 0;
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING || U.ins[i].st == INS_QUEUED)
			wait[nw++] = &U.ins[i];
	if (!nx && !nw && mu_on()) { // sin transferencias: lo que suena
		music_top(a);
		return;
	}
	if (!nx && !nw) {
		float cy = y + h / 2 - 18;
		draw_halo(TOP_W / 2, cy, 64, 0.25f * a);
		draw_cloud(TOP_W / 2, cy, 20, a);
		text_draw_center(_("Tu nube está lista"), TOP_W / 2, cy + 28, TS_BODY, col_alpha(T.text2, a));
		text_draw_center(_("SELECT vuelve al inicio"), TOP_W / 2, cy + 48, TS_SUB, col_alpha(T.text3, a));
		return;
	}
	if (nx == 1 && !nw) { // una sola: a lo grande
		draw_rrect(CARD_X, y + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
		draw_rrect(CARD_X, y, CARD_W, h, 16, col_alpha(T.surface, a));
		xfer_wide(CARD_X + 16, y + 12, CARD_W - 32, a);
		return;
	}
	draw_rrect(CARD_X, y + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28, RH = 36;
	char head[48];
	snprintf(head, sizeof head, _("En curso: %d"), nx + nw);
	text_draw_clip(head, X, y + 8, W, TS_SUB, col_alpha(T.text3, a));
	float ry = y + 26;
	int shown = 0, max = (int)((h - 30) / RH);
	for (int i = 0; i < nx && shown < max; i++, shown++, ry += RH) {
		const Xfer *x = xs[i];
		const char *hd;
		u32 col;
		IconId ic;
		xfer_head(x, &hd, &col, &ic);
		float frac = x->st == XS_DONE ? 1 : x->total ? (float)((double)x->done / x->total) : 0;
		draw_circle(X + 12, ry + 14, 11, col_alpha(col, a));
		draw_icon(ic, X + 12, ry + 14, 12, col_alpha(0xFFFFFFFF, a));
		char pc[16], sub[96];
		snprintf(pc, sizeof pc, "%d%%", (int)(frac * 100));
		text_draw_clip(x->name, X + 30, ry + 1, W - 80, TS_BODY, col_alpha(T.text, a));
		text_draw_right(pc, X + W, ry + 1, TS_BODY, col_alpha(T.accent, a));
		snprintf(sub, sizeof sub, "%s · %s", hd, x->device);
		text_draw_clip(sub, X + 30, ry + 17, W - 30 - 90, TS_SUB, col_alpha(T.text3, a));
		brand_bar(X + W - 84, ry + 21, 84, 5, frac);
	}
	for (int i = 0; i < nw && shown < max; i++, shown++, ry += RH) {
		draw_circle(X + 12, ry + 14, 11, col_alpha(T.surface2, a));
		draw_icon(IC_GEM, X + 12, ry + 14, 12, col_alpha(T.ok, a));
		text_draw_clip(wait[i]->title, X + 30, ry + 1, W - 30, TS_BODY, col_alpha(T.text, a));
		text_draw_clip(wait[i]->st == INS_PENDING ? _("Pendiente de confirmación") : _("En cola para instalar"), X + 30, ry + 17, W - 30,
		               TS_SUB, col_alpha(T.text3, a));
	}
	if (nx + nw > shown) {
		char more[32];
		snprintf(more, sizeof more, _("y %d más"), nx + nw - shown);
		text_draw_right(more, X + W, y + 8, TS_SUB, col_alpha(T.text3, a));
	}
}

// subpágina «Acerca de»: la tarjeta muestra el enlace elegido (al salir vuelve el QR de conexión)
/* «Acerca de» ocupa toda la pantalla de arriba: sin cabecera ni tarjeta */
static void about_top(void)
{
	int sel = U.list[TAB_COUNT + 1].sel;
	const float X = 222, W = TOP_W - X - 16;
	if (sel >= AB_GITHUB && sel <= AB_KOFI) {
		static const char *const NAMES[3] = {"GitHub", "Discord", "Ko-fi"};
		const char *desc[3] = {_("Código fuente y nuevas versiones. Si encuentras un error, puedes notificarlo abriendo una incidencia (issue)."),
		                       _("Únete a la comunidad: soporte, ideas y novedades de cada versión."),
		                       _("Si Cloud3DS te resulta útil, puedes apoyar su desarrollo con una donación.")};
		int k = sel - AB_GITHUB;
		/* el QR con su marco blanco termina en x≈212: el texto empieza 24 px después */
		const float QX = 236, QW = TOP_W - QX - 14;
		draw_qr(LINKS[k], 16, 24, 192, 1);
		text_draw(NAMES[k], QX, 26, 0.78f, T.text);
		text_wrap_lines(desc[k], QW, 0.44f, 6, QX, 60, T.text2, true);
		text_draw_clip(_("Escanéalo con tu móvil"), QX, 164, QW, TS_SUB, T.text3);
		/* si el enlace no cabe, se corta en la última «/» y no a mitad del código */
		const char *link = LINKS[k] + 8, *cut = strrchr(link, '/');
		if (text_width(link, TS_SUB) > QW && cut) {
			char first[64];
			snprintf(first, sizeof first, "%.*s", (int)(cut - link + 1), link);
			text_draw_clip(first, QX, 180, QW, TS_SUB, T.accent);
			text_draw_clip(cut + 1, QX, 180 + text_lineh(TS_SUB), QW, TS_SUB, T.accent);
		} else {
			text_draw_clip(link, QX, 180, QW, TS_SUB, T.accent);
		}
	} else {
		const float LX = 108;
		draw_halo(LX, 84, 96, 0.35f);
		draw_cloud(LX, 84, 46, 1);
		float w = text_width(APP_NAME, 1.05f);
		text_draw_grad(APP_NAME, LX - w / 2, 136, 1.05f, g_time * 0.1f, 1);
		char vb[80];
		snprintf(vb, sizeof vb, _("v%s · desarrollado por %s"), APP_VERSION, "VENOMSHiNE");
		text_draw_center(vb, LX, 178, TS_SUB, T.text2);
		text_draw(_("Gracias por usar Cloud3DS"), X, 18, TS_TITLE, T.text);
		// los dos textos tienen que caber antes de la licencia: si no (japonés), se reduce un poco la letra
		const char *t1 = _(ABOUT_TEXT), *t2 = _("Para obtener ayuda, contacta por GitHub o Discord.");
		float sc = 0.42f;
		while (sc > 0.34f && 46 + (text_wrap_lines(t1, W, sc, 12, 0, 0, 0, false) + text_wrap_lines(t2, W, sc, 4, 0, 0, 0, false)) *
		                              text_lineh(sc) + 8 > 212)
			sc -= 0.01f;
		int n = text_wrap_lines(t1, W, sc, 12, X, 46, T.text2, true);
		text_wrap_lines(t2, W, sc, 4, X, 54 + n * text_lineh(sc), T.accent, true);
		text_draw_clip(_("Software libre · licencia GPL-3.0"), X, 216, W, TS_SUB, T.text3);
	}
}

// ------------------------------------------------------------ presentación
static void splash_top(float alpha)
{
	float t = U.splash >= 0 ? U.splash : 2.0f;
	float a = ease_out_cubic(t / 0.9f);
	// halo radial suave que «respira» detrás del logo
	float breathe = 1 + 0.04f * sinf(t * 3.0f);
	draw_halo(200, 104, (120 + 110 * a) * breathe, 0.75f * a * alpha);
	// el logo crece con una escala amortiguada (sin rebotes bruscos) y su resplandor lo acompaña
	float sc = ease_out_back(clampf(t / 0.9f, 0, 1));
	if (sc > 0.01f) {
		draw_cloud_glow(200, 92, 44 * sc, 0.8f * a * alpha);
		draw_cloud(200, 92, 44 * sc, alpha * clampf(t / 0.25f, 0, 1));
	}
	float ta = clampf((t - 0.5f) / 0.5f, 0, 1) * alpha;
	float w = text_width(APP_NAME, 1.1f);
	text_draw_grad(APP_NAME, 200 - w / 2, 150 + (1 - ta) * 10, 1.1f, t * 0.3f, ta);
	text_draw_center(_("Tu nube local"), 200, 190, 0.5f, col_alpha(T.text3, ta));
}

static void splash_bot(float alpha)
{
	float t = U.splash >= 0 ? U.splash : 2.0f;
	float ta = clampf((t - 0.7f) / 0.4f, 0, 1) * alpha;
	for (int i = 0; i < 3; i++) {
		float ph = sinf(g_time * 6 - i * 0.7f) * 0.5f + 0.5f;
		draw_circle(148 + i * 12, 112, 3 + ph * 1.5f, col_alpha(brand_grad(i / 3.0f), ta * (0.4f + ph * 0.6f)));
	}
	text_draw_center("v" APP_VERSION, 160, 200, TS_SUB, col_alpha(T.text3, ta));
	text_draw_center(_("desarrollado por VENOMSHiNE"), 160, 214, TS_SUB, col_alpha(T.text3, ta));
}

static void files_top(float a);
static void vw_top(void);
static bool vw_is_open(void);
static bool device_top(float a);

static bool qrs_is_open(void);
static void qr_top(void);

// ------------------------------------------------------------ desinstalar (en un hilo)
// AM_DeleteTitle no dice cuánto lleva: se enseñan las partes (juego, actualización, DLC) con la que
// se está borrando, el tiempo y que no se apague. HOME queda bloqueado hasta que termine
static struct {
	Thread thr;
	bool busy;
	volatile bool done;
	volatile int part; // la que se está borrando (0 juego, 1 actualización, 2 DLC)
	int res;
	u64 tid, size[3];
	bool has[3];
	char name[64];
	u64 t0;
	Texture icon;
	float anim;
	int kind;          // UN_ALL desinstalar entero, UN_UPD la actualización, UN_DLC los DLC, UN_EXT copiar y borrar los datos extra
	TitleInfo ti;      // (UN_EXT: el título, para su copia)
	char extra[200];   // (UN_EXT: dónde quedó la copia)
	int ins_icon_id;   // instalando: de qué petición es el icono cargado
	bool ins_mode;     // la pantalla es de una instalación (se recuerda mientras se desvanece)
	bool upd;          // y es la de Cloud3DS
	Texture ins_icon;
} UN = {.ins_icon_id = -1};
enum { UN_ALL, UN_UPD, UN_DLC, UN_EXT };

static void un_step(int part)
{
	UN.part = part;
}

static void un_thread(void *arg)
{
	(void)arg;
	if (UN.kind == UN_ALL)
		UN.res = title_uninstall(UN.tid, un_step);
	else if (UN.kind == UN_UPD || UN.kind == UN_DLC)
		UN.part = UN.kind == UN_UPD ? 1 : 2, UN.res = title_uninstall_part(UN.tid, UN.kind == UN_UPD ? 1 : 2);
	else { // los datos extra: primero su copia; sin copia, no se borra nada
		UN.part = 3;
		UN.res = extdata_backup(&UN.ti, UN.extra, sizeof UN.extra);
		if (UN.res == 0) {
			UN.part = 4, UN.res = extdata_delete(&UN.ti);
			if (UN.res == -4) // compartidos con otro juego: no se borró nada, la copia sobra
				fsx_rmrf(UN.extra);
		} else
			UN.res = -3;
	}
	UN.done = true;
}

// la pantalla de espera: desinstalando o borrando (UN.busy), o instalando un CIA (de un archivo, del móvil, mientras se
// descarga o la actualización de Cloud3DS)
static bool guard_on(void)
{
	return UN.busy || ins_busy() || ins_streaming() || upd_installing();
}

static const char *un_part_name(int k)
{
	return k == 0 ? _("Juego") : k == 1 ? _("Actualización") : k == 2 ? _("Contenido descargable") : k == 3 ? _("Copia de seguridad") : _("Datos extra");
}

static void un_elapsed(char *out, size_t n)
{
	u64 s = (util_ms() - UN.t0) / 1000;
	snprintf(out, n, "%llu:%02llu", (unsigned long long)(s / 60), (unsigned long long)(s % 60));
}

// el título de la operación: con nombres largos (o en otros idiomas) se encoge un poco antes de cortarse
static void un_title(const char *t, float cx, float y, float a)
{
	float mw = TOP_W - 40, sc = TS_TITLE, w = text_width(t, sc);
	while (w > mw && sc > TS_TITLE * 0.55f)
		sc -= 0.01f, w = text_width(t, sc);
	text_draw_center_clip(t, cx, y + (text_lineh(TS_TITLE) - text_lineh(sc)) / 2, mw, sc, col_alpha(T.text, a));
}

// arriba, a pantalla completa: el juego en el centro con anillos girando (se está trabajando)
static void un_top(void)
{
	float a = ease_out_cubic(UN.anim);
	draw_rect(0, 0, TOP_W, SCR_H, guard_on() ? T.bg : col_alpha(T.bg, a)); // (puesta, se dibuja sola: el fondo, entero)
	const float cx = TOP_W / 2, cy = 92;
	draw_halo(cx, cy, 110, 0.35f * a);
	float pulse = 1 + 0.04f * sinf(g_time * 3);
	draw_ring(cx, cy, 58, 5, col_alpha(T.surface2, a));
	draw_arc(cx, cy, 58, 5, g_time * 4.5f, g_time * 4.5f + 2.0f, col_alpha(T.accent, a));
	draw_arc(cx, cy, 68, 2, -g_time * 2.6f, -g_time * 2.6f + 1.3f, col_alpha(T.accent2, a * 0.7f));
	draw_arc(cx, cy, 68, 2, -g_time * 2.6f + 3.14f, -g_time * 2.6f + 4.44f, col_alpha(T.accent2, a * 0.7f));
	float is = 64 * pulse;
	draw_rrect(cx - 40, cy - 40, 80, 80, 20, col_alpha(T.surface, a));
	if (UN.ins_mode) { // instalando un CIA: su icono, su nombre y cuánto lleva
		const InsReq *ir = NULL;
		for (int i = 0; i < U.ins_n && !ir; i++)
			if (U.ins[i].st == INS_RUNNING)
				ir = &U.ins[i];
		const Xfer *xs = NULL; // (o el que se instala mientras se descarga)
		for (int i = 0; i < MAX_XFERS && !xs; i++)
			if (V.xfers[i].st == XS_RUNNING && V.xfers[i].kind == XF_FETCH)
				xs = &V.xfers[i];
		if (ir && ir->has_icon && UN.ins_icon_id != ir->id) {
			UN.ins_icon_id = ir->id;
			tex_from_rgb565(&UN.ins_icon, ir->icon, 48, 48);
		}
		if (ir && ir->has_icon && UN.ins_icon.ok)
			draw_texture(&UN.ins_icon, cx - is / 2, cy - is / 2, is, is, a);
		else
			draw_icon(IC_DOWNLOAD, cx, cy, 34, col_alpha(T.accent, a));
		char t[120], l[120];
		u64 done = ir ? ir->done : xs ? xs->done : 0, tot = ir ? ir->size : xs ? xs->total : 0;
		snprintf(t, sizeof t, _("Instalando «%s»"), ir ? ir->title : xs ? xs->name : "CIA");
		if (!ir && !xs && UN.upd) { // la actualización de Cloud3DS
			UpdInfo ui;
			upd_get(&ui);
			done = ui.done, tot = ui.total;
			snprintf(t, sizeof t, "%s", _("Actualizando Cloud3DS"));
		}
		un_title(t, cx, cy + 84, a);
		float f = tot ? clampf((float)((double)done / tot), 0, 1) : 0;
		float bw = 220, bx = cx - bw / 2, by = cy + 84 + text_lineh(TS_TITLE) + 8;
		draw_rrect(bx, by, bw, 6, 3, col_alpha(T.surface2, a));
		draw_grad_h(bx, by, fmaxf(6, bw * f), 6, col_alpha(T.accent, a), col_alpha(T.accent2, a));
		char d1[24], d2[24];
		util_fmt_size(d1, sizeof d1, done), util_fmt_size(d2, sizeof d2, tot);
		snprintf(l, sizeof l, "%d%% · %s / %s", (int)(f * 100), d1, d2);
		text_draw_center(l, cx, by + 12, TS_SUB, col_alpha(T.text3, a));
		return;
	}
	if (UN.icon.ok)
		draw_texture(&UN.icon, cx - is / 2, cy - is / 2, is, is, a);
	else
		draw_icon(IC_GAMEPAD, cx, cy, 34, col_alpha(T.text3, a));
	char t[120], l[160], sz[24], el[24];
	if (UN.kind == UN_ALL)
		snprintf(t, sizeof t, _("Desinstalando «%s»"), UN.name);
	else if (UN.kind == UN_EXT)
		snprintf(t, sizeof t, _("Datos extra de «%s»"), UN.name);
	else
		snprintf(t, sizeof t, UN.kind == UN_UPD ? _("Actualización de «%s»") : _("DLC de «%s»"), UN.name);
	un_title(t, cx, cy + 84, a);
	int np = UN.has[0] + UN.has[1] + UN.has[2], k = UN.part, idx = 0;
	for (int i = 0; i < k && i < 3; i++)
		idx += UN.has[i];
	util_fmt_size(sz, sizeof sz, UN.size[k < 3 ? k : 0]);
	un_elapsed(el, sizeof el);
	if (UN.kind == UN_EXT)
		snprintf(l, sizeof l, k == 3 ? _("Copiando los datos extra (%s) · %s") : _("Borrando los datos extra · %s"), k == 3 ? sz : el, el);
	else if (UN.kind != UN_ALL)
		snprintf(l, sizeof l, _("Borrando: %s (%s) · %s"), un_part_name(k), sz, el);
	else if (np > 1)
		snprintf(l, sizeof l, _("Borrando: %s (%s) · paso %d de %d · %s"), un_part_name(k), sz, idx + 1, np, el);
	else
		snprintf(l, sizeof l, _("Borrando: %s (%s) · %s"), un_part_name(k), sz, el);
	text_draw_center_clip(l, cx, cy + 84 + text_lineh(TS_TITLE) + 4, TOP_W - 40, TS_SUB, col_alpha(T.text3, a));
}

// cinta azul y negra que avanza, medio transparente
static void un_tape(float y, float h, float a)
{
	gfx_clip(0, y, BOT_W, h);
	draw_rect(0, y, BOT_W, h, col_alpha(0xFF000000, a * 0.35f));
	float sw = h * 0.9f, off = fmodf(g_time * 24, 2 * sw);
	for (float x = -2 * sw - h + off; x < BOT_W + h; x += 2 * sw) {
		u32 c = col_alpha(T.accent, a * 0.45f);
		draw_tri(x, y + h, x + sw, y + h, x + sw + h, y, c);
		draw_tri(x, y + h, x + sw + h, y, x + h, y, c);
	}
	gfx_clip_off();
}

// abajo, a pantalla completa: el cartel de no tocar nada
static void un_card(void)
{
	if (guard_on())
		UN.ins_mode = !UN.busy, UN.upd = upd_installing();
	UN.anim = approach(UN.anim, guard_on() ? 1 : 0, 10);
	if (UN.anim < 0.01f)
		return;
	float a = ease_out_cubic(UN.anim);
	draw_rect(0, 0, BOT_W, SCR_H, guard_on() ? T.bg : col_alpha(T.bg, a * 0.93f)); // (al irse, se adivina lo de debajo)
	un_tape(0, 18, a);
	un_tape(SCR_H - 18, 18, a);
	const float cx = BOT_W / 2;
	float k = 0.5f + 0.5f * sinf(g_time * 4);
	draw_circle(cx, 62, 28 + 3 * k, col_alpha(T.accent, a * (0.15f + 0.1f * k)));
	draw_circle(cx, 62, 22, col_alpha(T.accent, a));
	draw_icon(IC_POWER, cx, 62, 22, col_alpha(0xFFFFFFFF, a));
	text_draw_center(_("NO INTERRUMPAS LA OPERACIÓN"), cx, 98, 0.85f, col_alpha(T.text, a));
	wrap_center(UN.ins_mode ? _("Instalación en curso. No apagues la consola, no extraigas la tarjeta SD ni pulses HOME: si se interrumpe, la instalación podría quedar dañada.")
	                     : _("No apagues la consola, no extraigas la tarjeta SD ni pulses HOME. Si se interrumpe, la lista de títulos de la consola podría quedar dañada."), cx,
	            132, BOT_W - 40, TS_SUB, 4, col_alpha(T.text, a));
	if (UN.ins_mode && ins_streaming()) { // descargando e instalando a la vez: se puede cancelar sin riesgo
		char cl[64];
		snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cancelar la descarga"));
		float cw = text_width(cl, TS_SUB) + 28;
		draw_rrect(cx - cw / 2, SCR_H - 54, cw, 26, 13, col_alpha(T.surface2, a));
		text_draw_center(cl, cx, SCR_H - 41 - text_lineh(TS_SUB) / 2, TS_SUB, col_alpha(T.text, a));
	} else
		wrap_center(_("Cloud3DS continuará automáticamente al terminar."), cx, SCR_H - 44, BOT_W - 40, TS_SUB, 1, col_alpha(T.text3, a));
}

// cada vuelta: al terminar el hilo, se avisa y se vuelve a leer la lista
static void titles_reload(void);
static void tl_poll(void);
static void un_poll(void)
{
	if (!UN.busy || !UN.done)
		return;
	threadJoin(UN.thr, U64_MAX);
	threadFree(UN.thr);
	UN.busy = false;
	if (UN.kind != UN_ALL) { // por partes: su aviso
		const char *ok = UN.kind == UN_UPD ? _("Actualización desinstalada") : UN.kind == UN_DLC ? _("DLC desinstalados") : _("Datos extra borrados (con copia en la SD)");
		const char *ko = UN.res == -3 ? _("No se pudo crear la copia de seguridad. No se ha eliminado nada.")
		               : UN.res == -4 ? _("Otro título instalado usa estos datos extra. No se han eliminado.")
		                              : _("No se pudo completar");
		toast_icon(UN.res == 0 ? ok : ko, UN.res == 0 ? IC_TRASH : IC_CLOSE, UN.res == 0 ? T.ok : T.danger);
		titles_reload();
		return;
	}
	if (UN.res == 0) {
		char m[120];
		snprintf(m, sizeof m, _("Desinstalado: %s"), UN.name);
		toast_icon(m, IC_TRASH, T.ok);
		state_lock();
		event_add(EV_DELETE, UN.name, G.console, 0);
		state_unlock();
	} else
		toast_icon(_("No se pudo desinstalar"), IC_CLOSE, T.danger);
	titles_reload();
}

static bool hm_is_open(void);
static void dl_top(void);
static bool DLV;      // Descargas: viendo lo descargado (si no, sus tarjetas)
static bool DLQ;      // Descargas: la cola (pausar, reanudar, cancelar…)
static int DLV_n = -1; // cuántos hay en la carpeta (-1: contarlos)
static void hm_top(void);
static void hm_bottom(void);
static void hm_show(void);

// arriba, en una aplicación sin nada elegido: su icono y una frase
static void app_idle_top(IconId ic, const char *title, const char *text)
{
	float y = BODY_Y, h = SCR_H - 8 - y, cy = y + h / 2 - 20;
	draw_rrect(CARD_X, y + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y, CARD_W, h, 16, T.surface);
	draw_circle(TOP_W / 2, cy, 30, T.surface2);
	draw_icon(ic, TOP_W / 2, cy, 30, T.text2);
	text_draw_center(title, TOP_W / 2, cy + 40, TS_TITLE, T.text);
	if (text)
		wrap_center(text, TOP_W / 2, cy + 64, 300, TS_SUB, 2, T.text3);
}

// Conectar: el código QR y la dirección (o por qué aún no se puede)
static void conn_top(void)
{
	if (!strcmp(V.srv_error, "soc")) {
		char msg[160];
		snprintf(msg, sizeof msg, _("El servicio de red de la consola no responde (error %08lX). Reintentando automáticamente…"), (unsigned long)V.net_err);
		card_message(IC_REFRESH, T.warn, _("Iniciando la red"), msg);
	} else if (!V.ip && V.wifi == 3)
		card_message(IC_WIFI, T.accent, _("Conectado al Wi-Fi"), _("Esperando a que el router asigne una dirección IP…"));
	else if (!V.ip)
		card_message(IC_WIFI_OFF, T.text3, _("Sin conexión Wi-Fi"),
		             _("Conecta la consola a una red Wi-Fi. Si no tienes router, puedes usar el punto de acceso de tu móvil."));
	else if (V.srv_error[0])
		card_message(IC_INFO, T.warn, _("No se pudo iniciar el servidor"), _("Cierra otras aplicaciones de red (como FTPD) y vuelve a abrir Cloud3DS."));
	else if (V.srv_off)
		card_message(IC_POWER, T.text3, _("Servidor apagado"), _("Actívalo en Ajustes > Conexión para conectar otros dispositivos."));
	else if (!V.running) {
		card();
		draw_arc(TOP_W / 2, CARD_Y + 80, 16, 3, g_time * 6, g_time * 6 + 4.2f, T.accent);
		text_draw_center(_("Iniciando el servidor…"), TOP_W / 2, CARD_Y + 114, TS_BODY, T.text3);
	} else {
		card();
		char url[64], qr[96];
		url_text(url, sizeof url, true);
		snprintf(qr, sizeof qr, "%s/#k=%s", url, V.qr_key);
		draw_qr(qr, QR_X, QR_Y, QR_S, 1);
		connect_col(1, 0);
		draw_status(COL_X, QR_Y + QR_S - 12, COL_W, 1);
	}
}

// Actividad: lo que está en curso o, sin nada, cómo funciona
static void activity_top(void)
{
	const Xfer *xs[1];
	bool any = visible_xfers(xs, 1) > 0;
	for (int i = 0; i < U.ins_n && !any; i++)
		any = U.ins[i].st == INS_PENDING || U.ins[i].st == INS_QUEUED;
	if (any || mu_on())
		mini_body(1);
	else
		app_idle_top(IC_CLOCK, _("Sin transferencias en curso"), _("Aquí se muestra el progreso de lo que envíes, recibas, descargues o instales."));
}


static void tab_header(const char *title, bool back);
static void qrs_open(void);
static const char *dl_dest_dir(const char *url);
// Descargas: desde un enlace o escaneando un QR; arriba, lo que se está descargando
static void dl_recent_top(void);
static void dl_top(void)
{
	const Xfer *xs[MAX_XFERS];
	int n = visible_xfers(xs, MAX_XFERS), f = 0;
	for (int i = 0; i < n; i++)
		f += xs[i]->kind == XF_FETCH;
	(void)f;
	if (DLV) {
		files_top(1);
		sd_bar(1);
		return;
	}
	dl_recent_top();
}
static void titles_space_bar(void);
static bool f_is_titles(void);
static void ml_top(void);
static void sh_top(void);
static void rl_top(void);
static void rv_top(void);
static bool rv_is_open(void);
static u32 ap_col(int i);

static void draw_top(void)
{
	gfx_target(g_top, T.bg);
	if (U.splash >= 0) {
		splash_top(1);
		return;
	}
	if (UN.anim > 0.99f || guard_on()) { // instalando o borrando: solo la pantalla de espera (lo de debajo podría esperar a la SD)
		un_top();
		return;
	}
	if (br_is_open()) { // navegador: las dos pantallas
		br_top();
		toast_draw_float();
		return;
	}
	if (rv_is_open()) { // leyendo: las dos pantallas
		rv_top();
		return;
	}
	if (CLU.open) { // la nube (y lo que se abre desde ella: el visor o el reproductor)
		if (vw_cloud_open()) {
			vw_top();
			toast_draw_top();
		} else if (MU.open) {
			draw_rect(0, 0, TOP_W, SCR_H, T.bg);
			draw_header();
			music_top(1);
			toast_draw_top();
		} else
			cl_top();
		return;
	}
	if (qrs_is_open()) { // escáner de QR: la cámara a pantalla completa
		qr_top();
		toast_draw_top();
		return;
	}
	if (hm_is_open()) { // el inicio por aplicaciones
		hm_top();
		if (UN.anim > 0.01f)
			un_top();
		if (U.fade > 0) {
			draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, U.fade));
			splash_top(U.fade);
		}
		return;
	}
	if (U.tab == TAB_FILES && vw_is_open()) { // visor: pantalla completa, sin cabecera
		vw_top();
		toast_draw_top();
		return;
	}
	bool about = U.tab == TAB_SETTINGS && U.page == PAGE_ABOUT;
	if (!about)
		draw_header();
	g_body_y = CARD_Y; // (sin la tira del servidor: cada aplicación, su pantalla de arriba entera)
	if (about)
		about_top();
	else if (U.tab == TAB_SETTINGS && U.page == PAGE_UPDATE)
		update_top();
	else if (U.ins_pend)
		install_top(1);
	else if (U.app == AP_CONNECT) {
		const Xfer *xs[1];
		bool busy = visible_xfers(xs, 1) > 0;
		if (busy)
			mini_body(1);
		else if (!(U.tab == TAB_DEVICES && device_top(1)))
			conn_top();
	} else if (U.app == AP_DL)
		dl_top();
	else if (U.app == AP_MUSIC)
		ml_top();
	else if (U.app == AP_SHARE)
		sh_top();
	else if (U.app == AP_READER)
		rl_top();
	else if (MU.open)
		music_top(1);
	else if (U.tab == TAB_FILES) {
		files_top(1);
		if (f_is_titles())
			titles_space_bar();
		else
			sd_bar(1);
	} else if (U.tab == TAB_SETTINGS && U.page == PAGE_MAIN)
		settings_top(1);
	else if (U.tab == TAB_SETTINGS && (U.page == PAGE_PIN || U.page == PAGE_CONN))
		settings_sub_top(1);
	else if (U.tab == TAB_SETTINGS)
		app_idle_top(IC_GEAR, _("Ajustes"), NULL);
	else if (U.tab == TAB_DEVICES) {
		if (!device_top(1))
			app_idle_top(IC_DEVICES, _("Dispositivos"), _("Elige un dispositivo para ver su información y sus permisos."));
	} else
		activity_top();
	if (UN.anim > 0.01f)
		un_top();
	toast_draw_top();
	if (U.fade > 0) {
		draw_rect(0, 0, TOP_W, SCR_H, col_alpha(T.bg, U.fade));
		splash_top(U.fade);
	}
}

// ------------------------------------------------------------ pantalla inferior
static List *cur_list(void)
{
	if (U.tab == TAB_SETTINGS && U.page == PAGE_LANG)
		return &U.list[TAB_COUNT];
	if (U.tab == TAB_SETTINGS && U.page == PAGE_ABOUT)
		return &U.list[TAB_COUNT + 1];
	if (U.tab == TAB_SETTINGS && U.page == PAGE_PIN)
		return &U.list[TAB_COUNT + 2];
	if (U.tab == TAB_SETTINGS && U.page == PAGE_UPDATE)
		return &U.list[TAB_COUNT + 3];
	if (U.tab == TAB_SETTINGS && U.page == PAGE_CONN)
		return &U.list[TAB_COUNT + 4];
	return &U.list[U.tab];
}

static bool hdr_enter(void);
static void ml_top(void);    // la app Música: su biblioteca
static void rv_open(const char *path, int ret_app); // Lector: abrir un documento (al cerrarlo, vuelve a ret_app)
static void rv_cfg_load(void);
static const char *rb_title(const RBook *b, char *buf, size_t n);
static void ml_bottom(void);
static void sh_bottom(void);
static void sh_prompts(void);
static void hm_launch(int i); // (↑ en lo primero de una lista: a los botones de la cabecera)
static float g_list_bar; // lo que tapa una barra al pie de la lista (la de pegar, por ejemplo): se puede bajar hasta verlo todo
static void list_update(List *l, int count, float rh)
{
	float vh = VIEW_H - g_list_bar;
	float max = count * rh + QR_PAD - vh; // al final, un poco de aire
	if (max < 0)
		max = 0;
	if (g_touch.held && g_touch.drag && g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H) {
		l->target -= g_touch.y - g_touch.py;
		l->off = l->target;
	}
	if (U.buttons && !g_dialog.open) {
		if (g_krepeat & KEY_DOWN) l->sel++;
		if ((g_krepeat & KEY_UP) && !(l->sel == 0 && hdr_enter())) l->sel--;
		if (l->sel >= count) l->sel = count - 1;
		if (l->sel < 0) l->sel = 0;
		if (g_krepeat & (KEY_UP | KEY_DOWN)) {
			float y = l->sel * rh;
			if (y < l->target) l->target = y;
			if (y + rh > l->target + vh) l->target = y + rh - vh;
		}
	}
	l->target = clampf(l->target, 0, max);
	l->off = approach(l->off, l->target, 18);
}

// fila: resaltado (selección con botones o al pulsar) y si se tocó
static bool row(List *l, int i, float y, float h)
{
	bool vis = y + h > VIEW_Y && y < VIEW_Y + VIEW_H;
	bool inview = g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H;
	bool press = vis && inview && touch_in(6, y, BOT_W - 12, h);
	if (press || (U.buttons && i == l->sel && !g_dialog.open))
		draw_rrect(6, y + 1, BOT_W - 12, h - 2, 10, T.surface);
	bool t = vis && inview && !g_dialog.open && tap(6, y, BOT_W - 12, h);
	if (t)
		l->sel = i;
	return t;
}

static bool activate(List *l, int i)
{
	return U.buttons && !g_dialog.open && l->sel == i && (g_kdown & KEY_A);
}

// título de una pestaña (con «‹» si se puede volver); maxw: hasta dónde llega el texto
static void hm_show(void);
// ---- los botones de la cabecera, también con la cruceta: ↑ desde lo primero de la lista entra en ellos, ← → eligen,
// A pulsa y ↓ o B vuelven a la lista. Cada botón se apunta al dibujarse (hdr_hit), de izquierda a derecha
static struct {
	int n, prev;   // los apuntados en esta vuelta y en la anterior
	int focus;     // el elegido (-1: se está en la lista)
	int fire;      // el que se pulsó con A (se atiende al dibujarlo)
} HB = {.focus = -1, .fire = -1};

static bool hdr_hit(float x, float y, float w, float h)
{
	int i = HB.n++;
	if (HB.focus == i)
		draw_rrect_outline(x - 2, y - 2, w + 4, h + 4, fminf(w, h) / 2 + 2, 2, T.accent);
	bool f = HB.fire == i;
	if (f)
		HB.fire = -1;
	return tap(x, y, w, h) || f;
}

// ↑ en lo primero de una lista: a la cabecera (al botón de más a la derecha, el principal)
static bool hdr_enter(void)
{
	if (HB.prev <= 0)
		return false;
	HB.focus = HB.prev - 1;
	U.buttons = true;
	return true;
}

// al empezar a dibujar la pantalla de abajo. active: sin hojas ni diálogos encima (si no, la cabecera no mira nada)
static void hdr_frame(bool active)
{
	HB.prev = HB.n;
	HB.n = 0;
	HB.fire = -1;
	if (HB.focus >= HB.prev)
		HB.focus = HB.prev - 1;
	if (HB.focus < 0 || !active || g_dialog.open)
		return;
	if (g_touch.down) { // con el dedo, a la lista
		HB.focus = -1;
		return;
	}
	if (g_krepeat & KEY_LEFT)
		HB.focus = HB.focus > 0 ? HB.focus - 1 : 0;
	if (g_krepeat & KEY_RIGHT)
		HB.focus = HB.focus < HB.prev - 1 ? HB.focus + 1 : HB.focus;
	if (g_kdown & KEY_A)
		HB.fire = HB.focus;
	if (g_kdown & (KEY_DOWN | KEY_B))
		HB.focus = -1;
	g_kdown &= ~(KEY_A | KEY_B | KEY_X | KEY_Y | KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
	g_krepeat &= ~(KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT);
}

static void tab_header_w(const char *title, bool back, float maxw)
{
	float x = 14;
	if (!back) { // en la raíz de una aplicación: al inicio (como B)
		bool hp = touch_in(0, 0, 34, VIEW_Y);
		draw_rrect(6, 5, 26, 22, 11, hp ? col_alpha(T.accent, 0.35f) : col_alpha(T.accent, 0.18f));
		for (int q = 0; q < 4; q++)
			draw_rrect(13 + (q % 2) * 6.5f, 10 + (q / 2) * 6.5f, 5, 5, 1.4f, T.accent);
		if (!g_dialog.open && hdr_hit(6, 5, 26, 22)) {
			HB.focus = -1;
			hm_show();
		}
		x = 40;
	} else {
		if (touch_in(0, 0, 170, VIEW_Y))
			draw_rrect(6, 4, fminf(text_width(title, TS_TITLE), maxw) + 36, 24, 12, T.surface);
		draw_icon(IC_CHEV_LEFT, 20, 16, 14, T.text2);
		if (hdr_hit(8, 6, 24, 20) && !g_touch.up) // (con A en la cabecera: como B, atrás; el toque lo mira cada pantalla)
			g_kdown |= KEY_B, HB.focus = -1;
		x = 32;
	}
	text_draw_clip(title, x, 7, maxw, TS_TITLE, T.text);
}

static void tab_header(const char *title, bool back)
{
	tab_header_w(title, back, 190);
}

static void empty_state(IconId ic, const char *title, const char *text)
{
	float cy = VIEW_Y + VIEW_H / 2 - 26;
	draw_circle(BOT_W / 2, cy, 24, T.surface);
	draw_icon(ic, BOT_W / 2, cy, 24, T.text3);
	text_draw_center(title, BOT_W / 2, cy + 32, TS_BODY, T.text);
	wrap_center(text, BOT_W / 2, cy + 52, 260, TS_SUB, 3, T.text3);
}

// descargas de Internet: una copia de la cola (se renueva cuando cambia) y la hoja de una
static struct {
	DlItem it[DL_MAX];
	int n;
	u32 rev;
	bool open, just;
	float anim;
	int sel;
	u32 id; // de cuál es la hoja
} DS = {.rev = 0xFFFFFFFF};

static void ds_refresh(void)
{
	if (dl_rev() != DS.rev) {
		DS.rev = dl_rev();
		DS.n = dl_list(DS.it, DL_MAX);
	}
}

static const DlItem *ds_find(u32 id)
{
	for (int i = 0; i < DS.n; i++)
		if (DS.it[i].id == id)
			return &DS.it[i];
	return NULL;
}

static const char *dl_err_text(const char *e)
{
	static const struct { const char *k, *v; } E[] = {
		{"offline", N_("Sin conexión a Internet")}, {"tls", N_("No se pudo comprobar el certificado")}, {"net", N_("Error de red")},
		{"not_found", N_("No existe (404)")}, {"denied", N_("El servidor ha denegado el acceso")}, {"http", N_("Error del servidor")},
		{"no_space", N_("No hay espacio en la SD")}, {"io", N_("No se pudo escribir en la SD")}, {"no_cia", N_("Esa versión de GitHub no incluye ningún archivo .cia")},
		{"unzip", N_("No se pudo descomprimir")}};
	for (size_t i = 0; i < ARRAY_LEN(E); i++)
		if (!strcmp(e, E[i].k))
			return tr(E[i].v);
	return _("No se pudo descargar");
}

// la línea de debajo del nombre: cómo va
static void dl_status(const DlItem *d, char *out, size_t n, u32 *col)
{
	char a[24], b[24], sp[24];
	*col = T.text3;
	util_fmt_size(a, sizeof a, d->done);
	util_fmt_size(b, sizeof b, d->total);
	switch (d->st) {
	case DL_RUN:
		if (d->installing && d->total) {
			*col = T.ok;
			snprintf(out, n, _("Instalando durante la descarga · %d %%"), (int)(d->done * 100 / d->total));
		} else if (d->total && d->speed) {
			util_fmt_speed(sp, sizeof sp, d->speed);
			snprintf(out, n, "%d %% · %s · %s", (int)(d->done * 100 / d->total), sp, d->host);
		} else
			snprintf(out, n, "%s · %s", a, d->host);
		break;
	case DL_QUEUED: snprintf(out, n, "%s · %s", _("En cola"), d->host); break;
	case DL_PAUSED: snprintf(out, n, d->total ? "%s · %s / %s" : "%s · %s", _("En pausa"), a, b); break;
	case DL_CANCELED: snprintf(out, n, "%s", _("Cancelada")); break;
	case DL_FAIL:
		*col = T.danger;
		snprintf(out, n, "%s", dl_err_text(d->err));
		break;
	default:
		*col = T.ok;
		if (d->installed)
			snprintf(out, n, "%s · %s", _("Instalado"), b);
		else if (d->hash[0] && d->hash_ok == 0)
			*col = T.danger, snprintf(out, n, "%s · %s", b, _("la suma de comprobación no coincide"));
		else
			snprintf(out, n, d->hash_ok == 1 ? "%s · %s · %s" : "%s · %s", b, d->hash_ok == 1 ? _("suma de comprobación correcta") : d->host, d->host);
	}
}

// Descargas, arriba: las más recientes primero (cómo van o por qué fallaron)
static void dl_recent_top(void)
{
	ds_refresh();
	if (!DS.n) {
		app_idle_top(IC_DOWNLOAD, _("Sin descargas recientes"), _("Los archivos CIA se instalan durante la descarga. Todo lo descargado se guarda en /Downloads."));
		return;
	}
	// las descargas (las más recientes primero): cómo van o por qué fallaron
	float y0 = BODY_Y, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
	const float X = CARD_X + 14, W = CARD_W - 28;
	int run = 0;
	for (int i = 0; i < DS.n; i++)
		run += DS.it[i].st == DL_RUN || DS.it[i].st == DL_QUEUED;
	char t[64];
	snprintf(t, sizeof t, run == 1 ? _("1 descarga en curso") : run ? _("%d descargas en curso") : _("Descargas recientes"), run);
	text_draw(t, X, y0 + 10, TS_TITLE, T.text);
	float y = y0 + 12 + text_lineh(TS_TITLE) + 6;
	for (int k = DS.n - 1, shown = 0; k >= 0 && shown < 4; k--, shown++) {
		const DlItem *d = &DS.it[k];
		u32 col;
		char st[160];
		dl_status(d, st, sizeof st, &col);
		bool fail = d->st == DL_FAIL;
		draw_circle(X + 13, y + 15, 13, col_alpha(fail ? T.danger : d->st == DL_DONE ? T.ok : T.accent, 0.16f));
		draw_icon(fail ? IC_CLOSE : d->st == DL_DONE ? IC_CHECK : IC_DOWNLOAD, X + 13, y + 15, 13, fail ? T.danger : d->st == DL_DONE ? T.ok : T.accent);
		text_draw_clip(d->name[0] ? d->name : d->url, X + 34, y, W - 34, TS_BODY, T.text);
		text_draw_clip(st, X + 34, y + 17, W - 34, TS_SUB, col);
		if (d->st == DL_RUN && d->total) {
			draw_rrect(X + 34, y + 32, W - 34, 4, 2, T.surface2);
			draw_rrect(X + 34, y + 32, fmaxf(4, (W - 34) * (float)((double)d->done / d->total)), 4, 2, T.accent);
		}
		y += 42;
	}
	text_draw_right(_("Gestiónalas en Conectar > Actividad"), X + W, y0 + h - 8 - text_lineh(TS_SUB), TS_SUB, T.text3);
}

// ------------------------------------------------------------ nube (OneDrive)
static void explain_top(float a, IconId ic, u32 col, const char *title, const char *desc, const char *foot, u32 fc);
static int sheet_list(bool *open, bool *just, float *anim, int *sel, const char *title, const IconId *ic, const char *const *lab,
                      const u32 *col, int n);
static float info_row(float x, float y, float w, const char *k, const char *v, u32 vc, float a);
static int segmented(float x, float y, float w, float h, const char *const *opts, int n, int cur, bool focus);

static void cl_open(void)
{
	CLU.open = true;
	if (!CLU.it)
		CLU.it = malloc(sizeof(CloudItem) * 400);
	cloud_get(&CLU.info);
	if (CLU.info.st == CLS_ON && !cloud_depth())
		cloud_list("", "OneDrive");
	CLU.rev = (u32)-1;
}

static void cl_refresh(void)
{
	CloudInfo ci;
	cloud_get(&ci);
	if (ci.rev != CLU.rev && CLU.it) {
		CLU.rev = ci.rev;
		CLU.info = ci;
		CLU.n = cloud_items(CLU.it, 400, &CLU.loading);
		if (CLU.l.sel >= CLU.n)
			CLU.l.sel = CLU.n ? CLU.n - 1 : 0;
	}
}

static const char *cl_err_text(const char *e)
{
	if (!strcmp(e, "offline"))
		return _("No se pudo conectar con Microsoft. Comprueba el Wi-Fi.");
	if (!strcmp(e, "expired"))
		return _("El código caducó. Vuelve a pulsar «Conectar».");
	if (!strcmp(e, "declined"))
		return _("Se denegó el permiso desde el móvil.");
	if (!strcmp(e, "revoked"))
		return _("El permiso se retiró desde la cuenta de Microsoft. Vuelve a conectar.");
	if (!strcmp(e, "upload"))
		return _("No se pudo subir el archivo.");
	if (!strcmp(e, "download"))
		return _("No se pudo empezar la descarga.");
	if (!strcmp(e, "delete"))
		return _("No se pudo eliminar.");
	if (!strcmp(e, "backup"))
		return _("La última copia no terminó. Se volverá a intentar.");
	return _("Microsoft no respondió como se esperaba. Inténtalo de nuevo.");
}

static void cl_item_icon(const CloudItem *c, IconId *ic, u32 *col)
{
	const char *dot = strrchr(c->name, '.');
	const char *ext = dot ? dot + 1 : "";
	if (c->dir) {
		const char *aid = cloud_app_folder_id(); // (la de la app, por su id: aunque se le cambie el nombre)
		bool app = aid[0] ? !strcmp(c->id, aid) : !strcmp(c->name, "Cloud3DS");
		*ic = app ? IC_CLOUD : IC_FOLDER, *col = app ? T.accent : T.warn;
	} else if (!strcasecmp(ext, "cia") || !strcasecmp(ext, "3dsx"))
		*ic = IC_GEM, *col = T.ok;
	else if (!strcasecmp(ext, "png") || !strcasecmp(ext, "jpg") || !strcasecmp(ext, "jpeg") || !strcasecmp(ext, "bmp") || !strcasecmp(ext, "gif"))
		*ic = IC_IMAGE, *col = T.accent2;
	else if (!strcasecmp(ext, "zip") || !strcasecmp(ext, "7z") || !strcasecmp(ext, "rar"))
		*ic = IC_ARCHIVE, *col = T.warn;
	else if (!strcasecmp(ext, "mp3") || !strcasecmp(ext, "ogg") || !strcasecmp(ext, "wav"))
		*ic = IC_SPEAKER, *col = T.accent;
	else
		*ic = IC_FILE, *col = T.text2;
}


// arriba, con «Capturas en la nube» abierta: si se suben solas y lo que hay pendiente
static void cl_backups_top(void)
{
	const CloudInfo *ci = &CLU.info;
	float y0 = BODY_Y, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
	const float X = CARD_X + 16, W = CARD_W - 32;
	bool err = !strcmp(ci->err, "backup");
	u32 sc = err ? T.danger : ci->caps ? T.ok : T.text3;
	float y = y0 + 14;
	draw_circle(X + 20, y + 20, 20, col_alpha(sc, 0.16f));
	draw_icon(IC_CAMERA, X + 20, y + 20, 20, sc);
	float tx = X + 52, tw = X + W - tx;
	const char *t1 = err ? _("La última subida no terminó") : ci->caps ? _("Subida automática de capturas activada") : _("Subida automática de capturas desactivada");
	text_draw_clip(t1, tx, y + 3, tw, TS_TITLE, T.text);
	const char *sub = err ? _("Se volverá a intentar. Comprueba el Wi-Fi y el espacio de OneDrive.")
	                : ci->caps ? _("Cada captura nueva que hagas desde el menú de Rosalina se sube a /Cloud3DS/Capturas.")
	                           : _("Actívalo abajo para subir cada captura nueva a /Cloud3DS/Capturas.");
	int nl = text_wrap_lines(sub, tw, TS_SUB, 3, tx, y + 3 + text_lineh(TS_TITLE) + 2, T.text3, true);
	y += fmaxf(44, 3 + text_lineh(TS_TITLE) + 2 + nl * text_lineh(TS_SUB)) + 10;
	draw_rect(X, y, W, 1, T.divider);
	y += 10;
	if (ci->caps_queue > 0) {
		draw_rrect(X, y, 34, 34, 9, col_alpha(T.accent, 0.18f));
		draw_icon(IC_UPLOAD, X + 17, y + 17, 18, T.accent);
		char l[64];
		snprintf(l, sizeof l, ci->caps_queue == 1 ? _("1 captura nueva") : _("%d capturas nuevas"), ci->caps_queue);
		text_draw_clip(l, X + 46, y + 2, W - 46, TS_SUB, T.text);
		text_draw_clip(_("Subiendo a /Cloud3DS/Capturas/"), X + 46, y + 4 + text_lineh(TS_SUB), W - 46, TS_SUB, T.text3);
	} else
		text_wrap_lines(_("No hay elementos pendientes."), W, TS_SUB, 2, X, y, T.text2, true);
	toast_draw_top();
}

static void cl_top(void)
{
	draw_header();
	cl_refresh();
	g_body_y = CARD_Y; // (como las demás aplicaciones: sin la tira del servidor, la pantalla de arriba entera)
	const CloudInfo *ci = &CLU.info;
	if (CLU.bk && ci->st == CLS_ON) {
		cl_backups_top();
		return;
	}
	if (ci->st == CLS_CODE) { // el QR y el código, a lo grande (debajo de la tira)
		float y0 = BODY_Y, h = SCR_H - 8 - y0;
		draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
		draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
		const float QS = h - 32;
		draw_qr(ci->verify, CARD_X + 16, y0 + 16, QS, 1);
		float rx = CARD_X + 16 + QS + 18, rw = CARD_X + CARD_W - 16 - rx, y = y0 + 16;
		text_draw_clip(_("Conecta tu OneDrive"), rx, y, rw, TS_TITLE, T.text);
		y += text_lineh(TS_TITLE) + 4;
		y += text_wrap_lines(_("Escanea el QR con el móvil y entra con tu cuenta de Microsoft. Cuando te lo pida, escribe este código:"), rw, TS_SUB, 4, rx, y, T.text2, true) *
		         text_lineh(TS_SUB) + 8;
		float cw = text_width(ci->user_code, 0.72f) + 20;
		draw_rrect(rx, y, fminf(cw, rw), text_lineh(0.72f) + 10, 10, T.bg);
		text_draw(ci->user_code, rx + 10, y + 5, 0.72f, T.accent);
		y += text_lineh(0.72f) + 16;
		const char *v = strstr(ci->verify, "://");
		char pc[160];
		snprintf(pc, sizeof pc, _("O en el PC: %s"), v ? v + 3 : ci->verify);
		u64 now = util_ms();
		int mins = ci->code_until > now ? (int)((ci->code_until - now) / 60000) + 1 : 0;
		char w[96];
		snprintf(w, sizeof w, _("Pendiente de confirmación en el móvil · caduca en %d min"), mins);
		int nl = text_wrap_lines(w, rw - 16, TS_SUB, 2, 0, 0, 0, false);
		float wy = y0 + h - 10 - nl * text_lineh(TS_SUB);
		// «O en el PC», en lo que quede entre el código y la espera (sin pisarla)
		int pl = (int)((wy - 6 - y) / text_lineh(TS_SUB));
		if (pl > 0)
			text_wrap_lines(pc, rw, TS_SUB, pl < 2 ? pl : 2, rx, y, T.text3, true);
		draw_arc(rx + 6, wy + text_lineh(TS_SUB) / 2, 5, 1.6f, g_time * 6, g_time * 6 + 4.2f, T.accent);
		text_wrap_lines(w, rw - 16, TS_SUB, 2, rx + 16, wy, T.text3, true);
		toast_draw_top();
		return;
	}
	if (ci->st == CLS_ON) {
		// abajo, la cuenta y su espacio (donde en el resto va la tarjeta de la SD)
		float h = SDB_H, y = SCR_H - 8 - h;
		draw_rrect(CARD_X, y + 3, CARD_W, h, 14, T.shadow);
		draw_rrect(CARD_X, y, CARD_W, h, 14, T.surface);
		draw_circle(CARD_X + 26, y + h / 2, 13, T.accent);
		char ini[8] = "?";
		const char *src = ci->name[0] ? ci->name : ci->email;
		if (src[0])
			snprintf(ini, sizeof ini, "%.*s", utf8_len((u8)src[0]), src);
		text_draw_center(ini, CARD_X + 26, y + h / 2 - text_lineh(TS_BODY) / 2, TS_BODY, 0xFFFFFFFF);
		float lx = CARD_X + 48, rx = CARD_X + CARD_W - 14;
		char acc[200], fr[24], tot[24], r1[48];
		snprintf(acc, sizeof acc, "OneDrive · %s", ci->email[0] ? ci->email : ci->name);
		util_fmt_size(fr, sizeof fr, ci->total > ci->used ? ci->total - ci->used : 0);
		util_fmt_size(tot, sizeof tot, ci->total);
		snprintf(r1, sizeof r1, _("%s libres"), fr);
		float w2 = text_width(tot, TS_SUB) + 4;
		if (ci->total) {
			text_draw_right(tot, rx, y + 9, TS_SUB, T.text3);
			text_draw_right(r1, rx - w2 - 4, y + 9, TS_SUB, T.text);
		}
		text_draw_clip(acc, lx, y + 9, rx - lx - (ci->total ? text_width(r1, TS_SUB) + w2 + 14 : 0), TS_SUB, T.text);
		float bw = rx - lx, by = y + h - 15;
		draw_rrect(lx, by, bw, 6, 3, T.surface2);
		if (ci->total)
			draw_rrect(lx, by, fmaxf(6, bw * clampf((float)((double)ci->used / ci->total), 0, 1)), 6, 3, T.accent);
		// lo marcado, en medio
		float y0 = BODY_Y, hh = BODY_B - y0;
		draw_rrect(CARD_X, y0 + 3, CARD_W, hh, 16, T.shadow);
		draw_rrect(CARD_X, y0, CARD_W, hh, 16, T.surface);
		const float X = CARD_X + 14, W = CARD_W - 28;
		const CloudItem *c = CLU.n && CLU.l.sel < CLU.n ? &CLU.it[CLU.l.sel] : NULL;
		if (!c) {
			draw_icon(IC_CLOUD, TOP_W / 2, y0 + hh / 2 - 12, 26, T.text3);
			text_draw_center(CLU.loading ? _("Cargando…") : ci->err[0] ? cl_err_text(ci->err) : _("Carpeta vacía"), TOP_W / 2, y0 + hh / 2 + 10,
			                 TS_SUB, ci->err[0] ? T.danger : T.text3);
		} else {
			IconId ic;
			u32 col;
			cl_item_icon(c, &ic, &col);
			const float PS = hh - 20;
			draw_rrect(X - 4, y0 + 10, PS, PS, 12, T.surface2);
			draw_icon(ic, X - 4 + PS / 2, y0 + 10 + PS / 2, 34, col);
			float rx2 = X - 4 + PS + 14, rw = X + W - rx2, yy = y0 + 11;
			text_draw_clip(c->name, rx2, yy, rw, TS_TITLE, T.text);
			yy += text_lineh(TS_TITLE) + 5;
			char v[96];
			if (c->dir) {
				snprintf(v, sizeof v, c->children == 1 ? _("1 elemento") : _("%d elementos"), c->children);
				yy = info_row(rx2, yy, rw, _("Contenido"), v, T.text2, 1);
			} else {
				util_fmt_size(v, sizeof v, c->size);
				yy = info_row(rx2, yy, rw, _("Tamaño"), v, T.text2, 1);
			}
			if (c->mtime[0]) {
				snprintf(v, sizeof v, "%.10s %.5s", c->mtime, c->mtime + 11);
				yy = info_row(rx2, yy, rw, _("Modificado"), v, T.text2, 1);
			}
			char hint[96];
			if (c->dir)
				snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_A, _("Abrir"), GLYPH_Y, _("Opciones"));
			else
				snprintf(hint, sizeof hint, "%s %s", GLYPH_A, _("Opciones"));
			text_draw_right(hint, X + W, y0 + hh - 10 - text_lineh(TS_SUB), TS_SUB, T.text2);
		}
		toast_draw_top();
		return;
	}
	// sin conectar (o sin configurar)
	if (ci->st == CLS_NOCFG)
		explain_top(1, IC_CLOUD, T.text3, _("Nube"), _("Esta versión de prueba aún no tiene configurada la conexión con Microsoft."), NULL, 0);
	else
		explain_top(1, IC_CLOUD, T.accent, _("Conecta tu OneDrive"),
		            _("Tus archivos de OneDrive en la consola, con subida automática de tus capturas. Se conecta con un QR y un código, sin contraseñas."),
		            ci->err[0] ? cl_err_text(ci->err) : _("Solo es necesario una vez: el permiso se guarda y se renueva automáticamente."), ci->err[0] ? T.danger : T.text3);
	sd_bar(1);
	toast_draw_top();
}

static const char *cl_parent(void) // la carpeta que se ve, para subir, crear o pegar ("root": la raíz)
{
	const char *id = cloud_folder_id();
	return id[0] ? id : "root";
}

static void cl_options(void)
{
	// opciones de un elemento
	const CloudItem *c = CLU.om_item >= 0 && CLU.om_item < CLU.n ? &CLU.it[CLU.om_item] : NULL;
	if (!c && CLU.om)
		CLU.om = false;
	IconId ic[8];
	const char *lab[8];
	u32 col[8];
	int act[8], n = 0;
	if (c && c->dir)
		ic[n] = IC_FOLDER, lab[n] = _("Abrir"), col[n] = T.text, act[n++] = 0;
	ic[n] = IC_DOWNLOAD, lab[n] = _("Descargar a la SD"), col[n] = T.ok, act[n++] = 1;
	ic[n] = IC_DUP, lab[n] = _("Copiar"), col[n] = T.text, act[n++] = 3;
	ic[n] = IC_FOLDER, lab[n] = _("Cortar"), col[n] = T.text, act[n++] = 4;
	ic[n] = IC_PEN, lab[n] = _("Cambiar el nombre"), col[n] = T.text, act[n++] = 5;
	ic[n] = IC_CHECK, lab[n] = _("Seleccionar varios"), col[n] = T.text, act[n++] = 6;
	ic[n] = IC_TRASH, lab[n] = _("Eliminar"), col[n] = T.danger, act[n++] = 2;
	int r = sheet_list(&CLU.om, &CLU.om_just, &CLU.om_anim, &CLU.om_sel, c ? c->name : "", ic, lab, col, n);
	if (r >= 0 && c) {
		if (act[r] == 0)
			cloud_list(c->id, c->name);
		else if (act[r] == 1) {
			if (c->dir)
				cloud_download_to(c, DL_DIR);
			else
				cloud_download(c); // (un .cia, a /cias, y se instala mientras baja)
			toast_icon(c->dir ? _("Descargando la carpeta en Descargas (/Downloads)") : _("Descargando en Descargas (/Downloads)"), IC_DOWNLOAD, T.ok);
		} else if (act[r] == 3 || act[r] == 4) {
			fp_clear(); // un solo portapapeles: lo de Archivos se sustituye
			CC.mode = act[r] == 3 ? CC_COPY : CC_CUT;
			CC.n = 1;
			CC.it[0] = *c;
			toast_icon(act[r] == 3 ? _("Copiado. Pulsa X en otra carpeta de la Nube o en Archivos para pegarlo.") : _("Cortado. Pulsa X en otra carpeta de la Nube para pegarlo."),
			           IC_DUP, T.accent2);
		} else if (act[r] == 5)
			CLU.want_ren = CLU.om_item + 1;
		else if (act[r] == 6) {
			memset(CLU.mark, 0, sizeof CLU.mark);
			CLU.mark[CLU.om_item] = true;
			CLU.ms = true;
			util_strlcpy(CLU.ms_folder, cloud_folder_id(), sizeof CLU.ms_folder);
		}
		else {
			char msg[260];
			snprintf(msg, sizeof msg, _("«%s» irá a la papelera de tu OneDrive. Podrás recuperarlo desde la web de OneDrive."), c->name);
			U.dlg_arg = CLU.om_item;
			dialog_open(DLG_CLDEL, _("¿Eliminar de OneDrive?"), msg, _("Eliminar"), true);
		}
	}
	// opciones de lo seleccionado
	{
		int mn = 0;
		for (int i = 0; i < CLU.n; i++)
			mn += CLU.ms && CLU.mark[i];
		static const IconId mic[4] = {IC_DOWNLOAD, IC_DUP, IC_FOLDER, IC_TRASH};
		const char *mlab[4] = {_("Descargar a la SD"), _("Copiar"), _("Cortar"), _("Eliminar")};
		const u32 mcol[4] = {T.ok, T.text, T.text, T.danger};
		char mt[48];
		snprintf(mt, sizeof mt, mn == 1 ? _("1 seleccionado") : _("%d seleccionados"), mn);
		int mr = sheet_list(&CLU.mo, &CLU.mo_just, &CLU.mo_anim, &CLU.mo_sel, mt, mic, mlab, mcol, 4);
		if (mr >= 0 && mn) {
			if (mr == 0) {
				for (int i = 0; i < CLU.n; i++)
					if (CLU.mark[i])
						cloud_download_to(&CLU.it[i], DL_DIR);
				toast_icon(_("Descargando en Descargas (/Downloads)"), IC_DOWNLOAD, T.ok);
				CLU.ms = false;
			} else if (mr == 1 || mr == 2) {
				fp_clear();
				CC.mode = mr == 1 ? CC_COPY : CC_CUT;
				CC.n = 0;
				for (int i = 0; i < CLU.n && CC.n < (int)(sizeof CC.it / sizeof CC.it[0]); i++)
					if (CLU.mark[i])
						CC.it[CC.n++] = CLU.it[i];
				toast_icon(mr == 1 ? _("Copiado. Pulsa X en otra carpeta de la Nube o en Archivos para pegarlo.") : _("Cortado. Pulsa X en otra carpeta de la Nube para pegarlo."), IC_DUP,
				           T.accent2);
				CLU.ms = false;
			} else {
				char msg[200];
				snprintf(msg, sizeof msg, mn == 1 ? _("1 elemento irá a la papelera de tu OneDrive. Podrás recuperarlo desde la web de OneDrive.")
				                                  : _("%d elementos irán a la papelera de tu OneDrive. Podrás recuperarlos desde la web de OneDrive."),
				         mn);
				dialog_open(DLG_CLDELN, _("¿Eliminar de OneDrive?"), msg, _("Eliminar"), true);
			}
		}
	}
	// opciones de la cuenta
	static const IconId hic[3] = {IC_SHIELD, IC_REFRESH, IC_LOGOUT};
	const char *hlab[3] = {_("Capturas en la nube"), _("Actualizar"), _("Desconectar OneDrive")};
	const u32 hcol[3] = {T.ok, T.text, T.danger};
	int hr = sheet_list(&CLU.hm, &CLU.hm_just, &CLU.hm_anim, &CLU.hm_sel, CLU.info.email[0] ? CLU.info.email : "OneDrive", hic, hlab, hcol, 3);
	if (hr == 0)
		CLU.bk = true, CLU.bk_sel = 0;
	else if (hr == 1)
		cloud_list(cloud_folder_id(), cloud_folder_name());
	else if (hr == 2)
		dialog_open(DLG_CLOUT, _("¿Desconectar OneDrive?"), _("Cloud3DS dejará de tener acceso a tu OneDrive. Tus archivos permanecerán en la nube."), _("Desconectar"), true);
}

// abajo: las capturas en la nube (subir solas, subir ahora) y desconectar
static bool cl_backups_bottom(void)
{
	const CloudInfo *ci = &CLU.info;
	tab_header(_("Capturas en la nube"), true);
	bool back = tap(0, 0, 200, VIEW_Y) || (g_kdown & KEY_B);
	const int N = 3;
	if (g_krepeat & KEY_DOWN)
		CLU.bk_sel = CLU.bk_sel < N - 1 ? CLU.bk_sel + 1 : CLU.bk_sel;
	if (g_krepeat & KEY_UP)
		CLU.bk_sel = CLU.bk_sel > 0 ? CLU.bk_sel - 1 : 0;
	const float RH = 38;
	static const IconId ic[3] = {IC_CAMERA, IC_UPLOAD, IC_LOGOUT};
	const u32 col[3] = {T.accent, T.warn, T.danger};
	for (int i = 0; i < N; i++) {
		float y = VIEW_Y + 2 + i * RH;
		bool p = touch_in(6, y, BOT_W - 12, RH);
		if (CLU.bk_sel == i || p)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, p && CLU.bk_sel != i ? T.surface2 : T.surface);
		draw_circle(27, y + RH / 2, 13, col_alpha(col[i], 0.18f));
		draw_icon(ic[i], 27, y + RH / 2, 14, col[i]);
		float tx = 50, tw = BOT_W - 64;
		const char *t = i == 0 ? _("Subir las capturas nuevas") : i == 1 ? _("Subir ahora") : _("Desconectar OneDrive");
		char sb[200];
		if (i == 0)
			snprintf(sb, sizeof sb, "%s", ci->caps ? _("Al instante, en /Cloud3DS/Capturas") : _("Desactivado"));
		else if (i == 1)
			snprintf(sb, sizeof sb, "%s", _("Sube las capturas pendientes"));
		else
			snprintf(sb, sizeof sb, _("%s · Tus archivos permanecen en la nube"), ci->email[0] ? ci->email : ci->name);
		text_draw_clip(t, tx, y + 3, tw - (i == 0 ? 44 : 0), TS_BODY, i == 2 ? col_lerp(T.danger, T.text, 0.35f) : T.text);
		g_clip_ell = true;
		text_draw_clip(sb, tx, y + 20, tw - (i == 0 ? 44 : 0), TS_SUB, T.text3);
		g_clip_ell = false;
		if (i == 0)
			toggle(BOT_W - 52, y + (RH - 20) / 2, ci->caps ? 1 : 0);
		bool go = tap(6, y, BOT_W - 12, RH);
		if (go)
			CLU.bk_sel = i;
		go = go || (CLU.bk_sel == i && (g_kdown & KEY_A));
		if (!go)
			continue;
		if (i == 0) {
			cloud_set_caps(!ci->caps);
			CLU.info.caps = !ci->caps;
		} else if (i == 1) {
			cloud_backup_now();
			toast_icon(_("Subiendo las capturas pendientes a OneDrive"), IC_UPLOAD, T.ok);
		} else
			dialog_open(DLG_CLOUT, _("¿Desconectar OneDrive?"), _("Cloud3DS dejará de tener acceso a tu OneDrive. Tus archivos permanecerán en la nube."), _("Desconectar"),
			            true);
	}
	if (back)
		CLU.bk = false;
	return back;
}

static void cl_bottom(void)
{
	cl_refresh();
	const CloudInfo *ci = &CLU.info;
	bool sheet = CLU.om || CLU.hm || CLU.mo || g_dialog.open;
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	if (sheet) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	bool close = false;
	if (ci->st != CLS_ON)
		CLU.bk = false;
	if (ci->st == CLS_ON && CLU.bk)
		cl_backups_bottom();
	else if (ci->st == CLS_ON) {
		const char *fname = cloud_folder_name();
		bool sub = cloud_depth() > 1;
		if (CLU.ms && strcmp(CLU.ms_folder, cloud_folder_id()))
			CLU.ms = false; // (otra carpeta: la selección era de la anterior)
		int mn = 0;
		for (int i = 0; i < CLU.n && CLU.ms; i++)
			mn += CLU.mark[i];
		char mt[48];
		snprintf(mt, sizeof mt, mn == 1 ? _("1 seleccionado") : _("%d seleccionados"), mn);
		tab_header_w(CLU.ms ? mt : sub ? fname : "OneDrive", true, 170);
		// a la derecha: carpeta nueva y opciones de la cuenta
		float hx = BOT_W - 24, hy = 16, px = hx - 32;
		if (CLU.ms) { // seleccionando: las opciones de lo elegido (también con Y); B deja de seleccionar
			draw_circle(hx, hy, 12, touch_in(hx - 14, hy - 14, 28, 28) ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
			draw_icon(IC_DOTS, hx, hy, 14, 0xFFFFFFFF);
			if ((hdr_hit(hx - 13, hy - 13, 26, 26) || (g_kdown & KEY_Y)) && mn)
				CLU.mo = CLU.mo_just = true, CLU.mo_sel = 0;
			else if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
				CLU.ms = false;
			g_kdown &= ~(KEY_B | KEY_Y | KEY_X);
		}
		bool clip = !CLU.ms && (CC.mode || fp_count());
		if (!CLU.ms) {
			draw_circle(hx, hy, 12, touch_in(hx - 14, hy - 14, 28, 28) ? T.surface2 : T.surface);
			draw_icon(IC_DOTS, hx, hy, 14, T.text2);
			draw_circle(px, hy, 12, touch_in(px - 14, hy - 14, 28, 28) ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
			draw_icon(IC_PLUS, px, hy, 13, 0xFFFFFFFF);
			bool h_new = hdr_hit(px - 13, hy - 13, 26, 26), h_acc = hdr_hit(hx - 13, hy - 13, 26, 26);
			if (h_acc)
				CLU.hm = CLU.hm_just = true, CLU.hm_sel = 0;
			else if (h_new || (!clip && (g_kdown & KEY_X)))
				CLU.want_mkdir = true;
			else if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B)) {
				if (sub)
					cloud_up();
				else
					close = true;
			}
		}
		const float RH = 40;
		g_list_bar = clip ? 42 : 0;
		list_update(&CLU.l, CLU.n, RH);
		g_list_bar = 0;
		gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
		if (!CLU.n) {
			if (CLU.loading) {
				draw_arc(BOT_W / 2, VIEW_Y + 70, 10, 2.5f, g_time * 6, g_time * 6 + 4.2f, T.accent);
				text_draw_center(_("Cargando…"), BOT_W / 2, VIEW_Y + 92, TS_SUB, T.text3);
			} else
				empty_state(IC_CLOUD, ci->err[0] ? _("No se pudo cargar") : _("Carpeta vacía"),
				            ci->err[0] ? cl_err_text(ci->err) : _("Copia un elemento en Archivos y pégalo aquí con X, o crea una carpeta con «+»."));
		}
		for (int i = 0; i < CLU.n; i++) {
			float y = VIEW_Y + i * RH - CLU.l.off;
			if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
				continue;
			const CloudItem *c = &CLU.it[i];
			bool was = CLU.l.sel == i;
			bool t = row(&CLU.l, i, y, RH);
			if (!U.buttons && CLU.l.sel == i)
				draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
			IconId ic;
			u32 col;
			cl_item_icon(c, &ic, &col);
			draw_rrect(14, y + (RH - 26) / 2, 26, 26, 8, col_alpha(col, 0.16f));
			draw_icon(ic, 27, y + RH / 2, 14, col);
			g_clip_ell = CLU.l.sel != i;
			text_draw_clip(c->name, 50, y + 5, BOT_W - 90, TS_BODY, T.text);
			char sb[96];
			if (c->dir)
				snprintf(sb, sizeof sb, c->children == 1 ? _("Carpeta · 1 elemento") : _("Carpeta · %d elementos"), c->children);
			else {
				char sz[24];
				util_fmt_size(sz, sizeof sz, c->size);
				snprintf(sb, sizeof sb, "%s · %.10s", sz, c->mtime);
			}
			text_draw_clip(sb, 50, y + 22, BOT_W - 90, TS_SUB, T.text3);
			g_clip_ell = false;
			if (CLU.ms) { // seleccionando: su casilla; tocar o A la marca o la desmarca
				bool on = CLU.mark[i];
				draw_circle(BOT_W - 26, y + RH / 2, 10, on ? T.accent : T.surface2);
				if (on)
					draw_icon(IC_CHECK, BOT_W - 26, y + RH / 2, 12, 0xFFFFFFFF);
				if (t || activate(&CLU.l, i))
					CLU.mark[i] = !on;
				continue;
			}
			if (CLU.l.sel == i) { // ⋮ en la marcada
				draw_circle(BOT_W - 26, y + RH / 2, 11, col_alpha(T.accent, 0.3f));
				draw_icon(IC_DOTS, BOT_W - 26, y + RH / 2, 14, T.accent);
			}
			bool open = (t && was) || activate(&CLU.l, i);
			if (t && was && g_touch.px > BOT_W - 44) // el ⋮
				CLU.om = CLU.om_just = true, CLU.om_sel = 0, CLU.om_item = i;
			else if (open)
				cl_open_item(i);
		}
		gfx_clip_off();
		if ((g_kdown & KEY_Y) && CLU.n && !CLU.om && !CLU.ms)
			CLU.om = CLU.om_just = true, CLU.om_sel = 0, CLU.om_item = CLU.l.sel;
		// lo copiado (en la nube o en Archivos): «Pegar aquí», como en Archivos
		if (clip) {
			int cn = CC.mode ? CC.n : fp_count();
			float y = SCR_H - 40, w = BOT_W - 20;
			draw_rrect(10, y + 2, w, 30, 15, T.shadow);
			draw_rrect(10, y, w, 30, 15, T.accent);
			char t[96];
			const char *what = CC.mode == CC_CUT ? _("Mover aquí") : CC.mode ? _("Pegar aquí") : _("Subir aquí");
			if (cn > 1)
				snprintf(t, sizeof t, "%s %s (%d)", GLYPH_X, what, cn);
			else
				snprintf(t, sizeof t, "%s %s", GLYPH_X, what);
			text_draw_center(t, BOT_W / 2 - 30, y + (30 - text_lineh(0.46f)) / 2, 0.46f, 0xFFFFFFFF);
			draw_rect(BOT_W - 70, y + 6, 1, 18, col_alpha(0xFFFFFFFF, 0.4f));
			text_draw_center(_("Vaciar"), BOT_W - 42, y + (30 - text_lineh(TS_SUB)) / 2, TS_SUB, 0xFFFFFFFF);
			if (tap(BOT_W - 70, y, 60, 30)) {
				CC.mode = CC.n = 0;
				fp_clear();
			} else if ((g_kdown & KEY_X) || tap(10, y, w - 60, 30)) {
				const char *par = cl_parent();
				if (CC.mode) {
					for (int k = 0; k < CC.n; k++)
						CC.mode == CC_CUT ? cloud_move(&CC.it[k], par) : cloud_copy(&CC.it[k], par);
					toast_icon(CC.mode == CC_CUT ? _("Moviendo a esta carpeta") : _("Copiando en esta carpeta. OneDrive puede tardar unos segundos."), IC_DUP, T.accent);
					if (CC.mode == CC_CUT) // lo cortado ya no está donde estaba; lo copiado se puede volver a pegar
						CC.mode = CC.n = 0;
				} else {
					int k = fp_upload(par);
					char m[96];
					snprintf(m, sizeof m, k == 1 ? _("Subiendo a OneDrive. Puedes seguir el progreso en Conectar > Actividad.") : _("Subiendo %d elementos a OneDrive"), k);
					toast_icon(m, IC_UPLOAD, T.ok);
				}
			}
		}
	} else if (ci->st == CLS_CODE) {
		tab_header(_("Nube"), true);
		float x = 10, w = BOT_W - 20, y = VIEW_Y + 4;
		draw_rrect(x, y, w, 96, 14, T.surface);
		const char *st[3] = {_("Escanea el QR de arriba con el móvil"), _("Inicia sesión con tu cuenta de Microsoft (Outlook, Hotmail, Xbox…)"), NULL};
		char s3[96];
		snprintf(s3, sizeof s3, _("Escribe el código %s y pulsa «Permitir»"), ci->user_code);
		st[2] = s3;
		for (int i = 0; i < 3; i++) {
			float sy = y + 10 + i * 28;
			draw_circle(x + 20, sy + 8, 8, col_lerp(T.surface2, T.accent, 0.25f));
			char n[4];
			snprintf(n, sizeof n, "%d", i + 1);
			text_draw_center(n, x + 20, sy + 8 - text_lineh(TS_SUB) / 2, TS_SUB, T.accent);
			text_wrap_lines(st[i], w - 46, TS_SUB, 2, x + 36, sy, T.text, true);
		}

		char cl[48];
		snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cancelar"));
		float by = y + 104;
		draw_rrect(x, by, w, 30, 15, touch_in(x, by, w, 30) ? T.divider : T.surface2);
		text_draw_center(cl, BOT_W / 2, by + 15 - text_lineh(TS_BODY) / 2, TS_BODY, T.text);
		if (tap(x, by, w, 30) || tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
			cloud_cancel();
	} else {
		tab_header(_("Nube"), true);
		bool cfg = ci->st != CLS_NOCFG;
		// de arriba abajo sin pisarse: la nube, el título, el texto (dos líneas) y el botón, que acaba
		// por encima del botón redondo de la barra (sobresale QR_PAD)
		float cy = VIEW_Y + 30;
		draw_halo(BOT_W / 2, cy, 44, 0.3f);
		draw_cloud(BOT_W / 2, cy, 20, 1);
		text_draw_center(cfg ? _("Conecta tu OneDrive") : _("Nube sin configurar"), BOT_W / 2, cy + 26, TS_TITLE, T.text);
		wrap_center(cfg ? _("Conéctala con tu cuenta de Microsoft desde el móvil, mediante un código QR y un código de verificación.") : _("Falta el ID de la app de Microsoft en esta versión de prueba."),
		            BOT_W / 2, cy + 48, 270, TS_SUB, 2, T.text3);
		float bx = 40, bw = BOT_W - 80, by = SCR_H - TABBAR_H - QR_PAD - 6 - 32;
		if (cfg) {
			bool busy = ci->busy;
			draw_rrect(bx, by, bw, 32, 16, busy ? T.surface2 : touch_in(bx, by, bw, 32) ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
			char b[64];
			snprintf(b, sizeof b, "%s %s", GLYPH_A, busy ? _("Solicitando el código…") : _("Conectar"));
			text_draw_center(b, BOT_W / 2, by + 16 - text_lineh(TS_BODY) / 2, TS_BODY, busy ? T.text3 : 0xFFFFFFFF);
			if (!busy && (tap(bx, by, bw, 32) || (g_kdown & KEY_A)))
				cloud_connect();
		}
		if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
			close = true;
	}
	if (sheet)
		g_kdown = kd, g_krepeat = kr, g_touch = tc;
	cl_options();
	if (close)
		CLU.open = false;
	g_kdown = 0;
	g_touch.up = false;
}

static void qrs_open(void);

// Conectar: el inicio y sus secciones (Actividad · Dispositivos), también con L y R
static int segmented(float x, float y, float w, float h, const char *const *opts, int n, int cur, bool focus);
static void conn_header(void)
{
	tab_header_w("", false, 0);
	const char *opts[2] = {_("Actividad"), _("Dispositivos")};
	int r = segmented(40, 5, 150, 22, opts, 2, U.tab == TAB_DEVICES, false);
	if (hdr_hit(40, 5, 75, 22))
		r = 0;
	if (hdr_hit(115, 5, 75, 22))
		r = 1;
	if (r >= 0 && !g_dialog.open)
		U.tab = r ? TAB_DEVICES : TAB_ACTIVITY;
}
static void tab_activity(void)
{
	if ((g_kdown & KEY_B) && !g_dialog.open) { // en la raíz de la aplicación: al inicio
		hm_show();
		return;
	}
	List *l = cur_list();
	if (U.app == AP_CONNECT)
		conn_header();
	else
		tab_header(_("Actividad"), false);
	float hx = BOT_W - 14;
	if (V.hist_n) {
		// borrar el historial: siempre a mano, arriba a la derecha (también con Y)
		float cx = BOT_W - 24, cy = 16;
		bool p = touch_in(cx - 14, cy - 14, 28, 28);
		draw_circle(cx, cy, 12, p ? col_alpha(T.danger, 0.35f) : T.surface);
		draw_icon(IC_TRASH, cx, cy, 14, T.danger);
		if (!g_dialog.open && (hdr_hit(cx - 13, cy - 13, 26, 26) || (g_kdown & KEY_Y)))
			dialog_open(DLG_CLEAR, _("¿Borrar el historial?"), _("Se borrará la lista de actividad. Tus archivos no se modifican."),
			            _("Borrar"), true);
		hx = cx - 20;
	}
	if ((V.stat_up || V.stat_down) && U.app != AP_CONNECT) { // (en Conectar, ahí van sus secciones)
		char a[24], b[24];
		util_fmt_size(a, sizeof a, V.stat_up);
		util_fmt_size(b, sizeof b, V.stat_down);
		float x = hx - text_width(b, TS_SUB);
		// enviado (↑) a la derecha y recibido (↓) a su izquierda, con los colores del historial
		text_draw(b, x, 11, TS_SUB, T.text3);
		draw_icon(IC_UPLOAD, x - 9, 16, 12, T.accent);
		x -= 26 + text_width(a, TS_SUB);
		text_draw(a, x, 11, TS_SUB, T.text3);
		draw_icon(IC_DOWNLOAD, x - 9, 16, 12, T.ok);
	}
	int n = V.hist_n;
	const float RH = 40;
	ds_refresh();
	int nd = DS.n; // las descargas, arriba (la más reciente primero)
	enum { NA = 0 }; // descargar y escanear van en el botón central
	int rows = NA + n + nd;
	list_update(l, rows, RH);
	if (!rows) {
		empty_state(IC_CLOUD, _("Sin actividad reciente"), _("Aquí aparecerá lo que envíes, recibas, descargues o instales. Para conectar el móvil o el PC, escanea el código QR de la pantalla superior."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int r = 0; r < rows; r++) {
		float y = VIEW_Y + r * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		g_clip_ell = l->sel != r; // solo la marcada se desplaza si no cabe
		int i = r - NA;
		if (i < nd) {
			const DlItem *d = &DS.it[nd - 1 - i];
			bool t = row(l, r, y, RH);
			if (t || activate(l, r)) {
				DS.id = d->id;
				DS.open = DS.just = true, DS.sel = 0;
			}
			u32 sc;
			char st[160];
			dl_status(d, st, sizeof st, &sc);
			row_icon(28, y + RH / 2, IC_GLOBE, d->st == DL_FAIL ? T.danger : d->st == DL_DONE ? T.ok : T.accent);
			text_draw_clip(d->name[0] ? d->name : d->host, 50, y + 5, BOT_W - 64, TS_BODY, T.text);
			text_draw_clip(st, 50, y + 22, BOT_W - 64, TS_SUB, sc);
			if ((d->st == DL_RUN || d->st == DL_PAUSED) && d->total) {
				float w = BOT_W - 64 - 50;
				draw_rrect(50, y + RH - 5, w, 3, 1.5f, T.surface2);
				draw_rrect(50, y + RH - 5, fmaxf(3, w * (float)((double)d->done / d->total)), 3, 1.5f, d->st == DL_RUN ? T.accent : T.text3);
			}
			continue;
		}
		row(l, r, y, RH);
		const Event *e = ev(i - nd);
		char title[128], sub[140], when[24];
		IconId ic = IC_INFO;
		u32 col = T.accent;
		ev_info(e, title, sizeof title, sub, sizeof sub, &ic, &col);
		util_fmt_when(when, sizeof when, e->when);
		row_icon(28, y + RH / 2, ic, col);
		float ww = text_width(when, TS_SUB);
		text_draw_clip(title, 50, y + 5, BOT_W - 64 - ww - 8, TS_BODY, T.text);
		text_draw_right(when, BOT_W - 14, y + 7, TS_SUB, T.text3);
		text_draw_clip(sub, 50, y + 22, BOT_W - 64, TS_SUB, T.text3);
	}
	gfx_clip_off();
	g_clip_ell = false;
}

static void ask_revoke(int d)
{
	char msg[200];
	snprintf(msg, sizeof msg, _("%s tendrá que volver a escanear el QR o escribir el PIN."), V.dev[d].name);
	U.dlg_arg = d;
	dialog_open(DLG_REVOKE, _("¿Desvincular este dispositivo?"), msg, _("Desvincular"), true);
}

static int sheet_list(bool *open, bool *just, float *anim, int *sel, const char *title, const IconId *ic, const char *const *lab,
                      const u32 *col, int n);

// opciones de un dispositivo: renombrarlo o desvincularlo
static struct {
	bool open, just;
	float anim;
	int sel, dev;
} DVS;

static void dvs_draw(void)
{
	const Device *d = DVS.dev >= 0 && DVS.dev < MAX_DEVICES && V.dev[DVS.dev].used ? &V.dev[DVS.dev] : NULL;
	if (!d && DVS.open)
		DVS.open = false;
	static const IconId ic[2] = {IC_PEN, IC_LOGOUT};
	const char *lab[2] = {_("Cambiar el nombre"), _("Desvincular")};
	const u32 col[2] = {T.text, T.danger};
	int r = sheet_list(&DVS.open, &DVS.just, &DVS.anim, &DVS.sel, d ? d->name : "", ic, lab, col, 2);
	if (r == 0)
		U.want_rename = DVS.dev;
	else if (r == 1)
		ask_revoke(DVS.dev);
}

static void tab_devices(void)
{
	if ((g_kdown & KEY_B) && !g_dialog.open) { // en la raíz de la aplicación: al inicio
		hm_show();
		return;
	}
	List *l = cur_list();
	int idx[MAX_DEVICES];
	int n = device_order(idx);
	if (U.app == AP_CONNECT)
		conn_header();
	else
		tab_header(_("Dispositivos"), false);
	if (n) {
		char r[48];
		int on = online_count();
		snprintf(r, sizeof r, on == 1 ? _("%d conectado") : _("%d conectados"), on);
		text_draw_right(r, BOT_W - 14, 11, TS_SUB, on ? T.ok : T.text3);
	}
	const float RH = 44;
	list_update(l, n, RH);
	if (!n) {
		empty_state(IC_DEVICES, _("Sin dispositivos"), _("Escanea el código QR de la pantalla superior para vincular tu móvil."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < n; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		const Device *d = &V.dev[idx[i]];
		bool on = dev_online(d);
		g_clip_ell = !(l->sel == i || !strcmp(U.dev_tok, d->token)); // solo la marcada se desplaza si no cabe
		// el marcado lleva ⋮ (sus opciones); tocar otro lo marca y tocar el marcado abre sus opciones
		bool selected = !strcmp(U.dev_tok, d->token);
		float mx = BOT_W - 30;
		bool vis = g_touch.sy >= VIEW_Y && !g_dialog.open;
		bool mt = selected && vis && tap(mx - 4, y, 34, RH);
		bool rt = !mt && row(l, i, y, RH);
		if (!U.buttons && selected)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		draw_circle(28, y + RH / 2, 14, on ? col_alpha(T.ok, 0.16f) : T.surface2);
		draw_icon(dev_icon(d), 28, y + RH / 2, 16, on ? T.ok : T.text3);
		if (on) {
			draw_circle(39, y + RH / 2 + 10, 4.5f, T.bg);
			draw_circle(39, y + RH / 2 + 10, 3, T.ok);
		}
		float tw = (selected ? mx - 14 : BOT_W - 14) - 52;
		text_draw_clip(d->name, 52, y + 7, tw, TS_BODY, T.text);
		char sub[128], when[24];
		if (on)
			util_strlcpy(sub, _("Conectado"), sizeof sub);
		else {
			util_fmt_when(when, sizeof when, d->last_seen);
			snprintf(sub, sizeof sub, _("Última conexión: %s"), when);
		}
		text_draw_clip(sub, 52, y + 24, tw, TS_SUB, on ? T.ok : T.text3);
		if (selected) {
			draw_circle(mx + 12, y + RH / 2, 11, touch_in(mx - 4, y, 34, RH) ? T.surface2 : col_alpha(T.accent, 0.3f));
			draw_icon(IC_DOTS, mx + 12, y + RH / 2, 14, T.accent);
		}
		bool mine = U.buttons && !g_dialog.open && l->sel == i;
		if (mt || (rt && selected) || (mine && (g_kdown & KEY_Y))) {
			DVS.dev = idx[i];
			DVS.open = DVS.just = true, DVS.sel = 0;
		} else if (mine && (g_kdown & KEY_X))
			ask_revoke(idx[i]);
		if (rt || mine)
			util_strlcpy(U.dev_tok, d->token, sizeof U.dev_tok);
	}
	gfx_clip_off();
	g_clip_ell = false;
}

// ------------------------------------------------------------ explorador (pestaña Archivos)
#define F_MAX 2000
typedef struct {
	char name[128];
	bool dir, sys;
	u8 color;
	u8 grp; // grupo al agrupar (por tipo, fecha o tamaño)
	u32 tid; // en la papelera: su número
	u16 sdir; // en la búsqueda: su carpeta (SR.dirs)
	u64 size, mtime;
} FEnt;
enum { FV_CHIPS, FV_ITEM, FV_SYSHDR, FV_GROUP, FV_TRASH, FV_FILTER };
typedef struct {
	u8 kind;
	short idx;   // elemento de F.e; en las cabeceras, el grupo o cuántos hay
	float y, h;
	float x, w;  // en la vista de iconos, cada casilla de la fila
	short n;     // cabecera de grupo: cuántos tiene
} FView;
enum { CH_HOME, CH_SHOTS, CH_PHOTOS, CH_CIAS, CH_HB, CH_SAVES, CH_TITLES, CH_COUNT };
#define CH_SHOWN CH_SAVES /* (fuera de los accesos rápidos: las partidas se gestionan con Checkpoint y los títulos son su propia aplicación) */
static const char *const CHIP_PATH[CH_COUNT] = {"/", "/luma/screenshots", "/DCIM", "/cias", "/3ds", "@saves", "@titles"};
static const char *const SAVE_DIRS[3] = {"/3ds/Checkpoint/saves", "/JKSV/Saves", "/JKSV"};

static struct {
	char path[512];
	FEnt *e;
	int n;
	bool err, truncated;
	FView v[F_MAX + 24];
	u32 gclosed;       // grupos plegados (se despliegan al cambiar de carpeta)
	bool chips_on;     // accesos rápidos a la vista fuera de la raíz (se sacan tirando hacia abajo)
	bool trash;        // se está viendo la papelera (F.path = "@trash")
	bool search;       // se ven los resultados de una búsqueda (F.path = "@search")
	bool titles;       // los títulos instalados (F.path = "@titles"); FEnt.tid es su posición en TI
	char ret_from[512], ret_to[512]; // una categoría abierta (ret_from) y desde dónde: atrás vuelve ahí
	float pull;        // cuánto se está tirando hacia abajo estando arriba del todo
	int nv;
	float off, target, total;
	int sel, chip;
	bool sys_open;
	bool missing;      // la carpeta no existe (acceso rápido): se ofrece crearla
	bool nosaves;      // «Partidas» sin copias de Checkpoint ni JKSM
	bool need_reload;  // algo cambió en la SD (web, copias…): se vuelve a leer
	int info_for;      // de qué elemento es la información de arriba
	InsReq *cia;       // datos del CIA elegido (si lo es)
	bool cia_ok;
	Texture cia_icon;
	Texture thumb;     // foto elegida, en pequeño
	int iw, ih;        // su tamaño original
	char app_title[64], app_pub[64]; // de un .3dsx (su SMDH)
	int dir_count;     // elementos de la carpeta elegida
	bool dir_more;     // (se dejó de contar: hay más)
	int mt_next;       // fechas: se leen poco a poco tras abrir la carpeta (pedirlas todas a la vez la trababa)
	bool mt_pending;
	char preview[600]; // principio de un texto, para verlo arriba
} F = {.path = "/", .sys_open = true, .info_for = -1, .sel = 1};

// selección múltiple: los nombres elegidos en la carpeta abierta (se olvidan al cambiar de carpeta)
static struct {
	bool on;
	int n;
	char (*nm)[128];
} MS;

// títulos instalados: los de la última lectura
static TitleInfo *TI;
static int TI_n;
static bool f_is_titles(void) { return F.titles; }

// Títulos: en vez de la barra de la SD, en qué se va su espacio (juegos, actualizaciones, DLC, otros archivos y libre)
static void titles_space_bar(void)
{
	u64 g = 0, up = 0, dl = 0, fr = 0, tot = 0;
	for (int i = 0; i < TI_n; i++)
		g += TI[i].size, up += TI[i].upd_size, dl += TI[i].dlc_size;
	sd_space(&fr, &tot);
	float x = CARD_X, y = SCR_H - 8 - SDB_H, w = CARD_W;
	draw_rrect(x, y + 3, w, SDB_H, 14, T.shadow);
	draw_rrect(x, y, w, SDB_H, 14, T.surface);
	if (!tot)
		return;
	const u64 v[4] = {g, up, dl, tot > fr + g + up + dl ? tot - fr - g - up - dl : 0};
	const u32 c[4] = {T.warn, T.accent, T.accent2, T.text3};
	const char *n[4] = {_("Juegos"), _("Actualizaciones"), _("DLC"), _("Otros")};
	float bx = x + 14, bw = w - 28, by = y + 9, acc = 0;
	draw_rrect(bx, by, bw, 8, 4, T.surface2);
	for (int k = 0; k < 4; k++) {
		float pw = bw * (float)((double)v[k] / tot);
		if (pw >= 1)
			draw_rect(bx + acc, by, pw, 8, c[k]);
		acc += pw;
	}
	// la leyenda: si no cabe en una línea (otros idiomas), letra algo más pequeña y menos hueco
	char lt[4][64], z[24], ft[48];
	for (int k = 0; k < 4; k++)
		util_fmt_size(z, sizeof z, v[k]), snprintf(lt[k], sizeof lt[k], "%s %s", n[k], z);
	util_fmt_size(z, sizeof z, fr);
	snprintf(ft, sizeof ft, _("%s libres"), z);
	float sc = 0.34f, gap = 18;
	for (int tries = 0; tries < 6; tries++) {
		float need = text_width(ft, sc) + 8;
		for (int k = 0; k < 4; k++)
			need += 9 + text_width(lt[k], sc) + gap;
		if (need <= bw)
			break;
		sc -= 0.01f, gap = fmaxf(8, gap - 3);
	}
	float lx = bx, ly = by + 14;
	for (int k = 0; k < 4; k++) {
		draw_circle(lx + 3, ly + 6, 3, c[k]);
		text_draw(lt[k], lx + 9, ly, sc, T.text2);
		lx += 9 + text_width(lt[k], sc) + gap;
	}
	text_draw_right(ft, bx + bw, ly, sc, T.ok);
}


// comprimir en ZIP: lo elegido espera aquí a que se escriba el nombre (teclado, fuera del dibujo)
static struct {
	bool want;
	char base[512];
	char (*names)[256];
	int n;
} ZQ;

static void zq_set(const char *base, char (*names)[128], int n)
{
	free(ZQ.names);
	ZQ.names = n > 0 ? malloc(n * sizeof *ZQ.names) : NULL;
	if (!ZQ.names)
		return;
	for (int i = 0; i < n; i++)
		util_strlcpy(ZQ.names[i], names[i], sizeof ZQ.names[0]);
	ZQ.n = n;
	util_strlcpy(ZQ.base, base, sizeof ZQ.base);
	ZQ.want = true;
}

static const char *zip_err_text(int r)
{
	switch (r) {
	case ZIP_BAD: return _("El archivo no es un ZIP válido o está dañado");
	case ZIP_UNSUP: return _("Este ZIP está protegido con contraseña o usa un formato no compatible");
	case ZIP_NO_SPACE: return _("No hay espacio suficiente en la SD");
	case ZIP_CANCELED: return _("Operación cancelada");
	case ZIP_CRC: return _("Un archivo del ZIP está dañado");
	case ZIP_TOO_BIG: return _("Supera los 4 GB, el tamaño máximo de un ZIP");
	case ZIP_BUSY: return _("Ya hay una compresión o extracción en curso");
	case ZIP_EMPTY: return _("No hay nada que comprimir");
	default: return _("No se pudo leer o escribir en la SD");
	}
}

// búsqueda: un hilo recorre la carpeta y sus subcarpetas y deja aquí lo que encuentra
#define SR_MAX 500
#define SR_DIRS 400
#define SR_KINDS 12 // los tipos de archivo (FK_*) y, el último, las carpetas
static struct {
	char q[128];
	char scope[512];      // dónde se buscó (B vuelve ahí)
	char back[512];       // carpeta abierta desde un resultado: B en ella vuelve a los resultados
	char (*dirs)[384];    // las carpetas de los resultados
	int ndirs;
	FEnt *r;
	int n, shown;
	int cnt[SR_KINDS];
	int kind;             // filtro: -1 todo
	volatile bool cancel;
	bool running, done;
	u64 t0;
	int sel_keep;
	LightLock lock;
	Thread thr;
} SR = {.kind = -1};
static int sr_nkinds(void);

// acciones sobre archivos (menú de un elemento y selección múltiple)
enum { FA_INSTALL, FA_RENAME, FA_MOVE, FA_COPY, FA_DELETE, FA_COLORS, FA_ASTEXT, FA_TYPE, FA_SELECT, FA_HASH, FA_ZIP, FA_CLOUD, FA_READ };

static const char *chip_name(int i)
{
	switch (i) {
	case CH_HOME: return _("SD"); // (la raíz de la tarjeta; «inicio» es el menú de aplicaciones)
	case CH_SHOTS: return _("Capturas");
	case CH_PHOTOS: return _("Fotos");
	case CH_CIAS: return _("CIAs");
	case CH_HB: return _("Homebrew");
	case CH_TITLES: return _("Instalados");
	default: return _("Partidas");
	}
}

static IconId chip_icon(int i)
{
	static const IconId IC[CH_COUNT] = {IC_CLOUD, IC_CAMERA, IC_IMAGE, IC_GEM, IC_CODE, IC_STAR, IC_GAMEPAD};
	return IC[i];
}

static void f_full(int i, char *out, size_t n)
{
	if (F.titles) { // un título no es un archivo: su «ruta» es su ID (las miniaturas la usan para el icono)
		snprintf(out, n, "@t%016llX", F.e[i].tid < (u32)TI_n ? (unsigned long long)TI[F.e[i].tid].tid : 0ull);
		return;
	}
	const char *dir = F.search ? SR.dirs[F.e[i].sdir] : F.path;
	snprintf(out, n, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", F.e[i].name);
}

static const char *f_ext(const char *n)
{
	const char *d = strrchr(n, '.');
	return d ? d + 1 : "";
}

// tipos de archivo por extensión: icono, color y nombre (los mismos grupos que la web)
enum { FK_OTHER, FK_IMG, FK_VIDEO, FK_AUDIO, FK_APP, FK_ROM, FK_SAVE, FK_TEXT, FK_ARCHIVE, FK_FONT, FK_DOC };
typedef struct {
	const char *x;
	u8 k;
	const char *label; // NULL: el genérico del grupo con la extensión («Imagen PNG»)
	const char *name;  // si label lleva %s: el nombre del lenguaje («Código Python»)
} FType;
#define CODE(x, n) {x, FK_TEXT, N_("Código %s"), n}
static const FType FTYPES[] = {
	{"png", FK_IMG}, {"jpg", FK_IMG}, {"jpeg", FK_IMG}, {"bmp", FK_IMG}, {"gif", FK_IMG}, {"tga", FK_IMG},
	{"mpo", FK_IMG, N_("Foto 3D (MPO)")},
	{"mp4", FK_VIDEO}, {"mov", FK_VIDEO}, {"avi", FK_VIDEO}, {"mkv", FK_VIDEO}, {"webm", FK_VIDEO}, {"3gp", FK_VIDEO},
	{"moflex", FK_VIDEO, N_("Vídeo de 3DS (MOFLEX)")},
	{"mp3", FK_AUDIO}, {"wav", FK_AUDIO}, {"ogg", FK_AUDIO}, {"m4a", FK_AUDIO}, {"flac", FK_AUDIO}, {"aac", FK_AUDIO},
	{"opus", FK_AUDIO}, {"mid", FK_AUDIO}, {"midi", FK_AUDIO},
	{"bcstm", FK_AUDIO, N_("Sonido de 3DS (BCSTM)")}, {"bcwav", FK_AUDIO, N_("Sonido de 3DS (BCWAV)")},
	{"cia", FK_APP, N_("Programa instalable (CIA)")}, {"3dsx", FK_APP, N_("Homebrew (3DSX)")},
	{"3ds", FK_APP, N_("Volcado de cartucho de 3DS")}, {"cci", FK_APP, N_("Volcado de cartucho de 3DS")},
	{"cxi", FK_APP, N_("Programa de 3DS (CXI)")}, {"firm", FK_APP, N_("Firmware (FIRM)")},
	{"tik", FK_APP, N_("Ticket (TIK)")}, {"smdh", FK_APP, N_("Icono de programa (SMDH)")},
	{"nds", FK_ROM, N_("Juego de Nintendo DS")}, {"gba", FK_ROM, N_("Juego de Game Boy Advance")},
	{"gb", FK_ROM, N_("Juego de Game Boy")}, {"gbc", FK_ROM, N_("Juego de Game Boy Color")},
	{"nes", FK_ROM, N_("Juego de NES")}, {"sfc", FK_ROM, N_("Juego de Super Nintendo")}, {"smc", FK_ROM, N_("Juego de Super Nintendo")},
	{"md", FK_TEXT, N_("Texto con formato (Markdown)")}, {"gen", FK_ROM, N_("Juego de Mega Drive")},
	{"sms", FK_ROM, N_("Juego de Master System")}, {"gg", FK_ROM, N_("Juego de Game Gear")},
	{"sav", FK_SAVE, N_("Partida guardada (SAV)")}, {"srm", FK_SAVE, N_("Partida guardada (SRM)")},
	{"txt", FK_TEXT}, {"ini", FK_TEXT, N_("Configuración (INI)")}, {"cfg", FK_TEXT, N_("Configuración (CFG)")},
	{"conf", FK_TEXT, N_("Configuración (CONF)")}, {"json", FK_TEXT}, {"log", FK_TEXT, N_("Registro (LOG)")},
	{"xml", FK_TEXT}, {"csv", FK_TEXT}, {"yml", FK_TEXT}, {"yaml", FK_TEXT}, {"toml", FK_TEXT}, {"nfo", FK_TEXT},
	{"srt", FK_TEXT, N_("Subtítulos (SRT)")}, {"gm9", FK_TEXT, N_("Script de GodMode9")}, {"lua", FK_TEXT, N_("Script de Lua")},
	CODE("py", "Python"), CODE("sh", "Shell"), CODE("c", "C"), CODE("h", "C"), CODE("cpp", "C++"), CODE("hpp", "C++"),
	CODE("cc", "C++"), CODE("java", "Java"), CODE("js", "JavaScript"), CODE("ts", "TypeScript"), CODE("cs", "C#"),
	CODE("go", "Go"), CODE("rs", "Rust"), CODE("kt", "Kotlin"), CODE("swift", "Swift"), CODE("php", "PHP"),
	CODE("rb", "Ruby"), CODE("sql", "SQL"), CODE("css", "CSS"), CODE("html", "HTML"), CODE("htm", "HTML"),
	{"zip", FK_ARCHIVE}, {"7z", FK_ARCHIVE}, {"rar", FK_ARCHIVE}, {"tar", FK_ARCHIVE}, {"gz", FK_ARCHIVE},
	{"bz2", FK_ARCHIVE}, {"xz", FK_ARCHIVE},
	{"ttf", FK_FONT}, {"otf", FK_FONT}, {"bcfnt", FK_FONT, N_("Fuente de 3DS (BCFNT)")},
	{"pdf", FK_DOC, N_("Documento PDF")},
};

static const FType *f_type(const FEnt *e)
{
	const char *x = f_ext(e->name);
	if (e->dir || !*x)
		return NULL;
	for (size_t i = 0; i < ARRAY_LEN(FTYPES); i++)
		if (!strcasecmp(x, FTYPES[i].x))
			return &FTYPES[i];
	return NULL;
}

static int f_group(const FEnt *e)
{
	const FType *t = f_type(e);
	return t ? t->k : FK_OTHER;
}

// icono y color de cada tipo, como en la web
static void f_kind(const FEnt *e, IconId *ic, u32 *col)
{
	static const u32 MK[7] = {0, 0xFF4444EF, 0xFF1673F9, 0xFF08B3EA, 0xFF5EC522, 0xFFF6823B, 0xFFF755A8};
	if (F.titles) {
		*ic = IC_GAMEPAD;
		*col = T.ok;
		return;
	}
	if (e->dir) {
		*ic = IC_FOLDER;
		*col = e->color ? MK[e->color] : T.warn;
		return;
	}
	switch (f_group(e)) {
	case FK_IMG: *ic = IC_IMAGE, *col = T.accent; break;
	case FK_VIDEO: *ic = IC_CAMERA, *col = T.accent2; break;
	case FK_AUDIO: *ic = IC_SPEAKER, *col = T.accent2; break;
	case FK_APP: *ic = IC_GEM, *col = T.ok; break;
	case FK_ROM: *ic = IC_GAMEPAD, *col = T.ok; break;
	case FK_SAVE: *ic = IC_STAR, *col = T.warn; break;
	case FK_TEXT: *ic = IC_NOTE, *col = T.text2; break;
	case FK_ARCHIVE: *ic = IC_ARCHIVE, *col = T.warn; break;
	case FK_FONT: *ic = IC_FONT, *col = T.text2; break;
	case FK_DOC: *ic = IC_FILE, *col = T.danger; break;
	default: *ic = IC_FILE, *col = T.text3; break;
	}
	if (e->color)
		*col = MK[e->color];
}

static bool f_collect(const FsxEntry *x, void *u)
{
	(void)u;
	size_t l = strlen(x->name);
	if (!x->dir && !strncmp(x->name, ".c3d_", 5) && l > 5 && !strcmp(x->name + l - 5, ".part"))
		return true; // temporales de subidas y copias
	if (x->dir && !strcmp(x->name, "trash") && !strcasecmp(F.path, "/3ds/Cloud3DS"))
		return true; // la papelera se ve aparte
	if (F.n >= F_MAX) {
		F.truncated = true;
		return false;
	}
	FEnt *e = &F.e[F.n++];
	util_strlcpy(e->name, x->name, sizeof e->name);
	e->dir = x->dir;
	e->size = x->size;
	e->mtime = x->mtime;
	return true;
}

// grupo de cada elemento, como «Agrupar por» de Windows
static int f_grp(const FEnt *e)
{
	switch (V.f_group) {
	case 1: { // tipo
		static const u8 G[] = {[FK_OTHER] = 11, [FK_IMG] = 1, [FK_VIDEO] = 2, [FK_AUDIO] = 3, [FK_APP] = 4, [FK_ROM] = 5,
		                       [FK_SAVE] = 6, [FK_TEXT] = 7, [FK_ARCHIVE] = 8, [FK_FONT] = 9, [FK_DOC] = 10};
		return e->dir ? 0 : G[f_group(e)];
	}
	case 2: { // fecha
		if (!e->mtime)
			return 6;
		u64 now = util_unix_now() / 86400, d = e->mtime / 86400;
		u64 ago = now > d ? now - d : 0;
		return ago == 0 ? 0 : ago == 1 ? 1 : ago < 7 ? 2 : ago < 31 ? 3 : ago < 365 ? 4 : 5;
	}
	case 3: // tamaño
		return e->dir ? 0 : e->size == 0 ? 1 : e->size < (1 << 20) ? 2 : e->size < (100 << 20) ? 3 : e->size < (1 << 30) ? 4 : 5;
	case 4: // color: cada uno en su grupo y lo que no tiene, al final
		return e->color ? e->color - 1 : 6;
	}
	return 0;
}

static const char *f_grp_name(int g)
{
	static const char *const BY_TYPE[] = {N_("Carpetas"), N_("Imágenes"), N_("Vídeos"), N_("Audio"), N_("Programas de 3DS"),
	                                      N_("Juegos para emuladores"), N_("Partidas"), N_("Textos y código"),
	                                      N_("Comprimidos"), N_("Fuentes"), N_("Documentos"), N_("Otros")};
	static const char *const BY_DATE[] = {N_("Hoy"), N_("Ayer"), N_("Esta semana"), N_("Este mes"), N_("Este año"),
	                                      N_("Hace más de un año"), N_("Sin fecha")};
	static const char *const BY_SIZE[] = {N_("Carpetas"), N_("Vacíos"), N_("Pequeños (menos de 1 MB)"), N_("Medianos (hasta 100 MB)"),
	                                      N_("Grandes (hasta 1 GB)"), N_("Muy grandes (más de 1 GB)")};
	if (V.f_group == 1 && g < (int)ARRAY_LEN(BY_TYPE))
		return tr(BY_TYPE[g]);
	if (V.f_group == 2 && g < (int)ARRAY_LEN(BY_DATE))
		return tr(BY_DATE[g]);
	if (V.f_group == 3 && g < (int)ARRAY_LEN(BY_SIZE))
		return tr(BY_SIZE[g]);
	static const char *const BY_COLOR[] = {N_("Rojo"), N_("Naranja"), N_("Amarillo"), N_("Verde"), N_("Azul"), N_("Morado"), N_("Sin color")};
	if (V.f_group == 4 && g < (int)ARRAY_LEN(BY_COLOR))
		return tr(BY_COLOR[g]);
	return "";
}

// orden: grupo, carpetas primero, destacadas primero y luego lo elegido (nombre, fecha, tamaño o tipo)
static int f_cmp(const void *a, const void *b)
{
	const FEnt *x = a, *y = b;
	if (x->grp != y->grp)
		return x->grp - y->grp;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	if (V.f_pin && !!x->color != !!y->color)
		return !!y->color - !!x->color;
	int r = 0;
	if (V.f_sort == 1)
		r = x->mtime < y->mtime ? -1 : x->mtime > y->mtime;
	else if (V.f_sort == 2 && !x->dir)
		r = x->size < y->size ? -1 : x->size > y->size;
	else if (V.f_sort == 3)
		r = strcasecmp(f_ext(x->name), f_ext(y->name));
	if (!r)
		r = strcasecmp(x->name, y->name);
	return V.f_desc ? -r : r;
}

#define F_COLS 4    // casillas por fila en la vista de iconos
#define F_TILE_H 76
#define F_ROW_H 36

// coloca una fila (lista) o una casilla (iconos); col lleva la columna en la fila de iconos
static void f_put(int i, float *y, int *col)
{
	if (V.f_grid) {
		float tw = (BOT_W - 12) / (float)F_COLS;
		F.v[F.nv++] = (FView){FV_ITEM, (short)i, *y, F_TILE_H, 6 + *col * tw, tw, 0};
		if (++*col == F_COLS)
			*col = 0, *y += F_TILE_H;
	} else {
		F.v[F.nv++] = (FView){FV_ITEM, (short)i, *y, F_ROW_H, 6, BOT_W - 12, 0};
		*y += F_ROW_H;
	}
}

static void f_endrow(float *y, int *col)
{
	if (*col)
		*col = 0, *y += F_TILE_H;
}

// filas que se dibujan: accesos rápidos, lo del usuario (por grupos si se agrupa) y, aparte, lo del sistema
static void f_build_view(void)
{
	F.nv = 0;
	float y = 0;
	int col = 0;
	if (F.search) { // búsqueda: los filtros por tipo (si hay de varios) y una fila por resultado, con su carpeta
		if (sr_nkinds() > 1) {
			F.v[F.nv++] = (FView){FV_FILTER, -1, y, 30, 0, BOT_W, 0};
			y += 30;
		}
		for (int i = 0; i < F.n; i++) {
			F.v[F.nv++] = (FView){FV_ITEM, (short)i, y, F_ROW_H, 6, BOT_W - 12, 0};
			y += F_ROW_H;
		}
		F.total = y + QR_PAD;
		return;
	}
	// accesos rápidos: siempre en la raíz; en las demás carpetas, solo si se sacaron tirando hacia abajo
	if (!strcmp(F.path, "/") || F.chips_on) {
		F.v[F.nv++] = (FView){FV_CHIPS, -1, y, 44, 0, BOT_W, 0};
		y += 44;
	}
	int nsys = 0, last = -1;
	bool apart = V.f_sysapart;
	for (int i = 0; i < F.n; i++) {
		const FEnt *e = &F.e[i];
		if (e->sys && apart) {
			nsys++;
			continue;
		}
		if (V.f_group && !F.titles && e->grp != last) {
			f_endrow(&y, &col);
			last = e->grp;
			short cnt = 0;
			for (int k = i; k < F.n; k++)
				cnt += (!F.e[k].sys || !apart) && F.e[k].grp == last;
			F.v[F.nv++] = (FView){FV_GROUP, (short)last, y, 26, 0, BOT_W, cnt};
			y += 26;
		}
		if (!V.f_group || F.titles || !(F.gclosed & (1u << e->grp)))
			f_put(i, &y, &col);
	}
	f_endrow(&y, &col);
	if (nsys) {
		F.v[F.nv++] = (FView){FV_SYSHDR, (short)nsys, y, 28, 0, BOT_W, 0};
		y += 28;
		if (F.sys_open)
			for (int i = 0; i < F.n; i++)
				if (F.e[i].sys)
					f_put(i, &y, &col);
		f_endrow(&y, &col);
	}
	int tn;
	if (!strcmp(F.path, "/") && (trash_size(&tn), tn)) { // en la raíz, al final: la papelera
		F.v[F.nv++] = (FView){FV_TRASH, (short)tn, y + 6, 40, 6, BOT_W - 12, 0};
		y += 46;
	}
	F.total = y + QR_PAD + (MS.on ? 48 : 0); // con la selección, sitio para que su barra no tape lo último
}

static void ms_end(void);

// ---- búsqueda
static int sr_kind(const FEnt *e)
{
	return e->dir ? SR_KINDS - 1 : f_group(e);
}

static bool sr_cb(const char *dir, const FsxEntry *x, void *u)
{
	(void)u;
	char full[1024];
	snprintf(full, sizeof full, "%s%s%s", dir, strcmp(dir, "/") ? "/" : "", x->name);
	state_lock();
	u8 col = (u8)mark_get(full);
	state_unlock();
	bool sys = fsx_is_system(full);
	LightLock_Lock(&SR.lock);
	int d = SR.ndirs - 1; // los resultados de una carpeta llegan juntos: casi siempre es la última
	if (d < 0 || strcmp(SR.dirs[d], dir)) {
		for (d = 0; d < SR.ndirs && strcmp(SR.dirs[d], dir); d++)
			;
		if (d == SR.ndirs) {
			if (SR.ndirs == SR_DIRS || strlen(dir) >= sizeof SR.dirs[0]) {
				LightLock_Unlock(&SR.lock);
				return SR.ndirs < SR_DIRS;
			}
			util_strlcpy(SR.dirs[SR.ndirs++], dir, sizeof SR.dirs[0]);
		}
	}
	FEnt *e = &SR.r[SR.n];
	memset(e, 0, sizeof *e);
	util_strlcpy(e->name, x->name, sizeof e->name);
	e->dir = x->dir, e->size = x->size, e->mtime = x->mtime, e->color = col, e->sys = sys, e->sdir = (u16)d;
	SR.cnt[sr_kind(e)]++;
	SR.n++;
	bool more = SR.n < SR_MAX;
	LightLock_Unlock(&SR.lock);
	return more;
}

static void sr_thread(void *arg)
{
	(void)arg;
	search_walk(SR.scope, SR.q, &SR.cancel, 0, NULL, sr_cb, NULL);
	SR.done = true;
}

static void sr_stop(void)
{
	if (!SR.thr)
		return;
	SR.cancel = true;
	threadJoin(SR.thr, U64_MAX);
	threadFree(SR.thr);
	SR.thr = NULL;
}

static void f_load(const char *path);

static void sr_start(const char *q)
{
	sr_stop();
	if (!SR.r) {
		LightLock_Init(&SR.lock);
		SR.r = malloc(sizeof(FEnt) * SR_MAX);
		SR.dirs = malloc(sizeof SR.dirs[0] * SR_DIRS);
		if (!SR.r || !SR.dirs) {
			free(SR.r), free(SR.dirs);
			SR.r = NULL, SR.dirs = NULL;
			return;
		}
	}
	if (!F.search) // se busca en la carpeta abierta (buscar otra vez desde los resultados: en la misma)
		util_strlcpy(SR.scope, F.trash || F.nosaves ? "/" : F.path, sizeof SR.scope);
	util_strlcpy(SR.q, q, sizeof SR.q);
	SR.n = SR.shown = SR.ndirs = 0;
	memset(SR.cnt, 0, sizeof SR.cnt);
	SR.kind = -1;
	SR.back[0] = 0;
	SR.cancel = SR.done = false;
	SR.running = true;
	SR.t0 = osGetTime();
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	SR.thr = threadCreate(sr_thread, NULL, 64 * 1024, prio + 2, -2, false);
	if (!SR.thr)
		SR.done = true;
	F.path[0] = 0; // para que f_load lo trate como una carpeta nueva
	f_load("@search");
}

// lo encontrado hasta ahora (con el filtro de tipo) pasa a la lista
static void sr_fill(void)
{
	F.n = 0;
	if (!SR.r)
		return;
	LightLock_Lock(&SR.lock);
	for (int i = 0; i < SR.n && F.n < F_MAX; i++)
		if (SR.kind < 0 || sr_kind(&SR.r[i]) == SR.kind)
			F.e[F.n++] = SR.r[i];
	SR.shown = SR.n;
	LightLock_Unlock(&SR.lock);
}

static int sr_nkinds(void)
{
	int k = 0;
	for (int i = 0; i < SR_KINDS; i++)
		k += SR.cnt[i] > 0;
	return k;
}

static const char *sr_kind_name(int k)
{
	static const char *const N[SR_KINDS] = {
		[FK_OTHER] = N_("Otros"), [FK_IMG] = N_("Imágenes"), [FK_VIDEO] = N_("Vídeos"), [FK_AUDIO] = N_("Audio"),
		[FK_APP] = N_("Programas"), [FK_ROM] = N_("Juegos"), [FK_SAVE] = N_("Partidas"), [FK_TEXT] = N_("Textos"),
		[FK_ARCHIVE] = N_("Comprimidos"), [FK_FONT] = N_("Fuentes"), [FK_DOC] = N_("Documentos"), [SR_KINDS - 1] = N_("Carpetas")};
	return k < 0 ? _("Todo") : tr(N[k]);
}

// abre la carpeta de un resultado con él elegido; B en ella vuelve a los resultados
static void sr_goto(int idx)
{
	const FEnt *e = &F.e[idx];
	char dir[400], nm[128];
	util_strlcpy(dir, SR.dirs[e->sdir], sizeof dir);
	util_strlcpy(nm, e->name, sizeof nm);
	SR.sel_keep = F.sel;
	f_load(dir);
	util_strlcpy(SR.back, dir, sizeof SR.back);
	for (int i = 0; i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm)) {
			F.sel = i;
			F.target = fmaxf(0, F.v[i].y - VIEW_H / 2);
		}
}

// B en la carpeta abierta desde un resultado: de vuelta a los resultados, en el mismo
static bool f_back_to_results(void)
{
	if (!SR.back[0] || !SR.r || strcmp(F.path, SR.back))
		return false;
	SR.back[0] = 0;
	f_load("@search");
	F.sel = SR.sel_keep < F.nv ? SR.sel_keep : F.nv - 1;
	if (F.sel >= 0)
		F.target = fmaxf(0, F.v[F.sel].y - VIEW_H / 2);
	return true;
}

// filtro de tipo: d = +1 el siguiente que haya, -1 el anterior (-1 es «Todo»)
static void sr_next_kind(int d)
{
	int k = SR.kind;
	do
		k = k + d < -1 ? SR_KINDS - 1 : k + d >= SR_KINDS ? -1 : k + d;
	while (k >= 0 && !SR.cnt[k]);
	SR.kind = k;
	f_load("@search");
	F.sel = F.nv > 1 && F.v[0].kind == FV_FILTER ? 1 : 0;
	F.off = F.target = 0;
}

// la fila de filtros: píldoras que se deslizan para que se vea la elegida
static void sr_filter_row(float y, bool inview)
{
	static float scroll;
	float x = 8, xs[SR_KINDS + 1], ws[SR_KINDS + 1];
	int ks[SR_KINDS + 1], n = 0;
	for (int k = -1; k < SR_KINDS; k++) {
		if (k >= 0 && !SR.cnt[k])
			continue;
		char t[64];
		snprintf(t, sizeof t, "%s %d", sr_kind_name(k), k < 0 ? SR.n : SR.cnt[k]);
		ks[n] = k, xs[n] = x, ws[n] = text_width(t, TS_SUB) + 18;
		x += ws[n++] + 6;
	}
	float want = scroll;
	for (int i = 0; i < n; i++)
		if (ks[i] == SR.kind) {
			if (xs[i] - want < 8)
				want = xs[i] - 8;
			if (xs[i] + ws[i] - want > BOT_W - 8)
				want = xs[i] + ws[i] - (BOT_W - 8);
		}
	scroll = approach(scroll, fmaxf(0, fminf(want, x - BOT_W + 2)), 18);
	for (int i = 0; i < n; i++) {
		float cx = xs[i] - scroll;
		bool on = ks[i] == SR.kind;
		char t[64];
		snprintf(t, sizeof t, "%s %d", sr_kind_name(ks[i]), ks[i] < 0 ? SR.n : SR.cnt[ks[i]]);
		bool tp = inview && touch_in(cx, y + 4, ws[i], 22);
		draw_rrect(cx, y + 4, ws[i], 22, 11, on ? T.accent : tp ? T.surface2 : T.surface);
		text_draw_center(t, cx + ws[i] / 2, y + 15 - text_lineh(TS_SUB) / 2, TS_SUB, on ? 0xFFFFFFFF : T.text2);
		if (inview && tap(cx, y + 4, ws[i], 22) && !on) {
			SR.kind = ks[i];
			f_load("@search");
			F.sel = F.nv > 1 ? 1 : 0;
			F.off = F.target = 0;
			return;
		}
	}
}

static void sr_close(void)
{
	sr_stop();
	SR.back[0] = 0;
	f_load(SR.scope);
}

int g_app_now; // (para el registro del vigilante)
static char FILES_LAST[512] = "/"; // la última carpeta de Archivos
static bool TI_by_size; // Títulos: ordenados por tamaño (si no, por nombre)

// la lista de títulos se lee en su hilo (leer el nombre de cada uno lleva su tiempo): mientras, se ve la anterior
static struct {
	Thread thr;
	volatile bool done;
	bool run, again;
	TitleInfo *res;
	int n;
} TL;

static void tl_thread(void *arg)
{
	(void)arg;
	TL.n = titles_list(&TL.res);
	TL.done = true;
}

static void tl_start(void)
{
	if (TL.run) { // ya se está leyendo: otra vez al terminar (lo de ahora puede llegar tarde)
		TL.again = true;
		return;
	}
	TL.run = true, TL.done = false, TL.res = NULL;
	s32 prio = 0x30;
	svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
	TL.thr = threadCreate(tl_thread, NULL, 32 * 1024, prio + 1, -2, false);
	if (!TL.thr)
		tl_thread(NULL);
}

static bool tl_loading(void) { return TL.run; }
static int f_cmp_size_desc(const void *a, const void *b)
{
	const FEnt *x = a, *y = b;
	return x->size < y->size ? 1 : x->size > y->size ? -1 : strcasecmp(x->name, y->name);
}
static void ti_fill(void)
{
	F.n = 0;
	for (int i = 0; i < TI_n && F.n < F_MAX; i++) {
		FEnt *e = &F.e[F.n++];
		memset(e, 0, sizeof *e);
		util_strlcpy(e->name, TI[i].title, sizeof e->name);
		e->size = TI[i].size + TI[i].upd_size + TI[i].dlc_size;
		e->tid = (u32)i;
	}
	if (TI_by_size) // los que más ocupan, primero
		qsort(F.e, F.n, sizeof *F.e, f_cmp_size_desc);
	F.err = F.missing = false;
	f_build_view();
}

static void f_load(const char *path)
{
	WHERE("leer una carpeta");
	// la pulsación (A o el dedo) que lleva a otra carpeta se queda aquí: si siguiera, abriría lo que quede debajo en la nueva
	if (strcmp(F.path, path)) {
		g_kdown &= ~(KEY_A | KEY_B | KEY_X | KEY_Y);
		g_touch.up = false;
		U.nav_eat = true;
	}
	if (!F.e)
		F.e = malloc(sizeof(FEnt) * F_MAX);
	if (!F.e)
		return;
	bool same = !strcmp(F.path, path);
	util_strlcpy(F.path, path, sizeof F.path);
	F.n = 0;
	F.truncated = false;
	F.nosaves = false;
	F.need_reload = false;
	F.trash = !strcmp(path, "@trash");
	F.titles = !strcmp(path, "@titles");
	if (F.titles) { // juegos y apps de la SD, por nombre: al entrar se ve lo último leído y se vuelve a leer en su hilo
		if (!same || !TI)
			tl_start();
		ti_fill();
		if (!same) {
			ms_end();
			F.sel = 0;
			F.off = F.target = 0;
		}
		if (F.sel >= F.nv)
			F.sel = F.nv - 1;
		F.info_for = -1;
		return;
	}
	F.search = !strcmp(path, "@search");
	if (F.search) { // resultados de la búsqueda, en el orden en que se encuentran
		sr_fill();
		F.err = F.missing = false;
		f_build_view();
		if (!same) {
			ms_end();
			F.sel = F.nv > 1 && F.v[0].kind == FV_FILTER ? 1 : 0;
			F.off = F.target = 0;
		}
		if (F.sel >= F.nv)
			F.sel = F.nv - 1;
		F.info_for = -1;
		return;
	}
	if (F.trash) { // la papelera: sus elementos, lo más reciente primero
		static TrashItem it[TRASH_MAX];
		int n = trash_list(it, TRASH_MAX);
		for (int i = 0; i < n && F.n < F_MAX; i++) {
			FEnt *e = &F.e[F.n++];
			memset(e, 0, sizeof *e);
			util_strlcpy(e->name, it[i].name, sizeof e->name);
			e->dir = it[i].dir, e->size = it[i].size, e->mtime = it[i].when, e->tid = it[i].id, e->color = it[i].color;
		}
		F.err = F.missing = false;
		f_build_view();
		if (!same) {
			ms_end();
			F.sel = 0;
			F.off = F.target = 0;
		}
		if (F.sel >= F.nv)
			F.sel = F.nv - 1;
		F.info_for = -1;
		return;
	}
	F.err = fsx_list(path, false, f_collect, NULL) < 0; // (sin fechas: se leen después, poco a poco)
	F.mt_next = 0, F.mt_pending = F.n > 0;
	bool d;
	u64 sz;
	F.missing = F.err && fsx_stat(path, &d, &sz) < 0;
	state_lock();
	for (int i = 0; i < F.n; i++) {
		char full[700];
		f_full(i, full, sizeof full);
		F.e[i].sys = fsx_is_system(full);
		F.e[i].color = (u8)mark_get(full);
	}
	state_unlock();
	for (int i = 0; i < F.n; i++)
		F.e[i].grp = (u8)f_grp(&F.e[i]);
	qsort(F.e, F.n, sizeof *F.e, f_cmp);
	f_build_view();
	if (!same) {
		ms_end();
		F.gclosed = 0;
		F.chips_on = false;
		F.pull = 0;
		f_build_view();
		F.sel = F.nv > 1 && F.v[0].kind == FV_CHIPS ? 1 : 0;
		F.off = F.target = 0;
	}
	if (F.sel >= F.nv)
		F.sel = F.nv - 1;
	F.info_for = -1;
}

// las fechas de la carpeta abierta, unas pocas en cada vuelta (con tope de tiempo); al tenerlas todas, si se ordena o se
// agrupa por fecha, se vuelve a ordenar sin perder lo elegido
static void f_mtime_step(void)
{
	if (!F.mt_pending)
		return;
	if (F.trash || F.search || F.titles || F.path[0] != '/') {
		F.mt_pending = false;
		return;
	}
	WHERE("leer las fechas de la carpeta");
	u64 t0 = util_ms();
	while (F.mt_next < F.n && util_ms() - t0 < 6) {
		char full[700];
		f_full(F.mt_next, full, sizeof full);
		F.e[F.mt_next].mtime = fsx_mtime(full);
		F.mt_next++;
	}
	if (F.mt_next < F.n)
		return;
	F.mt_pending = false;
	F.info_for = -1; // (la información de arriba, con su fecha)
	if (V.f_sort != 1 && V.f_group != 2)
		return;
	char keep[128] = "";
	const FView *v = F.sel >= 0 && F.sel < F.nv ? &F.v[F.sel] : NULL;
	if (v && v->kind == FV_ITEM)
		util_strlcpy(keep, F.e[v->idx].name, sizeof keep);
	for (int i = 0; i < F.n; i++)
		F.e[i].grp = (u8)f_grp(&F.e[i]);
	qsort(F.e, F.n, sizeof *F.e, f_cmp);
	f_build_view();
	for (int i = 0; keep[0] && i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, keep))
			F.sel = i;
}

static void f_reload_keep(void)
{
	char keep[128] = "", p[512];
	const FView *v = F.sel >= 0 && F.sel < F.nv ? &F.v[F.sel] : NULL;
	if (v && v->kind == FV_ITEM)
		util_strlcpy(keep, F.e[v->idx].name, sizeof keep);
	util_strlcpy(p, F.path, sizeof p);
	float off = F.target;
	f_load(p);
	F.target = fminf(off, fmaxf(0, F.total - VIEW_H));
	for (int i = 0; keep[0] && i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, keep))
			F.sel = i;
}

static void f_open_chip(int c)
{
	const char *p = CHIP_PATH[c];
	// atrás desde la categoría vuelve a donde estaba (no a la carpeta de encima)
	util_strlcpy(F.ret_to, F.trash || F.search || F.nosaves || !F.path[0] ? "/" : F.path, sizeof F.ret_to);
	F.ret_from[0] = 0; // «Inicio» es la raíz: no hay a dónde volver
	if (c == CH_TITLES) {
		util_strlcpy(F.ret_from, "@titles", sizeof F.ret_from);
		f_load("@titles");
		return;
	}
	if (c == CH_SAVES) {
		p = NULL;
		for (int i = 0; i < 3 && !p; i++) {
			bool d;
			u64 sz;
			if (fsx_stat(SAVE_DIRS[i], &d, &sz) == 0 && d)
				p = SAVE_DIRS[i];
		}
		if (!p) { // como en la web: se explica en vez de abrir una carpeta vacía
			F.nosaves = true;
			F.n = 0;
			f_build_view();
			F.sel = 0;
			F.chip = CH_SAVES;
			return;
		}
	}
	util_strlcpy(F.ret_from, p, sizeof F.ret_from);
	f_load(p);
}

// atrás desde una categoría abierta con su acceso rápido: a donde se estaba
static bool f_back_to_origin(void)
{
	if (!F.ret_from[0] || strcasecmp(F.path, F.ret_from) || !strcmp(F.ret_to, F.path))
		return false;
	char to[512], from[512];
	util_strlcpy(to, F.ret_to, sizeof to);
	util_strlcpy(from, F.ret_from, sizeof from);
	F.ret_from[0] = 0;
	f_load(to);
	return true;
}

static void dl_view_close(void);
static void f_up(void)
{
	if (U.app == AP_DL && !strcmp(F.path, DL_DIR)) {
		dl_view_close();
		return;
	}
	if (f_back_to_origin())
		return;
	if (!strcmp(F.path, "/"))
		return;
	char p[512], from[128];
	util_strlcpy(p, F.path, sizeof p);
	char *sl = strrchr(p, '/');
	util_strlcpy(from, sl + 1, sizeof from);
	if (sl == p)
		sl[1] = 0;
	else
		*sl = 0;
	f_load(p);
	// vuelve a dejar elegida la carpeta de la que se sale
	for (int i = 0; i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, from)) {
			F.sel = i;
			F.target = fmaxf(0, F.v[i].y - VIEW_H / 2);
		}
}

static bool f_on_chips(void)
{
	return F.sel >= 0 && F.sel < F.nv && F.v[F.sel].kind == FV_CHIPS;
}

static bool f_chips_shown(void)
{
	return F.nv > 0 && F.v[0].kind == FV_CHIPS;
}

// saca los accesos rápidos arriba sin perder lo elegido
static void f_show_chips(void)
{
	if (f_chips_shown())
		return;
	F.chips_on = true;
	f_build_view();
	F.sel++;
	F.off = F.target = 0;
}

static const FEnt *f_selected(void)
{
	return F.sel >= 0 && F.sel < F.nv && F.v[F.sel].kind == FV_ITEM ? &F.e[F.v[F.sel].idx] : NULL;
}

// SMDH de un .3dsx (nombre, autor e icono), si lo lleva

static bool read_3dsx(const char *path, char *title, size_t tn, char *pub, size_t pn, Texture *icon)
{
	static u16 px[48 * 48];
	if (!smdh_3dsx(path, title, tn, pub, pn, px))
		return false;
	tex_from_rgb565(icon, px, 48, 48);
	return true;
}

typedef struct {
	int n;
	u64 t0;
} CountT;
static bool count_cb(const FsxEntry *e, void *u)
{
	(void)e;
	CountT *c = u;
	return ++c->n < 100000 && ((c->n & 31) || util_ms() - c->t0 < 120); // (una carpeta enorme no traba: se dice «más de»)
}

static bool f_is_img(const FEnt *e);
static bool f_is_txt(const FEnt *e);

// lee lo que hace falta para la pantalla de arriba: icono y datos del CIA o .3dsx, foto en pequeño…
static void f_info_refresh(void)
{
	WHERE("información de lo elegido");
	int idx = f_selected() ? F.v[F.sel].idx : -1;
	if (idx == F.info_for)
		return;
	F.info_for = idx;
	F.cia_ok = false;
	F.app_title[0] = F.app_pub[0] = 0;
	F.iw = F.ih = 0;
	F.dir_count = -1;
	F.preview[0] = 0;
	tex_free(&F.cia_icon);
	tex_free(&F.thumb);
	if (idx < 0)
		return;
	const FEnt *e = &F.e[idx];
	char full[700];
	f_full(idx, full, sizeof full);
	if (e->dir) {
		CountT c = {0, util_ms()};
		if (fsx_list(full, false, count_cb, &c) >= 0)
			F.dir_count = c.n, F.dir_more = util_ms() - c.t0 >= 120;
	} else if (!strcasecmp(f_ext(e->name), "cia")) {
		if (!F.cia)
			F.cia = malloc(sizeof *F.cia);
		if (F.cia && strlen(full) < sizeof F.cia->path && ins_peek(full, F.cia)) {
			F.cia_ok = true;
			if (F.cia->has_icon)
				tex_from_rgb565(&F.cia_icon, F.cia->icon, 48, 48);
		}
	} else if (!strcasecmp(f_ext(e->name), "3dsx")) {
		read_3dsx(full, F.app_title, sizeof F.app_title, F.app_pub, sizeof F.app_pub, &F.cia_icon);
	} else if (f_is_txt(e)) {
		char sd[720];
		snprintf(sd, sizeof sd, "sdmc:%s", full);
		FILE *f = fopen(sd, "rb");
		if (f) {
			size_t n = fread(F.preview, 1, sizeof F.preview - 1, f);
			fclose(f);
			F.preview[n] = 0;
			char *p = F.preview;
			if (!strncmp(p, "\xEF\xBB\xBF", 3))
				memmove(p, p + 3, strlen(p + 3) + 1);
			size_t l = strlen(p);
			if (n == sizeof F.preview - 1) { // se cortó: sin un carácter partido al final
				size_t j = l;
				while (j > 0 && l - j < 3 && ((u8)p[j - 1] & 0xC0) == 0x80)
					j--;
				if (j > 0 && (u8)p[j - 1] >= 0xC0)
					p[j - 1] = 0;
			}
			for (char *c = p; *c; c++)
				if (*c == '\t' || *c == '\r')
					*c = ' ';
			if (!util_utf8_valid(p) || memchr(p, 0, strlen(p)) != NULL)
				p[0] = 0;
		}
	} else if (f_is_img(e)) {
		u8 *px = NULL;
		int w, h;
		if (img_load_fit(full, 116, 116, &px, &w, &h, &F.iw, &F.ih) == IMG_OK)
			tex_from_rgba(&F.thumb, px, w, h);
		else
			img_size(full, &F.iw, &F.ih);
		free(px);
	}
}

static void f_menu_open(void);
static bool f_open_item(int vi);
static void zs_open(const char *path);

// mover o copiar: carpeta de destino que se está eligiendo
static struct {
	int mode;          // 0 nada, FA_MOVE o FA_COPY: eligiendo carpeta de destino
	char src[700], name[128];
	bool src_dir;
	int n;             // varios a la vez (selección múltiple): sus rutas en list
	char (*list)[512];
} FP;

static int fp_count(void)
{
	return FP.mode ? (FP.n > 1 ? FP.n : 1) : 0;
}
static void fp_clear(void)
{
	FP.mode = 0;
	FP.n = 0;
}
// pegar en la nube lo copiado (o cortado) en Archivos: se sube; el original sigue en la SD
static int fp_upload(const char *parent)
{
	int k = 0;
	if (FP.n > 1)
		for (int i = 0; i < FP.n; i++)
			k += cloud_upload(FP.list[i], parent);
	else if (FP.mode)
		k += cloud_upload(FP.src, parent);
	if (FP.mode == FA_MOVE) // (cortar de la SD a la nube no borra nada: se sube y ya)
		fp_clear();
	return k;
}


// copias en cola: se lanzan de una en una cuando termina la anterior
static struct {
	int n, i;
	char (*src)[512];
	char (*dst)[700];
} CQ;

// ---- visor (fotos y textos): pantalla completa arriba, controles abajo
enum { VW_NONE, VW_IMG, VW_TXT };
static struct {
	int kind;
	int idx;           // elemento de F.e
	Texture tex;
	int w, h, ow, oh;
	ImgResult res;
	float t;           // tiempo abierto, para el pie que se desvanece
	// texto: se ve arriba con números de línea y se edita línea a línea con el teclado
	TxtDoc doc;
	bool prot;         // en una carpeta protegida: solo lectura
	int cur, top;      // línea elegida y primera línea visible arriba
	float drag;        // arrastre en la lista de abajo
	char path[700];
	int lang;          // HL_*: colores de código según la extensión
	int *hls, hlcap;   // estado del resaltado al empezar cada línea (comentarios de varias líneas)
	int hlv;           // cuántos de esos estados son válidos (0 tras cada cambio)
	int hlpos;         // posición en el historial de cambios con la que se calcularon
	char find[128];    // lo último que se buscó
	// abierto desde la nube: el archivo se bajó a CLOUD_CACHE; al guardar, se vuelve a subir a su carpeta
	bool cloud;
	CloudItem citem;
	char cparent[64];
} VW;

static bool vw_cloud_open(void)
{
	return VW.cloud && VW.kind != VW_NONE;
}

static const char *vw_name(void)
{
	return VW.cloud ? VW.citem.name : F.e[VW.idx].name;
}
static void cl_view_step(int d); // (la nube: la foto anterior o la siguiente de su carpeta)
static void cl_view_menu(void);  // (la nube: las opciones del archivo abierto)
static void vw_menu(void)
{
	if (VW.cloud)
		cl_view_menu();
	else
		f_menu_open();
}

static bool f_is_img(const FEnt *e)
{
	return f_group(e) == FK_IMG;
}

static bool f_is_txt(const FEnt *e)
{
	return f_group(e) == FK_TEXT;
}

static bool vw_is_open(void)
{
	return VW.kind != VW_NONE;
}

static void cl_open_abort(void);
static void vw_close(void)
{
	if (VW.cloud) // (si se estaba bajando la foto siguiente, ya no se abre)
		cl_open_abort();
	tex_free(&VW.tex);
	txt_free(&VW.doc);
	VW.kind = VW_NONE;
}

static bool vw_editable(void)
{
	return VW.kind == VW_TXT && VW.doc.ro == TXT_RW && !VW.prot;
}

static bool vw_dirty(void)
{
	return VW.kind == VW_TXT && VW.doc.dirty;
}

// cerrar el visor; si hay cambios sin guardar, se pregunta antes
static void vw_try_close(void)
{
	if (vw_dirty())
		dialog_open(DLG_TXTDISCARD, _("¿Salir sin guardar?"), _("Se perderán los cambios realizados en este archivo."), _("Salir sin guardar"), true);
	else
		vw_close();
}

// abrir en el visor la foto o el texto full (name: su nombre, para saber de qué tipo es)
static void vw_open(const char *full, const char *name)
{
	tex_free(&VW.tex);
	txt_free(&VW.doc);
	VW.t = 0;
	FEnt fe = {0};
	util_strlcpy(fe.name, name, sizeof fe.name);
	util_strlcpy(VW.path, full, sizeof VW.path);
	if (f_is_img(&fe)) {
		VW.kind = VW_IMG;
		u8 *px = NULL;
		VW.res = img_load_fit(full, 400, 240, &px, &VW.w, &VW.h, &VW.ow, &VW.oh);
		if (VW.res == IMG_OK && !tex_from_rgba(&VW.tex, px, VW.w, VW.h))
			VW.res = IMG_BAD;
		free(px);
		return;
	}
	// lo demás, como texto
	VW.kind = VW_NONE;
	VW.cur = VW.top = 0;
	VW.hlpos = 0;
	VW.drag = 0;
	util_strlcpy(VW.path, full, sizeof VW.path);
	int r = txt_load(&VW.doc, full);
	if (r != TXT_OK) {
		toast_icon(r == TXT_ERR_MEM ? _("No hay memoria suficiente para abrir este archivo") : _("No se pudo abrir este archivo"), IC_CLOSE, T.danger);
		return;
	}
	VW.kind = VW_TXT;
	VW.prot = !VW.cloud && V.protect_sys && fsx_is_system(full);
	VW.lang = hl_lang(f_ext(name));
	VW.hlv = 0;
}

static void vw_load(int idx)
{
	char full[700];
	f_full(idx, full, sizeof full);
	VW.cloud = false;
	VW.idx = idx;
	vw_open(full, F.e[idx].name);
}

// fotos de la carpeta, para pasar con ← →
static void vw_step(int d)
{
	if (VW.kind != VW_IMG)
		return;
	if (VW.cloud) {
		cl_view_step(d);
		return;
	}
	for (int k = 1; k < F.n; k++) {
		int i = (VW.idx + d * k + F.n * 4) % F.n;
		if (f_is_img(&F.e[i])) {
			vw_load(i);
			for (int v = 0; v < F.nv; v++) // la lista sigue a la foto
				if (F.v[v].kind == FV_ITEM && F.v[v].idx == i)
					F.sel = v;
			return;
		}
	}
}

// una línea lista para dibujar: tabuladores como espacios y, si es enorme, solo el principio
static void vt_line(int i, char *b, size_t n)
{
	size_t o = 0;
	for (const char *p = VW.doc.ln[i]; *p && o + 5 < n; p++) {
		if (*p == '\t') {
			memcpy(b + o, "    ", 4);
			o += 4;
		} else {
			b[o++] = *p;
		}
	}
	while (o > 0 && n > 0 && o + 5 >= n && ((u8)b[o - 1] & 0xC0) == 0x80) // sin partir un carácter
		o--;
	if (o > 0 && o + 5 >= n && (u8)b[o - 1] >= 0xC0)
		o--;
	b[o] = 0;
	if (!o)
		strcpy(b, " ");
}

#define VT_MAXROWS 16 // una línea larguísima no ocupa más que la pantalla

static float vt_gutter(void)
{
	char b[16];
	snprintf(b, sizeof b, "%d", VW.doc.n);
	return text_width(b, TS_SUB) + 16;
}

// estado del resaltado al empezar la línea i (se calcula una vez y se guarda)
static int vt_state(int i)
{
	if (VW.hlcap < VW.doc.n + 1) {
		int cap = VW.doc.n + 256, *h = realloc(VW.hls, cap * sizeof *h);
		if (!h)
			return 0;
		VW.hls = h, VW.hlcap = cap;
	}
	if (VW.hlv == 0)
		VW.hls[0] = 0, VW.hlv = 1;
	while (VW.hlv <= i) {
		int st = VW.hls[VW.hlv - 1];
		hl_line(VW.lang, VW.doc.ln[VW.hlv - 1], &st, NULL, 0);
		VW.hls[VW.hlv++] = st;
	}
	return VW.hls[i];
}

static u32 hl_color(int c)
{
	switch (c) {
	case HC_KEY: return T.accent2;
	case HC_STR: return T.ok;
	case HC_COM: return T.text3;
	case HC_NUM: return T.warn;
	case HC_TAG: return T.accent;
	case HC_ATTR: return col_lerp(T.accent, T.text, 0.35f);
	case HC_HEAD: return T.accent;
	default: return T.text;
	}
}

// una línea de código con colores; se parte por caracteres al llegar al ancho. Devuelve las filas
static int vt_code(int i, const char *b, float x, float y, float w, bool draw)
{
	static HlSpan sp[96];
	int st = vt_state(i);
	int ns = hl_line(VW.lang, b, &st, sp, 96);
	float lh = text_lineh(TS_SUB), cx = 0, rx = 0; // ancho usado en la fila y dónde empieza el trozo
	int rows = 1, row0 = 0, pl = 0;                 // inicio de la fila en b y bytes del trozo
	char piece[600];
#define VT_FLUSH(col)                                                                             \
	do {                                                                                          \
		if (pl && draw) {                                                                         \
			piece[pl] = 0;                                                                        \
			text_draw(piece, x + rx, y + (rows - 1) * lh, TS_SUB, col);                           \
		}                                                                                         \
		pl = 0;                                                                                   \
	} while (0)
	for (int k = 0; k < ns; k++) {
		const char *p = b + sp[k].start, *end = p + sp[k].len;
		u32 col = hl_color(sp[k].c);
		rx = text_width_n(b + row0, (int)(p - b) - row0, TS_SUB);
		while (p < end) {
			const char *q = p;
			u32 cp = utf8_next(&q);
			float gw = glyph_w(cp, TS_SUB);
			if (cx + gw > w && cx > 0) { // fila llena: sigue en la siguiente
				VT_FLUSH(col);
				if (rows == VT_MAXROWS)
					return rows;
				rows++, cx = 0, rx = 0, row0 = (int)(p - b);
			}
			if (pl + (q - p) >= (int)sizeof piece - 1) {
				VT_FLUSH(col);
				rx = text_width_n(b + row0, (int)(p - b) - row0, TS_SUB);
			}
			memcpy(piece + pl, p, q - p);
			pl += (int)(q - p);
			cx += gw;
			p = q;
		}
		VT_FLUSH(col);
	}
#undef VT_FLUSH
	return rows;
}

static int vt_rows(int i, float w)
{
	static char b[2400];
	vt_line(i, b, sizeof b);
	if (VW.lang)
		return vt_code(i, b, 0, 0, w, false);
	return text_wrap_lines(b, w, TS_SUB, VT_MAXROWS, 0, 0, 0, false);
}

static int vt_visible(void)
{
	return (int)((SCR_H - 8) / text_lineh(TS_SUB));
}

static void vt_draw(void)
{
	draw_rect(0, 0, TOP_W, SCR_H, T.bg);
	float lh = text_lineh(TS_SUB), gw = vt_gutter(), x = gw + 4, w = TOP_W - x - 12;
	int vis = vt_visible();
	if (VW.cur >= VW.doc.n)
		VW.cur = VW.doc.n - 1;
	if (VW.cur < 0)
		VW.cur = 0;
	// la línea elegida siempre a la vista
	if (VW.cur < VW.top)
		VW.top = VW.cur;
	while (VW.top < VW.cur) {
		int r = 0;
		for (int i = VW.top; i <= VW.cur && r <= vis; i++)
			r += vt_rows(i, w);
		if (r <= vis)
			break;
		VW.top++;
	}
	draw_rect(0, 0, gw, SCR_H, T.surface);
	static char b[2400];
	float y = 4;
	for (int i = VW.top; i < VW.doc.n && y < SCR_H; i++) {
		vt_line(i, b, sizeof b);
		int r = VW.lang ? vt_code(i, b, 0, 0, w, false) : text_wrap_lines(b, w, TS_SUB, VT_MAXROWS, 0, 0, 0, false);
		bool here = i == VW.cur;
		if (here)
			draw_rect(0, y - 1, TOP_W, r * lh + 2, col_alpha(T.accent, 0.16f));
		char num[12];
		snprintf(num, sizeof num, "%d", i + 1);
		text_draw_right(num, gw - 8, y, TS_SUB, here ? T.accent : T.text3);
		if (VW.lang)
			vt_code(i, b, x, y, w, true);
		else
			text_wrap_lines(b, w, TS_SUB, VT_MAXROWS, x, y, T.text, true);
		y += r * lh;
	}
	if (VW.doc.n > vis) { // barra de desplazamiento fina
		float vh = fmaxf(14, SCR_H * vis / (float)VW.doc.n);
		draw_rrect(TOP_W - 5, (SCR_H - vh) * VW.top / (float)(VW.doc.n - 1), 3, vh, 1.5f, col_alpha(T.text3, 0.6f));
	}
}

static void vw_top(void)
{
	VW.t += g_dt;
	if (VW.kind == VW_IMG) {
		draw_rect(0, 0, TOP_W, SCR_H, 0xFF000000);
		if (VW.res == IMG_OK)
			draw_texture(&VW.tex, (TOP_W - VW.w) / 2.0f, (SCR_H - VW.h) / 2.0f, VW.w, VW.h, 1);
		else
			text_draw_center(VW.res == IMG_TOO_BIG ? _("La imagen es demasiado grande para mostrarla en la consola") : _("No se pudo abrir la imagen"),
			                 TOP_W / 2, SCR_H / 2 - 8, TS_BODY, 0xFFAAAAAA);
		// pie con el nombre, que se desvanece a los 3 s
		float a = clampf((3.5f - VW.t) / 0.5f, 0, 1);
		if (a > 0.01f) {
			draw_rect(0, SCR_H - 26, TOP_W, 26, col_alpha(0xFF000000, 0.55f * a));
			char cap[200];
			if (VW.res == IMG_OK)
				snprintf(cap, sizeof cap, "%s · %d × %d", vw_name(), VW.ow, VW.oh);
			else
				util_strlcpy(cap, vw_name(), sizeof cap);
			text_draw_center_clip(cap, TOP_W / 2, SCR_H - 21, TOP_W - 20, TS_SUB, col_alpha(0xFFFFFFFF, a));
		}
		return;
	}
	vt_draw();
}

// botón grande con icono y texto; true si se tocó
// botón grande: arriba el icono (y al lado el botón de la consola que hace lo mismo), debajo el texto
static bool vw_button(float x, float y, float w, float h, IconId ic, const char *glyph, const char *label, bool on)
{
	bool p = on && touch_in(x, y, w, h);
	draw_rrect(x, y, w, h, 12, p ? T.surface2 : T.surface);
	u32 c = on ? T.text2 : col_alpha(T.text3, 0.5f);
	float gw = glyph ? text_width(glyph, TS_SUB) + 3 : 0;
	draw_icon(ic, x + w / 2 - gw / 2, y + h / 2 - 7, 15, c);
	if (glyph)
		text_draw(glyph, x + w / 2 - gw / 2 + 10, y + h / 2 - 7 - text_lineh(TS_SUB) / 2, TS_SUB, T.text3);
	text_draw_center_clip(label, x + w / 2, y + h / 2 + 4, w - 6, TS_SUB, on ? T.text3 : col_alpha(T.text3, 0.5f));
	return on && tap(x, y, w, h);
}

static void vw_save(bool force);

// ¿se está escribiendo desde la web en el documento abierto?
static bool live_here(char *who, size_t n)
{
	char p[700];
	char dev[48];
	if (VW.kind != VW_TXT || !live_active(dev, sizeof dev))
		return false;
	u32 rev;
	int cur;
	bool d, ok;
	free(live_get(0xFFFFFFFF, &rev, &cur, &d, &ok, p, sizeof p)); // solo la ruta
	if (strcmp(p, VW.path))
		return false;
	if (who)
		util_strlcpy(who, dev, n);
	return true;
}

static void vw_bottom_text(void)
{
	bool ed = vw_editable();
	int n = VW.doc.n;
	// estado: línea, sin guardar o por qué es de solo lectura
	char sub[128];
	u32 sc = T.text3;
	if (VW.prot)
		util_strlcpy(sub, _("Solo lectura: carpeta protegida"), sizeof sub), sc = T.warn;
	else if (VW.doc.ro == TXT_RO_BIN)
		util_strlcpy(sub, _("Solo lectura: no es un archivo de texto"), sizeof sub), sc = T.warn;
	else if (VW.doc.ro == TXT_RO_ENC)
		util_strlcpy(sub, _("Solo lectura: el texto no está en UTF-8"), sizeof sub), sc = T.warn;
	else if (VW.doc.ro == TXT_RO_BIG)
		util_strlcpy(sub, VW.doc.cut ? _("Solo lectura: archivo demasiado grande (se muestra el principio)") : _("Solo lectura: archivo demasiado grande"), sizeof sub), sc = T.warn;
	else if (live_here(NULL, 0)) { // alguien escribe desde el PC en este mismo documento
		char who[48];
		live_here(who, sizeof who);
		snprintf(sub, sizeof sub, _("Edición en directo: %s"), who), sc = T.accent;
	} else if (VW.doc.dirty)
		snprintf(sub, sizeof sub, _("Línea %d de %d"), VW.cur + 1, n), sc = T.warn; // sin guardar: también se enciende «Guardar»
	else
		snprintf(sub, sizeof sub, _("Línea %d de %d"), VW.cur + 1, n);
	// arriba a la derecha, pequeños: guardar (START), el menú del archivo (⋮) y cerrar (B)
	float my = 30, xx = BOT_W - 22, mx = xx - 32, svw = 58, svx = mx - 18 - svw;
	text_draw_clip(sub, 60, 32, svx - 66, TS_SUB, sc);
	bool save = false;
	if (ed) {
		bool dirty = VW.doc.dirty, sp = dirty && touch_in(svx, my - 13, svw, 26);
		draw_rrect(svx, my - 13, svw, 26, 13, dirty ? (sp ? col_lerp(T.accent, T.text, 0.15f) : T.accent) : T.surface);
		text_draw_center_clip(_("Guardar"), svx + svw / 2, my - text_lineh(TS_SUB) / 2, svw - 8, TS_SUB, dirty ? 0xFFFFFFFF : col_alpha(T.text3, 0.7f));
		save = dirty && tap(svx, my - 13, svw, 26);
	}
	bool mp = touch_in(mx - 15, my - 15, 30, 30), xp = touch_in(xx - 15, my - 15, 30, 30);
	draw_circle(mx, my, 13, mp ? T.surface2 : T.surface);
	draw_icon(IC_DOTS, mx, my, 15, T.text2);
	draw_circle(xx, my, 13, xp ? T.surface2 : T.surface);
	draw_icon(IC_CLOSE, xx, my, 12, T.text2);
	bool menu = tap(mx - 15, my - 15, 30, 30), close = tap(xx - 15, my - 15, 30, 30);
	// las líneas de alrededor: tocar una la elige; tocar la elegida la edita; arrastrar desplaza
	const float LY = 56, RH = 20;
	const int LR = ed ? 5 : 7;
	int first = VW.cur - 2;
	if (first > n - LR)
		first = n - LR;
	if (first < 0)
		first = 0;
	draw_rrect(10, LY - 3, BOT_W - 20, LR * RH + 6, 10, T.surface);
	float gw = text_width("0000", TS_SUB);
	gfx_clip(10, LY - 3, BOT_W - 20, LR * RH + 6);
	static char b[2400];
	int edit = -1;
	for (int k = 0; k < LR && first + k < n; k++) {
		int i = first + k;
		float y = LY + k * RH;
		if (i == VW.cur)
			draw_rrect(12, y, BOT_W - 24, RH, 8, col_alpha(T.accent, 0.2f));
		char num[12];
		snprintf(num, sizeof num, "%d", i + 1);
		text_draw_right(num, 16 + gw, y + 2, TS_SUB, i == VW.cur ? T.accent : T.text3);
		vt_line(i, b, 400);
		text_draw_clip(b, 24 + gw, y + 2, BOT_W - 48 - gw, TS_SUB, T.text);
		if (tap(10, y, BOT_W - 20, RH)) {
			if (i == VW.cur && ed)
				edit = i;
			VW.cur = i;
		}
	}
	gfx_clip_off();
	if (g_touch.held && g_touch.drag && g_touch.sy >= LY && g_touch.sy < LY + LR * RH) {
		VW.drag += g_touch.y - g_touch.py;
		while (VW.drag >= RH && VW.cur > 0)
			VW.drag -= RH, VW.cur--;
		while (VW.drag <= -RH && VW.cur < n - 1)
			VW.drag += RH, VW.cur++;
	} else {
		VW.drag = 0;
	}
	// botones: dos filas de cinco; al lado de cada icono, el botón de la consola que hace lo mismo
	enum { B_LINE, B_BLOCK, B_ADD, B_DEL, B_DUP, B_UNDO, B_REDO, B_UP, B_DOWN, B_FIND, B_PGUP, B_PGDN };
	const float BH = 34, BY = LY + LR * RH + 6, bw = (BOT_W - 20 - 4 * 5) / 5.0f;
	int hit = -1;
	struct {
		int id;
		IconId ic;
		const char *g, *t;
		bool on;
	} B[10];
	int nb = 0;
	if (ed) {
		B[nb++] = (typeof(B[0])){B_LINE, IC_PEN, GLYPH_A, _("Línea"), true};
		B[nb++] = (typeof(B[0])){B_BLOCK, IC_NOTE, "SELECT", _("Bloque"), true};
		B[nb++] = (typeof(B[0])){B_ADD, IC_PLUS, GLYPH_X, _("Añadir"), true};
		B[nb++] = (typeof(B[0])){B_DEL, IC_TRASH, GLYPH_Y, _("Borrar"), true};
		B[nb++] = (typeof(B[0])){B_DUP, IC_DUP, NULL, _("Duplicar"), true};
		B[nb++] = (typeof(B[0])){B_UNDO, IC_UNDO, GLYPH_L, _("Deshacer"), txt_can_undo(&VW.doc)};
		B[nb++] = (typeof(B[0])){B_REDO, IC_REDO, GLYPH_R, _("Rehacer"), txt_can_redo(&VW.doc)};
		B[nb++] = (typeof(B[0])){B_UP, IC_UP, NULL, _("Subir"), VW.cur > 0};
		B[nb++] = (typeof(B[0])){B_DOWN, IC_DOWN, NULL, _("Bajar"), VW.cur < n - 1};
		B[nb++] = (typeof(B[0])){B_FIND, IC_SEARCH, NULL, _("Buscar"), true};
	} else {
		B[nb++] = (typeof(B[0])){B_PGUP, IC_UP, GLYPH_DPAD, _("Página anterior"), true};
		B[nb++] = (typeof(B[0])){B_PGDN, IC_DOWN, GLYPH_DPAD, _("Página siguiente"), true};
		B[nb++] = (typeof(B[0])){B_FIND, IC_SEARCH, NULL, _("Buscar"), true};
	}
	float rbw = ed ? bw : (BOT_W - 20 - 2 * 5) / 3.0f;
	for (int k = 0; k < nb; k++) {
		int r = ed ? k / 5 : 0, c = ed ? k % 5 : k;
		if (vw_button(10 + c * (rbw + 5), BY + r * (BH + 4), rbw, BH, B[k].ic, B[k].g, B[k].t, B[k].on))
			hit = B[k].id;
	}
	// botones de la consola
	int page = vt_visible() - 1;
	if (g_krepeat & KEY_UP)
		VW.cur--;
	if (g_krepeat & KEY_DOWN)
		VW.cur++;
	if ((g_krepeat & KEY_LEFT) || hit == B_PGUP)
		VW.cur -= page, VW.top -= page;
	if ((g_krepeat & KEY_RIGHT) || hit == B_PGDN)
		VW.cur += page, VW.top += page;
	VW.cur = VW.cur < 0 ? 0 : VW.cur >= n ? n - 1 : VW.cur;
	VW.top = VW.top < 0 ? 0 : VW.top > VW.cur ? VW.cur : VW.top;
	int r = -1;
	if (ed && (hit == B_LINE || edit >= 0 || (g_kdown & KEY_A)))
		U.want_txt = 1;
	else if (ed && (hit == B_ADD || (g_kdown & KEY_X)))
		U.want_txt = 2;
	else if (ed && (hit == B_BLOCK || (g_kdown & KEY_SELECT)))
		U.want_txt = 3;
	else if (hit == B_FIND)
		U.want_txt = 4;
	else if (ed && (hit == B_DEL || (g_kdown & KEY_Y))) {
		txt_remove(&VW.doc, VW.cur);
		if (VW.cur >= VW.doc.n)
			VW.cur = VW.doc.n - 1;
	} else if (ed && hit == B_DUP) {
		if (txt_insert(&VW.doc, VW.cur + 1, VW.doc.ln[VW.cur]))
			VW.cur++;
	} else if (ed && (hit == B_UP || hit == B_DOWN)) {
		int d = hit == B_UP ? -1 : 1;
		if (txt_move(&VW.doc, VW.cur, d))
			VW.cur += d;
	} else if (ed && (hit == B_UNDO || (g_kdown & KEY_L))) {
		if ((r = txt_undo(&VW.doc)) >= 0)
			VW.cur = r < VW.doc.n ? r : VW.doc.n - 1;
	} else if (ed && (hit == B_REDO || (g_kdown & KEY_R))) {
		if ((r = txt_redo(&VW.doc)) >= 0)
			VW.cur = r < VW.doc.n ? r : VW.doc.n - 1;
	} else if (menu) {
		vw_menu();
	} else if (save || (ed && VW.doc.dirty && (g_kdown & KEY_START))) {
		vw_save(false);
	} else if (close || (g_kdown & KEY_B)) {
		vw_try_close();
	}
	if (VW.doc.pos != VW.hlpos) { // cambió el texto: los comentarios de varias líneas pueden moverse
		VW.hlpos = VW.doc.pos;
		VW.hlv = 0;
	}
}

static void vw_bottom(void)
{
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	FEnt ce = {0}; // (de la nube: su nombre y tamaño)
	if (VW.cloud)
		util_strlcpy(ce.name, VW.citem.name, sizeof ce.name), ce.size = VW.citem.size;
	const FEnt *e = VW.cloud ? &ce : &F.e[VW.idx];
	IconId ic;
	u32 col;
	f_kind(e, &ic, &col);
	draw_rrect(14, 14, 36, 36, 10, col_alpha(col, 0.18f));
	draw_icon(ic, 32, 32, 18, col);
	text_draw_clip(e->name, 60, 14, VW.kind == VW_TXT ? BOT_W - 210 : BOT_W - 74, TS_BODY, T.text);
	if (VW.kind == VW_TXT) {
		vw_bottom_text();
		return;
	}
	char sub[64], sz[24];
	util_fmt_size(sz, sizeof sz, e->size);
	if (VW.res == IMG_OK)
		snprintf(sub, sizeof sub, "%d × %d · %s", VW.ow, VW.oh, sz);
	else
		util_strlcpy(sub, sz, sizeof sub);
	text_draw_clip(sub, 60, 32, BOT_W - 74, TS_SUB, T.text3);
	// botones grandes: anterior, opciones y siguiente; debajo, cerrar
	const float BY = 72, BH = 52;
	struct { const char *t; IconId ic; } B[3] = {
		{_("Anterior"), IC_CHEV_LEFT}, {_("Opciones"), IC_MENU}, {_("Siguiente"), IC_CHEV_RIGHT}};
	float bw = (BOT_W - 40) / 3.0f;
	int hit = -1;
	for (int k = 0; k < 3; k++) {
		float bx = 14 + k * (bw + 6);
		bool p = touch_in(bx, BY, bw, BH);
		draw_rrect(bx, BY, bw, BH, 14, p ? T.surface2 : T.surface);
		draw_icon(B[k].ic, bx + bw / 2, BY + 18, 16, T.text2);
		text_draw_center_clip(B[k].t, bx + bw / 2, BY + 31, bw - 8, TS_SUB, T.text3);
		if (tap(bx, BY, bw, BH))
			hit = k;
	}
	float cy = BY + BH + 14;
	bool cp = touch_in(14, cy, BOT_W - 28, 40);
	draw_rrect(14, cy, BOT_W - 28, 40, 20, cp ? T.surface2 : T.surface);
	char cl[64];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	text_draw_center(cl, BOT_W / 2, cy + (40 - text_lineh(0.48f)) / 2, 0.48f, T.text);
	if (hit == 0 || (g_krepeat & KEY_LEFT))
		vw_step(-1);
	if (hit == 2 || (g_krepeat & KEY_RIGHT))
		vw_step(1);
	// deslizar el dedo para pasar de foto
	if (g_touch.up && g_touch.drag && fabsf((float)(g_touch.x - g_touch.sx)) > 50)
		vw_step(g_touch.x < g_touch.sx ? 1 : -1);
	if (hit == 1 || (g_kdown & KEY_Y)) {
		g_kdown &= ~KEY_Y;
		vw_menu();
	} else if (tap(14, cy, BOT_W - 28, 40) || (g_kdown & KEY_B)) {
		vw_close();
	}
}

static void vw_save(bool force)
{
	bool d;
	u64 sz = 0;
	// si alguien lo cambió desde la web mientras se editaba, se avisa antes de pisarlo
	if (!force && fsx_stat(VW.path, &d, &sz) == 0 && sz != VW.doc.size) {
		dialog_open(DLG_TXTOVER, _("El archivo se modificó mientras lo editabas"),
		            _("Se modificó desde otro dispositivo. Si guardas, esos cambios se perderán."), _("Guardar de todos modos"), true);
		return;
	}
	if (txt_save(&VW.doc, VW.path) != TXT_OK) {
		toast_icon(_("No se pudo guardar"), IC_CLOSE, T.danger);
		return;
	}
	if (fsx_stat(VW.path, &d, &sz) == 0)
		VW.doc.size = sz;
	if (VW.cloud) { // de la nube: se vuelve a subir, en lugar del que había
		cloud_upload_replace(VW.path + 5 * !strncmp(VW.path, "sdmc:", 5), VW.cparent);
		toast_icon(_("Guardado. Subiendo a OneDrive…"), IC_UPLOAD, T.ok);
		return;
	}
	state_lock();
	event_add(EV_EDIT, vw_name(), G.console, sz);
	G.rev++;
	state_unlock();
	toast_icon(_("Guardado"), IC_CHECK, T.ok);
}

// un CIA no se abre de un solo toque (su tarjeta es la de instalar): el primer toque lo marca
// y el segundo, ya marcado, lo abre. Con A se abre el marcado, como siempre
static int f_tap_open(int vi)
{
	const FEnt *e = &F.e[F.v[vi].idx];
	if (vi != F.sel && !e->dir && !strcasecmp(f_ext(e->name), "cia")) {
		F.sel = vi;
		return -1;
	}
	return vi;
}

// abrir: carpeta → entra; foto o texto → visor; CIA → su tarjeta de instalar; lo demás → opciones
static bool f_open_item(int vi)
{
	F.sel = vi;
	const FEnt *e = &F.e[F.v[vi].idx];
	char full[700];
	f_full(F.v[vi].idx, full, sizeof full);
	if (e->dir) {
		if (strlen(full) < sizeof F.path)
			f_load(full);
	} else if (!strcasecmp(f_ext(e->name), "html") || !strcasecmp(f_ext(e->name), "htm")) { // en el navegador
		br_open_file(full);
	} else if (rd_kind(full) >= 0 && rd_kind(full) != RK_TXT) { // PDF, CBZ, EPUB y DOCX: en el Lector
		rv_open(full, U.app);
	} else if (f_is_img(e) || f_is_txt(e)) {
		vw_load(F.v[vi].idx);
	} else if (!strcasecmp(f_ext(e->name), "zip")) {
		zs_open(full);
	} else if (music_is_audio(e->name) && !F.search) { // la carpeta es la lista, en el orden en que se ve
		static char names[500][256];
		int n = 0, start = 0;
		for (int i = 0; i < F.nv && n < 500; i++)
			if (F.v[i].kind == FV_ITEM && !F.e[F.v[i].idx].dir && music_is_audio(F.e[F.v[i].idx].name)) {
				if (i == vi)
					start = n;
				util_strlcpy(names[n++], F.e[F.v[i].idx].name, sizeof names[0]);
			}
		if (!music_play(F.path, names, n, start)) {
			MusicState ms;
			music_get(&ms);
			toast_icon(!strcmp(ms.err, "no_dsp") ? _("Sin sonido: falta el firmware DSP (dspfirm.cdc)") : _("No se pudo reproducir"), IC_CLOSE, T.danger);
		}
	} else if (!strcasecmp(f_ext(e->name), "cia") && strlen(full) < 256) {
		char err[24] = "";
		if (ins_request(full, G.console, false, err, sizeof err) < 0)
			toast_icon(!strcmp(err, "system_title") ? _("Por seguridad, no se instalan títulos del sistema") : !strcmp(err, "already") ? _("Ya está pendiente de instalación en la consola")
			                                                                                                  : _("La consola no reconoce este CIA"),
			           IC_CLOSE, T.danger);
		else
			ins_refresh(); // sale la tarjeta de instalar, con su icono y su versión
	} else {
		f_menu_open();
	}
	return true;
}

// ---- abrir lo de la nube como en Archivos: fotos y textos en el visor, música en el reproductor, páginas en el
// navegador, ZIP en su hoja y CIA instalándose. Se baja primero (a CLOUD_CACHE; un ZIP, a /Descargas, para
// extraerlo ahí) y se abre al terminar; mientras, una tarjeta con el progreso que se puede cancelar
enum { CO_NONE, CO_VIEW, CO_MUSIC, CO_HTML, CO_ZIP };
static struct {
	int kind;          // qué se hace al terminar de bajarlo
	CloudItem it;
	char parent[64];
	u32 seen;          // open_rev ya atendido
	int mu_wait;       // música: vueltas que se espera a que empiece a sonar para enseñar el reproductor
} CO;

static void cl_open_item(int i)
{
	if (i < 0 || i >= CLU.n)
		return;
	const CloudItem *c = &CLU.it[i];
	if (c->dir) {
		cloud_list(c->id, c->name);
		return;
	}
	const char *ext = f_ext(c->name);
	if (!strcasecmp(ext, "cia")) { // se instala mientras baja, como al pedirlo desde Descargas
		cloud_download(c);
		toast_icon(_("Instalando desde OneDrive. Puedes seguir el progreso en Descargas."), IC_DOWNLOAD, T.ok);
		return;
	}
	FEnt fe = {0};
	util_strlcpy(fe.name, c->name, sizeof fe.name);
	int k = !strcasecmp(ext, "html") || !strcasecmp(ext, "htm") ? CO_HTML
	        : f_is_img(&fe) || f_is_txt(&fe)                     ? CO_VIEW
	        : !strcasecmp(ext, "zip")                             ? CO_ZIP
	        : music_is_audio(c->name)                             ? CO_MUSIC
	                                                              : CO_NONE;
	if (k == CO_NONE) { // como en Archivos: lo que no se abre, sus opciones
		CLU.om = CLU.om_just = true, CLU.om_sel = 0, CLU.om_item = i;
		return;
	}
	CO.kind = k;
	CO.it = *c;
	util_strlcpy(CO.parent, cl_parent(), sizeof CO.parent);
	if (!cloud_open(c, k == CO_ZIP ? DL_DIR : CLOUD_CACHE)) {
		CO.kind = CO_NONE;
		toast_icon(_("Espera a que termine de abrirse el archivo anterior"), IC_CLOCK, T.text2);
	}
}

// cuando termina de bajarse, se abre
static void cl_open_poll(void)
{
	if (CLU.info.open_rev == CO.seen)
		return;
	CO.seen = CLU.info.open_rev;
	int k = CO.kind;
	CO.kind = CO_NONE;
	if (!k)
		return;
	if (!CLU.info.open_ok) {
		char m[220];
		snprintf(m, sizeof m, _("No se pudo abrir «%s»"), CO.it.name);
		toast_icon(m, IC_CLOSE, T.danger);
		return;
	}
	const char *p = CLU.info.open_path;
	if (k == CO_VIEW) {
		VW.cloud = true;
		VW.citem = CO.it;
		util_strlcpy(VW.cparent, CO.parent, sizeof VW.cparent);
		VW.idx = -1;
		vw_open(p, CO.it.name);
	} else if (k == CO_HTML)
		br_open_file(p);
	else if (k == CO_ZIP)
		zs_open(p);
	else {
		static char one[1][256];
		char dir[700];
		util_strlcpy(dir, p, sizeof dir);
		char *sl = strrchr(dir, '/');
		util_strlcpy(one[0], sl ? sl + 1 : dir, sizeof one[0]);
		if (sl)
			*sl = 0;
		if (music_play(dir, one, 1, 0))
			CO.mu_wait = 300; // (el reproductor sale en cuanto suene)
		else
			toast_icon(_("No se pudo reproducir"), IC_CLOSE, T.danger);
	}
}

// la tarjeta de «Abriendo…»: lo que lleva y B para cancelar
static void cl_open_overlay(void)
{
	if (!CLU.info.opening)
		return;
	draw_rect(0, 0, BOT_W, SCR_H, T.dim);
	float x = 16, w = BOT_W - 32, h = 112, y = (SCR_H - h) / 2;
	draw_rrect(x, y + 3, w, h, 18, T.shadow);
	draw_rrect(x, y, w, h, 18, T.surface);
	char t[220];
	snprintf(t, sizeof t, _("Abriendo «%s»"), CLU.info.open_name);
	text_draw_center_clip(t, BOT_W / 2, y + 14, w - 24, TS_BODY, T.text);
	u64 got = CLU.info.open_got, tot = CLU.info.open_size;
	float f = tot ? clampf((float)((double)got / tot), 0, 1) : 0;
	draw_rrect(x + 16, y + 44, w - 32, 6, 3, T.surface2);
	draw_grad_h(x + 16, y + 44, fmaxf(6, (w - 32) * f), 6, T.accent, T.accent2);
	char a[24], b[24], l[64];
	util_fmt_size(a, sizeof a, got), util_fmt_size(b, sizeof b, tot);
	snprintf(l, sizeof l, "%s / %s", a, b);
	text_draw_center(l, BOT_W / 2, y + 56, TS_SUB, T.text3);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cancelar"));
	float cw = text_width(cl, TS_SUB) + 28, cx = BOT_W / 2 - cw / 2, cy = y + h - 34;
	draw_rrect(cx, cy, cw, 24, 12, touch_in(cx, cy, cw, 24) ? T.divider : T.surface2);
	text_draw_center(cl, BOT_W / 2, cy + 12 - text_lineh(TS_SUB) / 2, TS_SUB, T.text);
	if ((g_kdown & KEY_B) || tap(cx, cy, cw, 24)) {
		cloud_open_cancel();
		CO.kind = CO_NONE;
	}
	g_kdown = 0;
	g_touch.up = false;
}

static void cl_open_abort(void)
{
	if (CO.kind != CO_NONE) {
		CO.kind = CO_NONE;
		cloud_open_cancel();
	}
}

static int cl_index_of(const char *id)
{
	for (int i = 0; i < CLU.n; i++)
		if (!strcmp(CLU.it[i].id, id))
			return i;
	return -1;
}

static void cl_view_step(int d)
{
	int i = cl_index_of(VW.citem.id);
	if (i < 0)
		return;
	for (int k = 1; k < CLU.n; k++) {
		int j = (i + d * k + CLU.n * 4) % CLU.n;
		FEnt fe = {0};
		util_strlcpy(fe.name, CLU.it[j].name, sizeof fe.name);
		if (!CLU.it[j].dir && f_is_img(&fe)) {
			CLU.l.sel = j;
			cl_open_item(j);
			return;
		}
	}
}

static void cl_view_menu(void)
{
	int i = cl_index_of(VW.citem.id);
	if (i >= 0)
		CLU.om = CLU.om_just = true, CLU.om_sel = 0, CLU.om_item = i;
}

// ---- miniaturas para la lista y la vista de iconos: fotos e iconos de CIA y 3DSX.
// Se leen en otro hilo, de una en una, para que desplazarse no se trabe; la textura se crea aquí
#define TH_N 48  // cuántas se guardan (las menos usadas se van)
#define TH_S 56  // tamaño máximo
enum { TK_NONE, TK_IMG, TK_CIA, TK_3DSX, TK_TITLE };
static struct {
	char name[128];
	u64 mtime;
	Texture t;
	bool fail, pend;
	u32 used;
} TH[TH_N];
static char th_dir[512];
static u32 th_frame;
static struct {
	Thread thr;
	LightEvent ev;
	volatile bool quit;
	volatile int st; // 0 libre, 1 pedido, 2 listo
	char path[700];
	int kind, slot;
	bool stale;      // se cambió de carpeta mientras se leía: se tira
	u8 *rgba;
	u16 px[48 * 48];
	int w, h;
	bool ok;
} TJ;

static void th_thread(void *arg)
{
	(void)arg;
	while (!TJ.quit) {
		LightEvent_Wait(&TJ.ev);
		if (TJ.quit)
			break;
		if (TJ.st != 1)
			continue;
		TJ.ok = false;
		TJ.rgba = NULL;
		if (TJ.kind == TK_IMG) {
			int ow, oh;
			TJ.ok = img_load_fit(TJ.path, TH_S, TH_S, &TJ.rgba, &TJ.w, &TJ.h, &ow, &oh) == IMG_OK;
		} else if (TJ.kind == TK_CIA) {
			TJ.ok = ins_icon(TJ.path, TJ.px);
		} else if (TJ.kind == TK_TITLE) {
			TJ.ok = title_icon(strtoull(TJ.path + 2, NULL, 16), TJ.px);
		} else {
			char t[64], p[64];
			TJ.ok = smdh_3dsx(TJ.path, t, sizeof t, p, sizeof p, TJ.px);
		}
		TJ.st = 2;
	}
}

static void th_exit(void)
{
	if (!TJ.thr)
		return;
	TJ.quit = true;
	LightEvent_Signal(&TJ.ev);
	threadJoin(TJ.thr, 2000000000ull);
	threadFree(TJ.thr);
	TJ.thr = NULL;
	free(TJ.rgba);
	for (int i = 0; i < TH_N; i++)
		tex_free(&TH[i].t);
}

static int th_kind(const FEnt *e)
{
	if (F.titles)
		return TK_TITLE;
	if (e->dir)
		return TK_NONE;
	const char *x = f_ext(e->name);
	return f_is_img(e) ? TK_IMG : !strcasecmp(x, "cia") ? TK_CIA : !strcasecmp(x, "3dsx") ? TK_3DSX : TK_NONE;
}

// una vez por frame: recoge lo leído y, si la carpeta cambió, olvida lo de la anterior
static void th_poll(void)
{
	th_frame++;
	if (strcmp(th_dir, F.path)) {
		util_strlcpy(th_dir, F.path, sizeof th_dir);
		for (int i = 0; i < TH_N; i++) {
			tex_free(&TH[i].t);
			memset(&TH[i], 0, sizeof TH[i]);
		}
		if (TJ.st)
			TJ.stale = true;
	}
	if (TJ.st != 2)
		return;
	if (!TJ.stale && TH[TJ.slot].pend) {
		TH[TJ.slot].pend = false;
		bool ok = TJ.ok && (TJ.kind == TK_IMG ? tex_from_rgba(&TH[TJ.slot].t, TJ.rgba, TJ.w, TJ.h) : tex_from_rgb565(&TH[TJ.slot].t, TJ.px, 48, 48));
		TH[TJ.slot].fail = !ok;
	}
	free(TJ.rgba);
	TJ.rgba = NULL;
	TJ.stale = false;
	TJ.st = 0;
}

// la miniatura de un elemento a la vista; si no está, se pide y por ahora sale su icono
static const Texture *th_get(int idx)
{
	const FEnt *e = &F.e[idx];
	int kind = th_kind(e);
	if (!kind)
		return NULL;
	int lru = -1;
	for (int i = 0; i < TH_N; i++) {
		if (TH[i].name[0] && !strcmp(TH[i].name, e->name) && TH[i].mtime == e->mtime) {
			TH[i].used = th_frame;
			return TH[i].t.ok ? &TH[i].t : NULL;
		}
		if (!TH[i].pend && (lru < 0 || TH[i].used < TH[lru].used))
			lru = i;
	}
	if (TJ.st || lru < 0)
		return NULL; // ya hay una leyéndose
	if (!TJ.thr) {
		LightEvent_Init(&TJ.ev, RESET_ONESHOT);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		TJ.thr = threadCreate(th_thread, NULL, 64 * 1024, prio + 3, -2, false);
		if (!TJ.thr)
			return NULL;
	}
	tex_free(&TH[lru].t);
	util_strlcpy(TH[lru].name, e->name, sizeof TH[lru].name);
	TH[lru].mtime = e->mtime;
	TH[lru].fail = false;
	TH[lru].pend = true;
	TH[lru].used = th_frame;
	f_full(idx, TJ.path, sizeof TJ.path);
	TJ.kind = kind;
	TJ.slot = lru;
	TJ.st = 1;
	LightEvent_Signal(&TJ.ev);
	return NULL;
}

// ---- vista y orden del explorador (se guardan en la configuración)
static struct {
	bool open, just;
	float anim;
	int sel; // fila: 0 vista, 1 ordenar, 2 agrupar, 3 cerrar
} VS;

static void f_toggle(bool *gfield)
{
	state_lock();
	*gfield = !*gfield;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
	f_reload_keep();
}

static void f_set_view(int sort, int desc, int group, int grid)
{
	state_lock();
	if (sort >= 0)
		G.f_sort = (u8)sort;
	if (desc >= 0)
		G.f_desc = desc;
	if (group >= 0)
		G.f_group = (u8)group;
	if (grid >= 0)
		G.f_grid = grid;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
	f_reload_keep();
}

// fila de opciones a elegir una (como pestañas); devuelve la tocada o -1
static int segmented(float x, float y, float w, float h, const char *const *opts, int n, int cur, bool focus)
{
	draw_rrect(x, y, w, h, h / 2, T.bg);
	if (focus)
		draw_rrect_outline(x, y, w, h, h / 2, 1.5f, T.accent);
	float sw = w / n;
	int hit = -1;
	for (int k = 0; k < n; k++) {
		float sx = x + k * sw;
		if (k == cur)
			draw_rrect(sx + 2, y + 2, sw - 4, h - 4, (h - 4) / 2, T.accent);
		else if (touch_in(sx, y, sw, h))
			draw_rrect(sx + 2, y + 2, sw - 4, h - 4, (h - 4) / 2, T.surface2);
		text_draw_center_clip(opts[k], sx + sw / 2, y + (h - text_lineh(TS_SUB)) / 2, sw - 6, TS_SUB, k == cur ? 0xFFFFFFFF : T.text2);
		if (tap(sx, y, sw, h))
			hit = k;
	}
	return hit;
}

// botón que se enciende y apaga (para «Sistema aparte» y «Destacadas arriba»)
static bool switch_chip(float x, float y, float w, float h, const char *label, bool on, bool focus)
{
	bool p = touch_in(x, y, w, h);
	draw_rrect(x, y, w, h, h / 2, on ? col_alpha(T.accent, 0.22f) : p ? T.surface2 : T.bg);
	if (focus)
		draw_rrect_outline(x, y, w, h, h / 2, 1.5f, T.accent);
	float cx = x + 16, cy = y + h / 2;
	draw_circle(cx, cy, 7, on ? T.accent : T.surface2);
	if (on)
		draw_icon(IC_CHECK, cx, cy, 9, 0xFFFFFFFF);
	text_draw_clip(label, x + 28, cy - text_lineh(TS_SUB) / 2, w - 34, TS_SUB, on ? T.text : T.text2);
	return tap(x, y, w, h);
}

static void vs_draw(void)
{
	VS.anim = approach(VS.anim, VS.open ? 1 : 0, 16);
	if (VS.anim < 0.01f)
		return;
	float a = ease_out_cubic(VS.anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	const float X = 22, W = BOT_W - 44, SH = 24, LH = text_lineh(TS_SUB) + 1, RH = LH + SH + 6, TH = 28;
	float h = 36 + 2 * RH + TH + 8 + 42, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rrect(10, y + 3, BOT_W - 20, h, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, h, 16, T.surface);
	if (!VS.open)
		return;
	bool fresh = VS.just;
	VS.just = false;
	// filas para la cruceta: 0 vista, 1 ordenar, 2 agrupar, 3 interruptores, 4 cerrar
	if (U.buttons && !fresh) {
		if (g_krepeat & KEY_DOWN) VS.sel = (VS.sel + 1) % 5;
		if (g_krepeat & KEY_UP) VS.sel = (VS.sel + 4) % 5;
	}
	int lr = U.buttons && !fresh ? ((g_krepeat & KEY_RIGHT) ? 1 : (g_krepeat & KEY_LEFT) ? -1 : 0) : 0;
	bool A = U.buttons && !fresh && (g_kdown & KEY_A);
	// cabecera: título y, a la derecha, lista o iconos
	text_draw(_("Vista y orden"), X, y + 12, TS_BODY, T.text);
	const char *VIEW[2] = {_("Lista"), _("Iconos")};
	int hit = segmented(X + W - 130, y + 8, 130, SH, VIEW, 2, V.f_grid, U.buttons && VS.sel == 0);
	if (!fresh && (hit >= 0 || (VS.sel == 0 && (lr || A))))
		f_set_view(-1, -1, -1, hit >= 0 ? hit : !V.f_grid);
	float ry = y + 38;
	// ordenar por, y al lado el sentido
	const char *SORT[4] = {_("Nombre"), _("Fecha"), _("Tamaño"), _("Tipo")};
	text_draw(_("Ordenar por"), X, ry, TS_SUB, T.text3);
	float dw = 40;
	hit = segmented(X, ry + LH, W - dw - 6, SH, SORT, 4, V.f_sort, U.buttons && VS.sel == 1);
	if (!fresh && (hit >= 0 || (VS.sel == 1 && lr)))
		f_set_view(hit >= 0 ? hit : (V.f_sort + lr + 4) % 4, -1, -1, -1);
	float dx = X + W - dw;
	bool dp = touch_in(dx, ry + LH, dw, SH);
	draw_rrect(dx, ry + LH, dw, SH, SH / 2, dp ? T.surface2 : T.bg);
	draw_icon(V.f_desc ? IC_DOWN : IC_UP, dx + dw / 2, ry + LH + SH / 2, 14, T.accent);
	if (!fresh && (tap(dx, ry + LH, dw, SH) || (VS.sel == 1 && A)))
		f_set_view(-1, !V.f_desc, -1, -1);
	ry += RH;
	// agrupar por
	const char *GROUP[5] = {_("Ninguno"), _("Tipo"), _("Fecha"), _("Tamaño"), _("Color")};
	text_draw(_("Agrupar por"), X, ry, TS_SUB, T.text3);
	hit = segmented(X, ry + LH, W, SH, GROUP, 5, V.f_group, U.buttons && VS.sel == 2);
	if (!fresh && (hit >= 0 || (VS.sel == 2 && lr)))
		f_set_view(-1, -1, hit >= 0 ? hit : (V.f_group + lr + 5) % 5, -1);
	ry += RH + 2;
	// interruptores
	static int col;
	if (VS.sel == 3 && lr)
		col = !col;
	float hw = (W - 6) / 2;
	if (!fresh && (switch_chip(X, ry, hw, TH, _("Separar sistema"), V.f_sysapart, U.buttons && VS.sel == 3 && col == 0) || (VS.sel == 3 && col == 0 && A)))
		f_toggle(&G.f_sysapart);
	if (!fresh && (switch_chip(X + hw + 6, ry, hw, TH, _("Destacados primero"), V.f_pin, U.buttons && VS.sel == 3 && col == 1) || (VS.sel == 3 && col == 1 && A)))
		f_toggle(&G.f_pin);
	// cerrar
	float by = y + h - 38;
	bool bp = touch_in(X, by, W, 30);
	draw_rrect(X, by, W, 30, 15, bp || (U.buttons && VS.sel == 4) ? T.surface2 : T.bg);
	if (U.buttons && VS.sel == 4)
		draw_rrect_outline(X, by, W, 30, 15, 1.5f, T.accent);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	text_draw_center(cl, BOT_W / 2, by + (30 - text_lineh(TS_BODY)) / 2, TS_BODY, T.text);
	if (!fresh && (tap(X, by, W, 30) || (VS.sel == 4 && A) || (g_kdown & KEY_B) || (g_touch.up && !g_touch.drag && g_touch.sy < y)))
		VS.open = false;
	g_kdown &= ~(KEY_A | KEY_B);
	g_touch.up = false;
}

// menú pequeño que sale del botón «+» de arriba: carpeta o documento de texto, y Cerrar
static struct {
	bool open, just;
	float anim;
	int sel;
} PM;

// escanear un QR con la cámara (como en FBI): arriba la imagen; abajo lo leído y qué hacer con ello
#define QRS_URLS 8
static struct {
	bool open, inner;
	int st;              // 0 buscando, 1 leído
	char text[1024];
	char url[QRS_URLS][1024];
	int nurl;
	u16 *px;
	u32 seen;
	Texture tex;
	float flash;
	char dest[512];
} QRS;

static bool qrs_is_open(void)
{
	return QRS.open;
}

// dónde se guarda lo que se descarga: la carpeta abierta en Archivos; desde la raíz o desde
// otra pestaña, la raíz, y los .cia (o las releases de GitHub) en /cias
// adónde va lo que se descarga: siempre a la carpeta de descargas (la que enseña la app Descargas)
static const char *dl_dest_dir(const char *url)
{
	(void)url;
	fsx_mkdirs(DL_DIR);
	return DL_DIR;
}

static void qrs_open(void)
{
	WHERE("abrir el escáner de QR");
	if (!QRS.px)
		QRS.px = malloc(QRCAM_W * QRCAM_H * 2);
	if (!QRS.px)
		return;
	memset(QRS.px, 0, QRCAM_W * QRCAM_H * 2);
	QRS.seen = 0;
	QRS.st = 0;
	QRS.flash = 0;
	QRS.open = true;
	qrcam_start(QRS.inner);
}

static void qrs_close(void)
{
	qrcam_stop();
	tex_free(&QRS.tex);
	free(QRS.px);
	QRS.px = NULL;
	QRS.open = false;
}

// lo leído: los enlaces que traiga (separados por espacios o líneas)
static void qrs_parse(const char *t)
{
	util_strlcpy(QRS.text, t, sizeof QRS.text);
	QRS.nurl = 0;
	const char *p = t;
	while (*p && QRS.nurl < QRS_URLS) {
		while (*p && (unsigned char)*p <= ' ')
			p++;
		const char *e = p;
		while (*e && (unsigned char)*e > ' ')
			e++;
		char u[1024];
		snprintf(u, sizeof u, "%.*s", (int)(e - p), p);
		if (dl_url_ok(u))
			util_strlcpy(QRS.url[QRS.nurl++], u, sizeof QRS.url[0]);
		p = e;
	}
	// dónde se guarda: la carpeta abierta; desde la raíz, los .cia van a /cias
	bool cia = false;
	for (int i = 0; i < QRS.nurl; i++) {
		size_t l = strcspn(QRS.url[i], "?#");
		cia |= (l > 4 && !strncasecmp(QRS.url[i] + l - 4, ".cia", 4)) || strstr(QRS.url[i], "github.com/");
	}
	const char *dir = U.tab != TAB_FILES || F.trash || F.search || F.titles || F.nosaves || F.missing ? "/" : F.path;
	(void)dir, (void)cia;
	util_strlcpy(QRS.dest, dl_dest_dir(""), sizeof QRS.dest);
}

static bool qrs_cia(void)
{
	for (int i = 0; i < QRS.nurl; i++) {
		size_t l = strcspn(QRS.url[i], "?#");
		if ((l > 4 && !strncasecmp(QRS.url[i] + l - 4, ".cia", 4)) || strstr(QRS.url[i], "github.com/"))
			return true;
	}
	return false;
}

static void qrs_download(bool install)
{
	int ok = 0;
	for (int i = 0; i < QRS.nurl; i++)
		ok += dl_add(QRS.url[i], QRS.dest, "", install ? DLA_INSTALL | DLA_STREAM : DLA_NONE, G.console, NULL) == 0; // pedido aquí: se instala mientras baja
	char m[120];
	snprintf(m, sizeof m, ok == 1 ? _("Descargando en Descargas (/Downloads)") : _("Descargando %d enlaces en Descargas (/Downloads)"), ok);
	toast_icon(ok ? m : _("El enlace no es válido"), ok ? IC_GLOBE : IC_CLOSE, ok ? T.accent : T.danger);
	qrs_close();
}

// pantalla de arriba: la cámara a pantalla completa, con el recuadro donde poner el código
static void qr_top(void)
{
	if (QRS.px && qrcam_frame(QRS.px, &QRS.seen))
		tex_from_rgb565(&QRS.tex, QRS.px, QRCAM_W, QRCAM_H);
	draw_rect(0, 0, TOP_W, SCR_H, 0xFF000000);
	if (QRS.tex.ok)
		draw_texture(&QRS.tex, 0, 0, TOP_W, SCR_H, 1);
	Result err;
	int cs = qrcam_state(&err);
	const float S = 170, x0 = (TOP_W - S) / 2, y0 = (SCR_H - S) / 2 - 8;
	u32 dim = col_alpha(0xFF000000, 0.45f);
	draw_rect(0, 0, TOP_W, y0, dim);
	draw_rect(0, y0 + S, TOP_W, SCR_H - y0 - S, dim);
	draw_rect(0, y0, x0, S, dim);
	draw_rect(x0 + S, y0, TOP_W - x0 - S, S, dim);
	QRS.flash = approach(QRS.flash, QRS.st == 1 ? 1 : 0, 8);
	u32 bc = col_lerp(0xFFFFFFFF, T.ok, QRS.flash);
	const float L = 26, Wd = 3 + QRS.flash * 2;
	draw_rect(x0, y0, L, Wd, bc), draw_rect(x0, y0, Wd, L, bc);
	draw_rect(x0 + S - L, y0, L, Wd, bc), draw_rect(x0 + S - Wd, y0, Wd, L, bc);
	draw_rect(x0, y0 + S - Wd, L, Wd, bc), draw_rect(x0, y0 + S - L, Wd, L, bc);
	draw_rect(x0 + S - L, y0 + S - Wd, L, Wd, bc), draw_rect(x0 + S - Wd, y0 + S - L, Wd, L, bc);
	const char *msg = cs == QRC_FAIL ? _("No se pudo abrir la cámara") : cs == QRC_STARTING ? _("Abriendo la cámara…")
	                : QRS.st == 1 ? _("Código leído") : _("Apunta la cámara al código QR");
	float mw = text_width(msg, TS_BODY) + 28;
	draw_rrect(TOP_W / 2 - mw / 2, SCR_H - 34, mw, 24, 12, col_alpha(0xFF000000, 0.6f));
	text_draw_center(msg, TOP_W / 2, SCR_H - 34 + 12 - text_lineh(TS_BODY) / 2, TS_BODY, QRS.st == 1 ? T.ok : cs == QRC_FAIL ? T.danger : 0xFFFFFFFF);
}

static void qrs_draw(void)
{
	if (!QRS.open)
		return;
	if (QRS.st == 0) {
		char t[1024];
		if (qrcam_take(t, sizeof t)) {
			qrs_parse(t);
			QRS.st = 1;
		}
	}
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	// cabecera: título y cerrar
	text_draw(_("Escanear QR"), 14, 7, TS_TITLE, T.text);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	float cw = text_width(cl, TS_SUB) + 20, cx = BOT_W - 10 - cw;
	draw_rrect(cx, 6, cw, 20, 10, touch_in(cx, 4, cw, 24) ? T.surface2 : T.surface);
	text_draw_center(cl, cx + cw / 2, 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.text2);
	if (tap(cx, 4, cw, 24) || (g_kdown & KEY_B)) {
		qrs_close();
		g_kdown = 0;
		return;
	}
	float y = 40;
	if (QRS.st == 0) {
		Result err;
		int cs = qrcam_state(&err);
		draw_circle(BOT_W / 2, y + 40, 26, T.surface);
		draw_icon(IC_QR, BOT_W / 2, y + 40, 26, cs == QRC_FAIL ? T.danger : T.accent);
		if (cs == QRC_FAIL) {
			char e[160];
			snprintf(e, sizeof e, _("No se pudo abrir la cámara (error %08lX). Ciérrala en otras aplicaciones y vuelve a intentarlo."), (unsigned long)err);
			wrap_center(e, BOT_W / 2, y + 80, BOT_W - 40, TS_SUB, 4, T.text3);
		} else {
			text_draw_center(_("Buscando un código QR…"), BOT_W / 2, y + 78, TS_BODY, T.text);
			wrap_center(_("Admite cualquier código QR con un enlace, como los de FBI. Los archivos .cia se instalan durante la descarga."),
			            BOT_W / 2, y + 100, BOT_W - 40, TS_SUB, 3, T.text3);
		}
		char sw[64];
		snprintf(sw, sizeof sw, "%s %s", GLYPH_X, QRS.inner ? _("Cámara exterior") : _("Cámara interior"));
		float sx = BOT_W / 2 - 80;
		draw_rrect(sx, SCR_H - 50, 160, 30, 15, touch_in(sx, SCR_H - 50, 160, 30) ? T.surface2 : T.surface);
		text_draw_center(sw, BOT_W / 2, SCR_H - 35 - text_lineh(TS_SUB) / 2, TS_SUB, T.text2);
		if (tap(sx, SCR_H - 50, 160, 30) || (g_kdown & KEY_X)) {
			QRS.inner = !QRS.inner;
			qrcam_start(QRS.inner);
		}
		g_kdown = 0;
		g_touch.up = false;
		return;
	}
	// leído: los enlaces y adónde van
	if (QRS.nurl) {
		text_draw(QRS.nurl == 1 ? _("Enlace leído") : _("Enlaces leídos"), 14, y, TS_SUB, T.text3);
		y += 16;
		for (int i = 0; i < QRS.nurl && i < 3; i++, y += 18)
			text_draw_clip(QRS.url[i], 14, y, BOT_W - 28, TS_SUB, T.accent);
		if (QRS.nurl > 3) {
			char more[48];
			snprintf(more, sizeof more, _("y %d más"), QRS.nurl - 3);
			text_draw(more, 14, y, TS_SUB, T.text3);
			y += 18;
		}
		char d[600];
		snprintf(d, sizeof d, _("Se guardará en %s"), QRS.dest);
		text_draw_clip(d, 14, y + 4, BOT_W - 28, TS_SUB, T.text2);
	} else {
		text_draw(_("El código no contiene ningún enlace"), 14, y, TS_SUB, T.text3);
		wrap_center(QRS.text, BOT_W / 2, y + 22, BOT_W - 28, TS_BODY, 4, T.text);
	}
	// botones: A descargar (e instalar), X solo descargar, Y otro
	bool cia = qrs_cia();
	struct { const char *g, *t; u32 bg, fg; bool on; u32 key; } B[3] = {
		{GLYPH_A, cia ? _("Descargar e instalar") : _("Descargar"), T.accent, 0xFFFFFFFF, QRS.nurl > 0, KEY_A},
		{GLYPH_X, _("Solo descargar"), T.surface2, T.text, QRS.nurl > 0 && cia, KEY_X},
		{GLYPH_Y, _("Escanear otro"), T.surface2, T.text, true, KEY_Y}};
	float by = SCR_H - 12 - 3 * 32;
	for (int i = 0; i < 3; i++) {
		if (!B[i].on)
			continue;
		bool p = touch_in(14, by, BOT_W - 28, 28);
		draw_rrect(14, by, BOT_W - 28, 28, 14, p ? col_lerp(B[i].bg, T.text, 0.12f) : B[i].bg);
		char t[96];
		snprintf(t, sizeof t, "%s %s", B[i].g, B[i].t);
		text_draw_center(t, BOT_W / 2, by + 14 - text_lineh(TS_BODY) / 2, TS_BODY, B[i].fg);
		if (tap(14, by, BOT_W - 28, 28) || (g_kdown & B[i].key)) {
			g_kdown = 0;
			if (i == 0)
				qrs_download(cia);
			else if (i == 1)
				qrs_download(false);
			else
				QRS.st = 0;
			return;
		}
		by += 32;
	}
	g_kdown = 0;
	g_touch.up = false;
}

static void f_new_menu(void)
{
	PM.open = true;
	PM.just = true;
	PM.sel = 0;
}

static void pm_draw(void)
{
	PM.anim = approach(PM.anim, PM.open ? 1 : 0, 18);
	if (PM.anim < 0.01f)
		return;
	float a = ease_out_cubic(PM.anim);
	draw_rect(0, VIEW_Y, BOT_W, SCR_H - VIEW_Y, col_alpha(T.dim, a * 0.75f));
	enum { N = 2 };
	static const IconId IC[N] = {IC_FOLDER, IC_NOTE};
	const u32 C[N] = {T.warn, T.accent};
	const char *L[N] = {_("Carpeta nueva"), _("Documento de texto")};
	char where[160];
	const char *nm = strrchr(F.path, '/');
	snprintf(where, sizeof where, _("En «%s»"), nm && nm[1] ? nm + 1 : "SD");
	const char *S[N] = {where, _("Archivo .txt en blanco")};
	const float W = 214, RH = 40, x = BOT_W - 8 - W, y = 34 - (1 - a) * 8, H = N * RH + 12;
	draw_rrect(x, y + 4, W, H, 14, col_alpha(T.shadow, a));
	draw_rrect(x, y, W, H, 14, T.surface);
	if (!PM.open)
		return;
	bool fresh = PM.just;
	PM.just = false;
	if (fresh)
		return; // el toque que lo abrió no cuenta
	if (U.buttons) {
		if (g_krepeat & (KEY_DOWN | KEY_UP))
			PM.sel = (PM.sel + 1) % N;
	}
	int run = -1;
	for (int i = 0; i < N; i++) {
		float ry = y + 6 + i * RH;
		bool hl = touch_in(x + 6, ry, W - 12, RH) || (U.buttons && PM.sel == i);
		if (hl)
			draw_rrect(x + 6, ry, W - 12, RH, 10, T.surface2);
		draw_rrect(x + 14, ry + 6, 28, 28, 9, col_alpha(C[i], 0.16f));
		draw_icon(IC[i], x + 28, ry + 20, 14, C[i]);
		text_draw_clip(L[i], x + 52, ry + 5, W - 62, 0.44f, T.text);
		text_draw_clip(S[i], x + 52, ry + 22, W - 62, TS_SUB, T.text3);
		if (tap(x + 6, ry, W - 12, RH) || (U.buttons && PM.sel == i && (g_kdown & KEY_A)))
			run = i;
	}
	if ((g_kdown & (KEY_B | KEY_X)) || (g_touch.up && !g_touch.drag && !(g_touch.sx >= x && g_touch.sx < x + W && g_touch.sy >= y && g_touch.sy < y + H)))
		PM.open = false;
	else if (run == 0)
		PM.open = false, U.want_fnew = true;
	else if (run == 1)
		PM.open = false, U.want_tnew = true;
	g_kdown &= ~(KEY_A | KEY_B | KEY_X);
	g_touch.up = false;
}

static bool ms_has(const char *nm)
{
	for (int i = 0; i < MS.n; i++)
		if (!strcmp(MS.nm[i], nm))
			return true;
	return false;
}

static void ms_toggle(const char *nm)
{
	for (int i = 0; i < MS.n; i++)
		if (!strcmp(MS.nm[i], nm)) {
			memcpy(MS.nm[i], MS.nm[--MS.n], sizeof MS.nm[0]);
			return;
		}
	if (!MS.nm && !(MS.nm = malloc(F_MAX * sizeof *MS.nm)))
		return;
	if (MS.n < F_MAX)
		util_strlcpy(MS.nm[MS.n++], nm, sizeof MS.nm[0]);
}

static void ms_start(const char *first)
{
	MS.on = true;
	MS.n = 0;
	if (first)
		ms_toggle(first);
}

static void ms_end(void)
{
	MS.on = false;
	MS.n = 0;
}

// todo o nada: si ya está todo elegido, se desmarca
static void ms_all(void)
{
	if (MS.n >= F.n) {
		MS.n = 0;
		return;
	}
	for (int i = 0; i < F.n; i++)
		if (!ms_has(F.e[i].name))
			ms_toggle(F.e[i].name);
}

static u64 ms_size(int *ndir)
{
	u64 t = 0;
	*ndir = 0;
	for (int i = 0; i < F.n; i++)
		if (ms_has(F.e[i].name)) {
			t += F.e[i].dir ? 0 : F.e[i].size;
			*ndir += F.e[i].dir;
		}
	return t;
}

static int ms_cias(void)
{
	int n = 0;
	for (int i = 0; i < F.n; i++)
		n += !F.e[i].dir && ms_has(F.e[i].name) && !strcasecmp(f_ext(F.e[i].name), "cia");
	return n;
}

// ¿hay algo protegido entre lo elegido? (no se mueve ni se borra)
static bool ms_locked(void)
{
	for (int i = 0; i < F.n; i++)
		if (F.e[i].sys && V.protect_sys && ms_has(F.e[i].name))
			return true;
	return false;
}

static void ms_copy_move(int mode)
{
	if (!MS.n)
		return;
	free(FP.list);
	FP.list = malloc(MS.n * sizeof *FP.list);
	if (!FP.list)
		return;
	FP.n = 0;
	FP.src_dir = false;
	for (int i = 0; i < F.n; i++)
		if (ms_has(F.e[i].name)) {
			f_full(i, FP.list[FP.n], sizeof FP.list[0]);
			FP.src_dir |= F.e[i].dir;
			if (!FP.n)
				util_strlcpy(FP.src, FP.list[0], sizeof FP.src), util_strlcpy(FP.name, F.e[i].name, sizeof FP.name);
			FP.n++;
		}
	FP.mode = mode;
	CC.mode = CC.n = 0; // (un solo portapapeles: lo copiado en la nube se sustituye)
	ms_end();
	toast_icon(_("Copiado al portapapeles. Abre otra carpeta y pulsa X para pegar."), IC_DUP, T.accent);
}

static void ms_install(void)
{
	int n = 0;
	for (int i = 0; i < F.n; i++) {
		if (F.e[i].dir || !ms_has(F.e[i].name) || strcasecmp(f_ext(F.e[i].name), "cia"))
			continue;
		char full[700], err[24] = "";
		f_full(i, full, sizeof full);
		int id = strlen(full) < 256 ? ins_request(full, G.console, false, err, sizeof err) : -1;
		if (id >= 0) {
			ins_decide(id, INS_ACCEPT); // Cloud3DS y bajar de versión se quedan guardados, como siempre
			n++;
		}
	}
	char msg[64];
	snprintf(msg, sizeof msg, _("Instalando %d CIA…"), n);
	toast_icon(n ? msg : _("La consola no reconoce este CIA"), n ? IC_DOWNLOAD : IC_CLOSE, n ? T.ok : T.danger);
	ms_end();
}

static void ms_color(int c)
{
	state_lock();
	for (int i = 0; i < F.n; i++)
		if (ms_has(F.e[i].name)) {
			char full[700];
			f_full(i, full, sizeof full);
			mark_set(full, c);
		}
	G.rev++;
	state_unlock();
	ms_end();
	f_reload_keep();
}

static void ms_ask_delete(void)
{
	if (!MS.n)
		return;
	if (ms_locked()) {
		toast_icon(_("La selección incluye carpetas protegidas"), IC_LOCK, T.warn);
		return;
	}
	char msg[200];
	if (MS.n == 1)
		snprintf(msg, sizeof msg, _("1 elemento irá a la papelera. Podrás recuperarlo durante %d días."), TRASH_DAYS);
	else
		snprintf(msg, sizeof msg, _("%d elementos irán a la papelera. Podrás recuperarlos durante %d días."), MS.n, TRASH_DAYS);
	dialog_open(DLG_FDELMULTI, _("¿Eliminar?"), msg, _("Eliminar"), true);
}

// menú de acciones de la selección (Y) y colores para destacar varios a la vez
static struct {
	bool open, just;
	float anim;
	int sel;
} MA, MC;

static void ms_do(int act)
{
	if (act == FA_COPY || act == FA_MOVE)
		ms_copy_move(act);
	else if (act == FA_COLORS)
		MC.open = MC.just = true, MC.sel = 0;
	else if (act == FA_DELETE)
		ms_ask_delete();
	else if (act == FA_INSTALL)
		ms_install();
	else if (act == FA_ZIP)
		zq_set(F.path, MS.nm, MS.n);
}

// hoja de abajo con una lista de opciones y «Cerrar» al final; devuelve la elegida o -1
static int sheet_list(bool *open, bool *just, float *anim, int *sel, const char *title, const IconId *ic, const char *const *lab,
                      const u32 *col, int n)
{
	*anim = approach(*anim, *open ? 1 : 0, 16);
	if (*anim < 0.01f)
		return -1;
	float a = ease_out_cubic(*anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	// como el menú de un archivo: el título y «Cerrar» arriba, y una fila por acción con su icono
	const float X = 6, W = BOT_W - 12, PAD = 10, HH = 34, RH = n >= 6 ? 28 : 32;
	float h = PAD + HH + n * RH + PAD, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rrect(X, y + 3, W, h, 18, col_alpha(T.shadow, a));
	draw_rrect(X, y, W, h, 18, T.surface);
	if (!*open)
		return -1;
	bool fresh = *just;
	*just = false;
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	float cw = text_width(cl, TS_SUB) + 18, cx = X + W - PAD - cw, hy = y + PAD;
	draw_rrect(cx, hy + 7, cw, 20, 10, touch_in(cx, hy + 2, cw, 30) ? T.divider : T.surface2);
	text_draw_center(cl, cx + cw / 2, hy + 17 - text_lineh(TS_SUB) / 2, TS_SUB, T.text2);
	text_draw_clip(title, X + PAD + 6, hy + 17 - text_lineh(TS_BODY) / 2, cx - 10 - (X + PAD + 6), TS_BODY, T.text);
	if (*sel >= n)
		*sel = 0;
	if (U.buttons && !fresh && n) {
		if (g_krepeat & KEY_DOWN) *sel = (*sel + 1) % n;
		if (g_krepeat & KEY_UP) *sel = (*sel + n - 1) % n;
	}
	int run = -1;
	bool close = !fresh && tap(cx, hy + 2, cw, 30);
	for (int i = 0; i < n; i++) {
		float ry = hy + HH + i * RH, rx = X + PAD, rw = W - 2 * PAD;
		bool hl = (U.buttons && *sel == i) || touch_in(rx, ry, rw, RH);
		if (hl)
			draw_rrect(rx, ry + 1, rw, RH - 2, 10, T.surface2);
		if (U.buttons && *sel == i)
			draw_rrect_outline(rx, ry + 1, rw, RH - 2, 10, 1.5f, T.accent);
		bool tinted = col[i] != T.text && col[i] != T.text2;
		float ts = RH - 8;
		draw_rrect(rx + 6, ry + 4, ts, ts, 8, tinted ? col_alpha(col[i], 0.16f) : T.surface2);
		draw_icon(ic[i], rx + 6 + ts / 2, ry + RH / 2, 13, tinted ? col[i] : T.text2);
		text_draw_clip(lab[i], rx + 14 + ts, ry + (RH - text_lineh(0.44f)) / 2, rw - ts - 22, 0.44f, tinted ? col[i] : T.text);
		if (!fresh && (tap(rx, ry, rw, RH) || (U.buttons && *sel == i && (g_kdown & KEY_A))))
			run = i;
	}
	if (!fresh && (close || (g_kdown & (KEY_B | KEY_Y)) || (g_touch.up && !g_touch.drag && g_touch.sy < y))) // con Y se abren casi todas: Y también las cierra
		*open = false, run = -1;
	else if (run >= 0)
		*open = false;
	g_kdown &= ~(KEY_A | KEY_B | KEY_Y);
	g_touch.up = false;
	return run;
}

static void ma_draw(void)
{
	int acts[6], n = 0;
	IconId ic[6];
	const char *lab[6];
	u32 col[6];
	bool lock = ms_locked();
	if (MS.n) {
		acts[n] = FA_COPY, ic[n] = IC_DUP, lab[n] = _("Copiar"), col[n++] = T.text;
		if (!lock)
			acts[n] = FA_MOVE, ic[n] = IC_FOLDER, lab[n] = _("Cortar"), col[n++] = T.text;
		acts[n] = FA_COLORS, ic[n] = IC_STAR, lab[n] = _("Destacar"), col[n++] = T.text;
		if (!lock)
			acts[n] = FA_DELETE, ic[n] = IC_TRASH, lab[n] = _("Eliminar"), col[n++] = T.danger;
		acts[n] = FA_ZIP, ic[n] = IC_ARCHIVE, lab[n] = _("Comprimir en ZIP"), col[n++] = T.text;
		if (ms_cias())
			acts[n] = FA_INSTALL, ic[n] = IC_DOWNLOAD, lab[n] = _("Instalar"), col[n++] = T.ok;
	}
	char t[48];
	snprintf(t, sizeof t, MS.n == 1 ? _("1 seleccionado") : _("%d seleccionados"), MS.n);
	int r = sheet_list(&MA.open, &MA.just, &MA.anim, &MA.sel, t, ic, lab, col, n);
	if (r >= 0)
		ms_do(acts[r]);
}

static void mc_draw(void)
{
	static const u32 MK[7] = {0, 0xFF4444EF, 0xFF1673F9, 0xFF08B3EA, 0xFF5EC522, 0xFFF6823B, 0xFFF755A8};
	MC.anim = approach(MC.anim, MC.open ? 1 : 0, 16);
	if (MC.anim < 0.01f)
		return;
	float a = ease_out_cubic(MC.anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	float h = 120, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rrect(10, y + 3, BOT_W - 20, h, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, h, 16, T.surface);
	if (!MC.open)
		return;
	bool fresh = MC.just;
	MC.just = false;
	text_draw_center(_("Destacar con un color"), BOT_W / 2, y + 10, TS_BODY, T.text);
	if (U.buttons && !fresh && (g_krepeat & (KEY_LEFT | KEY_RIGHT)))
		MC.sel = (MC.sel + ((g_krepeat & KEY_RIGHT) ? 1 : 6)) % 7;
	int pick = -1;
	for (int k = 0; k < 7; k++) {
		float cx = BOT_W / 2 + (k - 3) * 36, cy = y + 50;
		if (U.buttons && MC.sel == k)
			draw_circle(cx, cy, 15, T.text);
		draw_circle(cx, cy, 13, k ? MK[k] : T.surface2);
		if (!k)
			draw_line(cx - 6, cy + 6, cx + 6, cy - 6, 1.6f, T.text3);
		if (!fresh && (tap(cx - 16, cy - 16, 32, 32) || (U.buttons && MC.sel == k && (g_kdown & KEY_A))))
			pick = k;
	}
	float by = y + h - 38;
	draw_rrect(18, by, BOT_W - 36, 30, 15, touch_in(18, by, BOT_W - 36, 30) ? T.surface2 : T.bg);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	text_draw_center(cl, BOT_W / 2, by + (30 - text_lineh(TS_BODY)) / 2, TS_BODY, T.text2);
	if (pick >= 0) {
		MC.open = false;
		ms_color(pick);
	} else if (!fresh && (tap(18, by, BOT_W - 36, 30) || (g_kdown & KEY_B) || (g_touch.up && !g_touch.drag && g_touch.sy < y))) {
		MC.open = false;
	}
	g_kdown &= ~(KEY_A | KEY_B);
	g_touch.up = false;
}

// MD5 y SHA-256 del archivo elegido: la hoja de abajo (comparar, guardar) y los valores arriba
static struct {
	bool open, just, want_cmp;
	float anim;
	int sel, cmp; // cmp: lo que dio «Comparar» (HCHK_*)
	char path[512];
	u32 seen;     // para avisar una vez cuando termina con la hoja cerrada
} HS;
static HashJob HJ; // copia por frame

static void hs_draw(void)
{
	const char *nm = strrchr(HS.path, '/');
	static const IconId ic[2] = {IC_CHECK, IC_DOWNLOAD};
	const char *lab[2] = {_("Comparar con un valor"), _("Guardar .sha256 junto al archivo")};
	bool done = HJ.st == HASH_DONE && !strcmp(HJ.path, HS.path);
	const u32 col[2] = {done ? T.accent : T.text3, done ? T.text : T.text3};
	int r = sheet_list(&HS.open, &HS.just, &HS.anim, &HS.sel, nm ? nm + 1 : HS.path, ic, lab, col, 2);
	if (r >= 0 && !done) {
		toast_icon(_("El cálculo aún no ha terminado"), IC_CLOCK, T.text2);
		HS.open = true; // la hoja sigue abierta
	} else if (r == 0) {
		HS.open = true, HS.want_cmp = true; // el teclado, fuera del dibujo
	} else if (r == 1) {
		if (hash_save_sha256(&HJ) == 0) {
			toast_icon(_("Guardado junto al archivo como .sha256"), IC_CHECK, T.ok);
			F.need_reload = true;
		} else
			toast_icon(_("No se pudo guardar"), IC_CLOSE, T.danger);
	}
}

// pantalla de arriba: el archivo, el progreso y luego los dos valores y si coinciden
static void hash_top(float a)
{
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	bool mine = !strcmp(HJ.path, HS.path);
	const char *nm = strrchr(HS.path, '/');
	char sz[24];
	util_fmt_size(sz, sizeof sz, mine ? HJ.size : 0);
	float y = y0 + 10;
	draw_icon(IC_SHIELD, X + 8, y + 8, 15, col_alpha(T.accent, a));
	text_draw_clip(nm ? nm + 1 : HS.path, X + 24, y, W - 24 - text_width(sz, TS_SUB) - 10, TS_TITLE, col_alpha(T.text, a));
	text_draw_right(sz, X + W, y + 3, TS_SUB, col_alpha(T.text3, a));
	y += 26;
	if (!mine || HJ.st == HASH_RUN) {
		float f = mine && HJ.size ? (float)HJ.done / HJ.size : 0;
		char t[64];
		snprintf(t, sizeof t, _("Calculando… %d %%"), (int)(f * 100));
		text_draw(t, X, y, TS_BODY, col_alpha(T.text2, a));
		draw_rrect(X, y + 24, W, 6, 3, col_alpha(T.surface2, a));
		draw_rrect(X, y + 24, fmaxf(6, W * f), 6, 3, col_alpha(T.accent, a));
		wrap_center(_("Puedes seguir usando la consola mientras se calcula."), TOP_W / 2, y + 44, W, TS_SUB, 2, col_alpha(T.text3, a));
		return;
	}
	if (HJ.st == HASH_ERR) {
		text_draw(_("No se pudo leer el archivo"), X, y, TS_BODY, col_alpha(T.danger, a));
		return;
	}
	// en dos columnas: el nombre a la izquierda y el valor a la derecha
	const float VX = X + 62, LH = text_lineh(TS_BODY) + 2;
	float dy = (text_lineh(TS_BODY) - text_lineh(TS_SUB)) / 2;
	text_draw("MD5", X, y + dy, TS_SUB, col_alpha(T.text3, a));
	text_draw(HJ.md5, VX, y, TS_BODY, col_alpha(T.text, a));
	y += LH + 4;
	text_draw("SHA-256", X, y + dy, TS_SUB, col_alpha(T.text3, a));
	char h1[33], h2[33];
	memcpy(h1, HJ.sha, 32), h1[32] = 0;
	memcpy(h2, HJ.sha + 32, 32), h2[32] = 0;
	text_draw(h1, VX, y, TS_BODY, col_alpha(T.text, a));
	text_draw(h2, VX, y + LH, TS_BODY, col_alpha(T.text, a));
	y += 2 * LH + 6;
	// lo que dice «Comparar» manda; si no, el .md5/.sha256 de al lado
	int chk = HS.cmp != HCHK_NONE ? HS.cmp : HJ.check;
	if (chk != HCHK_NONE) {
		char t[200];
		if (HS.cmp != HCHK_NONE)
			util_strlcpy(t, chk == HCHK_OK ? _("Coincide con el valor introducido") : _("No coincide con el valor introducido"), sizeof t);
		else
			snprintf(t, sizeof t, chk == HCHK_OK ? _("Coincide con %s") : _("No coincide con %s"), HJ.check_file);
		u32 c = chk == HCHK_OK ? T.ok : T.danger;
		y = fminf(y, y0 + h - 30);
		draw_rrect(X, y, W, 22, 11, col_alpha(c, 0.16f * a));
		draw_icon(chk == HCHK_OK ? IC_CHECK : IC_CLOSE, X + 14, y + 11, 12, col_alpha(c, a));
		text_draw_clip(t, X + 28, y + 11 - text_lineh(TS_SUB) / 2, W - 36, TS_SUB, col_alpha(c, a));
	}
}

static float info_row(float x, float y, float w, const char *k, const char *v, u32 vc, float a);

// un .zip abierto: lo que trae (primer nivel) y extraer aquí o en una carpeta nueva
#define ZS_TOP 64
static struct {
	bool open, just;
	float anim, off;
	char path[700];
	ZipDir z;
	char (*names)[256];
	int cnt[ZS_TOP], n;
	u64 sizes[ZS_TOP];
	bool dirs[ZS_TOP];
} ZS;

static void zs_open(const char *path)
{
	zip_dir_free(&ZS.z);
	int r = zip_read_dir(path, &ZS.z);
	if (r != ZIP_OK) {
		toast_icon(zip_err_text(r), IC_CLOSE, T.danger);
		return;
	}
	if (!ZS.names)
		ZS.names = malloc(ZS_TOP * sizeof *ZS.names);
	if (!ZS.names)
		return;
	ZS.n = zip_top_level(&ZS.z, ZS.names, ZS.cnt, ZS.sizes, ZS.dirs, ZS_TOP);
	util_strlcpy(ZS.path, path, sizeof ZS.path);
	ZS.off = 0;
	ZS.open = ZS.just = true;
}

static void zs_extract(bool new_dir)
{
	int r = zipjob_extract(ZS.path, new_dir, G.console);
	if (r != ZIP_OK)
		toast_icon(zip_err_text(r), IC_CLOSE, T.danger);
	else {
		toast_icon(_("Extrayendo…"), IC_ARCHIVE, T.warn);
		ZS.open = false;
	}
}

static void zs_draw(void)
{
	ZS.anim = approach(ZS.anim, ZS.open ? 1 : 0, 16);
	if (ZS.anim < 0.01f)
		return;
	float a = ease_out_cubic(ZS.anim), y0 = (1 - a) * 40;
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	draw_rrect(6, y0 + 6, BOT_W - 12, SCR_H - 12, 16, T.surface);
	if (!ZS.open)
		return;
	bool fresh = ZS.just;
	ZS.just = false;
	const char *nm = strrchr(ZS.path, '/');
	char c[48];
	snprintf(c, sizeof c, ZS.z.n == 1 ? _("1 elemento") : _("%d elementos"), ZS.z.nfiles + ZS.z.ndirs);
	bool xp = touch_in(6, 6, 170, 28);
	draw_icon(IC_CHEV_LEFT, 22, y0 + 22, 13, xp ? T.text : T.text2);
	text_draw_clip(nm ? nm + 1 : ZS.path, 34, y0 + 13, BOT_W - 60 - text_width(c, TS_SUB) - 30, TS_TITLE, T.text);
	text_draw_right(c, BOT_W - 18, y0 + 17, TS_SUB, T.text3);
	// lo de primer nivel
	const float RH = 32, LY = 40, LH = SCR_H - 12 - 44 - LY;
	if (g_touch.held && g_touch.drag && g_touch.sy >= LY && g_touch.sy < LY + LH)
		ZS.off -= g_touch.y - g_touch.py;
	if (U.buttons && (g_krepeat & (KEY_UP | KEY_DOWN)))
		ZS.off += (g_krepeat & KEY_DOWN) ? RH : -RH;
	ZS.off = clampf(ZS.off, 0, fmaxf(0, ZS.n * RH - LH));
	gfx_clip(0, LY, BOT_W, LH);
	for (int i = 0; i < ZS.n; i++) {
		float ry = LY + i * RH - ZS.off;
		if (ry + RH < LY || ry > LY + LH)
			continue;
		u32 col = ZS.dirs[i] ? T.warn : T.text3;
		draw_rrect(18, ry + 4, 24, 24, 7, col_alpha(col, 0.18f));
		draw_icon(ZS.dirs[i] ? IC_FOLDER : IC_FILE, 30, ry + 16, 13, col);
		char sub[48], sz[24];
		util_fmt_size(sz, sizeof sz, ZS.sizes[i]);
		if (ZS.dirs[i])
			snprintf(sub, sizeof sub, ZS.cnt[i] == 1 ? _("1 archivo · %s") : _("%d archivos · %s"), ZS.cnt[i], sz);
		else
			util_strlcpy(sub, sz, sizeof sub);
		text_draw_clip(ZS.names[i], 50, ry + 2, BOT_W - 70, TS_BODY, T.text);
		text_draw_clip(sub, 50, ry + 17, BOT_W - 70, TS_SUB, T.text3);
	}
	gfx_clip_off();
	// abajo: extraer aquí (A) o en una carpeta nueva (X)
	bool can = !ZS.z.encrypted && !ZS.z.unsupported;
	float by = SCR_H - 12 - 38, bw = (BOT_W - 36) / 2.0f;
	char t1[64], t2[64];
	snprintf(t1, sizeof t1, "%s %s", GLYPH_A, _("Extraer aquí"));
	snprintf(t2, sizeof t2, "%s %s", GLYPH_X, _("En carpeta nueva"));
	bool p1 = can && touch_in(14, by, bw, 32), p2 = can && touch_in(22 + bw, by, bw, 32);
	draw_rrect(14, by, bw, 32, 16, !can ? T.surface2 : p1 ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
	draw_rrect(22 + bw, by, bw, 32, 16, p2 ? T.surface : T.surface2);
	text_draw_center_clip(t1, 14 + bw / 2, by + 16 - text_lineh(TS_SUB) / 2, bw - 8, TS_SUB, can ? 0xFFFFFFFF : T.text3);
	text_draw_center_clip(t2, 22 + bw * 1.5f, by + 16 - text_lineh(TS_SUB) / 2, bw - 8, TS_SUB, can ? T.text : T.text3);
	if (!fresh && can && (tap(14, by, bw, 32) || (g_kdown & KEY_A)))
		zs_extract(false);
	else if (!fresh && can && (tap(22 + bw, by, bw, 32) || (g_kdown & KEY_X)))
		zs_extract(true);
	else if (!fresh && ((g_kdown & KEY_B) || tap(6, 6, 170, 28)))
		ZS.open = false;
	g_kdown &= ~(KEY_A | KEY_B | KEY_X);
	g_touch.up = false;
}

// pantalla de arriba con un ZIP abierto: qué trae, cuánto ocupa al extraerlo y si cabe
static void zip_top(float a)
{
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28, PS = h - 20;
	draw_rrect(X - 4, y0 + 10, PS, PS, 12, col_alpha(T.surface2, a));
	draw_icon(IC_ARCHIVE, X - 4 + PS / 2, y0 + 10 + PS / 2, 38, col_alpha(T.warn, a));
	float rx = X - 4 + PS + 14, rw = X + W - rx, y = y0 + 11;
	const char *nm = strrchr(ZS.path, '/');
	text_draw_clip(nm ? nm + 1 : ZS.path, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 5;
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	g_info_ymax = by - 4;
	char v[160], s1[24], s2[24];
	snprintf(v, sizeof v, _("%d carpetas, %d archivos"), ZS.z.ndirs, ZS.z.nfiles);
	y = info_row(rx, y, rw, _("Contiene"), v, T.text2, a);
	util_fmt_size(s1, sizeof s1, ZS.z.csize);
	util_fmt_size(s2, sizeof s2, ZS.z.usize);
	snprintf(v, sizeof v, _("%s (%s al extraer)"), s1, s2);
	y = info_row(rx, y, rw, _("Tamaño"), v, T.text2, a);
	if (ZS.z.encrypted || ZS.z.unsupported) {
		y = info_row(rx, y, rw, _("Aviso"), ZS.z.encrypted ? _("Protegido con contraseña: no se puede extraer") : _("Formato de compresión no compatible"), T.danger, a);
	} else {
		u64 fr = 0, tot = 0;
		sd_space(&fr, &tot);
		util_fmt_size(s1, sizeof s1, fr);
		bool fits = fr > ZS.z.usize + 1024 * 1024;
		if (fits)
			snprintf(v, sizeof v, _("Hay espacio (quedan %s)"), s1);
		else {
			util_fmt_size(s2, sizeof s2, ZS.z.usize - fr);
			snprintf(v, sizeof v, _("Falta espacio: %s"), s2);
		}
		y = info_row(rx, y, rw, _("Espacio"), v, fits ? T.ok : T.danger, a);
	}
	g_info_ymax = 1e9f;
	char hint[96];
	snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_A, _("Extraer aquí"), GLYPH_X, _("En carpeta nueva"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
}

// un título instalado: hacer copia de la partida, ver sus copias o desinstalarlo
static struct {
	bool open, just;
	float anim;
	int sel;
	u64 icon_tid;   // de qué título es el icono de arriba
	Texture icon;
	int copies;     // copias de su partida (se cuentan al elegirlo)
	u64 last;
	u64 info_tid;
	s64 ext;        // cuánto ocupan sus datos extra (-1: no tiene)
} TT;

// lo de cada título que cuesta leer (icono, copias de la partida y datos extra) se lee en su hilo y se guarda: moverse
// por la lista nunca espera a la SD; mientras llega, la pantalla de arriba enseña lo que ya sabe
#define TW_N 16
typedef struct {
	u64  tid;
	bool icon;
	u16  px[48 * 48];
	int  copies;
	u64  last;
	s64  ext;
} TwEnt;
static struct {
	Thread thr;
	LightEvent ev;
	LightLock lk;
	volatile bool quit;
	bool init, has_want;
	TitleInfo want;
	u32 gen;
	TwEnt e[TW_N];
	int next;
} TW;

static void tw_thread(void *arg)
{
	(void)arg;
	static TwEnt r;
	while (!TW.quit) {
		LightEvent_Wait(&TW.ev);
		for (;;) {
			LightLock_Lock(&TW.lk);
			bool has = TW.has_want;
			TitleInfo t = TW.want;
			u32 gen = TW.gen;
			TW.has_want = false;
			LightLock_Unlock(&TW.lk);
			if (!has || TW.quit)
				break;
			r.tid = t.tid;
			r.icon = title_icon(t.tid, r.px);
			r.copies = save_backups_count(&t, &r.last);
			r.ext = extdata_size(&t);
			LightLock_Lock(&TW.lk);
			if (gen == TW.gen) { // (si se olvidó todo mientras se leía, esto ya no vale)
				int k = 0;
				while (k < TW_N && TW.e[k].tid != r.tid)
					k++;
				if (k == TW_N)
					k = TW.next, TW.next = (TW.next + 1) % TW_N;
				TW.e[k] = r;
			}
			LightLock_Unlock(&TW.lk);
		}
	}
}

// lo leído de un título; si aún no está, se pide (lo último pedido es lo siguiente que se lee) y devuelve false
static bool tw_get(const TitleInfo *t, TwEnt *out, bool with_px)
{
	if (!TW.init) {
		TW.init = true;
		LightEvent_Init(&TW.ev, RESET_ONESHOT);
		LightLock_Init(&TW.lk);
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		TW.thr = threadCreate(tw_thread, NULL, 64 * 1024, prio + 1, -2, false);
	}
	if (!TW.thr) { // sin hilo: como antes, aquí mismo
		out->tid = t->tid;
		out->icon = with_px && title_icon(t->tid, out->px);
		out->copies = save_backups_count(t, &out->last);
		out->ext = extdata_size(t);
		return true;
	}
	bool ok = false;
	LightLock_Lock(&TW.lk);
	for (int k = 0; k < TW_N; k++)
		if (TW.e[k].tid == t->tid) {
			if (with_px)
				*out = TW.e[k];
			else
				out->tid = t->tid, out->icon = TW.e[k].icon, out->copies = TW.e[k].copies, out->last = TW.e[k].last, out->ext = TW.e[k].ext;
			ok = true;
			break;
		}
	bool ask = !ok && (!TW.has_want || TW.want.tid != t->tid);
	if (ask)
		TW.want = *t, TW.has_want = true;
	LightLock_Unlock(&TW.lk);
	if (ask)
		LightEvent_Signal(&TW.ev);
	return ok;
}

// algo cambió (copia nueva, desinstalado, datos extra borrados): se vuelve a leer
static void tw_forget(void)
{
	if (TW.init)
		LightLock_Lock(&TW.lk);
	TW.gen++;
	for (int k = 0; k < TW_N; k++)
		TW.e[k].tid = 0;
	if (TW.init)
		LightLock_Unlock(&TW.lk);
	TT.info_tid = 0;
}

static void tw_exit(void)
{
	if (!TW.thr)
		return;
	TW.quit = true;
	LightEvent_Signal(&TW.ev);
	threadJoin(TW.thr, 3000000000ull);
	threadFree(TW.thr);
	TW.thr = NULL;
}

// pasa al título elegido lo que ya se haya leído de él; ¿está todo?
static bool tt_sync(const TitleInfo *t)
{
	if (!t)
		return false;
	if (TT.info_tid == t->tid && TT.icon_tid == t->tid)
		return true;
	static TwEnt r;
	bool need_px = TT.icon_tid != t->tid;
	if (!tw_get(t, &r, need_px))
		return false;
	if (need_px) {
		TT.icon_tid = t->tid;
		if (r.icon)
			tex_from_rgb565(&TT.icon, r.px, 48, 48);
		else
			tex_free(&TT.icon);
	}
	TT.info_tid = t->tid, TT.copies = r.copies, TT.last = r.last, TT.ext = r.ext;
	return true;
}

static const TitleInfo *tt_selected(void)
{
	const FEnt *e = f_selected();
	return F.titles && e && e->tid < (u32)TI_n ? &TI[e->tid] : NULL;
}

static void tt_backup(const TitleInfo *t)
{
	char out[600];
	int r = save_backup(t, out, sizeof out);
	if (r == 0) {
		state_lock();
		event_add(EV_CREATE, t->title, G.console, 0);
		state_unlock();
		toast_icon(_("Copia de seguridad guardada (formato de Checkpoint)"), IC_CHECK, T.ok);
		tw_forget(); // que se vuelvan a contar
	} else
		toast_icon(r == -1 ? _("Este juego todavía no tiene datos guardados") : _("No se pudo crear la copia de seguridad"), IC_CLOSE, r == -1 ? T.warn : T.danger);
}

static void tt_draw(void)
{
	const TitleInfo *t = tt_selected();
	if (!t && TT.open)
		TT.open = false;
	bool rdy = tt_sync(t); // cuántas copias tiene y sus datos extra (la pantalla de arriba usa lo mismo)
	// «Ver sus copias» solo si las hay; liberar espacio, por partes; Cloud3DS no se desinstala a sí mismo
	IconId ic[6];
	const char *lab[6];
	u32 col[6];
	int act[6], n = 0;
	static char l_upd[80], l_dlc[80], l_ext[80];
	bool self = t && title_is_self(t->tid);
	ic[n] = IC_DOWNLOAD, lab[n] = _("Crear copia de la partida"), col[n] = T.ok, act[n++] = 0;
	if (rdy && TT.copies > 0)
		ic[n] = IC_FOLDER, lab[n] = _("Ver copias de la partida"), col[n] = T.text, act[n++] = 1;
	if (t && !self && t->upd_size) {
		char z[24];
		util_fmt_size(z, sizeof z, t->upd_size);
		snprintf(l_upd, sizeof l_upd, _("Desinstalar la actualización (%s)"), z);
		ic[n] = IC_UNDO, lab[n] = l_upd, col[n] = T.warn, act[n++] = 3;
	}
	if (t && !self && t->dlc > 0) {
		char z[24];
		util_fmt_size(z, sizeof z, t->dlc_size);
		snprintf(l_dlc, sizeof l_dlc, _("Desinstalar los DLC (%s)"), z);
		ic[n] = IC_GEM, lab[n] = l_dlc, col[n] = T.warn, act[n++] = 4;
	}
	if (rdy && !self && TT.ext > 0) {
		char z[24];
		util_fmt_size(z, sizeof z, (u64)TT.ext);
		snprintf(l_ext, sizeof l_ext, _("Borrar los datos extra (%s)"), z);
		ic[n] = IC_ARCHIVE, lab[n] = l_ext, col[n] = T.warn, act[n++] = 5;
	}
	if (t && !self)
		ic[n] = IC_TRASH, lab[n] = _("Desinstalar"), col[n] = T.danger, act[n++] = 2;
	int r = sheet_list(&TT.open, &TT.just, &TT.anim, &TT.sel, t ? t->title : "", ic, lab, col, n);
	if (r < 0 || !t)
		return;
	r = act[r];
	U.dlg_arg = (int)(t - TI);
	if (r == 3 || r == 4) {
		char msg[300], sz[24];
		util_fmt_size(sz, sizeof sz, r == 3 ? t->upd_size : t->dlc_size);
		snprintf(msg, sizeof msg, r == 3 ? _("Se liberarán %s. El juego seguirá instalado, sin la actualización; podrás volver a instalarla.")
		                                 : _("Se liberarán %s. El juego seguirá instalado, sin sus contenidos descargables; podrás volver a instalarlos."), sz);
		dialog_open(r == 3 ? DLG_UNUPD : DLG_UNDLC, r == 3 ? _("¿Desinstalar la actualización?") : _("¿Desinstalar los DLC?"), msg, _("Desinstalar"), true);
		return;
	}
	if (r == 5) {
		char msg[320], sz[24];
		util_fmt_size(sz, sizeof sz, (u64)TT.ext);
		snprintf(msg, sizeof msg, _("Se liberarán %s. Antes se creará una copia en /3ds/Checkpoint/extdata (se puede restaurar con Checkpoint); si la copia falla, no se elimina nada. La partida no se modifica."), sz);
		dialog_open(DLG_EXTDEL, _("¿Borrar los datos extra?"), msg, _("Copiar y borrar"), true);
		return;
	}
	if (r == 0)
		tt_backup(t);
	else if (r == 1) {
		char gd[512];
		save_dir(t, gd, sizeof gd);
		bool d;
		u64 sz;
		if (fsx_stat(gd, &d, &sz) < 0)
			toast_icon(_("Este juego no tiene copias de seguridad"), IC_INFO, T.text2);
		else { // atrás (B o la flecha) vuelve a la lista de títulos, no sube por la SD
			f_load(gd);
			util_strlcpy(F.ret_from, F.path, sizeof F.ret_from);
			util_strlcpy(F.ret_to, "@titles", sizeof F.ret_to);
		}
	} else {
		char msg[300], sz[24];
		util_fmt_size(sz, sizeof sz, t->size + t->upd_size + t->dlc_size);
		snprintf(msg, sizeof msg, _("Se eliminarán el juego, su actualización, sus DLC y su partida (%s). Si quieres conservar la partida, crea antes una copia de seguridad."), sz);
		U.dlg_arg = (int)(t - TI);
		dialog_open(DLG_UNINSTALL, _("¿Desinstalar?"), msg, _("Desinstalar"), true);
	}
}

static float info_row(float x, float y, float w, const char *k, const char *v, u32 vc, float a);
static void f_fmt_date(char *out, size_t n, u64 t);

static void ver_text(u16 v, char *out, size_t n)
{
	snprintf(out, n, "%u.%u.%u", v >> 10, (v >> 4) & 0x3F, v & 0xF);
}

// pantalla de arriba con un título elegido: icono, nombre, versión, tamaño, DLC, copias e ID
static void titles_top(float a)
{
	const TitleInfo *t = tt_selected();
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	if (!t) { // nada elegido: en qué se va el espacio de la SD
		u64 g = 0, up = 0, dl = 0, fr = 0, tot = 0;
		for (int i = 0; i < TI_n; i++)
			g += TI[i].size, up += TI[i].upd_size, dl += TI[i].dlc_size;
		sd_space(&fr, &tot);
		float y = y0 + 14;
		text_draw(_("Espacio en la tarjeta SD"), X, y, TS_TITLE, col_alpha(T.text, a));
		y += text_lineh(TS_TITLE) + 10;
		const u64 v4[4] = {g, up, dl, tot > fr + g + up + dl ? tot - fr - g - up - dl : 0};
		const u32 c4[4] = {T.warn, T.accent, T.accent2, T.text3};
		const char *n4[4] = {_("Juegos y aplicaciones"), _("Actualizaciones"), _("DLC"), _("Otros archivos")};
		float bx = X, bw = W, acc = 0;
		draw_rrect(bx, y, bw, 10, 5, col_alpha(T.surface2, a));
		for (int k = 0; k < 4 && tot; k++) {
			float w = bw * (float)((double)v4[k] / tot);
			if (w >= 1)
				draw_rect(bx + acc, y, w, 10, col_alpha(c4[k], a));
			acc += w;
		}
		y += 22;
		for (int k = 0; k < 4; k++) {
			char z[24];
			util_fmt_size(z, sizeof z, v4[k]);
			draw_circle(X + 5, y + 7, 4, col_alpha(c4[k], a));
			text_draw(n4[k], X + 16, y, TS_SUB, col_alpha(T.text2, a));
			text_draw_right(z, X + W, y, TS_SUB, col_alpha(T.text, a));
			y += 18;
		}
		char z[24], z2[24], f2[64];
		util_fmt_size(z, sizeof z, fr), util_fmt_size(z2, sizeof z2, tot);
		snprintf(f2, sizeof f2, _("%s libres de %s"), z, z2);
		text_draw(f2, X + 16, y + 2, TS_SUB, col_alpha(T.ok, a));
		text_draw_right(_("Elige un título para gestionarlo"), X + W, y0 + h - 22, TS_SUB, col_alpha(T.text3, a));
		return;
	}
	bool rdy = tt_sync(t);
	const float IS = h - 20;
	float px = X - 4, py = y0 + 10;
	draw_rrect(px, py, IS, IS, 12, col_alpha(T.surface2, a));
	if (rdy && TT.icon.ok)
		draw_texture(&TT.icon, px + (IS - 72) / 2, py + (IS - 72) / 2, 72, 72, a);
	else
		draw_icon(IC_GAMEPAD, px + IS / 2, py + IS / 2, 38, col_alpha(T.ok, a));
	float rx = px + IS + 14, rw = X + W - rx, y = py + 1;
	text_draw_clip(t->title, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 2;
	if (t->pub[0]) {
		text_draw_clip(t->pub, rx, y, rw, TS_SUB, col_alpha(T.text2, a));
		y += 15;
	}
	y += 3;
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	g_info_ymax = by - 4;
	char v[160], b1[24], b2[24];
	ver_text(t->has_upd ? t->upd_version : t->version, b1, sizeof b1);
	if (t->has_upd)
		snprintf(v, sizeof v, _("%s · actualización instalada"), b1);
	else
		util_strlcpy(v, b1, sizeof v);
	y = info_row(rx, y, rw, _("Versión"), v, T.text2, a);
	util_fmt_size(b1, sizeof b1, t->size + t->upd_size + t->dlc_size);
	if (t->upd_size || t->dlc_size) {
		util_fmt_size(b2, sizeof b2, t->size);
		snprintf(v, sizeof v, _("%s (juego %s)"), b1, b2);
	} else
		util_strlcpy(v, b1, sizeof v);
	y = info_row(rx, y, rw, _("Tamaño"), v, T.text2, a);
	if (!rdy)
		util_strlcpy(v, "…", sizeof v);
	else if (TT.copies) {
		f_fmt_date(b1, sizeof b1, TT.last);
		snprintf(v, sizeof v, TT.copies == 1 ? _("1 · %s") : _("%d · última: %s"), TT.copies, b1);
	} else
		util_strlcpy(v, _("Ninguna"), sizeof v);
	y = info_row(rx, y, rw, _("Copias"), v, rdy && TT.copies ? T.text2 : T.text3, a);
	if (t->dlc) {
		util_fmt_size(b1, sizeof b1, t->dlc_size);
		snprintf(v, sizeof v, t->dlc == 1 ? _("1 contenido · %s") : _("%d contenidos · %s"), t->dlc, b1);
		y = info_row(rx, y, rw, _("DLC"), v, T.text2, a);
	}
	if (rdy && TT.ext > 0) {
		util_fmt_size(b1, sizeof b1, (u64)TT.ext);
		y = info_row(rx, y, rw, _("Datos extra"), b1, T.text2, a);
	}
	snprintf(v, sizeof v, "%016llX", (unsigned long long)t->tid);
	y = info_row(rx, y, rw, _("ID"), v, T.text3, a);
	g_info_ymax = 1e9f;
	char hint[64];
	snprintf(hint, sizeof hint, "%s %s", GLYPH_A, _("Opciones"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
}

// papelera: recuperar o borrar del todo el elemento elegido
static struct {
	bool open, just;
	float anim;
	int sel;
} TS;

static void trash_do_restore(int idx)
{
	TrashItem t;
	char dst[700];
	if (!trash_get(F.e[idx].tid, &t) || trash_restore(t.id, dst, sizeof dst) < 0) {
		toast_icon(_("No se pudo recuperar"), IC_CLOSE, T.danger);
		return;
	}
	state_lock();
	if (t.color)
		mark_set(dst, t.color);
	event_add(EV_RESTORE, t.name, G.console, 0);
	G.rev++;
	state_unlock();
	char msg[200];
	snprintf(msg, sizeof msg, _("Recuperado en %s"), dst);
	toast_icon(msg, IC_REFRESH, T.ok);
	f_reload_keep();
}

static void ts_draw(void)
{
	const FEnt *e = f_selected();
	static const IconId ic[2] = {IC_REFRESH, IC_TRASH};
	const char *lab[2] = {_("Recuperar"), _("Eliminar definitivamente")};
	const u32 col[2] = {T.ok, T.danger};
	int r = sheet_list(&TS.open, &TS.just, &TS.anim, &TS.sel, e ? e->name : "", ic, lab, col, 2);
	if (r == 0 && e)
		trash_do_restore(F.v[F.sel].idx);
	else if (r == 1 && e) {
		char msg[200];
		snprintf(msg, sizeof msg, _("«%s» se eliminará de la tarjeta SD. Esta acción no se puede deshacer."), e->name);
		dialog_open(DLG_TRASHDEL, _("¿Eliminar definitivamente?"), msg, _("Eliminar"), true);
	}
}

static void ds_open_folder(const DlItem *d)
{
	char dir[512], nm[256];
	util_strlcpy(dir, d->dir, sizeof dir);
	const char *b = strrchr(d->path, '/');
	util_strlcpy(nm, b ? b + 1 : "", sizeof nm);
	U.tab = TAB_FILES;
	f_load(dir);
	for (int i = 0; i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm)) {
			F.sel = i;
			F.target = fmaxf(0, F.v[i].y - VIEW_H / 2);
		}
}

static void ds_draw(void)
{
	const DlItem *d = ds_find(DS.id);
	if (!d && DS.open)
		DS.open = false;
	int acts[6] = {0}, n = 0;
	IconId ic[6] = {0};
	const char *lab[6] = {0};
	u32 col[6] = {0};
	enum { A_PAUSE, A_RESUME, A_CANCEL, A_REMOVE, A_INSTALL, A_UNZIP, A_FOLDER, A_INSECURE };
	if (d) {
		size_t l = strlen(d->name);
		bool isdir;
		u64 fsz;
		bool there = d->st == DL_DONE && d->path[0] && fsx_stat(d->path, &isdir, &fsz) == 0; // pudo borrarse (rechazar la instalación lo borra)
		bool cia = there && l > 4 && !strcasecmp(d->name + l - 4, ".cia"), zip = there && l > 4 && !strcasecmp(d->name + l - 4, ".zip");
		if (d->st == DL_RUN)
			acts[n] = A_PAUSE, ic[n] = IC_CLOCK, lab[n] = _("Pausar"), col[n++] = T.text;
		if (d->st == DL_PAUSED || d->st == DL_FAIL)
			acts[n] = A_RESUME, ic[n] = IC_REFRESH, lab[n] = d->st == DL_FAIL ? _("Reintentar") : _("Reanudar"), col[n++] = T.accent;
		if (d->st == DL_FAIL && !strcmp(d->err, "tls"))
			acts[n] = A_INSECURE, ic[n] = IC_SHIELD, lab[n] = _("Descargar sin verificar"), col[n++] = T.warn;
		if (d->st == DL_DONE && cia)
			acts[n] = A_INSTALL, ic[n] = IC_DOWNLOAD, lab[n] = _("Instalar"), col[n++] = T.ok;
		if (d->st == DL_DONE && zip)
			acts[n] = A_UNZIP, ic[n] = IC_ARCHIVE, lab[n] = _("Descomprimir aquí"), col[n++] = T.text;
		if (there)
			acts[n] = A_FOLDER, ic[n] = IC_FOLDER, lab[n] = _("Ver en su carpeta"), col[n++] = T.text;
		if (d->st == DL_RUN || d->st == DL_QUEUED || d->st == DL_PAUSED || d->st == DL_FAIL)
			acts[n] = A_CANCEL, ic[n] = IC_CLOSE, lab[n] = _("Cancelar la descarga"), col[n++] = T.danger;
		else
			acts[n] = A_REMOVE, ic[n] = IC_TRASH, lab[n] = _("Quitar de la lista"), col[n++] = T.text2;
	}
	int r = sheet_list(&DS.open, &DS.just, &DS.anim, &DS.sel, d ? (d->name[0] ? d->name : d->host) : "", ic, lab, col, n);
	if (r < 0 || !d)
		return;
	switch (acts[r]) {
	case A_PAUSE: dl_op(d->id, DL_PAUSE); break;
	case A_INSECURE:
		U.dlg_arg = (int)d->id;
		dialog_open(DLG_DLINSECURE, _("¿Descargar sin comprobar el certificado?"),
		            _("La consola no pudo verificar la identidad del servidor. Si continúas, no hay garantía de que el archivo no se haya modificado durante la transferencia. Continúa solo si confías en el enlace y en tu red."),
		            _("Descargar"), true);
		break;
	case A_RESUME: dl_op(d->id, DL_RESUME); break;
	case A_CANCEL: dl_op(d->id, DL_CANCEL); break;
	case A_REMOVE: dl_op(d->id, DL_REMOVE); break;
	case A_INSTALL: {
		char err[24] = "";
		if (strlen(d->path) >= 256 || ins_request(d->path, G.console, false, err, sizeof err) < 0)
			toast_icon(!strcmp(err, "already") ? _("Ya está pendiente de instalación en la consola") : _("La consola no reconoce este CIA"), IC_CLOSE, T.danger);
		else
			ins_refresh();
		break;
	}
	case A_UNZIP: {
		int zr = zipjob_extract(d->path, false, G.console);
		toast_icon(zr == ZIP_OK ? _("Extrayendo…") : zip_err_text(zr), zr == ZIP_OK ? IC_ARCHIVE : IC_CLOSE, zr == ZIP_OK ? T.warn : T.danger);
		break;
	}
	case A_FOLDER: ds_open_folder(d); break;
	}
}

// la hoja del reproductor: aleatorio, anterior, play/pausa, siguiente, repetir; parar e ir a la carpeta
static void music_sheet(void)
{
	if (CO.mu_wait > 0 && (--CO.mu_wait, mu_on())) // recién pedida: el reproductor sale en cuanto suena
		MU.open = MU.just = true, MU.sel = 2, CO.mu_wait = 0;
	MU.anim = approach(MU.anim, MU.open ? 1 : 0, 16);
	if (MU.anim < 0.01f)
		return;
	float a = ease_out_cubic(MU.anim), h = 150, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	draw_rrect(10, y + 3, BOT_W - 20, h, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, h, 16, T.surface);
	if (!MU.open)
		return;
	bool fresh = MU.just;
	MU.just = false;
	if (MU.s.st == MU_OFF) { // (con un archivo que falla, sigue abierta: pasa a la siguiente)
		MU.open = false;
		return;
	}
	text_draw_center_clip(MU.s.title, BOT_W / 2, y + 10, BOT_W - 50, TS_BODY, T.text);
	char sub[200], t1[16], t2[16];
	mu_fmt(MU.s.pos_ms, t1, sizeof t1);
	mu_fmt(MU.s.dur_ms, t2, sizeof t2);
	snprintf(sub, sizeof sub, "%d / %d · %s / %s", MU.s.idx + 1, MU.s.n, t1, MU.s.dur_ms ? t2 : "--:--");
	text_draw_center(sub, BOT_W / 2, y + 28, TS_SUB, T.text3);
	// los cinco botones
	static const IconId IC5[5] = {IC_SPARK, IC_CHEV_LEFT, IC_STOP, IC_CHEV_RIGHT, IC_REFRESH};
	float bw = 52, gx = (BOT_W - 5 * bw) / 2, by = y + 48;
	int run = -1;
	if (!fresh && U.buttons) {
		if (g_krepeat & KEY_LEFT) MU.sel = (MU.sel + 4) % 5;
		if (g_krepeat & KEY_RIGHT) MU.sel = (MU.sel + 1) % 5;
	}
	for (int i = 0; i < 5; i++) {
		float cx = gx + i * bw + bw / 2, cy = by + 20;
		bool on = (i == 0 && MU.s.shuffle) || (i == 4 && MU.s.repeat != MR_OFF);
		bool hl = touch_in(cx - 22, by, 44, 40) || (U.buttons && MU.sel == i);
		draw_circle(cx, cy, i == 2 ? 19 : 15, i == 2 ? T.accent : hl ? T.surface2 : T.bg);
		if (U.buttons && MU.sel == i)
			draw_ring(cx, cy, i == 2 ? 20 : 16, 1.5f, T.accent);
		if (i == 2)
			mu_icon(cx, cy, 17, MU.s.st == MU_PLAY, 0xFFFFFFFF);
		else
			draw_icon(IC5[i], cx, cy, 13, on ? T.accent : T.text2);
		if (i == 4 && MU.s.repeat == MR_ONE)
			text_draw_center("1", cx + 9, cy + 2, 0.34f, T.accent);
		if (!fresh && tap(cx - 22, by, 44, 40))
			run = i;
	}
	if (!fresh && U.buttons && (g_kdown & KEY_A))
		run = MU.sel;
	const char *mode = MU.s.repeat == MR_ONE ? _("Repetir canción") : MU.s.repeat == MR_ALL ? _("Repetir carpeta") : _("Sin repetir");
	char line[160];
	snprintf(line, sizeof line, "%s · %s", MU.s.shuffle ? _("Aleatorio") : _("En orden"), mode);
	text_draw_center(line, BOT_W / 2, by + 44, TS_SUB, T.text3);
	// parar y su carpeta
	float ly = y + h - 34, lw = (BOT_W - 44) / 2;
	char b1[48], b2[48];
	snprintf(b1, sizeof b1, "%s %s", GLYPH_X, _("Detener"));
	snprintf(b2, sizeof b2, "%s %s", GLYPH_Y, _("Ir a la carpeta"));
	draw_rrect(18, ly, lw, 26, 13, touch_in(18, ly, lw, 26) ? T.surface2 : T.bg);
	draw_rrect(26 + lw, ly, lw, 26, 13, touch_in(26 + lw, ly, lw, 26) ? T.surface2 : T.bg);
	text_draw_center_clip(b1, 18 + lw / 2, ly + 13 - text_lineh(TS_SUB) / 2, lw - 8, TS_SUB, T.danger);
	text_draw_center_clip(b2, 26 + lw * 1.5f, ly + 13 - text_lineh(TS_SUB) / 2, lw - 8, TS_SUB, T.text2);
	if (!fresh && (tap(18, ly, lw, 26) || (g_kdown & KEY_X))) {
		music_stop();
		MU.open = false;
	} else if (!fresh && (tap(26 + lw, ly, lw, 26) || (g_kdown & KEY_Y)) && strstr(MU.s.dir, CLOUD_CACHE)) {
		MU.open = false; // (de la nube: su carpeta es la de la nube, no la copia que se bajó)
		if (!CLU.open)
			cl_open();
	} else if (!fresh && (tap(26 + lw, ly, lw, 26) || (g_kdown & KEY_Y))) {
		MU.open = false;
		U.tab = TAB_FILES;
		char dir[512], nm[256];
		util_strlcpy(dir, MU.s.dir, sizeof dir);
		util_strlcpy(nm, MU.s.name, sizeof nm);
		if (!dir[0]) { // (de la biblioteca: la ruta entera, en el nombre)
			util_strlcpy(dir, MU.s.name, sizeof dir);
			char *sl = strrchr(dir, '/');
			util_strlcpy(nm, sl ? sl + 1 : dir, sizeof nm);
			if (sl)
				*sl = 0;
			if (!dir[0])
				util_strlcpy(dir, "/", sizeof dir);
			hm_launch(AP_FILES); // (a Archivos, a su carpeta)
		}
		f_load(dir);
		for (int i = 0; i < F.nv; i++)
			if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm))
				F.sel = i;
	} else if (run == 0)
		music_shuffle();
	else if (run == 1)
		music_prev();
	else if (run == 2)
		music_toggle();
	else if (run == 3)
		music_next();
	else if (run == 4)
		music_repeat();
	if (!fresh && ((g_kdown & KEY_B) || (g_touch.up && !g_touch.drag && g_touch.sy < y)))
		MU.open = false;
	g_kdown &= ~(KEY_A | KEY_B | KEY_X | KEY_Y);
	g_touch.up = false;
}

// ---- Música: la biblioteca (mlib) por canciones, artistas, álbumes y lo que no tiene etiquetas. A reproduce desde ahí
// la lista que se ve; Y, las opciones (ocultar, eliminar, ir a la carpeta); ⋮ en la cabecera, buscar de nuevo, dónde
// buscar y ver las ocultas. Todo lo de ocultar o borrar es opcional: la biblioteca solo lo apunta
enum { MLT_SONGS, MLT_ARTISTS, MLT_ALBUMS, MLT_NOTAG };
static struct {
	bool init;
	int tab, level;       // level 0: la lista de la pestaña; 1: los álbumes de un artista; 2: las canciones de un álbum
	char artist[64], album[64];
	List l[3];
	u32 rev, built;       // de qué biblioteca y de qué vista son las listas
	MSong *lib;           // copia de la biblioteca
	int nlib, nnotag, nhidden;
	int *ix, n;           // las canciones a la vista (índices de lib)
	char (*grp)[64];      // artistas o álbumes a la vista
	char (*gsub)[64];     // (de cada álbum: su artista)
	int *gcnt, ng;
	bool show_hidden;
	bool om, om_just;     // opciones de una canción o de un grupo
	float om_anim;
	int om_sel, om_i;     // om_i: fila de la lista
	bool hm, hm_just;     // opciones de la cabecera
	float hm_anim;
	int hm_sel;
	char del_path[256];   // (lo que se va a eliminar)
} ML;

static const char *ml_title(const MSong *m, char *buf, size_t n)
{
	if (m->title[0])
		return m->title;
	const char *b = strrchr(m->path, '/');
	util_strlcpy(buf, b ? b + 1 : m->path, n);
	char *dot = strrchr(buf, '.');
	if (dot)
		*dot = 0;
	return buf;
}

static const MSong *ML_SORT; // (para qsort)
static int ml_cmp_title(const void *a, const void *b)
{
	char x[96], y[96];
	return strcasecmp(ml_title(&ML_SORT[*(const int *)a], x, sizeof x), ml_title(&ML_SORT[*(const int *)b], y, sizeof y));
}
static int ml_cmp_track(const void *a, const void *b)
{
	const MSong *x = &ML_SORT[*(const int *)a], *y = &ML_SORT[*(const int *)b];
	if (x->track != y->track)
		return (x->track ? x->track : 9999) - (y->track ? y->track : 9999);
	return ml_cmp_title(a, b);
}

static bool ml_grouping(void) // ¿la lista que se ve es de grupos (artistas o álbumes)?
{
	return (ML.tab == MLT_ARTISTS && ML.level <= 1) || (ML.tab == MLT_ALBUMS && ML.level == 0);
}

// rehace la vista: de la biblioteca (si cambió), las canciones o los grupos que tocan
static void ml_build(void)
{
	u32 r = mlib_rev();
	if (!ML.lib) {
		ML.lib = malloc(sizeof(MSong) * MLIB_MAX);
		ML.ix = malloc(sizeof(int) * MLIB_MAX);
		ML.grp = malloc(sizeof *ML.grp * MLIB_MAX);
		ML.gsub = malloc(sizeof *ML.gsub * MLIB_MAX);
		ML.gcnt = malloc(sizeof(int) * MLIB_MAX);
		if (!ML.lib || !ML.ix || !ML.grp || !ML.gsub || !ML.gcnt)
			return;
		ML.rev = r - 1;
	}
	u32 key = (u32)ML.tab * 7919u + (u32)ML.level * 31u + (ML.show_hidden ? 1u : 0u) + (u32)strlen(ML.artist) * 131u + (u32)strlen(ML.album) * 977u;
	if (r == ML.rev && key == ML.built)
		return;
	if (r != ML.rev) {
		const MSong *src;
		int n = mlib_lock(&src);
		memcpy(ML.lib, src, sizeof(MSong) * n);
		mlib_unlock();
		ML.nlib = n, ML.rev = r;
		ML.nnotag = ML.nhidden = 0;
		for (int i = 0; i < n; i++)
			ML.nnotag += ML.lib[i].notag && !ML.lib[i].hidden, ML.nhidden += ML.lib[i].hidden;
		if (ML.tab == MLT_NOTAG && !ML.nnotag)
			ML.tab = MLT_SONGS, ML.level = 0;
	}
	ML.built = key;
	ML.n = ML.ng = 0;
	const char *art_unknown = _("Artista desconocido"), *alb_unknown = _("Álbum desconocido");
	for (int i = 0; i < ML.nlib; i++) {
		const MSong *m = &ML.lib[i];
		if (m->hidden && !ML.show_hidden)
			continue;
		const char *ar = m->artist[0] ? m->artist : art_unknown, *al = m->album[0] ? m->album : alb_unknown;
		bool group = false;
		const char *gname = NULL, *gsub = "";
		switch (ML.tab) {
		case MLT_SONGS: ML.ix[ML.n++] = i; break;
		case MLT_NOTAG:
			if (m->notag)
				ML.ix[ML.n++] = i;
			break;
		case MLT_ARTISTS:
			if (m->notag)
				break;
			if (ML.level == 0)
				group = true, gname = ar;
			else if (strcmp(ar, ML.artist))
				break;
			else if (ML.level == 1)
				group = true, gname = al;
			else if (!ML.album[0] || !strcmp(al, ML.album)) // (album vacío: todas las del artista)
				ML.ix[ML.n++] = i;
			break;
		case MLT_ALBUMS:
			if (m->notag)
				break;
			if (ML.level == 0)
				group = true, gname = al, gsub = ar;
			else if (!strcmp(al, ML.album))
				ML.ix[ML.n++] = i;
			break;
		}
		if (group) {
			int g = 0;
			while (g < ML.ng && strcmp(ML.grp[g], gname))
				g++;
			if (g == ML.ng && ML.ng < MLIB_MAX) {
				util_strlcpy(ML.grp[g], gname, sizeof ML.grp[0]);
				util_strlcpy(ML.gsub[g], gsub, sizeof ML.gsub[0]);
				ML.gcnt[g] = 0;
				ML.ng++;
			}
			if (g < ML.ng)
				ML.gcnt[g]++;
		}
	}
	ML_SORT = ML.lib;
	qsort(ML.ix, ML.n, sizeof *ML.ix, ML.level == 2 ? ml_cmp_track : ml_cmp_title);
	// los grupos, por nombre (los «desconocidos», al final); con sus contadores y subtítulos a la par
	for (int a = 1; a < ML.ng; a++)
		for (int b = a; b > 0; b--) {
			bool ua = !strcmp(ML.grp[b - 1], art_unknown) || !strcmp(ML.grp[b - 1], alb_unknown);
			bool ub = !strcmp(ML.grp[b], art_unknown) || !strcmp(ML.grp[b], alb_unknown);
			if (ua == ub ? strcasecmp(ML.grp[b - 1], ML.grp[b]) <= 0 : ub)
				break;
			char t[64], u[64];
			memcpy(t, ML.grp[b], 64), memcpy(ML.grp[b], ML.grp[b - 1], 64), memcpy(ML.grp[b - 1], t, 64);
			memcpy(u, ML.gsub[b], 64), memcpy(ML.gsub[b], ML.gsub[b - 1], 64), memcpy(ML.gsub[b - 1], u, 64);
			int c = ML.gcnt[b];
			ML.gcnt[b] = ML.gcnt[b - 1], ML.gcnt[b - 1] = c;
		}
	List *l = &ML.l[ML.level];
	int cnt = ml_grouping() ? ML.ng + (ML.tab == MLT_ARTISTS && ML.level == 1) : ML.n;
	if (l->sel >= cnt)
		l->sel = cnt ? cnt - 1 : 0;
}

// reproducir las canciones a la vista, empezando por start (aleatorio: en otro orden)
static void ml_play(int start, bool shuffle)
{
	static char (*names)[256];
	if (!names && !(names = malloc(sizeof *names * MLIB_MAX)))
		return;
	if (!ML.n)
		return;
	for (int k = 0; k < ML.n; k++)
		util_strlcpy(names[k], ML.lib[ML.ix[k]].path, sizeof names[0]);
	music_set_shuffle(shuffle); // (antes de empezar: la elegida suena la primera)
	if (!music_play("", names, ML.n, start)) {
		MusicState ms;
		music_get(&ms);
		toast_icon(!strcmp(ms.err, "no_dsp") ? _("Sin sonido: falta el firmware DSP (dspfirm.cdc)") : _("No se pudo reproducir"), IC_CLOSE, T.danger);
		return;
	}
	CO.mu_wait = 300; // el reproductor, en cuanto suene
}

// las canciones de un grupo (artista o álbum) a la vista, para reproducirlas sin entrar
static void ml_enter(int i, bool play, bool shuffle)
{
	if (ML.tab == MLT_ARTISTS && ML.level == 0)
		util_strlcpy(ML.artist, ML.grp[i], sizeof ML.artist), ML.album[0] = 0, ML.level = 1;
	else if (ML.tab == MLT_ARTISTS && ML.level == 1) {
		if (i == 0) // «Todas las canciones»
			ML.album[0] = 0;
		else
			util_strlcpy(ML.album, ML.grp[i - 1], sizeof ML.album);
		ML.level = 2;
	} else if (ML.tab == MLT_ALBUMS && ML.level == 0)
		util_strlcpy(ML.album, ML.grp[i], sizeof ML.album), ML.level = 2;
	ML.l[ML.level].sel = 0, ML.l[ML.level].off = ML.l[ML.level].target = 0;
	ml_build();
	if (play) { // (sin quedarse dentro)
		ml_play(0, shuffle);
		ML.level = ML.tab == MLT_ARTISTS && ML.level == 2 && i == 0 ? 1 : ML.level - (ML.tab == MLT_ALBUMS ? 2 : 1);
		if (ML.level < 0)
			ML.level = 0;
		ml_build();
	}
}

static void ml_top(void)
{
	if (mu_on()) { // sonando: el reproductor
		music_top(1);
		return;
	}
	float y0 = BODY_Y, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
	const float X = CARD_X + 16, W = CARD_W - 32;
	draw_circle(X + 30, y0 + 40, 26, col_alpha(T.accent, 0.16f));
	draw_icon(IC_SPEAKER, X + 30, y0 + 40, 24, T.accent);
	float tx = X + 70, tw = X + W - tx, y = y0 + 18;
	text_draw_clip(_("Biblioteca de música"), tx, y, tw, TS_TITLE, T.text);
	y += text_lineh(TS_TITLE) + 4;
	int na = 0, nb = 0;
	{ // (artistas y álbumes distintos)
		static u32 seen = 0xFFFFFFFF;
		static int ca, cb;
		if (seen != ML.rev) {
			seen = ML.rev, ca = cb = 0;
			for (int i = 0; i < ML.nlib; i++) {
				const MSong *m = &ML.lib[i];
				if (m->hidden || m->notag)
					continue;
				bool na1 = true, nb1 = true;
				for (int k = 0; k < i && (na1 || nb1); k++) {
					const MSong *o = &ML.lib[k];
					if (o->hidden || o->notag)
						continue;
					if (!strcmp(o->artist, m->artist))
						na1 = false;
					if (!strcmp(o->album, m->album))
						nb1 = false;
				}
				ca += na1, cb += nb1;
			}
		}
		na = ca, nb = cb;
	}
	char l1[160];
	snprintf(l1, sizeof l1, _("%d canciones · %d artistas · %d álbumes"), ML.nlib - ML.nhidden, na, nb);
	text_draw_clip(l1, tx, y, tw, TS_SUB, T.text2);
	y += text_lineh(TS_SUB) + 4;
	int found;
	if (mlib_scanning(&found)) {
		char l2[96];
		snprintf(l2, sizeof l2, _("Buscando música en la SD… %d"), found);
		text_draw_clip(l2, tx, y, tw, TS_SUB, T.accent);
		y += text_lineh(TS_SUB) + 4;
	}
	y = fmaxf(y, y0 + 76) + 8;
	draw_rect(X, y, W, 1, T.divider);
	y += 10;
	char l3[200];
	if (ML.nnotag)
		snprintf(l3, sizeof l3, _("%d canciones no tienen etiquetas (título, artista o álbum). Aparecen en «Sin datos», donde puedes ocultarlas o eliminarlas."), ML.nnotag);
	else
		snprintf(l3, sizeof l3, "%s", _("Las canciones se agrupan por artista y álbum según sus etiquetas."));
	y += text_wrap_lines(l3, W, TS_SUB, 3, X, y, T.text2, true) * text_lineh(TS_SUB) + 8;
	char hint[160];
	snprintf(hint, sizeof hint, _("%s Reproducir · %s Opciones · L R Pestañas"), GLYPH_A, GLYPH_Y);
	text_draw_right(hint, X + W, y0 + h - 10 - text_lineh(TS_SUB), TS_SUB, T.text3);
}

static void ml_bottom(void)
{
	if (!ML.init) {
		ML.init = true;
		mlib_init();
	}
	ml_build();
	if (!ML.lib)
		return;
	bool sheet = ML.om || ML.hm || g_dialog.open;
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	if (sheet) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	// cabecera: en la lista, las pestañas; dentro de un artista o un álbum, atrás y su nombre
	if (ML.level == 0) {
		tab_header_w("", false, 0);
		const char *opts[4] = {_("Canciones"), _("Artistas"), _("Álbumes"), _("Sin datos")};
		int nt = ML.nnotag ? 4 : 3;
		float sw = (BOT_W - 40 - 36) / nt;
		int r = segmented(40, 5, sw * nt, 22, opts, nt, ML.tab, false);
		for (int k = 0; k < nt; k++)
			if (hdr_hit(40 + k * sw, 5, sw, 22))
				r = k;
		if ((g_kdown & (KEY_L | KEY_ZL)) && ML.tab > 0)
			r = ML.tab - 1;
		if ((g_kdown & (KEY_R | KEY_ZR)) && ML.tab < nt - 1)
			r = ML.tab + 1;
		if (r >= 0 && r != ML.tab)
			ML.tab = r, ML.l[0].sel = 0, ML.l[0].off = ML.l[0].target = 0;
		if ((g_kdown & KEY_B) && !g_dialog.open) {
			hm_show();
			return;
		}
	} else {
		tab_header_w(ML.level == 2 && ML.album[0] ? ML.album : ML.artist, true, 230);
		if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
			ML.level = ML.tab == MLT_ARTISTS ? ML.level - 1 : 0;
	}
	float hx = BOT_W - 24, hy = 16;
	draw_circle(hx, hy, 12, touch_in(hx - 14, hy - 14, 28, 28) ? T.surface2 : T.surface);
	draw_icon(IC_DOTS, hx, hy, 14, T.text2);
	if (hdr_hit(hx - 13, hy - 13, 26, 26))
		ML.hm = ML.hm_just = true, ML.hm_sel = 0;
	ml_build();
	List *l = &ML.l[ML.level];
	bool grp = ml_grouping();
	bool all_row = ML.tab == MLT_ARTISTS && ML.level == 1; // «Todas las canciones» del artista, la primera
	int cnt = grp ? ML.ng + all_row : ML.n;
	const float RH = 40;
	list_update(l, cnt, RH);
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	if (!cnt) {
		int found;
		bool sc = mlib_scanning(&found);
		empty_state(IC_SPEAKER, sc ? _("Buscando música…") : _("No hay música"),
		            sc ? _("Se analiza toda la tarjeta SD. La primera vez puede tardar unos minutos.") : _("Copia canciones MP3, OGG o WAV a la SD (por ejemplo, a /Music) y aparecerán aquí."));
	}
	for (int i = 0; i < cnt; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		bool was = l->sel == i;
		bool t = row(l, i, y, RH);
		if (!U.buttons && l->sel == i)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		char t1[128], t2[160];
		IconId ic = IC_SPEAKER;
		u32 col = T.accent;
		bool dim = false;
		if (grp && all_row && i == 0) {
			util_strlcpy(t1, _("Todas las canciones"), sizeof t1);
			int c = 0;
			for (int g = 0; g < ML.ng; g++)
				c += ML.gcnt[g];
			snprintf(t2, sizeof t2, c == 1 ? _("1 canción") : _("%d canciones"), c);
			ic = IC_LIST, col = T.accent2;
		} else if (grp) {
			int g = i - all_row;
			util_strlcpy(t1, ML.grp[g], sizeof t1);
			if (ML.tab == MLT_ALBUMS && ML.gsub[g][0])
				snprintf(t2, sizeof t2, ML.gcnt[g] == 1 ? _("%s · 1 canción") : _("%s · %d canciones"), ML.gsub[g], ML.gcnt[g]);
			else
				snprintf(t2, sizeof t2, ML.gcnt[g] == 1 ? _("1 canción") : _("%d canciones"), ML.gcnt[g]);
			ic = ML.tab == MLT_ARTISTS && ML.level == 0 ? IC_USER : IC_STAR, col = ML.tab == MLT_ARTISTS && ML.level == 0 ? T.accent2 : T.warn;
		} else {
			const MSong *m = &ML.lib[ML.ix[i]];
			char tb[128];
			util_strlcpy(t1, ml_title(m, tb, sizeof tb), sizeof t1);
			if (m->notag) {
				util_strlcpy(t2, m->path, sizeof t2);
				char *sl = strrchr(t2, '/');
				if (sl && sl != t2)
					*sl = 0;
			} else if (ML.level == 2 && m->track)
				snprintf(t2, sizeof t2, "%d · %s", m->track, m->artist[0] ? m->artist : _("Artista desconocido"));
			else
				snprintf(t2, sizeof t2, "%s%s%s", m->artist, m->artist[0] && m->album[0] ? " · " : "", m->album);
			dim = m->hidden;
			if (m->notag)
				col = T.text3;
		}
		draw_rrect(14, y + (RH - 26) / 2, 26, 26, 8, col_alpha(col, 0.16f));
		draw_icon(ic, 27, y + RH / 2, 14, col);
		g_clip_ell = l->sel != i;
		text_draw_clip(t1, 50, y + 5, BOT_W - 90, TS_BODY, dim ? T.text3 : T.text);
		if (dim) {
			char t3[200];
			snprintf(t3, sizeof t3, "%s · %s", _("Oculta"), t2);
			util_strlcpy(t2, t3, sizeof t2);
		}
		text_draw_clip(t2, 50, y + 22, BOT_W - 90, TS_SUB, T.text3);
		g_clip_ell = false;
		if (l->sel == i) {
			draw_circle(BOT_W - 26, y + RH / 2, 11, col_alpha(T.accent, 0.3f));
			draw_icon(IC_DOTS, BOT_W - 26, y + RH / 2, 14, T.accent);
		}
		bool open = (t && was) || activate(l, i);
		if (t && was && g_touch.px > BOT_W - 44)
			ML.om = ML.om_just = true, ML.om_sel = 0, ML.om_i = i;
		else if (open) {
			if (grp && all_row && i == 0)
				ml_enter(0, false, false);
			else if (grp)
				ml_enter(i, false, false);
			else
				ml_play(i, false);
		}
	}
	gfx_clip_off();
	if ((g_kdown & KEY_Y) && cnt && !ML.om)
		ML.om = ML.om_just = true, ML.om_sel = 0, ML.om_i = l->sel;
	if (sheet)
		g_kdown = kd, g_krepeat = kr, g_touch = tc;
	// opciones de lo elegido
	{
		IconId ic[6];
		const char *lab[6];
		u32 col[6];
		int act[6], n = 0;
		char title[128] = "";
		const MSong *m = !grp && ML.om_i >= 0 && ML.om_i < ML.n ? &ML.lib[ML.ix[ML.om_i]] : NULL;
		if (grp) {
			util_strlcpy(title, all_row && ML.om_i == 0 ? _("Todas las canciones") : ML.om_i - all_row >= 0 && ML.om_i - all_row < ML.ng ? ML.grp[ML.om_i - all_row] : "", sizeof title);
			ic[n] = IC_SPEAKER, lab[n] = _("Reproducir"), col[n] = T.accent, act[n++] = 0;
			ic[n] = IC_SPARK, lab[n] = _("Reproducir en aleatorio"), col[n] = T.text, act[n++] = 1;
			ic[n] = IC_FOLDER, lab[n] = _("Abrir"), col[n] = T.text, act[n++] = 2;
		} else if (m) {
			char tb[128];
			util_strlcpy(title, ml_title(m, tb, sizeof tb), sizeof title);
			ic[n] = IC_SPEAKER, lab[n] = _("Reproducir"), col[n] = T.accent, act[n++] = 3;
			ic[n] = IC_SPARK, lab[n] = _("Reproducir en aleatorio"), col[n] = T.text, act[n++] = 4;
			ic[n] = m->hidden ? IC_CHECK : IC_CLOSE, lab[n] = m->hidden ? _("Volver a mostrar") : _("Ocultar de la biblioteca"), col[n] = T.text, act[n++] = 5;
			ic[n] = IC_FOLDER, lab[n] = _("Ir a la carpeta"), col[n] = T.text, act[n++] = 6;
			ic[n] = IC_TRASH, lab[n] = _("Eliminar (a la papelera)"), col[n] = T.danger, act[n++] = 7;
		}
		if (!n)
			ML.om = false;
		int r = sheet_list(&ML.om, &ML.om_just, &ML.om_anim, &ML.om_sel, title, ic, lab, col, n);
		if (r >= 0) {
			int a = act[r];
			if (a <= 2)
				ml_enter(all_row ? ML.om_i : ML.om_i, a != 2, a == 1);
			else if (m && (a == 3 || a == 4)) {
				ml_play(ML.om_i, a == 4);
			} else if (m && a == 5) {
				mlib_hide(m->path, !m->hidden);
				toast_icon(m->hidden ? _("La canción vuelve a mostrarse en la biblioteca") : _("Canción oculta. Puedes mostrarla con «Mostrar ocultas»."), IC_CHECK, T.text2);
			} else if (m && a == 6) {
				char dir[256];
				util_strlcpy(dir, m->path, sizeof dir);
				char *sl = strrchr(dir, '/');
				if (sl)
					*sl = 0;
				hm_launch(AP_FILES);
				f_load(dir[0] ? dir : "/");
			} else if (m && a == 7) {
				util_strlcpy(ML.del_path, m->path, sizeof ML.del_path);
				char msg[300], tb[96];
				snprintf(msg, sizeof msg, _("«%s» se moverá a la papelera de la SD. Podrás recuperarla durante unos días."), ml_title(m, tb, sizeof tb));
				dialog_open(DLG_MLDEL, _("¿Eliminar la canción?"), msg, _("Eliminar"), true);
			}
		}
	}
	// opciones de la cabecera: buscar otra vez, dónde, las ocultas y lo que no tiene datos
	{
		IconId ic[6];
		const char *lab[6];
		u32 col[6];
		int act[6], n = 0;
		ic[n] = IC_REFRESH, lab[n] = _("Volver a buscar música"), col[n] = T.text, act[n++] = 0;
		ic[n] = IC_FOLDER, lab[n] = mlib_where() == MLS_ALL ? _("Buscar solo en /Music") : _("Buscar en toda la SD"), col[n] = T.text, act[n++] = 1;
		ic[n] = ML.show_hidden ? IC_CLOSE : IC_CHECK, lab[n] = ML.show_hidden ? _("No mostrar ocultas") : _("Mostrar ocultas"), col[n] = T.text, act[n++] = 2;
		if (ML.tab == MLT_NOTAG && ML.n) {
			ic[n] = IC_CLOSE, lab[n] = _("Ocultar todas las canciones sin datos"), col[n] = T.text, act[n++] = 3;
			ic[n] = IC_TRASH, lab[n] = _("Eliminar todas las canciones sin datos"), col[n] = T.danger, act[n++] = 4;
		}
		int r = sheet_list(&ML.hm, &ML.hm_just, &ML.hm_anim, &ML.hm_sel, _("Música"), ic, lab, col, n);
		if (r >= 0) {
			int a = act[r];
			if (a == 0) {
				mlib_scan();
				toast_icon(_("Buscando música en la SD…"), IC_REFRESH, T.accent);
			} else if (a == 1) {
				mlib_set_where(mlib_where() == MLS_ALL ? MLS_MUSIC : MLS_ALL);
				toast_icon(mlib_where() == MLS_ALL ? _("Buscando en toda la SD…") : _("Buscando solo en las carpetas de música…"), IC_REFRESH, T.accent);
			} else if (a == 2)
				ML.show_hidden = !ML.show_hidden, ML.built = 0xFFFFFFFF;
			else if (a == 3) {
				for (int k = 0; k < ML.n; k++)
					mlib_hide(ML.lib[ML.ix[k]].path, true);
				toast_icon(_("Canciones ocultadas. Puedes volver a mostrarlas desde ⋮."), IC_CHECK, T.text2);
			} else if (a == 4) {
				char msg[200];
				snprintf(msg, sizeof msg, _("Las %d canciones sin datos se moverán a la papelera de la SD. Podrás recuperarlas durante unos días."), ML.n);
				ML.del_path[0] = 0;
				dialog_open(DLG_MLDEL, _("¿Eliminar las canciones sin datos?"), msg, _("Eliminar"), true);
			}
		}
	}
}

// copias en cola: una por frame como mucho, cuando la anterior terminó
static void cq_tick(void)
{
	if (CQ.i >= CQ.n)
		return;
	int st, err;
	u64 d, t;
	ops_job(&st, &err, &d, &t);
	if (st == OPS_RUNNING)
		return;
	int r = ops_copy_start(CQ.src[CQ.i], CQ.dst[CQ.i], G.console);
	if (r != OPS_BUSY)
		CQ.i++;
	if (CQ.i >= CQ.n) {
		free(CQ.src), free(CQ.dst);
		CQ.src = NULL, CQ.dst = NULL;
		CQ.n = CQ.i = 0;
	}
}

static const char *ap_name(int i);
// ¿en la carpeta con la que se abrió la aplicación? (Archivos: la raíz de la SD)
static bool f_app_root(void) { return !strcmp(F.path, U.app_root[0] ? U.app_root : "/"); }
static void tab_files(void)
{
	if (!F.e)
		f_load("/");
	if (F.need_reload)
		f_reload_keep();
	f_mtime_step();
	// cabecera: atrás, la carpeta actual y, a la derecha, «nueva carpeta»
	bool root = f_app_root() && U.app != AP_DL; // (en lo descargado, la flecha vuelve a las tarjetas)
	const char *nm = F.trash ? _("Papelera") : F.titles ? _("Títulos") : F.nosaves ? _("Partidas") : f_app_root() && U.app == AP_DL ? _("Archivos descargados") : root && U.app == AP_MUSIC ? ap_name(U.app) : !strcmp(F.path, "/") ? "SD" : strrchr(F.path, '/') + 1;
	char sq[160];
	if (F.search) {
		snprintf(sq, sizeof sq, "«%s»", SR.q);
		nm = sq;
		if (SR.shown != SR.n || (SR.done && SR.running)) { // llegaron más resultados: a la lista, sin perder el elegido
			SR.running = !SR.done;
			int keep = F.sel;
			float off = F.target;
			f_load("@search");
			F.sel = keep < F.nv ? keep : F.nv - 1;
			F.target = off;
		}
	}
	bool grid = V.f_grid && !F.search; // los resultados, siempre en lista: cada uno dice su carpeta
	th_poll();
	if (MS.on) { // selección múltiple: ✕ para salir, cuántos van y «Todo»
		char t[48];
		snprintf(t, sizeof t, MS.n == 1 ? _("1 seleccionado") : _("%d seleccionados"), MS.n);
		bool xp = touch_in(0, 0, 150, VIEW_Y);
		if (xp)
			draw_rrect(6, 4, text_width(t, TS_TITLE) + 40, 24, 12, T.surface);
		draw_icon(IC_CLOSE, 20, 16, 12, T.text2);
		text_draw_clip(t, 34, 7, 180, TS_TITLE, T.text);
		const char *all = MS.n >= F.n && F.n ? _("Ninguno") : _("Todo");
		float aw = text_width(all, TS_SUB) + 20, ax = BOT_W - 12 - aw;
		draw_rrect(ax, 6, aw, 20, 10, touch_in(ax, 4, aw, 24) ? T.surface2 : T.surface);
		text_draw_center(all, ax + aw / 2, 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.accent);
		if (!g_dialog.open && (hdr_hit(ax, 6, aw, 20) || (U.buttons && (g_kdown & KEY_X))))
			ms_all();
		if (!g_dialog.open && (tap(0, 0, 150, VIEW_Y) || (U.buttons && (g_kdown & KEY_B)))) {
			ms_end();
			g_kdown &= ~KEY_B;
		}
		g_kdown &= ~KEY_X;
	} else if (F.search) { // búsqueda: atrás a donde se buscó y, a la derecha, cuántos van
		tab_header_w(nm, true, 170);
		char c[48];
		if (!SR.done)
			snprintf(c, sizeof c, "%s", _("Buscando…"));
		else
			snprintf(c, sizeof c, SR.n >= SR_MAX ? _("%d o más") : SR.n == 1 ? _("1 resultado") : _("%d resultados"), SR.n);
		if (!SR.done) // un punto que gira mientras se busca
			draw_circle(BOT_W - 18 + 5 * cosf(osGetTime() / 120.0f), 16 + 5 * sinf(osGetTime() / 120.0f), 2.5f, T.accent);
		text_draw_right(c, BOT_W - (SR.done ? 14 : 30), 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.text3);
		if (!g_dialog.open && (tap(0, 0, 170, VIEW_Y) || (U.buttons && (g_kdown & KEY_B)))) {
			sr_close();
			g_kdown &= ~KEY_B;
			return;
		}
	} else if (F.titles) { // instalados: atrás a la raíz y, a la derecha, cuántos y cuánto ocupan
		tab_header_w(nm, !root, 130);
		u64 tot = 0;
		for (int i = 0; i < TI_n; i++)
			tot += TI[i].size + TI[i].upd_size + TI[i].dlc_size;
		char c[64], sz[24];
		util_fmt_size(sz, sizeof sz, tot);
		snprintf(c, sizeof c, TI_n == 1 ? _("1 título · %s") : _("%d títulos · %s"), TI_n, sz);
		// el orden: por nombre o por tamaño (tocándolo o con Y)
		const char *ord = TI_by_size ? _("Por tamaño") : _("Por nombre");
		float ow = text_width(ord, TS_SUB) + 20, ox = BOT_W - 10 - ow;
		draw_rrect(ox, 6, ow, 20, 10, touch_in(ox, 4, ow, 24) ? T.surface2 : T.surface);
		text_draw_center(ord, ox + ow / 2, 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.accent);
		text_draw_right(c, ox - 8, 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.text3);
		if (!g_dialog.open && (hdr_hit(ox, 6, ow, 20) || (U.buttons && (g_kdown & KEY_Y) && !TT.open))) {
			TI_by_size = !TI_by_size;
			g_kdown &= ~KEY_Y;
			f_load("@titles");
			return;
		}
		if (root && !g_dialog.open && U.buttons && (g_kdown & KEY_B)) { // abierta desde el inicio: B, al inicio
			hm_show();
			return;
		}
		if (!root && !g_dialog.open && (tap(0, 0, 150, VIEW_Y) || (U.buttons && (g_kdown & KEY_B)))) {
			if (!f_back_to_origin())
				f_load("/");
			g_kdown &= ~KEY_B;
			return;
		}
	} else if (F.trash) { // papelera: atrás a la raíz y «Vaciar»
		tab_header(nm, !root);
		if (F.n) {
			const char *vt = _("Vaciar");
			float vw = text_width(vt, TS_SUB) + 22, vx = BOT_W - 12 - vw;
			draw_rrect(vx, 6, vw, 20, 10, touch_in(vx, 4, vw, 24) ? col_alpha(T.danger, 0.35f) : col_alpha(T.danger, 0.16f));
			text_draw_center(vt, vx + vw / 2, 16 - text_lineh(TS_SUB) / 2, TS_SUB, T.danger);
			if (!g_dialog.open && hdr_hit(vx, 6, vw, 20))
				dialog_open(DLG_TRASHEMPTY, _("¿Vaciar la papelera?"), _("Todo el contenido de la papelera se eliminará de forma permanente."), _("Vaciar"), true);
		}
		if (root && !g_dialog.open && U.buttons && (g_kdown & KEY_B)) {
			hm_show();
			return;
		}
		if (!root && !g_dialog.open && (tap(0, 0, 150, VIEW_Y) || (U.buttons && (g_kdown & KEY_B)))) {
			f_load("/");
			g_kdown &= ~KEY_B;
			return;
		}
	} else {
		tab_header_w(nm, !root || F.nosaves, 150);
	}
	float nx = BOT_W - 24, ny = 16;
	bool np = touch_in(nx - 14, ny - 14, 28, 28);
	// a la derecha: buscar, ordenar y agrupar, lista o iconos, y «+»
	float gx = nx - 32, sx = nx - 64, lx = nx - 96;
	if (!F.missing && !F.nosaves && !MS.on && !F.trash && !F.search && !F.titles) {
		draw_circle(lx, ny, 12, touch_in(lx - 14, ny - 14, 28, 28) ? T.surface2 : T.surface);
		draw_icon(IC_SEARCH, lx, ny, 14, T.text2);
		draw_circle(gx, ny, 12, touch_in(gx - 14, ny - 14, 28, 28) ? T.surface2 : T.surface);
		draw_icon(V.f_grid ? IC_LIST : IC_GRID, gx, ny, 14, T.text2);
		draw_circle(sx, ny, 12, touch_in(sx - 14, ny - 14, 28, 28) ? T.surface2 : T.surface);
		draw_icon(IC_SORT, sx, ny, 14, T.text2);
		draw_circle(nx, ny, 12, np ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
		draw_icon(IC_PLUS, nx, ny, 13, 0xFFFFFFFF);
		bool h_find = hdr_hit(lx - 13, ny - 13, 26, 26), h_sort = hdr_hit(sx - 13, ny - 13, 26, 26), h_grid = hdr_hit(gx - 13, ny - 13, 26, 26),
		     h_new = hdr_hit(nx - 13, ny - 13, 26, 26);
		if (!g_dialog.open && h_find) {
			U.want_search = true;
			return;
		}
		if (!g_dialog.open && (h_new || (U.buttons && !FP.mode && !CC.mode && (g_kdown & KEY_X)))) {
			f_new_menu();
			return;
		}
		if (!g_dialog.open && h_grid) {
			f_set_view(-1, -1, -1, !V.f_grid);
			return;
		}
		// también con Y en los accesos rápidos
		if (!g_dialog.open && (h_sort || (U.buttons && f_on_chips() && (g_kdown & KEY_Y)))) {
			VS.open = VS.just = true;
			VS.sel = 0;
			return;
		}
	}
	if ((!root || F.nosaves) && !MS.on && !F.trash && !F.search && !F.titles && !g_dialog.open && tap(0, 0, 170, VIEW_Y)) {
		if (F.nosaves)
			f_reload_keep();
		else if (!f_back_to_results())
			f_up();
		return;
	}
	// desplazamiento con el dedo o siguiendo la selección (con lo copiado, la barra de pegar tapa el final)
	float barh = FP.mode || CC.mode ? 42 : 0, max = fmaxf(0, F.total + barh - VIEW_H);
	if (g_touch.held && g_touch.drag && g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H) {
		float dy = g_touch.y - g_touch.py;
		if (!f_chips_shown() && F.target <= 0 && (dy > 0 || F.pull > 0)) {
			F.pull = fmaxf(0, F.pull + dy); // arriba del todo y tirando: se estira
		} else {
			F.target -= dy;
			F.off = F.target;
		}
	} else if (F.pull > 0) {
		if (F.pull >= 40)
			f_show_chips();
		F.pull = 0;
	}
	// con la cruceta: ↑ en los accesos rápidos va a la cabecera; ↑ en lo primero los saca (y donde no hay, a la cabecera)
	if (U.buttons && !g_dialog.open && F.sel == 0 && (g_krepeat & KEY_UP)) {
		if (f_on_chips())
			hdr_enter();
		else if (!f_chips_shown()) {
			f_show_chips();
			if (f_chips_shown())
				F.sel = 0;
			else
				hdr_enter();
		} else
			hdr_enter();
		g_krepeat &= ~KEY_UP;
	}
	if (U.buttons && !g_dialog.open && (g_krepeat & (KEY_UP | KEY_DOWN))) {
		int d = (g_krepeat & KEY_DOWN) ? 1 : -1;
		const FView *c = &F.v[F.sel];
		if (grid && c->kind == FV_ITEM) { // iconos: la casilla de la fila de arriba o de abajo más cerca en x
			int best = -1;
			for (int k = F.sel + d; k >= 0 && k < F.nv; k += d) {
				const FView *o = &F.v[k];
				if (o->y == c->y)
					continue;
				if (best >= 0 && o->y != F.v[best].y)
					break;
				if (o->kind != FV_ITEM) {
					if (best < 0)
						best = k;
					break;
				}
				if (best < 0 || fabsf(o->x - c->x) < fabsf(F.v[best].x - c->x))
					best = k;
			}
			F.sel = best >= 0 ? best : F.sel;
		} else {
			F.sel += d;
		}
		F.sel = F.sel < 0 ? 0 : F.sel >= F.nv ? F.nv - 1 : F.sel;
		const FView *v = &F.v[F.sel];
		if (v->y < F.target)
			F.target = v->y;
		if (v->y + v->h > F.target + VIEW_H - barh)
			F.target = v->y + v->h - VIEW_H + barh;
	}
	if (U.buttons && !g_dialog.open && f_on_chips() && (g_krepeat & (KEY_LEFT | KEY_RIGHT)))
		F.chip = (F.chip + ((g_krepeat & KEY_RIGHT) ? 1 : CH_SHOWN - 1)) % CH_SHOWN;
	if (U.buttons && !g_dialog.open && F.search && (g_krepeat & (KEY_LEFT | KEY_RIGHT)) && sr_nkinds() > 1)
		sr_next_kind((g_krepeat & KEY_RIGHT) ? 1 : -1);
	if (U.buttons && !g_dialog.open && grid && F.sel >= 0 && F.v[F.sel].kind == FV_ITEM && (g_krepeat & (KEY_LEFT | KEY_RIGHT))) {
		int k = F.sel + ((g_krepeat & KEY_RIGHT) ? 1 : -1);
		if (k >= 0 && k < F.nv && F.v[k].kind == FV_ITEM && F.v[k].y == F.v[F.sel].y)
			F.sel = k;
	}
	F.target = clampf(F.target, 0, max);
	F.off = approach(F.off, F.target, 18);

	bool inview = g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H && !g_dialog.open;
	if (MS.on && g_touch.sy >= VIEW_Y + VIEW_H - QR_PAD - 46) // la barra de la selección tapa las filas de debajo
		inview = false;
	int open = -1, menu = -1, pick = -1, msel = -1; // fila que se abre, cuyo menú se pide, que se marca o con la que empieza la selección
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	float po = fminf(F.pull, 70) * 0.6f; // lo que baja la lista al tirar
	if (po > 1) {
		float a = clampf(F.pull / 40, 0, 1);
		draw_icon(F.pull >= 40 ? IC_UP : IC_DOWN, BOT_W / 2 - text_width(_("Accesos rápidos"), TS_SUB) / 2 - 12, VIEW_Y + po / 2, 11, col_alpha(T.accent, a));
		text_draw_center(_("Accesos rápidos"), BOT_W / 2, VIEW_Y + po / 2 - text_lineh(TS_SUB) / 2, TS_SUB, col_alpha(T.accent, a));
	}
	for (int i = 0; i < F.nv; i++) {
		const FView *v = &F.v[i];
		float y = VIEW_Y + v->y - F.off + po;
		if (y + v->h < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		if (v->kind == FV_CHIPS) {
			float cw = (BOT_W - 12) / (float)CH_SHOWN;
			for (int c = 0; c < CH_SHOWN; c++) {
				float cx = 6 + c * cw;
				bool here = c == CH_SAVES ? (F.nosaves || strstr(F.path, "/Checkpoint/saves") || !strncasecmp(F.path, "/JKSV", 5))
				                          : !F.nosaves && !strcasecmp(F.path, CHIP_PATH[c]);
				bool hl = here || (U.buttons && f_on_chips() && F.chip == c) || (inview && touch_in(cx, y + 2, cw, 40));
				if (hl)
					draw_rrect(cx + 2, y + 3, cw - 4, 38, 10, here ? col_alpha(T.accent, 0.18f) : T.surface);
				draw_icon(chip_icon(c), cx + cw / 2, y + 15, 16, here ? T.accent : T.text2);
				// el nombre entero: si no cabe, un poco más pequeño (nunca cortado)
				float sc = 0.32f, lw = text_width(chip_name(c), sc);
				if (lw > cw - 4)
					sc *= (cw - 4) / lw, lw = cw - 4;
				text_draw(chip_name(c), cx + (cw - lw) / 2, y + 26 + (text_lineh(0.32f) - text_lineh(sc)) / 2, sc, here ? T.accent : T.text3);
				if (inview && tap(cx, y + 2, cw, 40))
					f_open_chip(c);
			}
			if (U.buttons && f_on_chips() && !g_dialog.open && (g_kdown & KEY_A))
				f_open_chip(F.chip);
			continue;
		}
		bool pressed = inview && touch_in(6, y, BOT_W - 12, v->h);
		bool selected = i == F.sel && (U.buttons || f_selected());
		if (v->kind == FV_GROUP) { // «Agrupar por»: se pliega y despliega tocándola (o con A)
			bool closed = F.gclosed & (1u << v->idx);
			if (pressed || (U.buttons && selected))
				draw_rrect(6, y + 1, BOT_W - 12, v->h - 2, 8, T.surface);
			static const u32 MKC[6] = {0xFF4444EF, 0xFF1673F9, 0xFF08B3EA, 0xFF5EC522, 0xFFF6823B, 0xFFF755A8};
			u32 gc = V.f_group == 4 && v->idx < 6 ? MKC[v->idx] : T.accent;
			draw_icon(closed ? IC_CHEV_RIGHT : IC_CHEV_DOWN, 18, y + v->h / 2, 11, gc);
			text_draw(f_grp_name(v->idx), 30, y + (v->h - text_lineh(TS_SUB)) / 2, TS_SUB, gc);
			char nn[8];
			snprintf(nn, sizeof nn, "%d", v->n);
			text_draw_right(nn, BOT_W - 16, y + (v->h - text_lineh(TS_SUB)) / 2, TS_SUB, T.text3);
			draw_rect(30 + text_width(f_grp_name(v->idx), TS_SUB) + 8, y + v->h / 2, BOT_W - 60 - text_width(f_grp_name(v->idx), TS_SUB) - 8 - text_width(nn, TS_SUB), 1, T.divider);
			if ((inview && tap(6, y, BOT_W - 12, v->h)) || (U.buttons && selected && (g_kdown & KEY_A) && !g_dialog.open)) {
				F.gclosed ^= 1u << v->idx;
				f_build_view();
			}
			continue;
		}
		if (v->kind == FV_FILTER) { // tipos de lo encontrado: Todo, Imágenes, Carpetas…
			sr_filter_row(y, inview);
			continue;
		}
		if (v->kind == FV_TRASH) {
			bool tp = inview && touch_in(6, y, BOT_W - 12, v->h);
			draw_rrect(6, y, BOT_W - 12, v->h, 10, tp || (U.buttons && i == F.sel) ? T.surface2 : T.surface);
			draw_icon(IC_TRASH, 27, y + v->h / 2, 15, T.text2);
			int tc;
			char sz[24], sub[96];
			util_fmt_size(sz, sizeof sz, trash_size(&tc));
			if (tc == 1)
				snprintf(sub, sizeof sub, _("1 elemento · %s"), sz);
			else
				snprintf(sub, sizeof sub, _("%d elementos · %s"), tc, sz);
			text_draw(_("Papelera"), 48, y + 4, TS_BODY, T.text);
			text_draw_clip(sub, 48, y + 21, BOT_W - 90, TS_SUB, T.text3);
			draw_icon(IC_CHEV_RIGHT, BOT_W - 26, y + v->h / 2, 11, T.text3);
			if ((inview && tap(6, y, BOT_W - 12, v->h)) || (U.buttons && i == F.sel && (g_kdown & KEY_A) && !g_dialog.open)) {
				f_load("@trash");
				return;
			}
			continue;
		}
		if (v->kind == FV_SYSHDR) {
			if (pressed || (U.buttons && selected))
				draw_rrect(6, y + 1, BOT_W - 12, v->h - 2, 8, T.surface);
			draw_icon(F.sys_open ? IC_CHEV_DOWN : IC_CHEV_RIGHT, 18, y + v->h / 2, 11, T.text3);
			text_draw(_("CARPETAS DEL SISTEMA"), 30, y + (v->h - text_lineh(TS_SUB)) / 2, TS_SUB, T.text3);
			char nn[8];
			snprintf(nn, sizeof nn, "%d", v->idx);
			text_draw_right(nn, BOT_W - 16, y + (v->h - text_lineh(TS_SUB)) / 2, TS_SUB, T.text3);
			if ((inview && tap(6, y, BOT_W - 12, v->h)) || (U.buttons && selected && (g_kdown & KEY_A) && !g_dialog.open)) {
				F.sys_open = !F.sys_open;
				f_build_view();
			}
			continue;
		}
		const FEnt *e = &F.e[v->idx];
		IconId ic;
		u32 col;
		f_kind(e, &ic, &col);
		g_clip_ell = !selected; // solo el marcado se desplaza si no cabe
		const Texture *thumb = th_get(v->idx);
		if (grid) { // casilla: icono o miniatura grande y el nombre debajo
			bool tp = inview && touch_in(v->x, y, v->w, v->h);
			if (tp || selected)
				draw_rrect(v->x + 2, y + 2, v->w - 4, v->h - 4, 12, selected ? col_alpha(T.accent, 0.16f) : T.surface);
			float cx = v->x + v->w / 2, cy = y + 30;
			if (thumb) {
				float k = fminf(52.0f / thumb->w, 52.0f / thumb->h), tw = thumb->w * k, th = thumb->h * k;
				draw_texture((Texture *)thumb, cx - tw / 2, cy - th / 2, tw, th, 1);
			} else {
				draw_rrect(cx - 22, cy - 22, 44, 44, 12, col_alpha(col, 0.18f));
				draw_icon(ic, cx, cy, 24, col);
			}
			if (e->color)
				draw_circle(v->x + 10, y + 9, 4, col);
			if (MS.on) { // casilla para elegir, arriba a la izquierda
				bool pk = ms_has(e->name);
				draw_circle(v->x + 13, y + 13, 8, pk ? T.accent : col_alpha(T.bg, 0.8f));
				draw_ring(v->x + 13, y + 13, 7.5f, 1.5f, pk ? T.accent : T.text3);
				if (pk)
					draw_icon(IC_CHECK, v->x + 13, y + 13, 10, 0xFFFFFFFF);
				if (pk)
					draw_rrect_outline(v->x + 2, y + 2, v->w - 4, v->h - 4, 12, 1.5f, T.accent);
			}
			if (e->sys && V.protect_sys && !MS.on) {
				draw_circle(cx + 20, cy + 18, 7, T.bg);
				draw_icon(IC_LOCK, cx + 20, cy + 18, 10, T.warn);
			}
			// el nombre y, a su derecha en la marcada, ⋮ para las opciones (lo mismo que Y)
			float dx = v->x + v->w - 10, dy = y + 58 + text_lineh(TS_SUB) / 2;
			text_draw_center_clip(e->name, cx - 6, y + 58, v->w - 22, TS_SUB, e->color ? col : T.text);
			bool tdots = !MS.on && selected; // ⋮ solo en la casilla marcada
			if (tdots) {
				draw_circle(dx, dy, 8, col_alpha(T.accent, 0.3f));
				draw_icon(IC_DOTS, dx, dy, 11, T.accent);
			}
			if (MS.on && inview && !g_touch.long_fired && tap(v->x, y, v->w, v->h))
				pick = i;
			else if (tdots && inview && tap(dx - 10, y + 44, 20, v->h - 44))
				menu = i;
			else if (!MS.on && inview && long_press(v->x, y, v->w, v->h))
				msel = i;
			else if (!MS.on && inview && tap(v->x, y, v->w, v->h))
				open = f_tap_open(i);
			continue;
		}
		// a la derecha, «⋮» para las opciones (como en la web); se comprueba antes que la fila
		float mx = BOT_W - 30;
		bool mp = inview && touch_in(mx - 4, y, 34, v->h);
		if (pressed || selected)
			draw_rrect(6, y + 1, BOT_W - 12, v->h - 2, 10, selected ? col_alpha(T.accent, 0.14f) : T.surface);
		if (e->color) // destacada: una raya de su color a la izquierda, como en la web
			draw_rrect(6, y + 5, 3, v->h - 10, 1.5f, col);
		if (thumb) { // la foto o el icono del programa, en pequeño
			float k = fminf(26.0f / thumb->w, 26.0f / thumb->h), tw = thumb->w * k, th = thumb->h * k;
			draw_texture((Texture *)thumb, 27 - tw / 2, y + 18 - th / 2, tw, th, 1);
		} else {
			draw_rrect(14, y + 5, 26, 26, 8, col_alpha(col, 0.18f));
			draw_icon(ic, 27, y + 18, 15, col);
		}
		if (MS.on) { // casilla para elegir, sobre la esquina del icono
			bool pk = ms_has(e->name);
			if (pk)
				draw_rrect(6, y + 1, BOT_W - 12, v->h - 2, 10, col_alpha(T.accent, 0.14f));
			draw_circle(37, y + 28, 7, pk ? T.accent : T.bg);
			draw_ring(37, y + 28, 6.5f, 1.4f, pk ? T.accent : T.text3);
			if (pk)
				draw_icon(IC_CHECK, 37, y + 28, 9, 0xFFFFFFFF);
		} else if (e->sys && V.protect_sys) {
			draw_circle(37, y + 28, 5.5f, T.bg);
			draw_icon(IC_LOCK, 37, y + 28, 8, T.warn);
		}
		char sub[64], when[24] = "", sz[24];
		if (e->mtime)
			util_fmt_when(when, sizeof when, e->mtime);
		if (e->dir)
			snprintf(sub, sizeof sub, "%s%s%s", _("Carpeta"), when[0] ? " · " : "", when);
		else {
			util_fmt_size(sz, sizeof sz, e->size);
			snprintf(sub, sizeof sub, "%s%s%s", sz, when[0] ? " · " : "", when);
		}
		// ⋮ solo en la fila marcada; en las demás el nombre llega hasta el borde
		bool dots = !MS.on && selected;
		float tw = dots ? mx - 58 : BOT_W - 62;
		text_draw_clip(e->name, 48, y + 3, tw, TS_BODY, e->color ? col : T.text);
		if (F.search) { // la carpeta donde está, en el color de acento, y luego lo de siempre
			const char *dn = SR.dirs[e->sdir];
			float dw = fminf(text_width(dn, TS_SUB), tw * 0.65f);
			text_draw_clip(dn, 48, y + 19, dw, TS_SUB, T.accent);
			char rest[80];
			snprintf(rest, sizeof rest, " · %s", sub);
			text_draw_clip(rest, 48 + dw, y + 19, tw - dw, TS_SUB, T.text3);
		} else
			text_draw_clip(sub, 48, y + 19, tw, TS_SUB, T.text3);
		if (dots) {
			draw_circle(mx + 12, y + v->h / 2, 11, mp ? T.surface2 : col_alpha(T.accent, 0.3f));
			draw_icon(IC_DOTS, mx + 12, y + v->h / 2, 14, T.accent);
		}
		float rw = dots ? mx - 10 : BOT_W - 12; // donde no hay ⋮, toda la fila abre
		if (MS.on && inview && !g_touch.long_fired && tap(6, y, BOT_W - 12, v->h))
			pick = i;
		else if (dots && inview && tap(mx - 4, y, 34, v->h))
			menu = i;
		else if (!MS.on && inview && long_press(6, y, rw, v->h))
			msel = i; // pulsación larga: a elegir varios, empezando por este
		else if (!MS.on && inview && tap(6, y, rw, v->h))
			open = f_tap_open(i);
	}
	gfx_clip_off();
	g_clip_ell = false;
	float my = VIEW_Y + (f_chips_shown() ? 44 : 0) - F.off;
	if (F.missing) {
		// acceso rápido a una carpeta que aún no existe: se ofrece crearla, como en la web
		text_draw_center(_("Esta carpeta aún no existe"), BOT_W / 2, my + 22, TS_BODY, T.text);
		bool bp = touch_in(BOT_W / 2 - 70, my + 50, 140, 32);
		draw_rrect(BOT_W / 2 - 70, my + 50, 140, 32, 16, bp ? col_lerp(T.accent, T.text, 0.15f) : T.accent);
		char t[64];
		snprintf(t, sizeof t, "%s %s", GLYPH_A, _("Crear carpeta"));
		text_draw_center(t, BOT_W / 2, my + 50 + (32 - text_lineh(TS_BODY)) / 2, TS_BODY, 0xFFFFFFFF);
		if (tap(BOT_W / 2 - 70, my + 50, 140, 32) || (U.buttons && (g_kdown & KEY_A))) {
			if (fsx_mkdirs(F.path) == 0) {
				state_lock();
				event_add(EV_MKDIR, strrchr(F.path, '/') + 1, G.console, 0);
				state_unlock();
			}
			f_reload_keep();
		}
	} else if (F.nosaves) {
		wrap_center(_("Las partidas de los juegos se guardan cifradas dentro de «Nintendo 3DS». Crea una copia con Checkpoint o JKSM y aparecerá aquí."),
		            BOT_W / 2, my + 14, BOT_W - 40, TS_SUB, 4, T.text3);
	} else if (F.search && F.n == 0) {
		if (!SR.done)
			text_draw_center(_("Buscando…"), BOT_W / 2, my + 40, TS_BODY, T.text3);
		else {
			char t[200];
			snprintf(t, sizeof t, _("No se encontró nada con «%s»"), SR.q);
			wrap_center(t, BOT_W / 2, my + 30, BOT_W - 40, TS_BODY, 2, T.text3);
			wrap_center(_("La búsqueda incluye esta carpeta y todas sus subcarpetas."), BOT_W / 2, my + 74, BOT_W - 60, TS_SUB, 2, T.text3);
		}
	} else if (F.titles && F.n == 0 && tl_loading()) {
		text_draw_center(_("Leyendo los títulos instalados…"), BOT_W / 2, my + 40, TS_BODY, T.text3);
	} else if (F.titles && F.n == 0) {
		text_draw_center(_("No hay juegos ni apps instalados en la SD"), BOT_W / 2, my + 40, TS_BODY, T.text3);
	} else if (F.trash && F.n == 0) {
		text_draw_center(_("La papelera está vacía"), BOT_W / 2, my + 40, TS_BODY, T.text3);
		char t[96];
		snprintf(t, sizeof t, _("Los elementos eliminados se conservan %d días"), TRASH_DAYS);
		text_draw_center(t, BOT_W / 2, my + 62, TS_SUB, T.text3);
	} else if (!F.err && F.n == 0 && U.app == AP_DL && f_app_root()) {
		text_draw_center(_("Aún no hay descargas"), BOT_W / 2, my + 30, TS_BODY, T.text3);
		wrap_center(_("Aquí se guarda lo que descargues desde un enlace, un código QR, la Nube o el Navegador."),
		            BOT_W / 2, my + 52, BOT_W - 40, TS_SUB, 4, T.text3);
	} else if (F.err || F.n == 0) {
		text_draw_center(F.err ? _("No se pudo abrir esta carpeta") : _("Carpeta vacía"), BOT_W / 2, my + 40, TS_BODY, T.text3);
	}
	if (U.buttons && !g_dialog.open && F.sel >= 0 && F.v[F.sel].kind == FV_ITEM) {
		if ((g_kdown & KEY_A) && MS.on)
			pick = F.sel;
		else if (g_kdown & KEY_A)
			open = F.sel;
		else if ((g_kdown & KEY_Y) && !MS.on)
			menu = F.sel;
	}
	if (MS.on && U.buttons && !g_dialog.open && (g_kdown & KEY_Y))
		MA.open = MA.just = true, MA.sel = 0;
	if (F.search) { // un resultado: se abre en su carpeta (B vuelve aquí); X solo lleva a ella
		if (msel >= 0)
			menu = msel;
		bool go = U.buttons && !g_dialog.open && (g_kdown & KEY_X) && f_selected();
		int i = menu >= 0 ? menu : open >= 0 ? open : go ? F.sel : -1;
		if (i >= 0) {
			F.sel = i;
			const FEnt *e = &F.e[F.v[i].idx];
			if (e->dir && open >= 0) { // una carpeta: se entra en ella
				char full[700];
				f_full(F.v[i].idx, full, sizeof full);
				SR.sel_keep = i;
				f_load(full);
				util_strlcpy(SR.back, full, sizeof SR.back);
				return;
			}
			sr_goto(F.v[i].idx);
			if (menu >= 0)
				f_menu_open();
			else if (open >= 0 && f_selected())
				f_open_item(F.v[F.sel].idx);
		}
		return;
	}
	if (F.titles) { // un título: sus opciones (copia de la partida, sus copias, desinstalar)
		int i = menu >= 0 ? menu : open >= 0 ? open : msel;
		if (i >= 0) {
			F.sel = i;
			TT.open = TT.just = true, TT.sel = 0;
		}
		return;
	}
	if (F.trash) {
		if (open >= 0 && U.buttons && (g_kdown & KEY_A))
			trash_do_restore(F.v[open].idx);
		else if (open >= 0 || menu >= 0) {
			F.sel = open >= 0 ? open : menu;
			TS.open = TS.just = true, TS.sel = 0;
		}
		return;
	}
	if (pick >= 0) {
		F.sel = pick;
		ms_toggle(F.e[F.v[pick].idx].name);
	}
	if (msel >= 0 && F.trash)
		menu = msel, msel = -1;
	if (msel >= 0) {
		F.sel = msel;
		ms_start(F.e[F.v[msel].idx].name);
		f_build_view();
	}
	if (MS.on) { // barra de acciones de la selección, sobre el botón del QR
		bool any = MS.n > 0, cia = ms_cias() > 0;
		struct {
			IconId ic;
			const char *t;
			int act;
			bool on;
		} B[5] = {{IC_DUP, _("Copiar"), FA_COPY, any},
		          {IC_FOLDER, _("Cortar"), FA_MOVE, any && !ms_locked()},
		          {IC_STAR, _("Destacar"), FA_COLORS, any},
		          {IC_TRASH, _("Borrar"), FA_DELETE, any && !ms_locked()},
		          {IC_DOWNLOAD, _("Instalar"), FA_INSTALL, cia}};
		int nb = cia ? 5 : 4;
		float by = VIEW_Y + VIEW_H - QR_PAD - 46, bw = (BOT_W - 20) / (float)nb;
		draw_rrect(10, by + 3, BOT_W - 20, 42, 14, T.shadow);
		draw_rrect(10, by, BOT_W - 20, 42, 14, T.surface2);
		for (int k = 0; k < nb; k++) {
			float bx = 10 + k * bw;
			u32 c = !B[k].on ? col_alpha(T.text3, 0.5f) : B[k].act == FA_DELETE ? T.danger : B[k].act == FA_INSTALL ? T.ok : T.text;
			if (B[k].on && touch_in(bx, by, bw, 42))
				draw_rrect(bx + 2, by + 2, bw - 4, 38, 12, T.surface);
			draw_icon(B[k].ic, bx + bw / 2, by + 14, 14, c);
			text_draw_center_clip(B[k].t, bx + bw / 2, by + 24, bw - 4, 0.36f, c);
			if (B[k].on && !g_dialog.open && tap(bx, by, bw, 42)) {
				ms_do(B[k].act);
				g_touch.up = false; // el mismo toque no sigue a «Copiar aquí», que está en el mismo sitio
			}
		}
	}
	if (!MS.on && U.buttons && !g_dialog.open && (g_kdown & KEY_B)) {
		if (F.nosaves)
			f_reload_keep();
		else if (f_app_root() && !F.search && U.app == AP_DL) // lo descargado: a las tarjetas de Descargas
			dl_view_close();
		else if (f_app_root() && !F.search) // en la carpeta de la aplicación: al inicio
			hm_show();
		else if (!f_back_to_results())
			f_up();
	}
	if (menu >= 0) {
		F.sel = menu;
		f_menu_open();
	} else if (open >= 0 && !MS.on) {
		f_open_item(open);
	}
}

// ---- menú de un archivo o carpeta
static struct {
	bool open;
	float anim;
	int idx;     // elemento de F.e
	int acts[12], n, sel, color;
	char name[128]; // de qué es: si la lista se vuelve a leer con el menú abierto, se busca otra vez por el nombre
	bool just;   // se acaba de abrir: el toque o el botón que lo abrió no cuenta
} FM;

static void f_menu_open(void)
{
	const FEnt *e = f_selected();
	if (!e)
		return;
	FM.idx = F.v[F.sel].idx;
	util_strlcpy(FM.name, e->name, sizeof FM.name);
	FM.n = 0;
	bool locked = e->sys && V.protect_sys;
	if (!e->dir && !strcasecmp(f_ext(e->name), "cia"))
		FM.acts[FM.n++] = FA_INSTALL;
	if (!e->dir && !strcasecmp(f_ext(e->name), "txt") && !vw_is_open()) // un texto, también para leerlo como un libro
		FM.acts[FM.n++] = FA_READ;
	bool html = !strcasecmp(f_ext(e->name), "html") || !strcasecmp(f_ext(e->name), "htm");
	if (!e->dir && !f_is_img(e) && (!f_is_txt(e) || html) && !vw_is_open()) // cualquier archivo se puede mirar como texto (y un .html, editar)
		FM.acts[FM.n++] = FA_ASTEXT;
	if (!locked && !e->dir && (f_is_txt(e) || !*f_ext(e->name))) // txt → py, cpp, java…
		FM.acts[FM.n++] = FA_TYPE;
	if (!locked) {
		FM.acts[FM.n++] = FA_RENAME;
		FM.acts[FM.n++] = FA_MOVE;
	}
	FM.acts[FM.n++] = FA_COPY;
	if (!locked)
		FM.acts[FM.n++] = FA_DELETE;
	if (!vw_is_open())
		FM.acts[FM.n++] = FA_SELECT;
	if (!e->dir)
		FM.acts[FM.n++] = FA_HASH;
	if (!F.trash)
		FM.acts[FM.n++] = FA_ZIP;
	if (!e->dir && !F.trash && cl_connected())
		FM.acts[FM.n++] = FA_CLOUD;
	FM.sel = 0;
	FM.color = e->color;
	FM.open = true;
	FM.just = true;
}

static const char *fa_name(int a)
{
	switch (a) {
	case FA_INSTALL: return _("Instalar");
	case FA_RENAME: return _("Renombrar");
	case FA_MOVE: return _("Cortar");
	case FA_COPY: return _("Copiar");
	case FA_DELETE: return _("Eliminar");
	case FA_ASTEXT: return _("Abrir como texto");
	case FA_TYPE: return _("Cambiar tipo");
	case FA_SELECT: return _("Seleccionar varios");
	case FA_HASH: return _("MD5 y SHA-256");
	case FA_ZIP: return _("Comprimir en ZIP");
	case FA_CLOUD: return _("Subir a OneDrive");
	case FA_READ: return _("Abrir en el Lector");
	default: return _("Destacar");
	}
}

static IconId fa_icon(int a)
{
	static const IconId IC[13] = {IC_DOWNLOAD, IC_PEN, IC_FOLDER, IC_DUP, IC_TRASH, IC_STAR, IC_NOTE, IC_CODE, IC_CHECK, IC_SHIELD, IC_ARCHIVE, IC_CLOUD, IC_BOOK};
	return IC[a];
}

static void f_reload(void)
{
	char p[512];
	util_strlcpy(p, F.path, sizeof p);
	f_load(p);
}

// cambiar el tipo de un texto: la extensión. Con el editor abierto, sigue abierto con los colores nuevos
static const char *const TY_EXT[] = {"txt", "py", "c", "cpp", "h", "java", "js", "html", "css", "json", "lua", "md", "sh", "ini", "xml", "gm9"};
#define TY_N ((int)ARRAY_LEN(TY_EXT))
static struct {
	bool open, just;
	float anim;
	int sel;
} TY;

static void f_set_type(const char *ext)
{
	const FEnt *e = &F.e[FM.idx];
	char nm[140], full[700], dst[900], a[720], b[920];
	util_strlcpy(nm, e->name, sizeof nm);
	char *dot = strrchr(nm, '.');
	if (dot && dot != nm)
		*dot = 0;
	snprintf(nm + strlen(nm), sizeof nm - strlen(nm), ".%s", ext);
	if (!strcmp(nm, e->name))
		return;
	f_full(FM.idx, full, sizeof full);
	snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
	bool d;
	u64 sz;
	snprintf(a, sizeof a, "sdmc:%s", full);
	snprintf(b, sizeof b, "sdmc:%s", dst);
	if (fsx_stat(dst, &d, &sz) == 0) {
		toast_icon(_("Ya existe un archivo con ese nombre"), IC_CLOSE, T.danger);
		return;
	}
	if (rename(a, b) != 0) {
		toast_icon(_("No se pudo renombrar"), IC_CLOSE, T.danger);
		return;
	}
	state_lock();
	mark_moved(full, dst);
	event_add(EV_RENAME, nm, G.console, 0);
	G.rev++;
	state_unlock();
	bool open = VW.kind == VW_TXT;
	f_reload_keep();
	for (int i = 0; i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm)) {
			F.sel = i;
			if (open) { // el editor sigue con el mismo texto (con o sin cambios), ahora con otros colores
				VW.idx = F.v[i].idx;
				util_strlcpy(VW.path, dst, sizeof VW.path);
				VW.lang = hl_lang(ext);
				VW.hlv = 0;
			}
		}
	char msg[80];
	snprintf(msg, sizeof msg, _("Tipo cambiado a .%s"), ext);
	toast_icon(msg, IC_CODE, T.accent);
}

static void ty_draw(void)
{
	TY.anim = approach(TY.anim, TY.open ? 1 : 0, 16);
	if (TY.anim < 0.01f)
		return;
	float a = ease_out_cubic(TY.anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	const int COLS = 4;
	const float CW = 66, CH = 30, G = 6, X0 = (BOT_W - COLS * CW - (COLS - 1) * G) / 2;
	int rows = (TY_N + COLS - 1) / COLS;
	float h = 38 + rows * (CH + G) + 44, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rrect(10, y + 3, BOT_W - 20, h, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, h, 16, T.surface);
	if (!TY.open)
		return;
	bool fresh = TY.just;
	TY.just = false;
	const char *cur = f_ext(F.e[FM.idx].name);
	text_draw_center(_("Cambiar tipo"), BOT_W / 2, y + 10, TS_BODY, T.text);
	int pick = -1;
	bool close = false;
	if (U.buttons && !fresh) {
		if (g_krepeat & KEY_RIGHT) TY.sel = (TY.sel + 1) % (TY_N + 1);
		if (g_krepeat & KEY_LEFT) TY.sel = (TY.sel + TY_N) % (TY_N + 1);
		if (g_krepeat & KEY_DOWN) TY.sel = TY.sel == TY_N ? 0 : TY.sel + COLS < TY_N ? TY.sel + COLS : TY_N;
		if (g_krepeat & KEY_UP) TY.sel = TY.sel == TY_N ? TY_N - 1 : TY.sel >= COLS ? TY.sel - COLS : TY_N;
	}
	for (int i = 0; i < TY_N; i++) {
		float cx = X0 + (i % COLS) * (CW + G), cy = y + 36 + (i / COLS) * (CH + G);
		bool now = !strcasecmp(cur, TY_EXT[i]), hl = (U.buttons && TY.sel == i) || touch_in(cx, cy, CW, CH);
		draw_rrect(cx, cy, CW, CH, CH / 2, now ? col_alpha(T.accent, 0.25f) : hl ? T.surface2 : T.bg);
		if (U.buttons && TY.sel == i)
			draw_rrect_outline(cx, cy, CW, CH, CH / 2, 1.5f, T.accent);
		char t[12];
		snprintf(t, sizeof t, ".%s", TY_EXT[i]);
		text_draw_center(t, cx + CW / 2, cy + (CH - text_lineh(TS_BODY)) / 2, TS_BODY, now ? T.accent : T.text);
		if (!fresh && (tap(cx, cy, CW, CH) || (U.buttons && TY.sel == i && (g_kdown & KEY_A))))
			pick = i;
	}
	float by = y + h - 40;
	bool bp = touch_in(X0, by, BOT_W - 2 * X0, 32);
	draw_rrect(X0, by, BOT_W - 2 * X0, 32, 16, bp || (U.buttons && TY.sel == TY_N) ? T.surface2 : T.bg);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	text_draw_center(cl, BOT_W / 2, by + (32 - text_lineh(TS_BODY)) / 2, TS_BODY, T.text);
	if (!fresh && (tap(X0, by, BOT_W - 2 * X0, 32) || (U.buttons && TY.sel == TY_N && (g_kdown & KEY_A)) || (g_kdown & KEY_B) ||
	               (g_touch.up && !g_touch.drag && g_touch.sy < y)))
		close = true;
	if (pick >= 0) {
		TY.open = false;
		f_set_type(TY_EXT[pick]);
	} else if (close) {
		TY.open = false;
	}
	g_kdown &= ~(KEY_A | KEY_B);
	g_touch.up = false;
}

static void fa_run(int a)
{
	const FEnt *e = &F.e[FM.idx];
	char full[700];
	f_full(FM.idx, full, sizeof full);
	FM.open = false;
	if (a == FA_ASTEXT) {
		vw_load(FM.idx);
		return;
	}
	if (a == FA_READ) {
		rv_open(full, U.app);
		return;
	}
	if (a == FA_SELECT) {
		ms_start(e->name);
		f_build_view();
		return;
	}
	if (a == FA_ZIP) { // el nombre se pide con el teclado, fuera del dibujo
		zq_set(F.path, (char (*)[128])e->name, 1);
		return;
	}
	if (a == FA_HASH) { // se calcula aparte; la hoja y la pantalla de arriba lo enseñan
		hash_start(full);
		util_strlcpy(HS.path, full, sizeof HS.path);
		HS.cmp = HCHK_NONE;
		HS.open = HS.just = true;
		HS.sel = 0;
		return;
	}
	if (a == FA_CLOUD) { // a la carpeta de la nube que se está viendo; si no, a Cloud3DS
		const FEnt *e = f_selected();
		if (!e)
			return;
		char full[700];
		f_full(F.v[F.sel].idx, full, sizeof full);
		cloud_upload(full, cloud_depth() > 1 ? cloud_folder_id() : "");
		char m[200];
		snprintf(m, sizeof m, _("Subiendo «%s» a OneDrive"), e->name);
		toast_icon(m, IC_UPLOAD, T.accent);
		return;
	}
	if (a == FA_TYPE) { // también con el editor abierto: solo cambia la extensión
		TY.open = TY.just = true;
		TY.sel = 0;
		return;
	}
	// renombrar, mover, copiar o borrar lo que se está viendo: primero se cierra el visor
	if (vw_is_open() && a != FA_INSTALL) {
		if (vw_dirty()) {
			toast_icon(_("Primero guarda o descarta los cambios"), IC_INFO, T.warn);
			return;
		}
		vw_close();
	}
	if (a == FA_INSTALL) {
		char err[24] = "";
		int id = strlen(full) < 256 ? ins_request(full, G.console, false, err, sizeof err) : -1;
		if (id < 0) {
			toast_icon(!strcmp(err, "system_title") ? _("Por seguridad, no se instalan títulos del sistema") : !strcmp(err, "already") ? _("Ya está pendiente de instalación en la consola")
			                                                                                                  : _("La consola no reconoce este CIA"),
			           IC_CLOSE, T.danger);
			return;
		}
		// elegido aquí mismo: no hace falta la tarjeta de confirmación, salvo que no se pueda
		InsReq r;
		ins_peek(full, &r);
		if (r.self && !(r.inst_ver >= 0 && r.inst_ver > r.ver)) { // Cloud3DS: con el actualizador
			ins_decide(id, INS_KEEP);
			UL.n = 1;
			util_strlcpy(UL.f[0].path, full, sizeof UL.f[0].path);
			ins_ver_text(r.ver, UL.f[0].ver, sizeof UL.f[0].ver);
			char msg[400];
			snprintf(msg, sizeof msg, _("Se instalará Cloud3DS v%s desde %s. No apagues la consola durante el proceso."), UL.f[0].ver, full);
			U.dlg_arg = 0;
			dialog_open(DLG_UPDFILE, _("¿Actualizar desde la SD?"), msg, _("Actualizar"), false);
			return;
		}
		if (r.inst_ver >= 0 && r.inst_ver > r.ver) {
			ins_decide(id, INS_KEEP);
			toast_icon(_("Ya hay una versión más reciente instalada"), IC_INFO, T.warn);
			return;
		}
		ins_decide(id, INS_ACCEPT);
		toast_icon(_("Instalando…"), IC_DOWNLOAD, T.ok);
	} else if (a == FA_RENAME) {
		U.want_frename = true;
	} else if (a == FA_DELETE) {
		char msg[200];
		snprintf(msg, sizeof msg, e->dir ? _("«%s» y todo su contenido irán a la papelera. Podrás recuperarlo durante %d días.")
		                                 : _("«%s» irá a la papelera. Podrás recuperarlo durante %d días."),
		         e->name, TRASH_DAYS);
		dialog_open(DLG_FDELETE, _("¿Eliminar?"), msg, _("Eliminar"), true);
	} else if (a == FA_MOVE || a == FA_COPY) {
		FP.mode = a;
		FP.n = 0;
		CC.mode = CC.n = 0;
		util_strlcpy(FP.src, full, sizeof FP.src);
		util_strlcpy(FP.name, e->name, sizeof FP.name);
		FP.src_dir = e->dir;
		toast_icon(_("Copiado al portapapeles. Abre otra carpeta y pulsa X para pegar."), IC_DUP, T.accent);
	}
}

// menú de un archivo: arriba qué es (y «Cerrar»), las cuatro acciones de siempre en grande,
// el resto en una lista a dos columnas y abajo los colores. Con botones, la cruceta va a la
// casilla más cercana en esa dirección; en los colores, izquierda y derecha cambian el color
static void f_type_text(const FEnt *e, char *out, size_t n);

// la casilla más cercana desde la marcada en la dirección pulsada (dx, dy: -1, 0 o 1)
static int nav_nearest(const float (*r)[4], int n, int cur, int dx, int dy)
{
	float cx = r[cur][0] + r[cur][2] / 2, cy = r[cur][1] + r[cur][3] / 2;
	int best = -1;
	float bd = 1e9f;
	for (int i = 0; i < n; i++) {
		if (i == cur)
			continue;
		float x = r[i][0] + r[i][2] / 2, y = r[i][1] + r[i][3] / 2, ddx = x - cx, ddy = y - cy;
		if (dy && (dy > 0 ? ddy <= 2 : ddy >= -2))
			continue;
		if (dx && (fabsf(ddy) > 4 || (dx > 0 ? ddx <= 2 : ddx >= -2)))
			continue;
		float d = dy ? fabsf(ddy) * 4 + fabsf(ddx) : fabsf(ddx);
		if (d < bd)
			bd = d, best = i;
	}
	return best < 0 ? cur : best;
}

static void f_menu(void)
{
	FM.anim = approach(FM.anim, FM.open ? 1 : 0, 16);
	if (FM.anim < 0.01f)
		return;
	float a = ease_out_cubic(FM.anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	if (FM.idx >= F.n || strcmp(F.e[FM.idx].name, FM.name)) { // la carpeta cambió (web, copia…): el mismo de antes
		int k;
		for (k = 0; k < F.n && strcmp(F.e[k].name, FM.name); k++)
			;
		if (k == F.n) { // ya no está
			FM.open = false;
			return;
		}
		FM.idx = k;
	}
	const FEnt *e = &F.e[FM.idx];
	// las principales (Renombrar, Mover, Copiar, Eliminar) y las demás, en el orden en que se añadieron
	int prim[4], np = 0, sec[16], ns = 0;
	static const int PRIM_ORDER[4] = {FA_RENAME, FA_MOVE, FA_COPY, FA_DELETE};
	for (int k = 0; k < 4; k++)
		for (int i = 0; i < FM.n; i++)
			if (FM.acts[i] == PRIM_ORDER[k])
				prim[np++] = FM.acts[i];
	for (int i = 0; i < FM.n; i++)
		if (FM.acts[i] != FA_RENAME && FM.acts[i] != FA_MOVE && FM.acts[i] != FA_COPY && FM.acts[i] != FA_DELETE)
			sec[ns++] = FM.acts[i];
	const float X = 6, W = BOT_W - 12, PAD = 10, HH = 34, PH = 56, RH = 24, CH = 26;
	int srows = (ns + 1) / 2;
	float h = PAD + HH + 6 + PH + 8 + 1 + 6 + srows * RH + 6 + CH + 6;
	float y = SCR_H - 6 - h + (1 - a) * 40;
	draw_rrect(X, y + 3, W, h, 18, col_alpha(T.shadow, a));
	draw_rrect(X, y, W, h, 18, T.surface);
	if (!FM.open)
		return;
	static const u32 MK[7] = {0, 0xFF4444EF, 0xFF1673F9, 0xFF08B3EA, 0xFF5EC522, 0xFFF6823B, 0xFFF755A8};
	int run = -1, close = 0;
	// el mismo toque o botón que abrió el menú no elige nada: ese frame no hay entrada
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	bool fresh = FM.just;
	FM.just = false;
	if (fresh) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	// casillas (para la cruceta): principales, secundarias y la fila de colores
	float rc[24][4];
	int nr = 0, act_of[24];
	// cabecera: icono, nombre, tipo y tamaño; a la derecha «Cerrar»
	float hy = y + PAD;
	IconId kic;
	u32 kcol;
	f_kind(e, &kic, &kcol);
	draw_rrect(X + PAD + 2, hy + 3, 28, 28, 9, col_alpha(kcol, 0.16f));
	draw_icon(kic, X + PAD + 16, hy + 17, 15, kcol);
	char cl[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	float cw = text_width(cl, TS_SUB) + 18, cx = X + W - PAD - cw;
	bool cp = touch_in(cx, hy + 2, cw, 30);
	draw_rrect(cx, hy + 7, cw, 20, 10, cp ? T.divider : T.surface2);
	text_draw_center(cl, cx + cw / 2, hy + 17 - text_lineh(TS_SUB) / 2, TS_SUB, T.text2);
	if (tap(cx, hy + 2, cw, 30))
		close = 1;
	float nx = X + PAD + 40, nw = cx - 8 - nx;
	text_draw_clip(e->name, nx, hy + 1, nw, TS_BODY, T.text);
	char tt[160], sz[24];
	f_type_text(e, tt, sizeof tt);
	if (!e->dir) {
		util_fmt_size(sz, sizeof sz, e->size);
		size_t l = strlen(tt);
		snprintf(tt + l, sizeof tt - l, " · %s", sz);
	}
	text_draw_clip(tt, nx, hy + 18, nw, TS_SUB, T.text3);
	// las cuatro de siempre, en grande
	float py = hy + HH + 6, pw = (W - 2 * PAD) / (np ? np : 1);
	for (int k = 0; k < np; k++) {
		int act = prim[k];
		float px = X + PAD + k * pw;
		int me = nr;
		rc[nr][0] = px, rc[nr][1] = py, rc[nr][2] = pw, rc[nr][3] = PH, act_of[nr++] = act;
		bool hl = (U.buttons && FM.sel == me) || touch_in(px, py, pw, PH);
		bool del = act == FA_DELETE;
		float icx = px + pw / 2, icy = py + 18;
		if (U.buttons && FM.sel == me)
			draw_rrect_outline(px + 4, py, pw - 8, PH, 12, 1.5f, T.accent);
		draw_circle(icx, icy, 17, del ? col_alpha(T.danger, hl ? 0.3f : 0.16f) : hl ? T.divider : T.surface2);
		draw_icon(fa_icon(act), icx, icy, 15, del ? T.danger : T.text2);
		const char *pl = act == FA_MOVE ? _("Cortar") : act == FA_COPY ? _("Copiar") : fa_name(act);
		text_draw_center_clip(pl, icx, py + 39, pw - 6, 0.38f, del ? T.danger : T.text);
		if (tap(px, py, pw, PH))
			run = act;
	}
	float dy = py + PH + 8;
	draw_rect(X + PAD + 4, dy, W - 2 * PAD - 8, 1, T.divider);
	// el resto, a dos columnas
	float sy = dy + 7, sw = (W - 2 * PAD) / 2;
	for (int k = 0; k < ns; k++) {
		int act = sec[k];
		float rx = X + PAD + (k % 2) * sw, ry = sy + (k / 2) * RH;
		int me = nr;
		rc[nr][0] = rx, rc[nr][1] = ry, rc[nr][2] = sw, rc[nr][3] = RH, act_of[nr++] = act;
		bool hl = (U.buttons && FM.sel == me) || touch_in(rx, ry, sw, RH);
		if (hl)
			draw_rrect(rx + 2, ry + 1, sw - 4, RH - 2, 8, T.surface2);
		if (U.buttons && FM.sel == me)
			draw_rrect_outline(rx + 2, ry + 1, sw - 4, RH - 2, 8, 1.5f, T.accent);
		u32 c = act == FA_INSTALL ? T.ok : T.text2;
		draw_icon(fa_icon(act), rx + 16, ry + RH / 2, 13, c);
		text_draw_clip(fa_name(act), rx + 30, ry + (RH - text_lineh(0.42f)) / 2, sw - 36, 0.42f, act == FA_INSTALL ? T.ok : T.text);
		if (tap(rx, ry, sw, RH))
			run = act;
	}
	// colores
	float cy = y + h - 6 - CH;
	int SC = nr;
	rc[nr][0] = X + PAD, rc[nr][1] = cy, rc[nr][2] = W - 2 * PAD, rc[nr][3] = CH, act_of[nr++] = FA_COLORS;
	if (U.buttons && FM.sel == SC) {
		draw_rrect(X + PAD, cy, W - 2 * PAD, CH, 10, T.surface2);
		draw_rrect_outline(X + PAD, cy, W - 2 * PAD, CH, 10, 1.5f, T.accent);
	}
	draw_icon(IC_STAR, X + PAD + 12, cy + CH / 2, 12, T.text3);
	text_draw(_("Destacar"), X + PAD + 26, cy + (CH - text_lineh(TS_SUB)) / 2, TS_SUB, T.text3);
	for (int k = 0; k < 7; k++) {
		float dx = X + W - PAD - 14 - (6 - k) * 22, dcy = cy + CH / 2;
		if (FM.color == k)
			draw_circle(dx, dcy, 9.5f, T.text);
		draw_circle(dx, dcy, 8, k ? MK[k] : T.surface2);
		if (!k)
			draw_line(dx - 3.5f, dcy + 3.5f, dx + 3.5f, dcy - 3.5f, 1.4f, T.text3);
		if (tap(dx - 11, cy, 22, CH)) {
			FM.color = k;
			run = FA_COLORS;
		}
	}
	// la cruceta
	if (U.buttons && nr) {
		if (FM.sel >= nr)
			FM.sel = 0;
		if (g_krepeat & KEY_DOWN)
			FM.sel = nav_nearest(rc, nr, FM.sel, 0, 1);
		if (g_krepeat & KEY_UP)
			FM.sel = nav_nearest(rc, nr, FM.sel, 0, -1);
		if (FM.sel == SC) {
			if (g_krepeat & (KEY_LEFT | KEY_RIGHT))
				FM.color = (FM.color + ((g_krepeat & KEY_RIGHT) ? 1 : 6)) % 7;
		} else {
			if (g_krepeat & KEY_RIGHT)
				FM.sel = nav_nearest(rc, nr, FM.sel, 1, 0);
			if (g_krepeat & KEY_LEFT)
				FM.sel = nav_nearest(rc, nr, FM.sel, -1, 0);
		}
		if (g_kdown & KEY_A)
			run = act_of[FM.sel];
	}
	if (close || (g_kdown & (KEY_B | KEY_Y)) || (g_touch.up && !g_touch.drag && g_touch.sy < y)) // Y lo abre y Y lo cierra
		FM.open = false;
	else if (run == FA_COLORS) {
		char full[700];
		f_full(FM.idx, full, sizeof full);
		state_lock();
		mark_set(full, FM.color);
		G.rev++;
		state_unlock();
		FM.open = false;
		f_reload();
	} else if (run >= 0) {
		fa_run(run);
	}
	if (fresh)
		g_kdown = kd, g_krepeat = kr, g_touch = tc;
	g_kdown &= ~(KEY_A | KEY_B | KEY_Y); // el menú se queda con la pulsación
	g_touch.up = false;
}

static void f_pick_do(const char *dst);
enum { XP_ASK, XP_REPLACE, XP_KEEP, XP_SKIP }; // al pegar, lo que ya exista: preguntar, sustituir, los dos u omitir
static void f_paste(int policy);

// ---- mover o copiar: se elige la carpeta navegando y se confirma con X o con el botón
static void f_pick_bar(void)
{
	if (!FP.mode && CC.mode && !F.trash && !F.search && !F.titles && !F.nosaves && F.path[0] == '/') { // de la nube: se descarga aquí
		float y = VIEW_Y + VIEW_H - QR_PAD - 34, w = BOT_W - 20;
		draw_rrect(10, y + 2, w, 30, 15, T.shadow);
		draw_rrect(10, y, w, 30, 15, T.accent);
		char t[96];
		if (CC.n > 1)
			snprintf(t, sizeof t, "%s %s (%d)", GLYPH_X, _("Descargar aquí"), CC.n);
		else
			snprintf(t, sizeof t, "%s %s", GLYPH_X, _("Descargar aquí"));
		text_draw_center(t, BOT_W / 2 - 30, y + (30 - text_lineh(0.46f)) / 2, 0.46f, 0xFFFFFFFF);
		draw_rect(BOT_W - 70, y + 6, 1, 18, col_alpha(0xFFFFFFFF, 0.4f));
		text_draw_center(_("Vaciar"), BOT_W - 42, y + (30 - text_lineh(TS_SUB)) / 2, TS_SUB, 0xFFFFFFFF);
		if (tap(BOT_W - 70, y, 60, 30))
			CC.mode = CC.n = 0;
		else if ((g_kdown & KEY_X) || tap(10, y, w - 60, 30)) {
			for (int k = 0; k < CC.n; k++)
				cloud_download_to(&CC.it[k], F.path);
			toast_icon(_("Descargando de OneDrive a esta carpeta. Puedes seguir el progreso en Descargas."), IC_DOWNLOAD, T.ok);
			if (CC.mode == CC_CUT) // (en la nube no se borra: de la nube a la SD siempre es una copia)
				CC.mode = CC.n = 0;
		}
		return;
	}
	if (!FP.mode)
		return;
	float y = VIEW_Y + VIEW_H - QR_PAD - 34, w = BOT_W - 20; // por encima del botón del QR
	draw_rrect(10, y + 2, w, 30, 15, T.shadow);
	draw_rrect(10, y, w, 30, 15, T.accent);
	char t[96];
	int cn = FP.n > 1 ? FP.n : 1;
	if (cn > 1)
		snprintf(t, sizeof t, "%s %s (%d)", GLYPH_X, _("Pegar aquí"), cn);
	else
		snprintf(t, sizeof t, "%s %s", GLYPH_X, _("Pegar aquí"));
	text_draw_center(t, BOT_W / 2 - 30, y + (30 - text_lineh(0.46f)) / 2, 0.46f, 0xFFFFFFFF);
	draw_rect(BOT_W - 70, y + 6, 1, 18, col_alpha(0xFFFFFFFF, 0.4f));
	text_draw_center(_("Vaciar"), BOT_W - 42, y + (30 - text_lineh(TS_SUB)) / 2, TS_SUB, 0xFFFFFFFF);
	bool cancel = tap(BOT_W - 70, y, 60, 30);
	bool go = (g_kdown & KEY_X) || (!cancel && tap(10, y, w - 60, 30));
	if (cancel) {
		FP.mode = 0;
		FP.n = 0;
		return;
	}
	if (!go)
		return;
	f_paste(XP_ASK);
}

// pegar lo copiado aquí. policy: qué hacer con lo que ya exista; XP_ASK pregunta antes (si hay algo repetido)
static struct {
	bool open, just;
	float anim;
	int sel, n;      // cuántos ya existen
	bool dir;        // (uno solo, y es una carpeta: «Combinar»)
	char name[128];
} XS;
static int f_paste_conflicts(bool *one_dir, char *one, size_t on)
{
	int c = 0;
	for (int k = 0; k < (FP.n > 1 ? FP.n : 1); k++) {
		const char *src = FP.n > 1 ? FP.list[k] : FP.src, *nm = strrchr(src, '/') + 1;
		char dst[1100];
		snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
		bool d;
		u64 sz;
		if (strcasecmp(dst, src) && fsx_stat(dst, &d, &sz) == 0) {
			if (!c)
				*one_dir = d, util_strlcpy(one, nm, on);
			c++;
		}
	}
	return c;
}
static void f_paste(int policy)
{
	if (policy == XP_ASK) {
		XS.n = f_paste_conflicts(&XS.dir, XS.name, sizeof XS.name);
		if (XS.n) {
			XS.open = XS.just = true, XS.sel = 0;
			return;
		}
		policy = XP_REPLACE; // (nada repetido)
	}
	if (FP.n > 1) { // varios: lo que ya exista, según lo elegido (sustituir, los dos u omitir)
		if (V.protect_sys && fsx_is_system(F.path) && strcmp(F.path, "/")) {
			toast_icon(_("Carpeta protegida. Elige otra."), IC_LOCK, T.warn);
			return;
		}
		int done = 0;
		if (FP.mode == FA_COPY && !CQ.n) {
			CQ.src = malloc(FP.n * sizeof *CQ.src);
			CQ.dst = malloc(FP.n * sizeof *CQ.dst);
			if (!CQ.src || !CQ.dst) {
				free(CQ.src), free(CQ.dst);
				CQ.src = NULL, CQ.dst = NULL;
				return;
			}
			CQ.i = 0;
		} else if (FP.mode == FA_COPY) {
			toast_icon(_("Ya hay una copia en curso"), IC_CLOSE, T.danger);
			return;
		}
		for (int k = 0; k < FP.n; k++) {
			const char *src = FP.list[k], *nm = strrchr(src, '/') + 1;
			size_t pl = nm - 1 - src;
			bool here = (pl == 0 && !strcmp(F.path, "/")) || (strlen(F.path) == pl && !strncasecmp(F.path, src, pl));
			if (FP.mode == FA_MOVE && here)
				continue; // ya está aquí
			size_t sl = strlen(src);
			if (!strncasecmp(F.path, src, sl) && (F.path[sl] == 0 || F.path[sl] == '/'))
				continue; // una carpeta dentro de sí misma
			char dst[700];
			snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
			bool d, sdir = false;
			u64 sz;
			if (fsx_stat(dst, &d, &sz) == 0) {
				if (policy == XP_SKIP)
					continue;
				fsx_stat(src, &sdir, &sz);
				if (policy == XP_KEEP || (FP.mode == FA_COPY && here) || d != sdir) // (carpeta y archivo con el mismo nombre: los dos)
					ops_free_name(dst, sizeof dst, F.path, nm);
			}
			if (FP.mode == FA_MOVE) {
				if (ops_move(src, dst) == OPS_OK) {
					state_lock();
					mark_moved(src, dst);
					event_add(EV_MOVE, nm, G.console, 0);
					G.rev++;
					state_unlock();
					done++;
				}
			} else {
				util_strlcpy(CQ.src[CQ.n], src, sizeof CQ.src[0]);
				util_strlcpy(CQ.dst[CQ.n], dst, sizeof CQ.dst[0]);
				CQ.n++, done++;
			}
		}
		char msg[64];
		snprintf(msg, sizeof msg, FP.mode == FA_MOVE ? _("Elementos movidos: %d") : _("Copiando %d elementos…"), done);
		toast_icon(msg, IC_FOLDER, FP.mode == FA_MOVE ? T.accent2 : T.accent);
		if (FP.mode == FA_MOVE) // lo cortado ya no está donde estaba; lo copiado se puede volver a pegar
			FP.mode = 0, FP.n = 0;
		f_reload();
		return;
	}
	char dst[1100];
	snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", FP.name);
	if (FP.src_dir && (!strcasecmp(F.path, FP.src) || (!strncasecmp(F.path, FP.src, strlen(FP.src)) && F.path[strlen(FP.src)] == '/'))) {
		toast_icon(_("No se puede mover una carpeta dentro de sí misma"), IC_CLOSE, T.danger);
		return;
	}
	if (V.protect_sys && fsx_is_system(F.path) && strcmp(F.path, "/")) {
		toast_icon(_("Carpeta protegida. Elige otra."), IC_LOCK, T.warn);
		return;
	}
	bool d;
	u64 sz;
	if (fsx_stat(dst, &d, &sz) == 0) {
		if (FP.mode == FA_MOVE && !strcasecmp(dst, FP.src)) {
			FP.mode = 0; // ya está aquí
			return;
		}
		if (policy == XP_SKIP)
			return;
		if (policy == XP_KEEP || (FP.mode == FA_COPY && !strcasecmp(dst, FP.src)) || d != FP.src_dir) {
			char dir[512];
			util_strlcpy(dir, F.path, sizeof dir);
			ops_free_name(dst, sizeof dst, dir, FP.name);
		}
	}
	f_pick_do(dst);
}

// la hoja de «Ya existe»: sustituir (o combinar, si son carpetas), conservar los dos u omitir; vale para todos
static void xs_draw(void)
{
	static const IconId ic[3] = {IC_REFRESH, IC_DUP, IC_CLOSE};
	const char *lab[3] = {XS.n == 1 && XS.dir ? _("Combinar las carpetas") : XS.n == 1 ? _("Reemplazar") : _("Reemplazar todos (las carpetas se combinan)"),
	                      _("Conservar ambos (con otro nombre)"), XS.n == 1 ? _("Omitir") : _("Omitir los que ya existen")};
	const u32 col[3] = {T.warn, T.text, T.text2};
	char t[200];
	if (XS.n == 1)
		snprintf(t, sizeof t, _("Ya existe «%s» aquí"), XS.name);
	else
		snprintf(t, sizeof t, _("Ya existen %d elementos aquí"), XS.n);
	int r = sheet_list(&XS.open, &XS.just, &XS.anim, &XS.sel, t, ic, lab, col, 3);
	if (r >= 0)
		f_paste(r == 0 ? XP_REPLACE : r == 1 ? XP_KEEP : XP_SKIP);
}

static void f_pick_do(const char *dst)
{
	if (FP.mode == FA_MOVE) {
		int r = ops_move(FP.src, dst);
		if (r == OPS_OK) {
			state_lock();
			mark_moved(FP.src, dst);
			event_add(EV_MOVE, FP.name, G.console, 0);
			G.rev++;
			state_unlock();
			toast_icon(_("Movido"), IC_FOLDER, T.accent2);
		} else {
			toast_icon(_("No se pudo mover"), IC_CLOSE, T.danger);
		}
	} else {
		int r = ops_copy_start(FP.src, dst, G.console);
		toast_icon(r == OPS_OK ? _("Copiando…") : r == OPS_BUSY ? _("Ya hay una copia en curso") : r == OPS_NO_SPACE ? _("No hay espacio suficiente en la SD")
		                                                                                                    : _("No se pudo copiar"),
		           r == OPS_OK ? IC_FOLDER : IC_CLOSE, r == OPS_OK ? T.accent : T.danger);
	}
	if (FP.mode == FA_MOVE)
		FP.mode = 0;
	f_reload();
}

static void f_fmt_date(char *out, size_t n, u64 t)
{
	time_t tt = (time_t)t;
	struct tm tm;
	gmtime_r(&tt, &tm);
	if (g_lang == LANG_JA)
		snprintf(out, n, "%04d/%02d/%02d %02d:%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
	else if (g_lang == LANG_EN)
		snprintf(out, n, "%02d/%02d/%04d %02d:%02d", tm.tm_mon + 1, tm.tm_mday, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
	else
		snprintf(out, n, "%02d/%02d/%04d %02d:%02d", tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, tm.tm_hour, tm.tm_min);
}

static void f_type_text(const FEnt *e, char *out, size_t n)
{
	const char *x = f_ext(e->name);
	char up[12] = "";
	for (int i = 0; x[i] && i < 10; i++)
		up[i] = (char)(x[i] >= 'a' && x[i] <= 'z' ? x[i] - 32 : x[i]);
	const FType *t = f_type(e);
	if (e->dir)
		util_strlcpy(out, e->sys ? _("Carpeta del sistema") : _("Carpeta"), n);
	else if (t && t->label && t->name)
		snprintf(out, n, tr(t->label), t->name);
	else if (t && t->label)
		util_strlcpy(out, tr(t->label), n);
	else if (!t && up[0])
		snprintf(out, n, _("Archivo %s"), up);
	else if (!t)
		util_strlcpy(out, _("Archivo"), n);
	else {
		static const char *const G[] = {
			[FK_IMG] = N_("Imagen %s"), [FK_VIDEO] = N_("Vídeo %s"), [FK_AUDIO] = N_("Audio %s"),
			[FK_TEXT] = N_("Texto %s"), [FK_ARCHIVE] = N_("Archivo comprimido %s"), [FK_FONT] = N_("Fuente %s"),
		};
		const char *g = t->k < ARRAY_LEN(G) && G[t->k] ? G[t->k] : N_("Archivo %s");
		snprintf(out, n, tr(g), up);
	}
}

// fila «etiqueta  valor» de la información
static float info_row(float x, float y, float w, const char *label, const char *value, u32 vc, float a)
{
	if (y + 13 > g_info_ymax)
		return y;
	const float LW = 74;
	float ls = TS_SUB, lw = text_width(label, ls); // (etiquetas largas, en otros idiomas: algo más pequeñas antes que cortadas)
	while (lw > LW - 6 && ls > TS_SUB * 0.6f) // (paso a paso: el japonés no encoge en proporción, ver cjk_scale)
		ls -= 0.01f, lw = text_width(label, ls);
	text_draw_clip(label, x, y + (text_lineh(TS_SUB) - text_lineh(ls)) / 2, LW - 6, ls, col_alpha(T.text3, a));
	text_draw_clip(value, x + LW, y, w - LW, TS_SUB, col_alpha(vc, a));
	return y + 15;
}

// pantalla de arriba en Archivos con el QR cerrado: la información de lo elegido
// arriba, mientras se eligen varios: cuántos, cuánto ocupan y una tira con los primeros
static void ms_top(float a)
{
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 16, W = CARD_W - 32;
	char t[64], sz[24], sub[96];
	snprintf(t, sizeof t, MS.n == 1 ? _("1 seleccionado") : _("%d seleccionados"), MS.n);
	text_draw(t, X, y0 + 10, 0.62f, col_alpha(T.text, a));
	int nd;
	util_fmt_size(sz, sizeof sz, ms_size(&nd));
	if (nd)
		snprintf(sub, sizeof sub, _("%s en archivos · %d carpetas"), sz, nd);
	else
		snprintf(sub, sizeof sub, _("%s en total"), sz);
	text_draw(sub, X + text_width(t, 0.62f) + 12, y0 + 18, TS_SUB, col_alpha(T.text2, a));
	// los primeros elegidos, en pequeño
	float x = X, ty = y0 + 40;
	int shown = 0;
	for (int i = 0; i < F.n && shown < 6; i++) {
		if (!ms_has(F.e[i].name))
			continue;
		IconId ic;
		u32 col;
		f_kind(&F.e[i], &ic, &col);
		draw_rrect(x, ty, 46, 46, 10, col_alpha(T.surface2, a));
		const Texture *th = th_get(i);
		if (th) {
			float k = fminf(42.0f / th->w, 42.0f / th->h);
			draw_texture((Texture *)th, x + 23 - th->w * k / 2, ty + 23 - th->h * k / 2, th->w * k, th->h * k, a);
		} else {
			draw_icon(ic, x + 23, ty + 23, 22, col_alpha(col, a));
		}
		x += 52;
		shown++;
	}
	if (MS.n > shown && shown) {
		char more[16];
		snprintf(more, sizeof more, "+%d", MS.n - shown);
		text_draw(more, x + 4, ty + 16, TS_BODY, col_alpha(T.text3, a));
	}
	if (!MS.n)
		text_draw(_("Toca un elemento o pulsa A para seleccionarlo"), X, ty + 16, TS_BODY, col_alpha(T.text3, a));
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	char hint[128];
	snprintf(hint, sizeof hint, "%s %s   %s %s   %s %s   %s %s", GLYPH_A, _("Seleccionar"), GLYPH_X, _("Todo"), GLYPH_Y, _("Acciones"), GLYPH_B, _("Salir"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
}

static void f_fmt_date(char *out, size_t n, u64 t);

// arriba, en la papelera: de dónde venía, cuándo y quién lo borró, y cuánto le queda
static void trash_top(float a)
{
	const FEnt *e = f_selected();
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	TrashItem t;
	if (!e || !trash_get(e->tid, &t)) {
		int n;
		char sz[24], msg[96];
		util_fmt_size(sz, sizeof sz, trash_size(&n));
		draw_icon(IC_TRASH, TOP_W / 2, y0 + h / 2 - 16, 30, col_alpha(T.text3, a));
		snprintf(msg, sizeof msg, _("Se conserva durante %d días · ocupa %s"), TRASH_DAYS, sz);
		text_draw_center(msg, TOP_W / 2, y0 + h / 2 + 8, TS_BODY, col_alpha(T.text3, a));
		return;
	}
	IconId ic;
	u32 col;
	f_kind(e, &ic, &col);
	const float PS = h - 20;
	draw_rrect(X - 4, y0 + 10, PS, PS, 12, col_alpha(T.surface2, a));
	draw_icon(ic, X - 4 + PS / 2, y0 + 10 + PS / 2, 38, col_alpha(col, a));
	float rx = X - 4 + PS + 14, rw = X + W - rx, y = y0 + 11;
	text_draw_clip(t.name, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 5;
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	g_info_ymax = by - 4;
	char v[512], b[48];
	util_strlcpy(v, t.from, sizeof v);
	char *sl = strrchr(v, '/');
	if (sl && sl != v)
		*sl = 0;
	else
		strcpy(v, "/");
	y = info_row(rx, y, rw, _("Origen"), v, T.text2, a);
	f_fmt_date(b, sizeof b, t.when);
	snprintf(v, sizeof v, t.device[0] ? "%s · %s" : "%s", b, t.device);
	y = info_row(rx, y, rw, _("Eliminado"), v, T.text2, a);
	u64 now = util_unix_now(), end = t.when + TRASH_DAYS * 86400ull;
	int days = end > now ? (int)((end - now + 86399) / 86400) : 0;
	if (days > TRASH_DAYS) // la hora de la consola pudo ir adelantada al borrar
		days = TRASH_DAYS;
	snprintf(v, sizeof v, days == 1 ? _("mañana") : _("en %d días"), days);
	y = info_row(rx, y, rw, _("Se elimina"), v, days <= 1 ? T.warn : T.text2, a);
	util_fmt_size(b, sizeof b, t.size);
	y = info_row(rx, y, rw, _("Tamaño"), b, T.text2, a);
	g_info_ymax = 1e9f;
	char hint[96];
	snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_A, _("Recuperar"), GLYPH_Y, _("Opciones"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
}

static void files_top(float a)
{
	const FEnt *se = f_selected();
	if (mu_on() && se && !F.search && !strcmp(F.path, MU.s.dir) && !strcmp(se->name, MU.s.name) && !ZS.open && !HS.open) {
		music_top(a); // la que suena: su carátula y su tiempo
		return;
	}
	if (F.titles && !ZS.open && !HS.open) {
		titles_top(a);
		return;
	}
	if (ZS.open) {
		zip_top(a);
		return;
	}
	if (HS.open) {
		hash_top(a);
		return;
	}
	if (MS.on) {
		ms_top(a);
		return;
	}
	if (F.trash) {
		trash_top(a);
		return;
	}
	f_info_refresh();
	const FEnt *e = f_selected();
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	if (!e) {
		draw_icon(IC_FOLDER, TOP_W / 2, y0 + h / 2 - 14, 30, col_alpha(T.text3, a));
		text_draw_center(_("Selecciona un archivo para ver su información"), TOP_W / 2, y0 + h / 2 + 8, TS_BODY, col_alpha(T.text3, a));
		return;
	}
	IconId ic;
	u32 col;
	f_kind(e, &ic, &col);
	// a la izquierda, la vista previa: la foto, el icono del programa o el del tipo
	const float PS = h - 20;
	float py = y0 + 10;
	draw_rrect(X - 4, py, PS, PS, 12, col_alpha(T.surface2, a));
	float px = X - 4;
	if (F.thumb.ok) {
		float k = fminf(1, fminf((PS - 4) / F.thumb.w, (PS - 4) / F.thumb.h)), tw = F.thumb.w * k, th = F.thumb.h * k;
		draw_texture(&F.thumb, px + (PS - tw) / 2, py + (PS - th) / 2, tw, th, a);
	} else if (F.cia_icon.ok)
		draw_texture(&F.cia_icon, px + (PS - 72) / 2, py + (PS - 72) / 2, 72, 72, a);
	else if (F.preview[0]) { // las primeras líneas, como una hoja
		gfx_clip(px, py, PS, PS);
		text_wrap_lines(F.preview, PS - 12, 0.32f, 9, px + 6, py + 5, col_alpha(T.text2, a), true);
		gfx_clip_off();
	} else
		draw_icon(ic, px + PS / 2, py + PS / 2, 38, col_alpha(col, a));
	// a la derecha: nombre, datos y, abajo, qué hacen los botones
	float rx = px + PS + 14, rw = X + W - rx, y = py + 1;
	const char *title = F.cia_ok && F.cia->title[0] ? F.cia->title : F.app_title[0] ? F.app_title : e->name;
	text_draw_clip(title, rx, y, rw, TS_TITLE, col_alpha(e->color ? col : T.text, a)); // si no cabe, se desplaza
	y += text_lineh(TS_TITLE) + 2;
	const char *pub = F.cia_ok ? F.cia->publisher : F.app_pub;
	if (pub[0]) {
		text_draw_clip(pub, rx, y, rw, TS_SUB, col_alpha(T.text2, a));
		y += 15;
	}
	y += 3;
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	g_info_ymax = by - 4;
	char v[160], b[48];
	if (F.search) { // en una búsqueda, dónde está
		char full[700];
		f_full(F.v[F.sel].idx, full, sizeof full);
		char *sl = strrchr(full, '/');
		if (sl)
			*(sl == full ? sl + 1 : sl) = 0;
		y = info_row(rx, y, rw, _("Carpeta"), full, T.text2, a);
	}
	if (F.cia_ok) {
		u32 vc;
		char vl[160];
		ins_version_line(F.cia, vl, sizeof vl, &vc);
		y = info_row(rx, y, rw, _("Versión"), vl, vc, a);
	} else {
		f_type_text(e, v, sizeof v);
		y = info_row(rx, y, rw, _("Tipo"), v, e->sys ? T.warn : T.text2, a);
	}
	if (e->dir) {
		if (F.dir_count >= 0) {
			if (F.dir_more)
				snprintf(v, sizeof v, _("Más de %d elementos"), F.dir_count);
			else
				snprintf(v, sizeof v, F.dir_count == 1 ? _("1 elemento") : _("%d elementos"), F.dir_count);
			y = info_row(rx, y, rw, _("Contenido"), v, T.text2, a);
		}
	} else {
		util_fmt_size(b, sizeof b, e->size);
		y = info_row(rx, y, rw, _("Tamaño"), b, T.text2, a);
	}
	if (F.iw > 0) {
		snprintf(v, sizeof v, "%d × %d", F.iw, F.ih);
		y = info_row(rx, y, rw, _("Dimensiones"), v, T.text2, a);
	}
	if (e->mtime) {
		f_fmt_date(v, sizeof v, e->mtime);
		y = info_row(rx, y, rw, _("Modificado"), v, T.text2, a);
	}
	if (F.cia_ok) {
		snprintf(v, sizeof v, "%016llX", (unsigned long long)F.cia->tid);
		y = info_row(rx, y, rw, _("ID"), v, T.text2, a);
	}
	g_info_ymax = 1e9f;
	bool can_open = e->dir || f_is_img(e) || f_is_txt(e) || F.cia_ok;
	const char *open_l = F.cia_ok ? _("Instalar") : _("Abrir");
	char hint[96];
	if (F.search)
		snprintf(hint, sizeof hint, "%s %s   %s %s   %s %s", GLYPH_A, open_l, GLYPH_Y, _("Opciones"), GLYPH_X, _("Ubicación"));
	else if (can_open)
		snprintf(hint, sizeof hint, "%s %s   %s %s   %s %s", GLYPH_A, open_l, GLYPH_Y, _("Opciones"), GLYPH_X, _("Nuevo"));
	else
		snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_Y, _("Opciones"), GLYPH_X, _("Nuevo"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
}

// pantalla de arriba en Dispositivos con el QR cerrado: la información del elegido
static bool device_top(float a)
{
	const Device *d = NULL;
	for (int i = 0; i < MAX_DEVICES && !d; i++)
		if (V.dev[i].used && !strcmp(V.dev[i].token, U.dev_tok))
			d = &V.dev[i];
	if (!d) { // aún no se eligió ninguno: el primero de la lista
		int idx[MAX_DEVICES];
		if (!device_order(idx))
			return false;
		d = &V.dev[idx[0]];
	}
	bool on = dev_online(d);
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	// a la izquierda el icono con su estado; a la derecha el nombre, si está conectado y sus datos
	const float IS = h - 20;
	float px = X - 4, py = y0 + 10;
	draw_rrect(px, py, IS, IS, 12, col_alpha(on ? col_alpha(T.ok, 0.14f) : T.surface2, a));
	draw_icon(dev_icon(d), px + IS / 2, py + IS / 2, 40, col_alpha(on ? T.ok : T.text3, a));
	float rx = px + IS + 14, rw = X + W - rx, y = py + 1;
	char v[96], when[24];
	text_draw_clip(d->name, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 2;
	if (!on && d->last_seen) {
		util_fmt_when(when, sizeof when, d->last_seen);
		snprintf(v, sizeof v, _("Última conexión: %s"), when);
	}
	if (on)
		draw_circle(rx + 3, y + text_lineh(TS_SUB) / 2, 3, col_alpha(T.ok, a));
	text_draw_clip(on ? _("Conectado") : d->last_seen ? v : _("Desconectado"), rx + (on ? 10 : 0), y, rw, TS_SUB,
	               col_alpha(on ? T.ok : T.text3, a));
	y += 18;
	float by = y0 + h - 10 - text_lineh(TS_SUB);
	g_info_ymax = by - 4;
	const char *kind = d->kind == 0 ? _("Móvil") : d->kind == 2 ? _("Tableta") : _("Ordenador");
	if (d->model[0])
		snprintf(v, sizeof v, "%s · %s", kind, d->model);
	else
		util_strlcpy(v, kind, sizeof v);
	y = info_row(rx, y, rw, _("Tipo"), v, T.text2, a);
	if (d->ip[0])
		y = info_row(rx, y, rw, on ? _("IP") : _("Última IP"), d->ip, T.text2, a);
	if (d->created) {
		f_fmt_date(v, sizeof v, d->created);
		y = info_row(rx, y, rw, _("Vinculado"), v, T.text2, a);
	}
	const char *src = d->named == NAME_CUSTOM ? _("Definido manualmente") : d->named == NAME_FOUND ? _("Obtenido de la red") : _("Detectado por el navegador");
	y = info_row(rx, y, rw, _("Nombre"), src, T.text2, a);
	g_info_ymax = 1e9f;
	char hint[96];
	snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_Y, _("Opciones"), GLYPH_X, _("Desvincular"));
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
	return true;
}

static void set_flag(bool *f)
{
	state_lock();
	*f = !*f;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
}

// enciende o apaga el servidor (se guarda: sigue así al volver a abrir)
static void srv_set(bool on)
{
	state_lock();
	G.srv_off = !on;
	if (!on)
		G.qr_min = true; // sin servidor, el QR no sirve
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
	server_set_enabled(on);
	toast_icon(on ? _("Servidor encendido") : _("Servidor apagado"), on ? IC_WIFI : IC_LOCK, on ? T.ok : T.text3);
}

static void ask_exit(void);

static void settings_action(int i)
{
	switch (i) {
	case SET_READONLY:
		set_flag(&G.readonly);
		toast_icon(V.readonly ? _("Modo solo lectura activado") : _("Modo solo lectura desactivado"), IC_LOCK, T.warn);
		break;
	case SET_PROTECT:
		set_flag(&G.protect_sys);
		break;
	case SET_PIN:
		U.page = PAGE_PIN;
		U.list[TAB_COUNT + 2].sel = PIN_CUSTOM;
		U.list[TAB_COUNT + 2].off = U.list[TAB_COUNT + 2].target = 0;
		break;
	case SET_CONN:
		U.page = PAGE_CONN;
		U.list[TAB_COUNT + 4].sel = 0;
		U.list[TAB_COUNT + 4].off = U.list[TAB_COUNT + 4].target = 0;
		break;
	case SET_SHOTS:
		set_flag(&G.shots_live);
		break;
	case SET_LANG:
		U.page = PAGE_LANG;
		U.list[TAB_COUNT].sel = V.lang;
		U.list[TAB_COUNT].off = U.list[TAB_COUNT].target = 0;
		break;
	case SET_THEME:
		set_flag(&G.light);
		theme_set(!V.light);
		break;
	case SET_UPDATE:
		U.page = PAGE_UPDATE;
		U.list[TAB_COUNT + 3].sel = 0;
		U.list[TAB_COUNT + 3].off = U.list[TAB_COUNT + 3].target = 0;
		if (U.upd.st == UPD_IDLE || U.upd.st == UPD_ERROR)
			upd_check();
		break;
	case SET_ABOUT:
		U.page = PAGE_ABOUT;
		U.list[TAB_COUNT + 1].sel = AB_APP;
		U.list[TAB_COUNT + 1].off = U.list[TAB_COUNT + 1].target = 0;
		break;
	case SET_EXIT:
		ask_exit(); // el mismo diálogo que START
		break;
	}
}

// tarjeta de arriba que explica algo: icono, título, texto y una línea al pie
static void explain_top(float a, IconId ic, u32 col, const char *title, const char *desc, const char *foot, u32 fc)
{
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 16, W = CARD_W - 32;
	float cx = X + 26, cy = y0 + 16 + 26;
	draw_circle(cx, cy, 26, col_alpha(col, 0.16f * a));
	draw_icon(ic, cx, cy, 24, col_alpha(col, a));
	float rx = X + 66, rw = X + W - rx, y = y0 + 14;
	text_draw_clip(title, rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 4;
	text_wrap_lines(desc, rw, TS_SUB, foot ? 4 : 5, rx, y, col_alpha(T.text2, a), true);
	if (foot) { // abajo, en una o dos líneas
		int nl = text_wrap_lines(foot, rw, TS_SUB, 2, 0, 0, 0, false);
		text_wrap_lines(foot, rw, TS_SUB, 2, rx, y0 + h - 10 - nl * text_lineh(TS_SUB), col_alpha(fc, a), true);
	}
}

// Ajustes → PIN y → Conexión: arriba, lo que hace la opción marcada
static void settings_sub_top(float a)
{
	char foot[96];
	if (U.page == PAGE_PIN) {
		int i = U.list[TAB_COUNT + 2].sel;
		snprintf(foot, sizeof foot, _("PIN actual: %s"), V.pin);
		if (i == PIN_CUSTOM)
			explain_top(a, IC_KEY, T.accent2, _("Definir un PIN"), _("Escribe cuatro cifras que te resulten fáciles de recordar. El código QR se renueva con él y los dispositivos ya vinculados siguen conectados."), foot, T.text3);
		else
			explain_top(a, IC_REFRESH, T.accent2, _("PIN aleatorio nuevo"), _("La consola genera un PIN aleatorio y un nuevo código QR; los anteriores dejan de ser válidos. Los dispositivos ya vinculados siguen conectados."), foot, T.text3);
		return;
	}
	int i = U.list[TAB_COUNT + 4].sel;
	if (i == 0 || V.srv_off) {
		if (V.srv_off)
			explain_top(a, IC_LOCK, T.text3, _("Servidor apagado"),
			            _("Sin servidor no están disponibles la web, cloud3ds.local, las subidas desde el PC ni la instalación remota. La Nube, las descargas y el Navegador funcionan con normalidad."),
			            _("Desactivado consume menos batería. Se activa aquí o al abrir Conectar."), T.text3);
		else
			explain_top(a, IC_WIFI, T.ok, _("Servidor para el PC y el móvil"),
			            _("Con el servidor activo, el móvil o el PC acceden a la consola desde el navegador para ver, subir y descargar archivos e instalar CIA."),
			            _("Si no lo usas, puedes desactivarlo para ahorrar batería."), T.text3);
		return;
	}
	i--;
	if (i == 0) {
		snprintf(foot, sizeof foot, V.mdns ? _("Activado: %s.local") : _("Desactivado"), V.net_name);
		explain_top(a, IC_GLOBE, T.ok, _("Nombre en la red"),
		            _("Permite acceder a la consola por su nombre aunque cambie su IP. Funciona en la mayoría de móviles y PC; si alguno no lo admite, usa la IP."),
		            foot, V.mdns ? T.ok : T.text3);
	} else if (i == 1)
		explain_top(a, IC_LAPTOP, T.accent, _("Instalación remota (FBI)"),
		            _("Las aplicaciones de PC diseñadas para FBI (como servefiles o sendurls) pueden enviar CIA a la consola por el puerto 5000, igual que a FBI."),
		            V.fbi_remote ? _("Activado") : _("Desactivado"), V.fbi_remote ? T.ok : T.text3);
	else {
		snprintf(foot, sizeof foot, _("Ahora: %s.local"), V.net_name);
		explain_top(a, IC_PEN, T.accent2, _("Cambiar el nombre"),
		            _("Si tienes varias consolas, asigna un nombre distinto a cada una (por ejemplo, 3ds-salon) para distinguirlas en la red."), foot, T.text3);
	}
}

// pantalla de arriba en Ajustes: qué hace el ajuste marcado, entero y sin prisas
static void settings_top(float a)
{
	int i = U.list[TAB_SETTINGS].sel;
	if (i < 0 || i >= SET_COUNT)
		i = 0;
	float y0 = BODY_Y, h = BODY_B - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	static const IconId IC[SET_COUNT] = {IC_LOCK, IC_SHIELD, IC_KEY, IC_WIFI, IC_CAMERA, IC_GLOBE, IC_PALETTE, IC_REFRESH, IC_INFO, IC_POWER};
	const u32 C[SET_COUNT] = {T.warn, T.danger, T.accent2, T.ok, T.accent, T.accent, T.accent2, T.ok, T.text3, T.danger};
	const char *TT_[SET_COUNT] = {_("Solo lectura"), _("Proteger carpetas del sistema"), _("PIN"), _("Conexión"), _("Capturas al instante"),
	                              _("Idioma"), _("Tema"), _("Actualizaciones"), _("Acerca de Cloud3DS"), _("Salir")};
	const char *D[SET_COUNT] = {
	    _("Con «Solo lectura», los dispositivos pueden ver y descargar archivos, pero no subir, renombrar ni eliminar."),
	    _("Las carpetas del sistema (Nintendo 3DS, 3ds, luma, boot.firm…) no se pueden renombrar, mover ni borrar, ni desde la consola ni desde la web."),
	    _("El PIN sirve para vincular un dispositivo sin escanear el QR. Se mantiene hasta que lo cambies; los ya vinculados siguen conectados."),
	    _("El nombre de la consola en la red (para acceder mediante .local) y la instalación remota desde FBI."),
	    _("Cada captura de pantalla nueva aparece al instante en la web abierta, lista para descargar."),
	    _("Idioma de la consola. La web usa el idioma de tu móvil o tu PC."),
	    _("Tema oscuro o claro para la consola. La web sigue el tema de tu móvil o tu PC."),
	    _("Busca versiones nuevas de Cloud3DS y las instala sin salir de la app."),
	    _("Versión, créditos y enlaces a la comunidad."),
	    _("Cierra Cloud3DS. Se detendrán las copias e instalaciones en curso."),
	};
	const float X = CARD_X + 16, W = CARD_W - 32;
	float cx = X + 26, cy = y0 + 16 + 26;
	draw_circle(cx, cy, 26, col_alpha(C[i], 0.16f * a));
	draw_icon(IC[i], cx, cy, 24, col_alpha(C[i], a));
	float rx = X + 66, rw = X + W - rx, y = y0 + 14;
	text_draw_clip(TT_[i], rx, y, rw, TS_TITLE, col_alpha(T.text, a));
	y += text_lineh(TS_TITLE) + 4;
	y += text_wrap_lines(D[i], rw, TS_SUB, 4, rx, y, col_alpha(T.text2, a), true) * text_lineh(TS_SUB) + 6;
	// el estado, si es un interruptor
	const char *st = NULL;
	u32 sc = T.text3;
	if (i == SET_READONLY)
		st = V.readonly ? _("Activado") : _("Desactivado"), sc = V.readonly ? T.warn : T.text3;
	else if (i == SET_PROTECT)
		st = V.protect_sys ? _("Activado") : _("Desactivado"), sc = V.protect_sys ? T.ok : T.text3;
	else if (i == SET_SHOTS)
		st = V.shots_live ? _("Activado") : _("Desactivado"), sc = V.shots_live ? T.ok : T.text3;
	if (st)
		text_draw_clip(st, rx, y0 + h - 10 - text_lineh(TS_SUB), rw, TS_SUB, col_alpha(sc, a));
}

static void settings_main(void)
{
	if ((g_kdown & KEY_B) && !g_dialog.open) { // en la raíz de la aplicación: al inicio
		hm_show();
		return;
	}
	List *l = cur_list();
	tab_header(_("Ajustes"), false);
	const float RH = 44;
	list_update(l, SET_COUNT, RH);
	U.t_ro = approach(U.t_ro, V.readonly ? 1 : 0, 14);
	U.t_prot = approach(U.t_prot, V.protect_sys ? 1 : 0, 14);
	U.t_shots = approach(U.t_shots, V.shots_live ? 1 : 0, 14);
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < SET_COUNT; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		bool t = row(l, i, y, RH);
		const char *title = "", *sub = NULL, *val = NULL;
		char vbuf[48];
		IconId ic = IC_GEAR;
		u32 col = T.accent;
		bool chev = false;
		switch (i) {
		case SET_READONLY:
			title = _("Solo lectura"); sub = _("Solo consulta y descarga"); ic = IC_LOCK; col = T.warn;
			break;
		case SET_PROTECT:
			title = _("Proteger carpetas del sistema"); sub = _("Nintendo 3DS, luma y otras");
			ic = IC_SHIELD; col = T.danger;
			break;
		case SET_PIN:
			title = _("PIN"); sub = _("No caduca"); ic = IC_KEY; col = T.accent2; val = V.pin;
			chev = true;
			break;
		case SET_CONN:
			title = _("Conexión"); ic = IC_WIFI; col = T.ok; chev = true;
			snprintf(vbuf, sizeof vbuf, "%s.local", V.net_name);
			sub = V.srv_off ? _("Servidor apagado") : V.mdns ? vbuf : _("Nombre en la red y FBI remoto");
			break;
		case SET_SHOTS:
			title = _("Capturas al instante"); sub = _("Aparecen al instante en la web"); ic = IC_CAMERA; col = T.accent;
			break;
		case SET_LANG:
			title = _("Idioma"); ic = IC_GLOBE; chev = true;
			snprintf(vbuf, sizeof vbuf, "%s", lang_setting_name(V.lang));
			val = vbuf;
			break;
		case SET_THEME:
			title = _("Tema"); ic = IC_PALETTE; col = T.accent2; val = V.light ? _("Claro") : _("Oscuro");
			break;
		case SET_UPDATE:
			title = _("Actualizaciones"); ic = IC_REFRESH; col = T.ok; chev = true;
			sub = U.upd.st == UPD_AVAILABLE ? _("Hay una versión nueva") : U.upd.st == UPD_LATEST ? _("Cloud3DS está al día")
			    : U.upd.st == UPD_CHECKING ? _("Buscando actualizaciones…") : _("Buscar versiones nuevas");
			if (U.upd.st == UPD_AVAILABLE) {
				snprintf(vbuf, sizeof vbuf, "v%s", U.upd.latest);
				val = vbuf;
			}
			break;
		case SET_ABOUT:
			title = _("Acerca de Cloud3DS"); ic = IC_INFO; col = T.text3; chev = true;
			snprintf(vbuf, sizeof vbuf, "v%s · %s", APP_VERSION, "VENOMSHiNE");
			sub = vbuf;
			break;
		case SET_EXIT:
			title = _("Salir"); sub = _("También con START"); ic = IC_POWER; col = T.danger;
			break;
		}
		row_icon(28, y + RH / 2, ic, col);
		float right = BOT_W - 16;
		if (i == SET_READONLY || i == SET_PROTECT || i == SET_SHOTS) {
			toggle(right - 36, y + RH / 2 - 10, i == SET_READONLY ? U.t_ro : i == SET_PROTECT ? U.t_prot : U.t_shots);
			right -= 46;
		} else {
			if (chev) {
				draw_icon(IC_CHEV_RIGHT, right - 4, y + RH / 2, 12, T.text3);
				right -= 16;
			}
			if (val) {
				float w = fminf(text_width(val, TS_BODY), 110);
				text_draw_clip(val, right - w, y + (RH - text_lineh(TS_BODY)) / 2, 110, TS_BODY, T.text3);
				right -= w + 10;
			}
		}
		float tw = right - 50;
		if (sub) {
			text_draw_clip(title, 50, y + 6, tw, TS_BODY, T.text);
			text_draw_clip(sub, 50, y + 23, tw, TS_SUB, T.text3);
		} else {
			text_draw_clip(title, 50, y + (RH - text_lineh(TS_BODY)) / 2, tw, TS_BODY, T.text);
		}
		if (t || activate(l, i))
			settings_action(i);
	}
	gfx_clip_off();
}

// Ajustes → Conexión: el nombre en la red (mDNS) y la instalación remota de FBI
static void remote_changed(void)
{
	state_lock();
	G.remote_rev++;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
}

static void settings_conn(void)
{
	List *l = cur_list();
	tab_header(_("Conexión"), true);
	const float RH = 48;
	list_update(l, 4, RH);
	static float t_srv = 1;
	t_srv = approach(t_srv, V.srv_off ? 0 : 1, 14);
	U.t_mdns = approach(U.t_mdns, V.mdns ? 1 : 0, 14);
	U.t_fbi = approach(U.t_fbi, V.fbi_remote ? 1 : 0, 14);
	for (int i = 0; i < 4; i++) {
		float y = VIEW_Y + i * RH - l->off;
		bool t = row(l, i, y, RH);
		bool dim = i > 0 && V.srv_off; // sin servidor, lo demás no hace nada
		char title[64], sub[120];
		IconId ic;
		u32 col;
		if (i == 0) {
			snprintf(title, sizeof title, "%s", _("Servidor para el PC y el móvil"));
			if (V.srv_off)
				snprintf(sub, sizeof sub, "%s", _("Desactivado · La Nube sigue disponible"));
			else
				snprintf(sub, sizeof sub, "%s", _("Activado · Web y subidas disponibles"));
			ic = IC_WIFI, col = V.srv_off ? T.text3 : T.ok;
		} else if (i == 1) {
			snprintf(title, sizeof title, "%s", _("Nombre en la red"));
			if (V.mdns && !(V.remote_ok & 1) && V.running)
				snprintf(sub, sizeof sub, "%s", _("No se pudo activar en esta red"));
			else
				snprintf(sub, sizeof sub, _("Acceso por %s.local"), V.net_name);
			ic = IC_GLOBE, col = T.ok;
		} else if (i == 2) {
			snprintf(title, sizeof title, "%s", _("Instalación remota (FBI)"));
			if (V.fbi_remote && !(V.remote_ok & 2) && V.running)
				snprintf(sub, sizeof sub, "%s", _("El puerto 5000 está ocupado"));
			else
				snprintf(sub, sizeof sub, "%s", _("Apps de PC para FBI · puerto 5000"));
			ic = IC_LAPTOP, col = T.accent;
		} else {
			snprintf(title, sizeof title, "%s", _("Cambiar el nombre"));
			snprintf(sub, sizeof sub, "%s", _("Útil con varias consolas, p. ej. 3ds-salon.local"));
			ic = IC_PEN, col = T.accent2;
		}
		float k = dim ? 0.4f : 1;
		row_icon(28, y + RH / 2, ic, col_alpha(col, k));
		float right = BOT_W - 16;
		if (i < 3) {
			if (dim)
				draw_rrect(right - 36, y + RH / 2 - 10, 36, 20, 10, T.surface2);
			else
				toggle(right - 36, y + RH / 2 - 10, i == 0 ? t_srv : i == 1 ? U.t_mdns : U.t_fbi);
			right -= 46;
		} else {
			draw_icon(IC_CHEV_RIGHT, right - 4, y + RH / 2, 12, col_alpha(T.text3, k));
			right -= 16;
		}
		text_draw_clip(title, 50, y + 7, right - 50, TS_BODY, col_alpha(T.text, k));
		text_draw_clip(sub, 50, y + 25, right - 50, TS_SUB, col_alpha(T.text3, k));
		if ((t || activate(l, i)) && !dim) {
			if (i == 0)
				srv_set(V.srv_off);
			else if (i == 1) {
				set_flag(&G.mdns);
				remote_changed();
			} else if (i == 2) {
				set_flag(&G.fbi_remote);
				remote_changed();
			} else
				U.want_netname = true;
		}
	}
}

static void settings_lang(void)
{
	List *l = cur_list();
	tab_header(_("Idioma"), true);
	const float RH = 40;
	list_update(l, 4, RH);
	for (int i = 0; i < 4; i++) {
		float y = VIEW_Y + i * RH - l->off;
		bool t = row(l, i, y, RH);
		row_icon(28, y + RH / 2, i == LSET_SYSTEM ? IC_GEAR : IC_GLOBE, i == LSET_SYSTEM ? T.text3 : T.accent);
		if (i == LSET_SYSTEM) {
			text_draw_clip(lang_setting_name(i), 50, y + 5, 220, TS_BODY, T.text);
			text_draw_clip(_("Usa el idioma de la consola"), 50, y + 22, 220, TS_SUB, T.text3);
		} else {
			text_draw_clip(lang_setting_name(i), 50, y + (RH - text_lineh(TS_BODY)) / 2, 220, TS_BODY, T.text);
		}
		if (V.lang == i)
			draw_icon(IC_CHECK, BOT_W - 26, y + RH / 2, 16, T.accent);
		if (t || activate(l, i)) {
			state_lock();
			G.lang = i;
			G.dirty_cfg = true;
			G.rev++;
			memcpy(&V, &G, sizeof G);
			state_unlock();
			i18n_apply(i);
			U.page = PAGE_MAIN;   // vuelve a Ajustes con el idioma ya aplicado
			U.list[TAB_SETTINGS].sel = SET_LANG;
		}
	}
}

static void settings_pin(void)
{
	List *l = cur_list();
	tab_header(_("PIN"), true);
	const float RH = 44;
	list_update(l, PIN_COUNT, RH);
	for (int i = 0; i < PIN_COUNT; i++) {
		float y = VIEW_Y + i * RH - l->off;
		bool t = row(l, i, y, RH);
		row_icon(28, y + RH / 2, i == PIN_CUSTOM ? IC_KEY : IC_REFRESH, T.accent2);
		text_draw_clip(i == PIN_CUSTOM ? _("Definir un PIN") : _("PIN aleatorio nuevo"), 50, y + 6, 230, TS_BODY, T.text);
		text_draw_clip(i == PIN_CUSTOM ? _("Cuatro cifras fáciles de recordar") : _("La consola genera uno aleatorio"),
		               50, y + 23, 230, TS_SUB, T.text3);
		if (t || activate(l, i)) {
			if (i == PIN_CUSTOM)
				U.want_pin = true;
			else
				dialog_open(DLG_NEWPIN, _("¿Crear un PIN nuevo?"),
				            _("El PIN y el código QR actuales dejarán de funcionar. Los dispositivos vinculados seguirán conectados."),
				            _("Cambiar"), false);
		}
	}
	float y = VIEW_Y + PIN_COUNT * RH + 10 - l->off;
	text_wrap_lines(_("Al cambiar el PIN también se renueva el código QR. Los dispositivos vinculados seguirán conectados."),
	                BOT_W - 40, TS_SUB, 3, 20, y, T.text3, true);
}


static void ul_draw(void)
{
	IconId ic[6];
	const char *lab[6];
	char tx[6][96];
	u32 col[6];
	for (int i = 0; i < UL.n; i++) {
		const char *b = strrchr(UL.f[i].path, '/');
		if (UL.f[i].ver[0])
			snprintf(tx[i], sizeof tx[i], "v%s · %s", UL.f[i].ver, b ? b + 1 : UL.f[i].path);
		else
			snprintf(tx[i], sizeof tx[i], "%s", b ? b + 1 : UL.f[i].path);
		ic[i] = IC_DOWNLOAD, lab[i] = tx[i], col[i] = T.ok;
	}
	int r = sheet_list(&UL.open, &UL.just, &UL.anim, &UL.sel, _("Selecciona el archivo"), ic, lab, col, UL.n);
	if (r >= 0 && r < UL.n) {
		char msg[400];
		if (UL.f[r].ver[0])
			snprintf(msg, sizeof msg, _("Se instalará Cloud3DS v%s desde %s. No apagues la consola durante el proceso."), UL.f[r].ver, UL.f[r].path);
		else
			snprintf(msg, sizeof msg, _("Se instalará Cloud3DS desde %s. No apagues la consola durante el proceso."), UL.f[r].path);
		U.dlg_arg = r;
		dialog_open(DLG_UPDFILE, _("¿Actualizar desde la SD?"), msg, _("Actualizar"), false);
	}
}

static void settings_update(void)
{
	List *l = cur_list();
	tab_header(_("Actualizaciones"), true);
	const float RH = 44;
	enum { R_MAIN, R_FILE, R_AUTO, R_N };
	list_update(l, R_N, RH);
	U.t_upd = approach(U.t_upd, V.upd_auto ? 1 : 0, 14);
	bool busy = U.upd.st == UPD_DOWNLOADING || U.upd.st == UPD_INSTALLING || U.upd.st == UPD_CHECKING;
	for (int i = 0; i < R_N; i++) {
		float y = VIEW_Y + i * RH - l->off;
		bool t = row(l, i, y, RH);
		const char *title, *sub;
		char tb[64];
		IconId ic;
		u32 col;
		if (i == R_AUTO) {
			title = _("Buscar al abrir la app");
			sub = _("Consulta GitHub en segundo plano");
			ic = IC_CLOCK, col = T.accent2;
			toggle(BOT_W - 52, y + RH / 2 - 10, U.t_upd);
		} else if (i == R_FILE) {
			title = _("Instalar desde un archivo");
			sub = envIsHomebrew() ? _("Un archivo .3dsx de Cloud3DS copiado en la SD") : _("Un archivo CIA de Cloud3DS copiado en la SD");
			ic = IC_FOLDER, col = busy ? T.text3 : T.warn;
		} else if (U.upd.st == UPD_AVAILABLE && U.upd.has_file) {
			snprintf(tb, sizeof tb, _("Actualizar a la versión %s"), U.upd.latest);
			title = tb, sub = _("Se descarga, se verifica y se instala");
			ic = IC_DOWNLOAD, col = T.ok;
		} else if (U.upd.st == UPD_DONE && !envIsHomebrew()) {
			title = _("Reiniciar Cloud3DS"), sub = _("Para usar la nueva versión");
			ic = IC_REFRESH, col = T.ok;
		} else {
			title = _("Buscar actualizaciones");
			snprintf(tb, sizeof tb, _("Versión instalada: v%s"), APP_VERSION);
			sub = tb, ic = IC_REFRESH, col = busy ? T.text3 : T.accent;
		}
		row_icon(28, y + RH / 2, ic, col);
		float tw = i == R_AUTO ? BOT_W - 120 : BOT_W - 66;
		text_draw_clip(title, 50, y + 6, tw, TS_BODY, busy && i != R_AUTO ? T.text3 : T.text);
		text_draw_clip(sub, 50, y + 23, tw, TS_SUB, T.text3);
		if (i == R_FILE)
			draw_icon(IC_CHEV_RIGHT, BOT_W - 20, y + RH / 2, 12, T.text3);
		if (!(t || activate(l, i)))
			continue;
		if (i == R_AUTO) {
			set_flag(&G.upd_auto);
		} else if (busy) {
			continue;
		} else if (i == R_FILE) {
			UL.n = upd_find_local(UL.f, 6);
			if (!UL.n)
				toast_icon(envIsHomebrew() ? _("No hay ningún Cloud3DS .3dsx en la SD") : _("No se encontró ningún CIA de Cloud3DS en /cias, /Downloads ni en la raíz de la SD"),
				           IC_INFO, T.warn);
			else
				UL.open = UL.just = true, UL.sel = 0;
		} else if (U.upd.st == UPD_AVAILABLE && U.upd.has_file) {
			char msg[200];
			snprintf(msg, sizeof msg, _("Se descargará la versión %s desde GitHub, se verificará y se instalará. No apagues la consola durante el proceso."),
			         U.upd.latest);
			dialog_open(DLG_UPDATE, _("¿Actualizar Cloud3DS?"), msg, _("Actualizar"), false);
		} else if (U.upd.st == UPD_DONE) {
			if (!envIsHomebrew()) {
				aptSetChainloaderToSelf(); // vuelve a abrir Cloud3DS, ya con la versión nueva
				U.quit = true;
			}
		} else {
			upd_check();
		}
	}
}

static void settings_about(void)
{
	List *l = cur_list();
	tab_header(_("Acerca de Cloud3DS"), true);
	const float RH = 40;
	list_update(l, AB_COUNT, RH);
	static const IconId IC[AB_COUNT] = {IC_CLOUD, IC_CODE, IC_CHAT, IC_HEART};
	const u32 COL[AB_COUNT] = {T.accent, col_hex(0x6E7681), col_hex(0x5865F2), col_hex(0xFF5E5B)};
	const char *title[AB_COUNT] = {"Cloud3DS " APP_VERSION, "GitHub", "Discord", "Ko-fi"};
	const char *sub[AB_COUNT] = {_("Gracias por usar Cloud3DS"), _("Código fuente e informes de errores"), _("Comunidad y ayuda"),
	                             _("Apoyar el desarrollo")};
	for (int i = 0; i < AB_COUNT; i++) {
		float y = VIEW_Y + i * RH - l->off;
		row(l, i, y, RH);
		row_icon(28, y + RH / 2, IC[i], COL[i]);
		text_draw_clip(title[i], 50, y + 5, 230, TS_BODY, T.text);
		text_draw_clip(sub[i], 50, y + 22, 230, TS_SUB, T.text3);
		if (i == l->sel)
			draw_icon(i == AB_APP ? IC_INFO : IC_QR, BOT_W - 26, y + RH / 2, 15, T.accent);
	}
}

static void qr_show(bool on)
{
	if (V.qr_min == !on)
		return;
	state_lock();
	G.qr_min = !on;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
}

// botón central: «Traer a la consola», con las tres formas de traer algo (conectar un
// dispositivo, escanear un QR o descargar un enlace) en tarjetas grandes

static void qa_run(int i)
{
	QA.open = false;
	if (i == QA_CLOUD)
		cl_open();
	else if (i == QA_CONNECT) {
		if (V.srv_off)
			srv_set(true);
		qr_show(true);
	} else if (i == QA_SCAN)
		qrs_open();
	else if (i == QA_WEB)
		U.want_web = true;
	else
		U.want_dlurl = true;
}

// el carrusel: la tarjeta marcada en el centro, más grande e iluminada; las de los lados asoman
#define QA_SP 106.0f // separación entre tarjetas
static void qa_card_text(int i, const char **lab, const char **sub);
static void cl_open(void);
static const IconId QA_IC[QA_N] = {IC_CLOUD, IC_QR, IC_CAMERA, IC_LINK, IC_GLOBE};
static u32 qa_col(int i)
{
	const u32 c[QA_N] = {T.accent, T.accent, T.ok, T.warn, T.accent2};
	return c[i];
}

static void qa_draw(void)
{
	QA.anim = approach(QA.anim, QA.open ? 1 : 0, 16);
	if (QA.anim < 0.01f)
		return;
	float a = ease_out_cubic(QA.anim);
	float bottom = SCR_H - TABBAR_H;
	draw_rect(0, 0, BOT_W, bottom, col_alpha(T.dim, a * 0.82f));
	if (!QA.drag)
		QA.pos = approach(QA.pos, (float)QA.sel, 14);
	float yo = (1 - a) * 24;
	text_draw_center(_("Traer a la consola"), BOT_W / 2, 6 + yo, TS_TITLE, col_alpha(T.text, a));
	// de la más lejana a la marcada, para que la del centro quede encima
	int order[QA_N];
	for (int i = 0; i < QA_N; i++)
		order[i] = i;
	for (int i = 0; i < QA_N; i++)
		for (int j = i + 1; j < QA_N; j++)
			if (fabsf(order[j] - QA.pos) > fabsf(order[i] - QA.pos)) {
				int t = order[i];
				order[i] = order[j], order[j] = t;
			}
	const float cy = 30 + 66 + yo;
	int run = -1, hit = -1;
	for (int k = 0; k < QA_N; k++) {
		int i = order[k];
		float d = i - QA.pos, ad = fminf(fabsf(d), 2.2f);
		float sc = 1 - 0.24f * fminf(ad, 1) - 0.06f * fmaxf(0, ad - 1);
		float w = 120 * sc, h = 138 * sc, cx = BOT_W / 2 + d * QA_SP;
		float x = cx - w / 2, y = cy - h / 2;
		if (x > BOT_W || x + w < 0)
			continue;
		float al = a * (1 - 0.32f * fminf(ad, 1.6f));
		bool sel = fabsf(d) < 0.5f;
		u32 c = qa_col(i);
		if (sel) {
			draw_rrect(x - 3, y - 3, w + 6, h + 6, 21, col_alpha(c, 0.22f * a));
			draw_rrect(x, y + 4, w, h, 18, col_alpha(T.shadow, al));
		}
		draw_rrect(x, y, w, h, 18, col_alpha(sel ? col_lerp(T.surface2, c, 0.16f) : T.surface2, al));
		if (sel)
			draw_rrect_outline(x, y, w, h, 18, 1.5f, col_alpha(c, 0.9f * a));
		float ir = 23 * sc;
		draw_circle(cx, y + h * 0.34f, ir, col_alpha(c, 0.18f * al));
		draw_icon(QA_IC[i], cx, y + h * 0.34f, 22 * sc, col_alpha(c, al));
		const char *lab, *sb;
		qa_card_text(i, &lab, &sb);
		float ls = (sel ? 0.52f : 0.44f) * fmaxf(sc, 0.8f);
		text_draw_center_clip(lab, cx, y + h * 0.58f, w - 8, ls, col_alpha(T.text, al));
		if (sel) { // el subtítulo, en dos líneas, solo en la del centro
			char l1[64];
			const char *nl = strchr(sb, '\n');
			snprintf(l1, sizeof l1, "%.*s", nl ? (int)(nl - sb) : (int)strlen(sb), sb);
			text_draw_center_clip(l1, cx, y + h * 0.58f + 18, w - 10, 0.36f, col_alpha(T.text2, al));
			if (nl)
				text_draw_center_clip(nl + 1, cx, y + h * 0.58f + 18 + text_lineh(0.36f), w - 10, 0.36f, col_alpha(T.text2, al));
		}
		if (g_touch.up && !g_touch.drag && g_touch.sx >= x && g_touch.sx < x + w && g_touch.sy >= y && g_touch.sy < y + h && hit < 0)
			hit = i;
	}
	// puntos de posición y ayuda
	float dy = cy + 76;
	float dw = QA_N * 9 + 8, dx = BOT_W / 2 - dw / 2;
	for (int i = 0; i < QA_N; i++) {
		bool on = i == QA.sel;
		draw_rrect(dx, dy, on ? 14 : 5, 5, 2.5f, col_alpha(on ? qa_col(i) : T.divider, a));
		dx += on ? 18 : 9;
	}
	char h1[48], h2[64]; // a los lados del botón central, que tapa el medio
	snprintf(h1, sizeof h1, "%s  L R", GLYPH_DPAD);
	snprintf(h2, sizeof h2, "%s %s   %s %s", GLYPH_A, _("Abrir"), GLYPH_B, _("Cerrar"));
	text_draw_center(h1, BOT_W / 4 - 6, dy + 12, 0.38f, col_alpha(T.text3, a));
	text_draw_center(h2, BOT_W * 3 / 4 + 6, dy + 12, 0.38f, col_alpha(T.text3, a));
	if (!QA.open)
		return;
	bool fresh = QA.just;
	QA.just = false;
	if (fresh) {
		g_kdown = 0;
		g_touch.up = false;
		return;
	}
	// arrastrar de lado: el carrusel sigue al dedo y al soltar se queda en la más cercana
	if (g_touch.held && g_touch.sy < bottom) {
		if (!QA.drag && abs(g_touch.x - g_touch.sx) > 6) {
			QA.drag = true;
			QA.drag_pos = QA.pos;
		}
		if (QA.drag)
			QA.pos = clampf(QA.drag_pos - (g_touch.x - g_touch.sx) / QA_SP, -0.4f, QA_N - 0.6f);
	} else if (QA.drag) {
		QA.drag = false;
		QA.sel = (int)clampf(roundf(QA.pos), 0, QA_N - 1);
	}
	if (g_krepeat & (KEY_RIGHT | KEY_R))
		QA.sel = QA.sel + 1 < QA_N ? QA.sel + 1 : QA.sel;
	if (g_krepeat & (KEY_LEFT | KEY_L))
		QA.sel = QA.sel > 0 ? QA.sel - 1 : 0;
	if (g_kdown & KEY_A)
		run = QA.sel;
	if (hit >= 0) { // tocar la del centro la abre; una de los lados, la trae al centro
		if (hit == QA.sel)
			run = hit;
		else
			QA.sel = hit;
	}
	// el botón central (que ahora es una X) también la cierra
	float fx = BOT_W / 2, fy = bottom + 8;
	bool fab = tap(fx - 25, fy - 25, 50, SCR_H - (fy - 25));
	if (run >= 0)
		qa_run(run);
	else if ((g_kdown & KEY_B) || fab || (g_touch.up && !g_touch.drag && hit < 0 && (g_touch.sy < cy - 80 || (g_touch.sy >= bottom && !fab))))
		QA.open = false;
	g_kdown &= ~(KEY_A | KEY_B | KEY_L | KEY_R);
	g_krepeat &= ~(KEY_LEFT | KEY_RIGHT | KEY_L | KEY_R);
	g_touch.up = false;
}

static void qa_card_text(int i, const char **lab, const char **sb)
{
	switch (i) {
	case QA_CLOUD:
		*lab = _("Nube");
		*sb = cl_connected() ? _("Archivos de\nOneDrive") : _("Conecta tu\nOneDrive");
		break;
	case QA_CONNECT:
		*lab = _("Conectar");
		*sb = V.srv_off ? _("Servidor\napagado") : _("Móvil o PC\ncon QR y PIN");
		break;
	case QA_SCAN: *lab = _("Escanear QR"), *sb = _("Con la cámara,\ncomo en FBI"); break;
	case QA_LINK: *lab = _("Descargar"), *sb = _("Escribir\nun enlace"); break;
	default: *lab = _("Navegador"), *sb = _("Páginas web\nligeras"); break;
	}
}

static const char *dl_dest_dir(const char *url);

// ---- Compartir: las consolas (vinculadas y las que se ven en la red) y lo que ofrece esta. Dentro de una vinculada,
// sus apps: A la pide. Lo que piden las demás se pregunta con un diálogo, en cualquier aplicación (sh_prompts)
static struct {
	int view, level;       // view 0 consolas, 1 mis apps; level 1: las apps de una consola
	List l[3];
	SharePeer peers[SHARE_PEERS];
	int np, order[SHARE_PEERS];
	ShareItem *items;      // de la consola abierta o las propias
	int ni, cat_peer;
	bool loading, failed;
	Texture tex[SHARE_ITEMS];
	u32 rev, tex_rev;
	bool om, om_just;
	float om_anim;
	int om_sel, om_i;
	bool want_ip;
	int pair_seen;         // (para avisar una vez al terminar de vincular)
} SH;

static void sh_refresh(void)
{
	u32 r = share_rev();
	if (r == SH.rev && SH.items)
		return;
	SH.rev = r;
	SH.np = share_peers(SH.peers, SHARE_PEERS);
	int k = 0; // primero las vinculadas, luego las que se ven
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < SH.np; i++)
			if (SH.peers[i].paired == !pass && (SH.peers[i].paired || SH.peers[i].online))
				SH.order[k++] = i;
	SH.np = k;
	if (!SH.items && !(SH.items = malloc(sizeof(ShareItem) * SHARE_ITEMS)))
		return;
	if (SH.level == 1)
		SH.ni = share_catalog(SH.items, SHARE_ITEMS, &SH.cat_peer, &SH.loading, &SH.failed);
	else if (SH.view == 1)
		SH.ni = share_mine(SH.items, SHARE_ITEMS), SH.loading = SH.failed = false;
	else
		SH.ni = 0;
	for (int i = 0; i < SHARE_ITEMS; i++) // los iconos, otra vez
		tex_free(&SH.tex[i]);
	for (int i = 0; i < SH.ni; i++)
		if (SH.items[i].has_icon)
			tex_from_rgb565(&SH.tex[i], SH.items[i].icon, 48, 48);
}

// lo que preguntan otras consolas: un diálogo (si no hay otro abierto ni nada delicado en marcha)
static void sh_prompts(void)
{
	// armando el instalador de una app para otra consola: se avisa al empezar y si falla (esté donde esté)
	static int bst;
	char bn[200], bw[48];
	u64 bd, bt;
	int b = share_build_state(bn, sizeof bn, bw, sizeof bw, &bd, &bt);
	if (b != bst) {
		if (b == 1) {
			char m[160];
			snprintf(m, sizeof m, _("Preparando «%s» para %s"), bn, bw);
			toast_icon(m, IC_SEND, T.accent);
		} else if (b == -1) {
			char m[260];
			snprintf(m, sizeof m, _("No se pudo preparar %s"), bn);
			toast_icon(m, IC_CLOSE, T.danger);
		}
		bst = b;
	}
	SharePrompt sp;
	if (g_dialog.open || guard_on() || !share_prompt(&sp))
		return;
	char t[120], m[300], sz[24];
	U.dlg_arg = sp.rid;
	if (sp.kind == SP_PAIR) {
		snprintf(t, sizeof t, _("«%s» quiere vincularse"), sp.who);
		snprintf(m, sizeof m, _("Comprueba que en su pantalla aparece el código %s. Una vez vinculadas, cada consola podrá ver las apps homebrew de la otra y solicitarlas."), sp.code);
		dialog_open(DLG_SHARE, t, m, _("Vincular"), false);
	} else {
		util_fmt_size(sz, sizeof sz, sp.size);
		snprintf(t, sizeof t, _("«%s» solicita una app"), sp.who);
		snprintf(m, sizeof m, _("Solicita «%s» (%s). Si aceptas, se enviará por la red Wi-Fi."), sp.item, sz);
		dialog_open(DLG_SHARE, t, m, _("Enviar"), false);
	}
}

static void sh_kind_text(const ShareItem *it, char *out, size_t n)
{
	char sz[24];
	util_fmt_size(sz, sizeof sz, it->size);
	const char *st = NULL;
	switch (it->st) {
	case SHR_WAIT: st = _("Solicitud enviada. Pendiente de aceptación."); break;
	case SHR_PREP: st = _("Solicitud aceptada. La otra consola está preparando el instalador."); break;
	case SHR_DOWNLOADING: st = it->kind == SK_3DSX ? _("Recibiendo. Se guardará en /3ds.") : _("Recibiendo. Al terminar, podrás instalarla."); break;
	case SHR_ASK: st = _("Recibida. Elige si instalarla."); break;
	case SHR_INSTALLING: st = _("Instalando…"); break;
	case SHR_DONE: st = it->kind == SK_3DSX ? _("Recibida y guardada en /3ds.") : _("Instalada correctamente."); break;
	case SHR_INSTFAIL: st = _("Recibida, pero no se pudo instalar. El archivo está en /Downloads."); break;
	case SHR_KEPT: st = _("Recibida y guardada en /Downloads sin instalar."); break;
	case SHR_CANCELED: st = _("Recepción cancelada."); break;
	case SHR_REFUSED: st = _("La otra consola rechazó la solicitud."); break;
	case SHR_FAIL: st = _("No se pudo completar la solicitud."); break;
	}
	if (st)
		snprintf(out, n, "%s", st);
	else
		snprintf(out, n, "%s%s%s · %s%s", it->author, it->author[0] ? " · " : "", it->kind == SK_TITLE ? _("Instalada") : it->kind == SK_CIA ? "CIA" : "3DSX", sz,
		         it->hidden ? _(" · No se comparte") : "");
}

static void sh_top(void)
{
	sh_refresh();
	float y0 = BODY_Y, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
	const float X = CARD_X + 16, W = CARD_W - 32;
	int pp;
	char code[8];
	int pst = share_pair_state(&pp, code, sizeof code);
	if (pst == 1) { // vinculando: el código, a lo grande
		text_draw_center(_("Vinculando"), TOP_W / 2, y0 + 16, TS_TITLE, T.text);
		text_draw_center(code, TOP_W / 2, y0 + 44, 1.6f, T.accent);
		wrap_center(_("Comprueba que la otra consola muestra este mismo código y acepta la vinculación desde ella."), TOP_W / 2, y0 + 110, W, TS_SUB, 3, T.text2);
		text_draw_center(_("B Cancelar"), TOP_W / 2, y0 + h - 24, TS_SUB, T.text3);
		return;
	}
	char bn[200], bw[48];
	u64 bd = 0, bt = 1;
	if (share_build_state(bn, sizeof bn, bw, sizeof bw, &bd, &bt) == 1) { // armando el instalador que se va a enviar
		draw_circle(X + 30, y0 + 40, 26, col_alpha(ap_col(AP_SHARE), 0.16f));
		draw_icon(IC_SEND, X + 30, y0 + 40, 24, ap_col(AP_SHARE));
		float tx = X + 70, tw = X + W - tx;
		char t[160];
		snprintf(t, sizeof t, _("Preparando «%s»"), bn);
		text_draw_clip(t, tx, y0 + 18, tw, TS_TITLE, T.text);
		snprintf(t, sizeof t, _("Destino: %s. Se está generando el instalador a partir de la app instalada; después se enviará."), bw);
		text_wrap_lines(t, tw, TS_SUB, 3, tx, y0 + 20 + text_lineh(TS_TITLE) + 4, T.text2, true);
		float f = bt ? (float)((double)bd / bt) : 0, by = y0 + h - 40;
		draw_rrect(X, by, W, 8, 4, T.surface2);
		draw_rrect(X, by, fmaxf(8, W * fminf(1, f)), 8, 4, T.accent);
		char pc[16];
		snprintf(pc, sizeof pc, "%d %%", (int)(f * 100));
		text_draw_right(pc, X + W, by + 12, TS_SUB, T.text2);
		return;
	}
	List *l = &SH.l[SH.level ? 2 : SH.view];
	const ShareItem *it = (SH.level == 1 || SH.view == 1) && l->sel >= 0 && l->sel < SH.ni ? &SH.items[l->sel] : NULL;
	if (it) { // la app elegida, en grande
		draw_rrect(X, y0 + 14, 80, 80, 18, T.bg);
		if (it->has_icon && SH.tex[l->sel].ok)
			draw_texture(&SH.tex[l->sel], X + 8, y0 + 22, 64, 64, 1);
		else
			draw_icon(IC_GEM, X + 40, y0 + 54, 30, T.ok);
		float tx = X + 96, tw = X + W - tx;
		text_draw_clip(it->name, tx, y0 + 18, tw, TS_TITLE, T.text);
		char sub[200];
		sh_kind_text(it, sub, sizeof sub);
		text_wrap_lines(sub, tw, TS_SUB, 3, tx, y0 + 20 + text_lineh(TS_TITLE) + 4, T.text2, true);
		const char *hint = SH.level == 1 ? _("A Solicitar esta app") : it->hidden ? _("A Volver a compartir") : _("A Dejar de compartir");
		text_draw_right(hint, X + W, y0 + h - 10 - text_lineh(TS_SUB), TS_SUB, T.text3);
		return;
	}
	draw_circle(X + 30, y0 + 40, 26, col_alpha(ap_col(AP_SHARE), 0.16f));
	draw_icon(IC_SEND, X + 30, y0 + 40, 24, ap_col(AP_SHARE));
	float tx = X + 70, tw = X + W - tx, y = y0 + 18;
	text_draw_clip(_("Compartir homebrew"), tx, y, tw, TS_TITLE, T.text);
	y += text_lineh(TS_TITLE) + 4;
	char me[96];
	snprintf(me, sizeof me, _("Esta consola: %s"), share_my_name());
	text_draw_clip(me, tx, y, tw, TS_SUB, T.text2);
	y = fmaxf(y + text_lineh(TS_SUB), y0 + 76) + 8;
	draw_rect(X, y, W, 1, T.divider);
	y += 10;
	y += text_wrap_lines(_("Ambas consolas deben estar en la misma red Wi-Fi y con Cloud3DS abierto. Selecciona la otra consola para vincularlas (solo la primera vez); después podrás ver sus apps y solicitarlas."),
	                     W, TS_SUB, 4, X, y, T.text2, true) *
	         text_lineh(TS_SUB) + 6;
	text_wrap_lines(_("Se comparten las apps homebrew instaladas: al aceptar una solicitud, se genera su instalador y se envía. No se comparten juegos de cartucho, de la eShop ni del sistema."), W, TS_SUB, 3, X, y,
	                T.text3, true);
}

static void sh_bottom(void)
{
	sh_refresh();
	int pp;
	char code[8];
	int pst = share_pair_state(&pp, code, sizeof code);
	if (pst != SH.pair_seen) { // terminó de vincular: se avisa y, si salió bien, a sus apps
		if (SH.pair_seen == 1 && pst == 2) {
			toast_icon(_("Vinculadas"), IC_LINK, T.ok);
			SH.level = 1, SH.l[2].sel = 0;
			share_open_catalog(pp);
		} else if (SH.pair_seen == 1 && pst == 3)
			toast_icon(_("La otra consola no aceptó la vinculación"), IC_CLOSE, T.danger);
		else if (SH.pair_seen == 1 && pst == 4)
			toast_icon(_("No se pudo conectar con esa consola"), IC_CLOSE, T.danger);
		SH.pair_seen = pst;
	}
	bool sheet = SH.om || g_dialog.open || pst == 1;
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	if (pst == 1 && (g_kdown & KEY_B))
		share_pair_cancel();
	if (sheet) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	const SharePeer *cp = NULL;
	for (int i = 0; i < SH.np; i++)
		if (SH.order[i] == SH.cat_peer)
			cp = &SH.peers[SH.order[i]];
	// cabecera
	if (SH.level == 0) {
		tab_header_w("", false, 0);
		const char *opts[2] = {_("Consolas"), _("Mis apps")};
		int r = segmented(40, 5, 150, 22, opts, 2, SH.view, false);
		if (hdr_hit(40, 5, 75, 22))
			r = 0;
		if (hdr_hit(115, 5, 75, 22))
			r = 1;
		if (g_kdown & (KEY_L | KEY_R | KEY_ZL | KEY_ZR))
			r = !SH.view;
		if (r >= 0 && r != SH.view)
			SH.view = r, SH.rev = 0;
		if (SH.view == 0) { // buscar otra vez y añadir por la IP
			float bx[2] = {BOT_W - 56, BOT_W - 24};
			static const IconId bi[2] = {IC_REFRESH, IC_PLUS};
			for (int k = 0; k < 2; k++) {
				draw_circle(bx[k], 16, 12, k ? T.accent : touch_in(bx[k] - 14, 2, 28, 28) ? T.surface2 : T.surface);
				draw_icon(bi[k], bx[k], 16, 13, k ? 0xFFFFFFFF : T.text2);
				if (hdr_hit(bx[k] - 13, 3, 26, 26)) {
					if (k)
						SH.want_ip = true;
					else {
						share_discover();
						toast_icon(_("Buscando consolas en la Wi-Fi…"), IC_REFRESH, T.accent);
					}
				}
			}
			if (g_kdown & KEY_X)
				SH.want_ip = true;
		}
		if ((g_kdown & KEY_B) && !g_dialog.open) {
			hm_show();
			return;
		}
	} else {
		tab_header_w(cp ? cp->name : "", true, 220);
		float bx = BOT_W - 24;
		draw_circle(bx, 16, 12, touch_in(bx - 14, 2, 28, 28) ? T.surface2 : T.surface);
		draw_icon(IC_REFRESH, bx, 16, 13, T.text2);
		if (hdr_hit(bx - 13, 3, 26, 26))
			share_open_catalog(SH.cat_peer);
		if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
			SH.level = 0, SH.rev = 0;
	}
	sh_refresh();
	bool apps = SH.level == 1 || SH.view == 1;
	List *l = &SH.l[SH.level ? 2 : SH.view];
	int cnt = apps ? SH.ni : SH.np;
	const float RH = 44;
	list_update(l, cnt, RH);
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	if (!cnt) {
		if (SH.level == 1)
			empty_state(IC_SEND, SH.loading ? _("Obteniendo su lista de apps…") : SH.failed ? _("No responde") : _("Sin apps disponibles"),
			            SH.loading ? _("Espera un momento.") : SH.failed ? _("Comprueba que la otra consola tiene Cloud3DS abierto y está en la misma Wi-Fi.") : _("No tiene apps homebrew disponibles para compartir."));
		else if (SH.view == 1)
			empty_state(IC_GEM, _("Nada que compartir"), _("Aquí aparecen las apps homebrew instaladas en esta consola."));
		else
			empty_state(IC_DEVICES, _("Buscando consolas…"), _("Abre Cloud3DS en la otra consola, conectada a la misma red Wi-Fi. Si no aparece, añádela por su IP con «+» (se muestra en su app Conectar)."));
	}
	for (int i = 0; i < cnt; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		bool was = l->sel == i;
		bool t = row(l, i, y, RH);
		if (!U.buttons && l->sel == i)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		char t1[96], t2[200];
		if (apps) {
			const ShareItem *it = &SH.items[i];
			draw_rrect(12, y + 5, 34, 34, 9, T.bg);
			if (it->has_icon && SH.tex[i].ok)
				draw_texture(&SH.tex[i], 13, y + 6, 32, 32, it->hidden ? 0.4f : 1);
			else
				draw_icon(IC_GEM, 29, y + RH / 2, 16, T.ok);
			util_strlcpy(t1, it->name, sizeof t1);
			sh_kind_text(it, t2, sizeof t2);
		} else {
			const SharePeer *p = &SH.peers[SH.order[i]];
			draw_circle(29, y + RH / 2, 16, col_alpha(p->paired ? T.accent : T.text3, 0.16f));
			draw_icon(IC_DEVICES, 29, y + RH / 2, 16, p->paired ? T.accent : T.text2);
			if (p->online)
				draw_circle(41, y + RH / 2 + 10, 4, T.ok);
			util_strlcpy(t1, p->name[0] ? p->name : "3DS", sizeof t1);
			if (p->paired)
				snprintf(t2, sizeof t2, "%s%s", p->online ? _("Vinculada · Disponible") : _("Vinculada · No disponible ahora"), p->trusted ? _(" · De confianza") : "");
			else
				snprintf(t2, sizeof t2, "%s", _("Disponible · Pulsa A para vincular"));
		}
		g_clip_ell = l->sel != i;
		text_draw_clip(t1, 54, y + 6, BOT_W - 94, TS_BODY, T.text);
		text_draw_clip(t2, 54, y + 24, BOT_W - 94, TS_SUB, T.text3);
		g_clip_ell = false;
		if (l->sel == i) {
			draw_circle(BOT_W - 26, y + RH / 2, 11, col_alpha(T.accent, 0.3f));
			draw_icon(IC_DOTS, BOT_W - 26, y + RH / 2, 14, T.accent);
		}
		bool open = (t && was) || activate(l, i);
		if (t && was && g_touch.px > BOT_W - 44)
			SH.om = SH.om_just = true, SH.om_sel = 0, SH.om_i = i;
		else if (open) {
			if (SH.level == 1) {
				share_request(SH.cat_peer, SH.items[i].id);
				toast_icon(_("Solicitud enviada. La otra consola debe aceptarla."), IC_SEND, T.accent);
			} else if (SH.view == 1)
				share_hide(SH.items[i].path, !SH.items[i].hidden);
			else {
				const SharePeer *p = &SH.peers[SH.order[i]];
				if (p->paired) {
					SH.level = 1, SH.l[2].sel = 0, SH.l[2].off = SH.l[2].target = 0;
					share_open_catalog(SH.order[i]);
				} else {
					char c[8];
					share_pair(SH.order[i], c, sizeof c);
					SH.pair_seen = 1;
				}
			}
		}
	}
	gfx_clip_off();
	if ((g_kdown & KEY_Y) && cnt && !SH.om)
		SH.om = SH.om_just = true, SH.om_sel = 0, SH.om_i = l->sel;
	if (sheet)
		g_kdown = kd, g_krepeat = kr, g_touch = tc;
	if (pst == 1) // vinculando: tapa la lista (el código está arriba)
		empty_state(IC_LINK, _("Vinculando…"), _("Acepta la vinculación en la otra consola. Pulsa B para cancelar."));
	// opciones: de una consola o de una app propia
	IconId ic[4];
	const char *lab[4];
	u32 col[4];
	int act[4], n = 0;
	char title[96] = "";
	const SharePeer *op = !apps && SH.om_i >= 0 && SH.om_i < SH.np ? &SH.peers[SH.order[SH.om_i]] : NULL;
	if (op) {
		util_strlcpy(title, op->name, sizeof title);
		if (op->paired) {
			ic[n] = IC_GEM, lab[n] = _("Ver sus apps"), col[n] = T.text, act[n++] = 0;
			ic[n] = op->trusted ? IC_CLOSE : IC_SHIELD, lab[n] = op->trusted ? _("Quitar de las consolas de confianza") : _("Marcar como de confianza (enviar sin confirmar)"), col[n] = T.text, act[n++] = 1;
			ic[n] = IC_LOGOUT, lab[n] = _("Desvincular"), col[n] = T.danger, act[n++] = 2;
		} else
			ic[n] = IC_LINK, lab[n] = _("Vincular"), col[n] = T.accent, act[n++] = 3;
	} else if (apps && SH.om_i >= 0 && SH.om_i < SH.ni) {
		const ShareItem *it = &SH.items[SH.om_i];
		util_strlcpy(title, it->name, sizeof title);
		if (SH.level == 1)
			ic[n] = IC_DOWNLOAD, lab[n] = _("Solicitar esta app"), col[n] = T.ok, act[n++] = 4;
		else
			ic[n] = it->hidden ? IC_CHECK : IC_CLOSE, lab[n] = it->hidden ? _("Volver a compartir") : _("No compartir esta app"), col[n] = T.text, act[n++] = 5;
	}
	if (!n)
		SH.om = false;
	int r = sheet_list(&SH.om, &SH.om_just, &SH.om_anim, &SH.om_sel, title, ic, lab, col, n);
	if (r >= 0) {
		int a = act[r], pi = op ? SH.order[SH.om_i] : -1;
		if (a == 0)
			SH.level = 1, SH.l[2].sel = 0, share_open_catalog(pi);
		else if (a == 1)
			share_set_trusted(pi, !op->trusted);
		else if (a == 2)
			share_unpair(pi);
		else if (a == 3) {
			char c[8];
			share_pair(pi, c, sizeof c);
			SH.pair_seen = 1;
		} else if (a == 4) {
			share_request(SH.cat_peer, SH.items[SH.om_i].id);
			toast_icon(_("Solicitud enviada. La otra consola debe aceptarla."), IC_SEND, T.accent);
		} else if (a == 5)
			share_hide(SH.items[SH.om_i].path, !SH.items[SH.om_i].hidden);
	}
}

/* ------------------------------------------------------------ inicio por aplicaciones
 * La pantalla de inicio: cada función de Cloud3DS es una aplicación con su icono. Arriba, la elegida en grande con
 * lo más útil de ella (espacio libre, la cuenta de la nube, el QR para conectar…); abajo, la cuadrícula (dos tamaños,
 * como en el menú de la consola). Al abrir una, se muestra la pantalla de siempre de esa función; el botón central de
 * la barra (o SELECT) vuelve aquí. Lo que se abre encima (la nube, el navegador, el escáner, la música) vuelve solo. */
#define HM_CFG APP_DIR "/inicio.cfg"
#define HM_BAR 30
static struct {
	bool open, small, loaded;
	int sel;
	float page_pos;  // página a la vista (animada hacia la de la elegida)
	float fade;      // entrada al abrir el inicio
	bool dragging;
	float drag_dx;
} HM = {.open = true};

static const IconId AP_IC[AP_N] = {IC_FOLDER, IC_GAMEPAD, IC_LAPTOP, IC_DOWNLOAD, IC_CLOUD, IC_GLOBE, IC_SPEAKER, IC_BOOK, IC_SEND, IC_GEAR};
static u32 ap_col(int i)
{
	static const u32 hex[AP_N] = {0x3B9CFF, 0xFF5E8A, 0x2EC47A, 0xF5B02E, 0x7A5CFF, 0x2BC0D6, 0xFF7A59, 0xC27BF0, 0x21C7A8, 0x9AA6BA};
	return col_hex(hex[i]);
}
static const char *ap_name(int i)
{
	switch (i) {
	case AP_FILES: return _("Archivos");
	case AP_TITLES: return _("Títulos");
	case AP_CONNECT: return _("Conectar");
	case AP_DL: return _("Descargas");
	case AP_CLOUD: return _("Nube");
	case AP_WEB: return _("Navegador");
	case AP_MUSIC: return _("Música");
	case AP_READER: return _("Lector");
	case AP_SHARE: return _("Compartir");
	default: return _("Ajustes");
	}
}
static const char *ap_desc(int i)
{
	switch (i) {
	case AP_FILES: return _("La tarjeta SD: carpetas, capturas, fotos, CIA, homebrew, partidas y papelera.");
	case AP_TITLES: return _("Juegos y aplicaciones instalados: cuánto ocupan, actualizaciones, DLC, datos extra, copias de la partida y desinstalación.");
	case AP_CONNECT: return _("Transfiere archivos con el móvil o el ordenador desde el navegador, con un código QR. Incluye la actividad y los dispositivos vinculados.");
	case AP_DL: return _("Descarga desde un enlace o un código QR, y abre lo descargado (carpeta /Downloads). Los CIA se instalan durante la descarga.");
	case AP_CLOUD:
		return cl_connected() ? _("Tu OneDrive en la consola: explora, sube, descarga y organiza tus archivos. Las capturas nuevas se suben automáticamente.")
		                      : _("Conecta tu cuenta de OneDrive con un código QR, sin escribir contraseñas en la consola.");
	case AP_WEB: return _("Navegador web con JavaScript, favoritos e historial.");
	case AP_MUSIC: return _("Toda la música de la SD, por canciones, artistas y álbumes. Sigue sonando en segundo plano.");
	case AP_READER: return _("PDF, manga y cómics (CBZ), EPUB, DOCX y TXT de la SD, con biblioteca. Se lee en horizontal o con la consola de lado, como un libro.");
	case AP_SHARE: return _("Comparte apps homebrew instaladas con otra consola de la misma red Wi-Fi: vincúlalas una vez, consulta sus apps y solicítalas.");
	default: return _("Conexión, seguridad, idioma, tema y actualizaciones.");
	}
}

static void hm_load(void)
{
	if (HM.loaded)
		return;
	HM.loaded = true;
	FILE *f = fopen(HM_CFG, "r");
	char ln[64];
	while (f && fgets(ln, sizeof ln, f)) {
		if (!strncmp(ln, "small=", 6))
			HM.small = atoi(ln + 6) != 0;
		else if (!strncmp(ln, "sel=", 4))
			HM.sel = clampf(atoi(ln + 4), 0, AP_N - 1);
	}
	if (f)
		fclose(f);
}
static void hm_save(void)
{
	FILE *f = fopen(HM_CFG, "w");
	if (f)
		fprintf(f, "small=%d\nsel=%d\n", HM.small ? 1 : 0, HM.sel), fclose(f);
}

static bool hm_is_open(void) { return HM.open && !U.ins_pend; }
static int hm_cols(void) { return HM.small ? 6 : 5; }
static int hm_rows(void) { return HM.small ? 3 : 2; }
static int hm_per_page(void) { return hm_cols() * hm_rows(); }
static int hm_pages(void) { return (AP_N + hm_per_page() - 1) / hm_per_page(); }

static void hm_show(void)
{
	hm_load();
	// las hojas abiertas en una aplicación se cierran: si no, al abrir otra seguirían tapando su entrada sin verse
	HB.focus = -1;
	XS.open = QA.open = DVS.open = UL.open = FM.open = PM.open = TY.open = VS.open = MA.open = MC.open = TS.open = HS.open = ZS.open = DS.open = TT.open = false;
	HM.open = true;
	HM.fade = 0;
	HM.page_pos = HM.sel / hm_per_page();
	U.nav_eat = true;
}

// abrir una aplicación: lo que tiene pantalla propia encima del inicio (vuelve sola aquí) o la suya
static void hm_launch(int i)
{
	WHERE("abrir una app");
	hm_save();
	if (U.app == AP_FILES && U.tab == TAB_FILES && (F.path[0] == '/' || F.trash))
		util_strlcpy(FILES_LAST, F.path, sizeof FILES_LAST);
	switch (i) {
	case AP_CLOUD: cl_open(); return;
	case AP_WEB: U.want_web = true; return;
	case AP_MUSIC:
		mlib_init(); // (la biblioteca: busca lo nuevo en segundo plano)
		break;
	case AP_SHARE:
		share_discover();
		break;
	case AP_READER:
		rlib_init(); // (la biblioteca: busca lo nuevo en segundo plano)
		rv_cfg_load();
		break;
	default: break;
	}
	HM.open = false;
	U.page = PAGE_MAIN;
	U.buttons = false;
	U.app = i;
	U.app_root[0] = 0;
	if (i == AP_CONNECT) { // el QR arriba; abajo, la actividad y los dispositivos
		if (V.srv_off)
			srv_set(true);
		if (U.tab != TAB_DEVICES)
			U.tab = TAB_ACTIVITY;
		return;
	}
	if (i == AP_SETTINGS) {
		U.tab = TAB_SETTINGS;
		return;
	}
	U.tab = TAB_FILES;
	F.ret_from[0] = F.ret_to[0] = 0; // (cada app con su propio «atrás»)
	if (i == AP_DL) { // sus tarjetas; la última abre la carpeta de descargas con lo de Archivos
		DLV = DLQ = false;
		DLV_n = -1;
		fsx_mkdirs(DL_DIR);
		util_strlcpy(U.app_root, DL_DIR, sizeof U.app_root);
		return;
	}
	if (i == AP_TITLES) {
		f_load("@titles");
		util_strlcpy(U.app_root, "@titles", sizeof U.app_root);
		return;
	}
	if (i == AP_MUSIC || i == AP_SHARE || i == AP_READER) // su pantalla (ml_bottom, sh_bottom, rl_bottom)
		return;
	// Archivos: en su última carpeta (no en la de otra app: Descargas, Títulos o sus copias)
	f_load(FILES_LAST[0] ? FILES_LAST : "/");
}

// un icono de aplicación: cuadrado redondeado con el color de la app; elegido, con su borde y su brillo
static void hm_tile(int i, float x, float y, float s, bool sel, bool label, float a)
{
	u32 c = ap_col(i);
	float r = s * 0.30f;
	if (sel) {
		draw_rrect(x - 4, y - 4, s + 8, s + 8, r + 4, col_alpha(T.accent, 0.22f * a));
		draw_rrect(x - 2, y - 2, s + 4, s + 4, r + 2, col_alpha(T.accent, a));
	}
	draw_rrect(x, y, s, s, r, col_alpha(col_lerp(T.bg, c, 0.20f), a)); // (su color, sobre el fondo)
	draw_rrect(x + 2, y + 2, s - 4, s * 0.5f, r - 2, col_alpha(c, 0.07f * a)); // un poco más de luz arriba
	draw_rrect_outline(x, y, s, s, r, 1, col_alpha(c, 0.45f * a));
	draw_icon(AP_IC[i], x + s / 2, y + s / 2, s * 0.46f, col_alpha(c, a));
	// aviso: lo que está en marcha (Actividad) o lo que falta por hacer
	int badge = 0;
	if (i == AP_CONNECT || i == AP_DL)
		for (int k = 0; k < MAX_XFERS; k++)
			badge += V.xfers[k].st == XS_RUNNING && (i == AP_CONNECT) == (V.xfers[k].kind != XF_FETCH);
	if (badge > 0) {
		char b[8];
		snprintf(b, sizeof b, "%d", badge > 9 ? 9 : badge);
		float bw = fmaxf(15, text_width(b, 0.36f) + 8), bx = x + s - bw + 5, by = y - 5;
		u32 bc = T.danger;
		draw_rrect(bx - 2, by - 2, bw + 4, 19, 9.5f, col_alpha(T.bg, a));
		draw_rrect(bx, by, bw, 15, 7.5f, col_alpha(bc, a));
		text_draw_center(b, bx + bw / 2, by + 7.5f - text_lineh(0.36f) / 2, 0.36f, col_alpha(0xFFFFFFFF, a));
	}
	if (label) {
		const char *n = ap_name(i);
		float lw = fminf(text_width(n, 0.36f), s + 22);
		text_draw_clip(n, x + s / 2 - lw / 2, y + s + 4, s + 22, 0.36f, col_alpha(sel ? T.text : T.text2, a));
	}
}

// arriba: la aplicación elegida, en grande, con lo más útil de ella
static void hm_top(void)
{
	draw_rect(0, 0, TOP_W, SCR_H, T.bg);
	draw_cloud_glow(80, 104, 52, 0.30f);
	draw_header();
	HM.fade = approach(HM.fade, 1, 8);
	float a = ease_out_cubic(HM.fade);
	if (MU.open) { // la música abierta desde aquí: su pantalla de siempre
		music_top(1);
		toast_draw_top();
		return;
	}
	int i = HM.sel;
	u32 c = ap_col(i);
	const float IX = 24, IY = 46, IS = 112;
	float yo = (1 - a) * 8;
	draw_rrect(IX - 6, IY - 6 + yo, IS + 12, IS + 12, IS * 0.30f + 6, col_alpha(c, 0.10f * a));
	draw_rrect(IX, IY + yo, IS, IS, IS * 0.30f, col_alpha(col_lerp(T.bg, c, 0.22f), a));
	draw_rrect(IX + 3, IY + 3 + yo, IS - 6, IS * 0.5f, IS * 0.30f - 3, col_alpha(c, 0.08f * a));
	draw_rrect_outline(IX, IY + yo, IS, IS, IS * 0.30f, 1.5f, col_alpha(c, 0.5f * a));
	draw_icon(AP_IC[i], IX + IS / 2, IY + IS / 2 + yo, 56, col_alpha(c, a));
	float tx = IX + IS + 22, tw = TOP_W - tx - 20, y = IY + 2 + yo;
	text_draw_clip(ap_name(i), tx, y, tw, 0.9f, col_alpha(T.text, a));
	y += text_lineh(0.9f) + 4;
	y += text_wrap_lines(ap_desc(i), tw, TS_SUB, 4, tx, y, col_alpha(T.text2, a), true) * text_lineh(TS_SUB) + 10;
	// lo de cada una
	char s1[160] = "", s2[96] = "";
	u32 c1 = T.text3;
	if (i == AP_FILES) {
		u64 fr = 0, tot = 0;
		if (sd_space(&fr, &tot) && tot) {
			float f = 1 - (float)((double)fr / tot);
			draw_rrect(tx, y, tw, 6, 3, col_alpha(T.surface2, a));
			draw_grad_h(tx, y, fmaxf(6, tw * f), 6, col_alpha(T.accent, a), col_alpha(T.accent2, a));
			char fs[24], ts[24];
			util_fmt_size(fs, sizeof fs, fr), util_fmt_size(ts, sizeof ts, tot);
			snprintf(s1, sizeof s1, _("%s libres de %s"), fs, ts);
			y += 12;
		}
	} else if (i == AP_TITLES) {
		if (TI_n) {
			u64 tt = 0;
			for (int k = 0; k < TI_n; k++)
				tt += TI[k].size + TI[k].upd_size + TI[k].dlc_size;
			char z[24];
			util_fmt_size(z, sizeof z, tt);
			snprintf(s1, sizeof s1, TI_n == 1 ? _("1 título · %s") : _("%d títulos · %s"), TI_n, z);
		}
	} else if (i == AP_CLOUD) {
		CloudInfo ci;
		cloud_get(&ci);
		if (ci.st == CLS_ON)
			snprintf(s1, sizeof s1, "%s", ci.email[0] ? ci.email : ci.name), c1 = T.ok;
		else
			util_strlcpy(s1, _("Sin conectar"), sizeof s1);
	} else if (i == AP_READER) { // lo último que se leyó
		const RBook *b;
		int n = rlib_lock(&b), best = -1;
		for (int k = 0; k < n; k++)
			if (b[k].last && (best < 0 || b[k].last > b[best].last))
				best = k;
		if (best >= 0) {
			char tb[128];
			snprintf(s1, sizeof s1, _("Continuar: %s"), rb_title(&b[best], tb, sizeof tb)), c1 = T.text2;
			if (b[best].pages > 0)
				snprintf(s2, sizeof s2, _("Página %d de %d"), b[best].page + 1, b[best].pages);
		}
		rlib_unlock();
	} else if (i == AP_MUSIC) {
		if (MU.s.st == MU_PLAY || MU.s.st == MU_PAUSE)
			snprintf(s1, sizeof s1, "%s %s", MU.s.st == MU_PLAY ? _("Reproduciendo:") : _("En pausa:"), MU.s.title), c1 = T.text2;
	} else if (i == AP_CONNECT) {
		int on = online_count(), run = 0;
		for (int k = 0; k < MAX_XFERS; k++)
			run += V.xfers[k].st == XS_RUNNING && V.xfers[k].kind != XF_FETCH;
		if (run)
			snprintf(s1, sizeof s1, run == 1 ? _("1 transferencia en curso") : _("%d transferencias en curso"), run), c1 = T.accent;
		else if (on)
			snprintf(s1, sizeof s1, on == 1 ? _("%d dispositivo conectado") : _("%d dispositivos conectados"), on), c1 = T.ok;
		else
			util_strlcpy(s1, _("Ningún dispositivo conectado"), sizeof s1);
		if (V.ip && !V.srv_off) {
			char url[64];
			url_text(url, sizeof url, false);
			snprintf(s2, sizeof s2, "%s · PIN %s", url, V.pin);
		}
	} else if (i == AP_DL) {
		int run = 0;
		for (int k = 0; k < MAX_XFERS; k++)
			run += V.xfers[k].st == XS_RUNNING && V.xfers[k].kind == XF_FETCH;
		if (run)
			snprintf(s1, sizeof s1, run == 1 ? _("1 descarga en curso") : _("%d descargas en curso"), run), c1 = T.accent;
		const char *cl = clip_text();
		if (!strncasecmp(cl, "http", 4))
			util_strlcpy(s2, _("Hay un enlace en el portapapeles"), sizeof s2);
	}
	if (s1[0])
		text_draw_clip(s1, tx, y, tw, TS_SUB, col_alpha(c1, a)), y += text_lineh(TS_SUB) + 2;
	if (s2[0])
		text_draw_clip(s2, tx, y, tw, TS_SUB, col_alpha(T.accent, a));
	char hint[120];
	if (hm_pages() > 1)
		snprintf(hint, sizeof hint, _("%s Abrir · %s Cambiar de página · %s Tamaño de los iconos"), GLYPH_A, "L R", GLYPH_Y);
	else
		snprintf(hint, sizeof hint, _("%s Abrir · %s Tamaño de los iconos · START Salir"), GLYPH_A, GLYPH_Y);
	text_draw_center(hint, TOP_W / 2, SCR_H - 18, 0.36f, T.text3);
	toast_draw_top();
}

// abajo: la barra (dirección, PIN y el tamaño de los iconos) y la cuadrícula, por páginas
static void hm_bottom(void)
{
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	bool modal = g_dialog.open || MU.open || QRS.open || U.ins_pend;
	// barra
	draw_rect(0, 0, BOT_W, HM_BAR, T.surface);
	draw_rect(0, HM_BAR - 1, BOT_W, 1, T.divider);
	float x = 8;
	char ip[48] = "", pin[24];
	if (V.ip && !V.srv_off)
		url_text(ip, sizeof ip, false);
	else
		util_strlcpy(ip, V.srv_off ? _("Servidor apagado") : _("Sin red"), sizeof ip);
	float w = text_width(ip, 0.38f) + 28;
	draw_rrect(x, 5, w, 20, 10, T.surface2);
	draw_circle(x + 11, 15, 3.5f, V.ip && !V.srv_off ? T.ok : T.text3);
	text_draw(ip, x + 20, 15 - text_lineh(0.38f) / 2, 0.38f, T.text2);
	x += w + 6;
	if (V.ip && !V.srv_off) {
		snprintf(pin, sizeof pin, "PIN %s", V.pin);
		w = text_width(pin, 0.38f) + 18;
		draw_rrect(x, 5, w, 20, 10, T.surface2);
		text_draw(pin, x + 9, 15 - text_lineh(0.38f) / 2, 0.38f, T.text);
	}
	// el portapapeles: archivos (tocar lleva a Archivos, a pegarlos) o un enlace
	if (FP.mode || CC.mode || clip_text()[0]) {
		char cb[40];
		if (FP.mode || CC.mode)
			snprintf(cb, sizeof cb, "%d", CC.mode ? CC.n : FP.n > 1 ? FP.n : 1);
		else
			util_strlcpy(cb, _("Enlace"), sizeof cb);
		float cw2 = text_width(cb, 0.38f) + 30, cx2 = BOT_W - 70 - cw2;
		if (cx2 > x + 4) {
			draw_rrect(cx2, 5, cw2, 20, 10, col_alpha(T.accent2, 0.22f));
			draw_icon(IC_DUP, cx2 + 12, 15, 11, T.accent2);
			text_draw(cb, cx2 + 22, 15 - text_lineh(0.38f) / 2, 0.38f, T.accent2);
			if (!modal && tap(cx2, 0, cw2, HM_BAR)) {
				if (FP.mode || CC.mode)
					hm_launch(AP_FILES);
				else
					toast_icon(clip_text(), IC_DUP, T.accent2);
			}
		}
	}
	// el tamaño de los iconos: grandes o pequeños
	for (int k = 0; k < 2; k++) {
		float bx = BOT_W - 64 + k * 30, on = (k == 1) == HM.small;
		draw_rrect(bx, 5, 26, 20, 10, on ? col_alpha(T.accent, 0.25f) : T.surface2);
		u32 ic = on ? T.accent : T.text3;
		if (k == 0)
			draw_rrect_outline(bx + 8, 10, 10, 10, 2.5f, 1.5f, ic);
		else
			for (int q = 0; q < 4; q++)
				draw_rrect(bx + 8 + (q % 2) * 6, 9 + (q / 2) * 6, 4.5f, 4.5f, 1.2f, ic);
		if (!modal && tap(bx, 0, 28, HM_BAR) && HM.small != (k == 1)) {
			HM.small = k == 1;
			HM.page_pos = HM.sel / hm_per_page();
			hm_save();
		}
	}
	// la cuadrícula
	int cols = hm_cols(), rows = hm_rows(), per = hm_per_page(), pages = hm_pages();
	float s = HM.small ? 38 : 50, cw = (BOT_W - 12) / (float)cols, gy = HM.small ? HM_BAR + 14 : HM_BAR + 16, rh = HM.small ? 54 : 80;
	if (!modal) {
		int sel = HM.sel;
		if (g_krepeat & KEY_RIGHT) sel = sel % cols == cols - 1 || sel + 1 >= AP_N ? (sel / per + 1 < pages ? (sel / per + 1) * per + (sel % per) / cols * cols : sel) : sel + 1;
		if (g_krepeat & KEY_LEFT) sel = sel % cols == 0 ? (sel / per > 0 ? (sel / per - 1) * per + (sel % per) / cols * cols + cols - 1 : sel) : sel - 1;
		if (g_krepeat & KEY_DOWN) sel = (sel % per) / cols < rows - 1 && sel + cols < AP_N ? sel + cols : sel;
		if (g_krepeat & KEY_UP) sel = (sel % per) / cols > 0 ? sel - cols : sel;
		if (g_kdown & (KEY_R | KEY_ZR)) sel = (sel / per + 1 < pages) ? (sel / per + 1) * per : sel;
		if (g_kdown & (KEY_L | KEY_ZL)) sel = sel / per > 0 ? (sel / per - 1) * per : sel;
		if (sel >= AP_N) sel = AP_N - 1;
		if (sel != HM.sel)
			HM.sel = sel, U.buttons = true;
		if (g_kdown & KEY_Y) {
			HM.small = !HM.small;
			hm_save();
		}
		// deslizar para cambiar de página
		if (g_touch.held && g_touch.drag && g_touch.sy > HM_BAR)
			HM.dragging = true, HM.drag_dx += g_touch.x - g_touch.px;
		if (HM.dragging && !g_touch.held) {
			int pg = HM.sel / per;
			if (HM.drag_dx < -40 && pg + 1 < pages)
				HM.sel = (pg + 1) * per;
			else if (HM.drag_dx > 40 && pg > 0)
				HM.sel = (pg - 1) * per;
			HM.dragging = false, HM.drag_dx = 0;
		}
	}
	int page = HM.sel / per;
	HM.page_pos = approach(HM.page_pos, page, 14);
	float shift = (HM.dragging ? HM.drag_dx : 0) - HM.page_pos * BOT_W;
	for (int i = 0; i < AP_N; i++) {
		int p = i / per, k = i % per;
		float tx = 6 + p * BOT_W + (k % cols) * cw + (cw - s) / 2 + shift, ty = gy + (k / cols) * rh;
		if (tx + s < -10 || tx > BOT_W + 10)
			continue;
		hm_tile(i, tx, ty, s, i == HM.sel, !HM.small, 1);
		if (!modal && !g_touch.drag && tap(tx - 4, ty - 4, s + 8, s + (HM.small ? 8 : 22))) {
			if (HM.sel == i || HM.small) // (como en el menú de la consola: tocar la elegida la abre)
				hm_launch(i);
			HM.sel = i, U.buttons = false;
		}
	}
	// pie: páginas o el nombre de la elegida (con los iconos pequeños no se ven los nombres)
	if (HM.small)
		text_draw_center(ap_name(HM.sel), BOT_W / 2, SCR_H - 22, TS_BODY, T.text);
	else if (pages > 1) {
		float dx = BOT_W / 2 - (pages * 10 + 6) / 2.0f;
		for (int p = 0; p < pages; p++) {
			bool on = p == page;
			draw_rrect(dx, SCR_H - 14, on ? 16 : 6, 6, 3, on ? T.accent : T.divider);
			dx += on ? 22 : 12;
		}
	}
	if (!modal && (g_kdown & KEY_A))
		hm_launch(HM.sel);
	music_sheet();
	qrs_draw();
	install_card();
	un_card();
	dialog_draw();
}

// ================================================================ Lector
// La biblioteca (rlib: lo que hay en la SD y por dónde se va) y la lectura (rd_*: MuPDF en su hilo). Al leer, el
// documento ocupa las dos pantallas: en horizontal, una página repartida entre las dos (de arriba abajo); como un
// libro (la consola de lado), una página en cada pantalla, giradas. B cierra; X, las opciones; Y, girar.

enum { RLT_RECENT, RLT_ALL, RLT_FOLDERS };
static struct {
	bool init;
	int tab, level;          // level 1: dentro de una carpeta (pestaña Carpetas)
	char folder[256];
	List l[2];
	u32 built;
	int built_tab, built_level;
	RBook *lib;              // copia de la biblioteca
	int nlib;
	int *ix, n;              // los documentos a la vista
	char (*dirs)[256];       // las carpetas (pestaña Carpetas)
	int *dcnt, nd;
	bool om, om_just;        // opciones de un documento
	float om_anim;
	int om_sel, om_i;
	bool hm, hm_just;        // opciones de la cabecera
	float hm_anim;
	int hm_sel;
	char cov_for[256];       // portada de lo elegido
	Texture cov;
	u64 sel_t;
	char sel_path[256];
} RL;

static struct {
	bool open;
	char path[256], name[128];
	int ret_app;             // a dónde se vuelve al cerrar
	int st, page, pages;     // st: RD_*
	bool reflow, first;      // first: aún no se colocó en la página guardada
	int saved_page, saved_pages;
	int orient;              // 0 horizontal; 1 libro con la pantalla de arriba a la izquierda; 2 al revés
	bool rtl;                // libro: de derecha a izquierda (manga)
	bool autorot;            // libro: girar solo hacia el lado al que se gire la consola (acelerómetro)
	float ax;                // (el acelerómetro, suavizado)
	u64 ax_t;                // desde cuándo apunta al otro lado
	int zoom;                // horizontal: 0 al ancho de la pantalla de abajo; 1 de la de arriba; 2 y 3, más
	int em;                  // letra de lo que se recoloca (puntos)
	u32 lgen;
	bool relaying;
	float scroll, cx;        // horizontal: desplazamiento (vertical) y centro (horizontal) en la página
	Texture tex[2];          // [0] arriba (o la página en horizontal), [1] abajo
	RdReq have[2];
	bool has[2];
	u64 hud_t, save_t;
	bool dirty;
	bool menu, menu_just;
	float menu_anim;
	int menu_sel;
	bool want_page;          // «Ir a la página»: teclado, fuera del dibujo
} RV = {.em = 11, .autorot = true};

#define RV_CFG APP_DIR "/lector.cfg"
static void rv_cfg_load(void)
{
	static bool done;
	if (done)
		return;
	done = true;
	FILE *f = fopen(RV_CFG, "r");
	char ln[64];
	while (f && fgets(ln, sizeof ln, f)) {
		if (!strncmp(ln, "orient=", 7))
			RV.orient = clampf(atoi(ln + 7), 0, 2);
		else if (!strncmp(ln, "rtl=", 4))
			RV.rtl = atoi(ln + 4) != 0;
		else if (!strncmp(ln, "zoom=", 5))
			RV.zoom = clampf(atoi(ln + 5), 0, 3);
		else if (!strncmp(ln, "em=", 3))
			RV.em = clampf(atoi(ln + 3), 8, 20);
		else if (!strncmp(ln, "auto=", 5))
			RV.autorot = atoi(ln + 5) != 0;
	}
	if (f)
		fclose(f);
}
static void rv_cfg_save(void)
{
	FILE *f = fopen(RV_CFG, "w");
	if (f)
		fprintf(f, "orient=%d\nrtl=%d\nzoom=%d\nem=%d\nauto=%d\n", RV.orient, RV.rtl ? 1 : 0, RV.zoom, RV.em, RV.autorot ? 1 : 0), fclose(f);
}

static const char *rd_kind_name(int k)
{
	static const char *const N[RK_N] = {"PDF", "CBZ", "EPUB", "DOCX", "TXT"};
	return k >= 0 && k < RK_N ? N[k] : "";
}
static IconId rd_kind_icon(int k) { return k == RK_CBZ ? IC_IMAGE : k == RK_EPUB ? IC_BOOK : k == RK_TXT ? IC_NOTE : IC_FILE; }
static u32 rd_kind_col(int k)
{
	return k == RK_PDF ? 0xFF4444EF : k == RK_CBZ ? 0xFFF755A8 : k == RK_EPUB ? T.accent : k == RK_DOCX ? 0xFFF6823B : T.text2;
}

// el nombre que se enseña: el título del documento o el del archivo sin extensión
static const char *rb_title(const RBook *b, char *buf, size_t n)
{
	if (b->title[0])
		return b->title;
	const char *s = strrchr(b->path, '/');
	util_strlcpy(buf, s ? s + 1 : b->path, n);
	char *dot = strrchr(buf, '.');
	if (dot && dot != buf)
		*dot = 0;
	return buf;
}

// orden natural: «Tomo 2» antes que «Tomo 10»
static int nat_cmp(const char *a, const char *b)
{
	while (*a && *b) {
		if (isdigit((unsigned char)*a) && isdigit((unsigned char)*b)) {
			while (*a == '0')
				a++;
			while (*b == '0')
				b++;
			const char *ea = a, *eb = b;
			while (isdigit((unsigned char)*ea))
				ea++;
			while (isdigit((unsigned char)*eb))
				eb++;
			if (ea - a != eb - b)
				return (int)((ea - a) - (eb - b));
			int c = strncmp(a, b, ea - a);
			if (c)
				return c;
			a = ea, b = eb;
			continue;
		}
		int ca = tolower((unsigned char)*a), cb = tolower((unsigned char)*b);
		if (ca != cb)
			return ca - cb;
		a++, b++;
	}
	return (unsigned char)*a - (unsigned char)*b;
}

static const RBook *RL_SORT;
static int rl_cmp_title(const void *x, const void *y)
{
	char a[128], b[128];
	return nat_cmp(rb_title(&RL_SORT[*(const int *)x], a, sizeof a), rb_title(&RL_SORT[*(const int *)y], b, sizeof b));
}
static int rl_cmp_last(const void *x, const void *y)
{
	const RBook *a = &RL_SORT[*(const int *)x], *b = &RL_SORT[*(const int *)y];
	return a->last < b->last ? 1 : a->last > b->last ? -1 : 0;
}
static int rl_cmp_dir(const void *x, const void *y)
{
	return nat_cmp((const char *)x, (const char *)y);
}

static void rl_parent(const char *path, char *out, size_t n)
{
	util_strlcpy(out, path, n);
	char *sl = strrchr(out, '/');
	if (sl && sl != out)
		*sl = 0;
	else if (sl)
		sl[1] = 0;
}

// rehace la vista (de la biblioteca, si cambió, y de la pestaña)
static void rl_build(void)
{
	u32 rev = rlib_rev();
	if (RL.lib && rev == RL.built && RL.built_tab == RL.tab && RL.built_level == RL.level)
		return;
	if (!RL.lib && !(RL.lib = malloc(sizeof(RBook) * RLIB_MAX)))
		return;
	if (!RL.ix)
		RL.ix = malloc(sizeof(int) * RLIB_MAX);
	if (!RL.dirs)
		RL.dirs = malloc(sizeof(RL.dirs[0]) * 400), RL.dcnt = malloc(sizeof(int) * 400);
	if (!RL.ix || !RL.dirs || !RL.dcnt)
		return;
	const RBook *b;
	int n = rlib_lock(&b);
	if (n)
		memcpy(RL.lib, b, sizeof(RBook) * n);
	rlib_unlock();
	RL.nlib = n;
	RL.built = rev, RL.built_tab = RL.tab, RL.built_level = RL.level;
	RL.n = RL.nd = 0;
	RL_SORT = RL.lib;
	if (RL.tab == RLT_FOLDERS && RL.level == 0) {
		for (int i = 0; i < n; i++) {
			char d[256];
			rl_parent(RL.lib[i].path, d, sizeof d);
			int k = 0;
			while (k < RL.nd && strcmp(RL.dirs[k], d))
				k++;
			if (k == RL.nd && RL.nd < 400)
				util_strlcpy(RL.dirs[RL.nd], d, sizeof RL.dirs[0]), RL.dcnt[RL.nd++] = 0;
			if (k < RL.nd)
				RL.dcnt[k]++;
		}
		// (por nombre, con su cuenta al lado: se ordenan los pares)
		for (int i = 1; i < RL.nd; i++)
			for (int j = i; j > 0 && rl_cmp_dir(RL.dirs[j - 1], RL.dirs[j]) > 0; j--) {
				char t[256];
				util_strlcpy(t, RL.dirs[j], sizeof t);
				util_strlcpy(RL.dirs[j], RL.dirs[j - 1], sizeof t);
				util_strlcpy(RL.dirs[j - 1], t, sizeof t);
				int c = RL.dcnt[j];
				RL.dcnt[j] = RL.dcnt[j - 1], RL.dcnt[j - 1] = c;
			}
		return;
	}
	for (int i = 0; i < n; i++) {
		if (RL.tab == RLT_RECENT && !RL.lib[i].last)
			continue;
		if (RL.tab == RLT_FOLDERS) {
			char d[256];
			rl_parent(RL.lib[i].path, d, sizeof d);
			if (strcmp(d, RL.folder))
				continue;
		}
		RL.ix[RL.n++] = i;
	}
	qsort(RL.ix, RL.n, sizeof(int), RL.tab == RLT_RECENT ? rl_cmp_last : rl_cmp_title);
}

static const RBook *rl_selected(void)
{
	if (RL.tab == RLT_FOLDERS && RL.level == 0)
		return NULL;
	List *l = &RL.l[RL.level];
	return l->sel >= 0 && l->sel < RL.n ? &RL.lib[RL.ix[l->sel]] : NULL;
}

static void rv_open(const char *path, int ret_app);

// la portada de lo elegido (la pide cuando la selección se queda quieta un momento)
static void rl_cover(const RBook *b)
{
	if (!b) {
		RL.sel_path[0] = 0;
		return;
	}
	if (strcmp(RL.sel_path, b->path)) {
		util_strlcpy(RL.sel_path, b->path, sizeof RL.sel_path);
		RL.sel_t = util_ms();
	}
	if (!strcmp(RL.cov_for, b->path) || util_ms() - RL.sel_t < 250)
		return;
	u16 *px;
	int w, h;
	if (rd_cover(b->path, 112, 158, &px, &w, &h)) {
		tex_from_rgb565(&RL.cov, px, w, h);
		free(px);
		util_strlcpy(RL.cov_for, b->path, sizeof RL.cov_for);
	}
}

static void rl_top(void)
{
	rl_build();
	float y0 = BODY_Y, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, T.shadow);
	draw_rrect(CARD_X, y0, CARD_W, h, 16, T.surface);
	const float X = CARD_X + 14, W = CARD_W - 28;
	const RBook *b = rl_selected();
	rl_cover(b);
	if (b) { // el documento elegido: portada, nombre y por dónde va
		const float CW = 112, CH = 158;
		float cx = X, cy = y0 + 12;
		draw_rrect(cx, cy, CW, CH, 8, T.surface2);
		if (!strcmp(RL.cov_for, b->path) && RL.cov.ok) {
			float s = fminf(CW / RL.cov.w, CH / RL.cov.h), w = RL.cov.w * s, hh = RL.cov.h * s;
			draw_texture(&RL.cov, cx + (CW - w) / 2, cy + (CH - hh) / 2, w, hh, 1);
		} else
			draw_icon(rd_kind_icon(b->kind), cx + CW / 2, cy + CH / 2, 36, rd_kind_col(b->kind));
		float tx = cx + CW + 14, tw = X + W - tx, y = cy + 2;
		char tb[128];
		y += text_wrap_lines(rb_title(b, tb, sizeof tb), tw, TS_TITLE, 2, tx, y, T.text, true) * text_lineh(TS_TITLE) + 4;
		char v[200], sz[24];
		util_fmt_size(sz, sizeof sz, b->size);
		snprintf(v, sizeof v, "%s · %s", rd_kind_name(b->kind), sz);
		y = info_row(tx, y, tw, _("Tipo"), v, T.text2, 1);
		char d[256];
		rl_parent(b->path, d, sizeof d);
		y = info_row(tx, y, tw, _("Carpeta"), d, T.text2, 1);
		if (b->last && b->pages > 0) {
			snprintf(v, sizeof v, _("Página %d de %d"), b->page + 1, b->pages);
			y = info_row(tx, y, tw, _("Progreso"), v, T.text2, 1);
			f_fmt_date(sz, sizeof sz, b->last);
			y = info_row(tx, y, tw, _("Última lectura"), sz, T.text3, 1);
			float pw = tw, py = y + 4;
			draw_rrect(tx, py, pw, 6, 3, T.surface2);
			draw_rrect(tx, py, fmaxf(6, pw * (b->page + 1) / (float)b->pages), 6, 3, ap_col(AP_READER));
		} else
			y = info_row(tx, y, tw, _("Progreso"), _("Sin empezar"), T.text3, 1);
		char hint[96];
		snprintf(hint, sizeof hint, _("%s Leer · %s Opciones"), GLYPH_A, GLYPH_Y);
		text_draw_right(hint, X + W, y0 + h - 10 - text_lineh(TS_SUB), TS_SUB, T.text2);
		return;
	}
	draw_circle(X + 30, y0 + 40, 26, col_alpha(ap_col(AP_READER), 0.16f));
	draw_icon(IC_BOOK, X + 30, y0 + 40, 24, ap_col(AP_READER));
	float tx = X + 70, tw = X + W - tx, y = y0 + 18;
	text_draw_clip(_("Biblioteca de lectura"), tx, y, tw, TS_TITLE, T.text);
	y += text_lineh(TS_TITLE) + 4;
	char l1[96];
	snprintf(l1, sizeof l1, RL.nlib == 1 ? _("1 documento") : _("%d documentos"), RL.nlib);
	text_draw_clip(l1, tx, y, tw, TS_SUB, T.text2);
	y += text_lineh(TS_SUB) + 4;
	int found;
	if (rlib_scanning(&found)) {
		char l2[96];
		snprintf(l2, sizeof l2, _("Buscando documentos en la SD… %d"), found);
		text_draw_clip(l2, tx, y, tw, TS_SUB, T.accent);
	}
	y = y0 + 84;
	draw_rect(X, y, W, 1, T.divider);
	y += 10;
	y += text_wrap_lines(_("PDF, manga y cómics (CBZ), EPUB, DOCX y TXT. Se puede leer en horizontal o con la consola de lado, como un libro; el manga, de derecha a izquierda."),
	                     W, TS_SUB, 3, X, y, T.text2, true) *
	         text_lineh(TS_SUB) + 6;
	text_wrap_lines(_("Los TXT se buscan solo en las carpetas de lectura (/Books, /Manga, /Documents, /Downloads…). Cualquier otro se abre desde Archivos."), W, TS_SUB, 3, X, y,
	                T.text3, true);
}

static void rl_bottom(void)
{
	if (!RL.init) {
		RL.init = true;
		rlib_init();
		rv_cfg_load();
	}
	rl_build();
	if (!RL.lib)
		return;
	bool sheet = RL.om || RL.hm || g_dialog.open;
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	if (sheet) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	if (RL.level == 0) {
		tab_header_w("", false, 0);
		const char *opts[3] = {_("Recientes"), _("Todos"), _("Carpetas")};
		float sw = (BOT_W - 40 - 36) / 3;
		int r = segmented(40, 5, sw * 3, 22, opts, 3, RL.tab, false);
		for (int k = 0; k < 3; k++)
			if (hdr_hit(40 + k * sw, 5, sw, 22))
				r = k;
		if ((g_kdown & (KEY_L | KEY_ZL)) && RL.tab > 0)
			r = RL.tab - 1;
		if ((g_kdown & (KEY_R | KEY_ZR)) && RL.tab < 2)
			r = RL.tab + 1;
		if (r >= 0 && r != RL.tab)
			RL.tab = r, RL.l[0].sel = 0, RL.l[0].off = RL.l[0].target = 0;
		if ((g_kdown & KEY_B) && !g_dialog.open) {
			hm_show();
			return;
		}
	} else {
		const char *nm = strrchr(RL.folder, '/');
		tab_header_w(nm && nm[1] ? nm + 1 : RL.folder, true, 230);
		if (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))
			RL.level = 0;
	}
	float hx = BOT_W - 24, hy = 16;
	draw_circle(hx, hy, 12, touch_in(hx - 14, hy - 14, 28, 28) ? T.surface2 : T.surface);
	draw_icon(IC_DOTS, hx, hy, 14, T.text2);
	if (hdr_hit(hx - 13, hy - 13, 26, 26))
		RL.hm = RL.hm_just = true, RL.hm_sel = 0;
	rl_build();
	List *l = &RL.l[RL.level];
	bool dirs = RL.tab == RLT_FOLDERS && RL.level == 0;
	int cnt = dirs ? RL.nd : RL.n;
	const float RH = 40;
	list_update(l, cnt, RH);
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	if (!cnt) {
		int found;
		bool sc = rlib_scanning(&found);
		if (RL.tab == RLT_RECENT && RL.nlib)
			empty_state(IC_CLOCK, _("Aún no has leído nada"), _("Lo que abras aparecerá aquí, con la página por la que vas. Elige un documento en «Todos» o «Carpetas»."));
		else
			empty_state(IC_BOOK, sc ? _("Buscando documentos…") : _("No hay documentos"),
			            sc ? _("Se analiza la tarjeta SD. La primera vez puede tardar unos minutos.")
			               : _("Copia PDF, CBZ, EPUB, DOCX o TXT a la SD (por ejemplo, a /Books o a /Manga) y aparecerán aquí."));
	}
	for (int i = 0; i < cnt; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		bool was = l->sel == i;
		bool t = row(l, i, y, RH);
		if (!U.buttons && l->sel == i)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		char t1[256], t2[200];
		IconId ic;
		u32 col;
		const RBook *b = NULL;
		if (dirs) {
			const char *nm = strrchr(RL.dirs[i], '/');
			util_strlcpy(t1, nm && nm[1] ? nm + 1 : RL.dirs[i], sizeof t1);
			snprintf(t2, sizeof t2, RL.dcnt[i] == 1 ? _("1 documento · %s") : _("%d documentos · %s"), RL.dcnt[i], RL.dirs[i]);
			ic = IC_FOLDER, col = T.warn;
		} else {
			b = &RL.lib[RL.ix[i]];
			char tb[128], sz[24];
			util_strlcpy(t1, rb_title(b, tb, sizeof tb), sizeof t1);
			util_fmt_size(sz, sizeof sz, b->size);
			if (b->last && b->pages > 0)
				snprintf(t2, sizeof t2, _("%s · %s · página %d de %d"), rd_kind_name(b->kind), sz, b->page + 1, b->pages);
			else
				snprintf(t2, sizeof t2, "%s · %s", rd_kind_name(b->kind), sz);
			ic = rd_kind_icon(b->kind), col = rd_kind_col(b->kind);
		}
		draw_rrect(14, y + (RH - 26) / 2, 26, 26, 8, col_alpha(col, 0.16f));
		draw_icon(ic, 27, y + RH / 2, 14, col);
		g_clip_ell = l->sel != i;
		text_draw_clip(t1, 50, y + 5, BOT_W - 90, TS_BODY, T.text);
		text_draw_clip(t2, 50, y + 22, BOT_W - 90, TS_SUB, T.text3);
		g_clip_ell = false;
		if (b && b->last && b->pages > 0) { // lo leído, en una línea fina debajo
			float pw = BOT_W - 90 - 50;
			draw_rect(50, y + RH - 4, pw, 2, T.surface2);
			draw_rect(50, y + RH - 4, pw * (b->page + 1) / (float)b->pages, 2, ap_col(AP_READER));
		}
		if (!dirs && l->sel == i) {
			draw_circle(BOT_W - 26, y + RH / 2, 11, col_alpha(T.accent, 0.3f));
			draw_icon(IC_DOTS, BOT_W - 26, y + RH / 2, 14, T.accent);
		}
		bool open = (t && was) || activate(l, i);
		if (!dirs && t && was && g_touch.px > BOT_W - 44)
			RL.om = RL.om_just = true, RL.om_sel = 0, RL.om_i = i;
		else if (open) {
			if (dirs) {
				util_strlcpy(RL.folder, RL.dirs[i], sizeof RL.folder);
				RL.level = 1, RL.l[1].sel = 0, RL.l[1].off = RL.l[1].target = 0;
			} else
				rv_open(b->path, AP_READER);
		}
	}
	gfx_clip_off();
	if ((g_kdown & KEY_Y) && cnt && !dirs && !RL.om)
		RL.om = RL.om_just = true, RL.om_sel = 0, RL.om_i = l->sel;
	if (sheet)
		g_kdown = kd, g_krepeat = kr, g_touch = tc;
	{ // opciones de un documento
		const RBook *b = !dirs && RL.om_i >= 0 && RL.om_i < RL.n ? &RL.lib[RL.ix[RL.om_i]] : NULL;
		IconId ic[4];
		const char *lab[4];
		u32 col[4];
		int act[4], n = 0;
		char title[128] = "";
		if (b) {
			char tb[128];
			util_strlcpy(title, rb_title(b, tb, sizeof tb), sizeof title);
			ic[n] = IC_BOOK, lab[n] = b->last ? _("Seguir leyendo") : _("Leer"), col[n] = T.accent, act[n++] = 0;
			if (b->last)
				ic[n] = IC_UNDO, lab[n] = _("Leer desde el principio"), col[n] = T.text, act[n++] = 1;
			ic[n] = IC_FOLDER, lab[n] = _("Ir a la carpeta"), col[n] = T.text, act[n++] = 2;
		}
		if (!n)
			RL.om = false;
		int r = sheet_list(&RL.om, &RL.om_just, &RL.om_anim, &RL.om_sel, title, ic, lab, col, n);
		if (r >= 0 && b) {
			char path[256];
			util_strlcpy(path, b->path, sizeof path);
			if (act[r] == 1)
				rlib_progress(path, 0, b->pages, NULL);
			if (act[r] <= 1)
				rv_open(path, AP_READER);
			else {
				char d[256];
				rl_parent(path, d, sizeof d);
				hm_launch(AP_FILES);
				f_load(d);
			}
		}
	}
	{ // opciones de la cabecera
		IconId ic[2] = {IC_REFRESH, IC_FOLDER};
		const char *lab[2] = {_("Volver a buscar documentos"), rlib_where() == RLS_ALL ? _("Buscar solo en las carpetas de lectura") : _("Buscar en toda la SD")};
		u32 col[2] = {T.text, T.text};
		int r = sheet_list(&RL.hm, &RL.hm_just, &RL.hm_anim, &RL.hm_sel, _("Lector"), ic, lab, col, 2);
		if (r == 0) {
			rlib_scan();
			toast_icon(_("Buscando documentos en la SD…"), IC_REFRESH, T.accent);
		} else if (r == 1) {
			rlib_set_where(rlib_where() == RLS_ALL ? RLS_BOOKS : RLS_ALL);
			toast_icon(rlib_where() == RLS_ALL ? _("Buscando en toda la SD…") : _("Buscando solo en las carpetas de lectura…"), IC_REFRESH, T.accent);
		}
	}
}

// ---------------------------------------------------------------- lectura

static void rv_drop_tex(void)
{
	for (int s = 0; s < 2; s++)
		tex_free(&RV.tex[s]), RV.has[s] = false;
}

static void rv_save(void)
{
	if (RV.pages > 0 && !RV.first) {
		char t[96];
		rd_title(t, sizeof t);
		rlib_progress(RV.path, RV.page, RV.pages, t);
	}
	RV.dirty = false, RV.save_t = util_ms();
}

static void rv_open(const char *path, int ret_app)
{
	rv_cfg_load();
	if (RV.open)
		rv_save();
	rv_drop_tex();
	util_strlcpy(RV.path, path, sizeof RV.path);
	const char *s = strrchr(path, '/');
	util_strlcpy(RV.name, s ? s + 1 : path, sizeof RV.name);
	RV.ret_app = ret_app;
	RV.saved_pages = 0;
	RV.saved_page = rlib_last_page(path, &RV.saved_pages);
	RV.page = 0, RV.pages = 0, RV.first = true, RV.relaying = false;
	RV.scroll = 0, RV.cx = -1, RV.menu = false, RV.hud_t = util_ms();
	RV.st = RD_OPENING;
	RV.open = rd_open(path);
	if (!RV.open)
		toast_icon(_("No se pudo abrir el documento"), IC_CLOSE, T.danger);
}

static void rv_close(void)
{
	rv_save();
	rv_cfg_save();
	rd_close();
	rv_drop_tex();
	RV.open = false, RV.menu = false;
	if (RV.ret_app == AP_READER)
		RL.built = 0xFFFFFFFF; // (el progreso cambió)
}

static bool rv_book(void) { return RV.orient != 0; }
static int rv_rot(void) { return RV.orient == 1 ? 90 : RV.orient == 2 ? 270 : 0; }
static int rv_zoom_w(void)
{
	static const int Z[4] = {320, 400, 560, 720};
	return RV.reflow ? 320 : Z[RV.zoom];
}

// lo que se recoloca (TXT, DOCX, EPUB): el tamaño de su página depende de cómo se lea
static void rv_relayout(int keep)
{
	if (!RV.reflow)
		return;
	float w = rv_book() ? 240 : 320, h = rv_book() ? 380 : 480;
	RV.lgen = rd_layout(w, h, (float)RV.em, keep);
	RV.relaying = true;
	rv_drop_tex();
}

// qué página va en cada pantalla (en el libro: la izquierda y la derecha según el giro y el sentido)
static int rv_screen_page(int s)
{
	if (!rv_book())
		return s == 0 ? RV.page : -1;
	int first = RV.page, second = RV.page + 1;
	int left = RV.rtl ? second : first, right = RV.rtl ? first : second;
	bool top_left = RV.orient == 1;
	int p = s == 0 ? (top_left ? left : right) : (top_left ? right : left);
	return p < RV.pages ? p : -1;
}

static void rv_req(int s, int page, RdReq *r)
{
	r->page = page, r->rot = rv_rot();
	if (rv_book())
		r->tw = s == 0 ? TOP_W : BOT_W, r->th = SCR_H, r->fit = 0;
	else
		r->tw = rv_zoom_w(), r->th = 1024, r->fit = 1;
}

static bool rv_req_eq(const RdReq *a, const RdReq *b)
{
	return a->page == b->page && a->tw == b->tw && a->th == b->th && a->rot == b->rot && a->fit == b->fit;
}

// cada vuelta: el estado del documento y lo que hay que tener dibujado
// libro: con el acelerómetro, hacia el lado al que se gira la consola (la gravedad, a lo largo del eje X de la
// consola: negativa con la pantalla de arriba a la izquierda). Medio segundo apuntando al otro lado para cambiar
static void rv_autorot(void)
{
	static bool on;
	if (!rv_book() || !RV.autorot) {
		if (on)
			HIDUSER_DisableAccelerometer(), on = false;
		return;
	}
	if (!on) {
		HIDUSER_EnableAccelerometer();
		on = true;
		RV.ax = 0, RV.ax_t = 0;
		return;
	}
	accelVector a;
	hidAccelRead(&a);
	RV.ax = RV.ax * 0.85f + a.x * 0.15f;
	int want = RV.ax < -220 ? 1 : RV.ax > 220 ? 2 : 0;
	if (!want || want == RV.orient) {
		RV.ax_t = 0;
		return;
	}
	if (!RV.ax_t)
		RV.ax_t = util_ms();
	else if (util_ms() - RV.ax_t > 500) {
		RV.orient = want, RV.ax_t = 0;
		RV.hud_t = util_ms();
	}
}

static void rv_poll(void)
{
	rv_autorot();
	int pages;
	bool reflow;
	char err[32];
	int st = rd_state(&pages, &reflow, err, sizeof err);
	RV.st = st;
	if (st != RD_OPEN)
		return;
	if (RV.first && !RV.relaying) { // recién abierto: a la página guardada
		RV.reflow = reflow, RV.pages = pages;
		if (reflow) {
			rv_relayout(0);
			return;
		}
		RV.page = RV.saved_page >= 0 && RV.saved_page < pages ? RV.saved_page : 0;
		RV.first = false;
	}
	if (RV.relaying) {
		int pg, np;
		if (!rd_relaid(RV.lgen, &pg, &np))
			return;
		RV.relaying = false, RV.pages = np;
		if (RV.first && RV.saved_page >= 0 && RV.saved_pages > 0) // (de la última vez: la misma parte del texto)
			pg = (int)((float)RV.saved_page / RV.saved_pages * np + 0.5f);
		RV.page = pg < 0 ? 0 : pg >= np ? np - 1 : pg;
		RV.first = false;
	}
	RV.pages = pages > 0 ? pages : RV.pages;
	// las pantallas
	int ns = rv_book() ? 2 : 1;
	for (int s = 0; s < ns; s++) {
		int p = rv_screen_page(s);
		if (p < 0) {
			if (RV.has[s])
				tex_free(&RV.tex[s]), RV.has[s] = false;
			continue;
		}
		RdReq r;
		rv_req(s, p, &r);
		if (RV.has[s] && rv_req_eq(&RV.have[s], &r))
			continue;
		u16 *px;
		int w, h;
		if (rd_get(&r, &px, &w, &h)) {
			tex_from_rgb565(&RV.tex[s], px, w, h);
			free(px);
			RV.have[s] = r, RV.has[s] = true;
			if (!rv_book() && s == 0) { // página nueva en horizontal: centrada
				if (RV.cx < 0 || RV.tex[0].w <= BOT_W)
					RV.cx = RV.tex[0].w / 2.0f;
			}
		}
	}
	// por adelantado: las siguientes (y la anterior)
	int step = rv_book() ? 2 : 1;
	int pre[3] = {RV.page + step, RV.page + 2 * step, RV.page - step};
	for (int k = 0; k < 3; k++) {
		for (int s = 0; s < (rv_book() ? 2 : 1); s++) {
			int save = RV.page;
			RV.page = pre[k];
			int p = RV.page >= 0 && RV.page < RV.pages ? rv_screen_page(s) : -1;
			RV.page = save;
			if (p < 0)
				continue;
			RdReq r;
			rv_req(s, p, &r);
			rd_prefetch(&r);
		}
	}
	if (RV.dirty && util_ms() - RV.save_t > 60000)
		rv_save();
}

static void rv_go(int page)
{
	if (RV.pages <= 0)
		return;
	if (page < 0)
		page = 0;
	if (page >= RV.pages)
		page = RV.pages - 1;
	if (page == RV.page)
		return;
	bool fwd = page > RV.page;
	RV.page = page, RV.dirty = true, RV.hud_t = util_ms();
	RV.scroll = fwd ? -1e9f : 1e9f; // (al principio de la siguiente o al final de la anterior)
}

// horizontal: una franja de la página, de alto SCR_H, desde «top» (en píxeles de la página), centrada en cx
static void rv_strip(const Texture *t, float sw, float top)
{
	if (!t->ok)
		return;
	float th = t->h, tw = t->w;
	float y0 = fmaxf(0, top), y1 = fminf(th, top + SCR_H);
	if (y1 <= y0)
		return;
	float x0 = tw <= sw ? 0 : fmaxf(0, fminf(tw - sw, RV.cx - sw / 2)), x1 = fminf(tw, x0 + sw);
	float dx = tw <= sw ? (sw - tw) / 2 : 0;
	draw_texture_part((Texture *)t, dx, y0 - top, x1 - x0, y1 - y0, x0 / tw, y0 / th, x1 / tw, y1 / th);
}

static void rv_spinner(float cx, float cy)
{
	float t = osGetTime() / 120.0f;
	for (int i = 0; i < 8; i++) {
		float a = t + i * (float)M_PI / 4;
		draw_circle(cx + 10 * cosf(a), cy + 10 * sinf(a), 1.2f + i * 0.25f, col_alpha(T.text2, 0.2f + i * 0.1f));
	}
}

static void rv_top(void)
{
	rv_poll();
	draw_rect(0, 0, TOP_W, SCR_H, 0xFF202020);
	if (RV.st == RD_ERROR) {
		char e[32];
		rd_state(NULL, NULL, e, sizeof e);
		draw_icon(IC_CLOSE, TOP_W / 2, 90, 30, T.danger);
		text_draw_center(!strcmp(e, "password") ? _("El documento está protegido con contraseña") : _("No se pudo abrir el documento"), TOP_W / 2, 120, TS_TITLE, 0xFFFFFFFF);
		text_draw_center(RV.name, TOP_W / 2, 146, TS_SUB, 0xFFB0B0B0);
		return;
	}
	if (rv_book()) {
		if (RV.has[0]) {
			const Texture *t = &RV.tex[0];
			draw_texture((Texture *)t, (TOP_W - t->w) / 2.0f, (SCR_H - t->h) / 2.0f, t->w, t->h, 1);
		}
	} else if (RV.has[0]) {
		const Texture *t = &RV.tex[0];
		float th = t->h, total = SCR_H * 2;
		float mn = th <= total ? (th - total) / 2 : 0, mx = th <= total ? mn : th - total;
		RV.scroll = fminf(mx, fmaxf(mn, RV.scroll));
		rv_strip(t, TOP_W, RV.scroll);
	}
	// cargando (la primera vez o la página nueva aún no está)
	int p0 = rv_screen_page(0);
	RdReq want;
	rv_req(0, p0, &want);
	bool loading = RV.st == RD_OPENING || RV.relaying || (p0 >= 0 && !(RV.has[0] && rv_req_eq(&RV.have[0], &want)));
	if (loading) {
		if (!RV.has[0]) {
			text_draw_center(RV.st == RD_OPENING ? _("Abriendo…") : RV.relaying ? _("Preparando las páginas…") : _("Cargando la página…"), TOP_W / 2, SCR_H / 2 + 22,
			                 TS_SUB, 0xFFB0B0B0);
			rv_spinner(TOP_W / 2, SCR_H / 2);
		} else {
			draw_rrect(TOP_W - 40, 8, 32, 32, 16, 0xA0000000);
			rv_spinner(TOP_W - 24, 24);
		}
	}
	toast_draw_float();
}

static void rv_menu(void);

static void rv_bottom(void)
{
	draw_rect(0, 0, BOT_W, SCR_H, 0xFF202020);
	if (rv_book()) {
		if (RV.has[1]) {
			const Texture *t = &RV.tex[1];
			draw_texture((Texture *)t, (BOT_W - t->w) / 2.0f, (SCR_H - t->h) / 2.0f, t->w, t->h, 1);
		} else if (rv_screen_page(1) >= 0 && RV.st == RD_OPEN)
			rv_spinner(BOT_W / 2, SCR_H / 2);
	} else if (RV.has[0])
		rv_strip(&RV.tex[0], BOT_W, RV.scroll + SCR_H);
	// número de página, un momento al cambiar
	u64 since = util_ms() - RV.hud_t;
	if (RV.pages > 0 && since < 1800 && !RV.menu) {
		char t[48];
		if (rv_book() && RV.page + 1 < RV.pages)
			snprintf(t, sizeof t, "%d-%d / %d", RV.page + 1, RV.page + 2, RV.pages);
		else
			snprintf(t, sizeof t, "%d / %d", RV.page + 1, RV.pages);
		float a = since < 1400 ? 1 : 1 - (since - 1400) / 400.0f;
		float w = text_width(t, TS_SUB) + 20;
		draw_rrect((BOT_W - w) / 2, SCR_H - 30, w, 22, 11, col_alpha(0xD0000000, a));
		text_draw_center(t, BOT_W / 2, SCR_H - 30 + (22 - text_lineh(TS_SUB)) / 2, TS_SUB, col_alpha(0xFFFFFFFF, a));
	}
	if (RV.menu || g_dialog.open) {
		rv_menu();
		return;
	}
	if (RV.st == RD_ERROR) {
		text_draw_center(_("B Volver"), BOT_W / 2, SCR_H / 2, TS_BODY, 0xFFB0B0B0);
		if ((g_kdown & KEY_B) || g_touch.up)
			rv_close();
		return;
	}
	// botones
	if (g_kdown & KEY_B) {
		int ret = RV.ret_app;
		rv_close();
		if (ret != AP_READER && ret != U.app)
			hm_launch(ret);
		return;
	}
	if (g_kdown & KEY_SELECT) {
		rv_close();
		hm_show();
		return;
	}
	if (g_kdown & (KEY_X | KEY_START)) {
		RV.menu = RV.menu_just = true, RV.menu_sel = 0;
		return;
	}
	if (g_kdown & KEY_Y) { // horizontal ↔ libro (con el giro automático, el lado lo da la consola; si no, de uno a otro)
		RV.orient = RV.autorot ? (RV.orient ? 0 : 1) : (RV.orient + 1) % 3;
		RV.cx = -1;
		rv_drop_tex();
		rv_relayout(RV.page);
		toast_icon(RV.orient == 0 ? _("Lectura en horizontal") : RV.orient == 1 ? _("Lectura como un libro (consola de lado)") : _("Lectura como un libro, al otro lado"),
		           IC_FLIP, T.accent);
		return;
	}
	if (RV.pages <= 0)
		return;
	int step = rv_book() ? 2 : 1;
	u32 next = KEY_R | KEY_ZR | KEY_A, prev = KEY_L | KEY_ZL;
	if (!rv_book())
		next |= KEY_DRIGHT | KEY_CPAD_RIGHT, prev |= KEY_DLEFT | KEY_CPAD_LEFT;
	else { // la cruceta, según cómo se sostiene: «siguiente» es la flecha que apunta hacia donde se lee
		u32 right = RV.orient == 1 ? KEY_DDOWN | KEY_CPAD_DOWN : KEY_DUP | KEY_CPAD_UP, left = RV.orient == 1 ? KEY_DUP | KEY_CPAD_UP : KEY_DDOWN | KEY_CPAD_DOWN;
		next |= RV.rtl ? left : right, prev |= RV.rtl ? right : left;
	}
	if (g_krepeat & next)
		rv_go(RV.page + step);
	else if (g_krepeat & prev)
		rv_go(RV.page - step);
	if (!rv_book()) { // desplazarse por la página: cruceta, Circle Pad o el dedo; al final, a la siguiente
		float th = RV.has[0] ? RV.tex[0].h : 0, total = SCR_H * 2;
		float mx = th > total ? th - total : 0;
		if (g_krepeat & KEY_DDOWN) {
			if (RV.scroll >= mx - 1 && (g_kdown & KEY_DDOWN))
				rv_go(RV.page + 1);
			else
				RV.scroll += 60;
		}
		if (g_krepeat & KEY_DUP) {
			if (RV.scroll <= 1 && (g_kdown & KEY_DUP))
				rv_go(RV.page - 1);
			else
				RV.scroll -= 60;
		}
		circlePosition cp;
		hidCircleRead(&cp);
		if (abs(cp.dy) > 20)
			RV.scroll -= cp.dy / 12.0f;
		if (abs(cp.dx) > 20 && RV.has[0] && RV.tex[0].w > BOT_W)
			RV.cx = fmaxf(BOT_W / 2.0f, fminf(RV.tex[0].w - BOT_W / 2.0f, RV.cx + cp.dx / 12.0f));
		if (g_touch.held && g_touch.drag) {
			RV.scroll -= g_touch.y - g_touch.py;
			if (RV.has[0] && RV.tex[0].w > BOT_W)
				RV.cx = fmaxf(BOT_W / 2.0f, fminf(RV.tex[0].w - BOT_W / 2.0f, RV.cx - (g_touch.x - g_touch.px)));
		}
	}
	// tocar: a los lados (los de verdad, según cómo se sostiene), página anterior o siguiente; en medio, las opciones
	if (g_touch.up && !g_touch.drag) {
		int side = 0; // -1 a la izquierda, 1 a la derecha (vistos por quien lee)
		if (!rv_book())
			side = g_touch.x < BOT_W / 4 ? -1 : g_touch.x > BOT_W * 3 / 4 ? 1 : 0;
		else {
			int t = g_touch.y < SCR_H / 3 ? -1 : g_touch.y > SCR_H * 2 / 3 ? 1 : 0; // (arriba / abajo de la pantalla)
			side = RV.orient == 1 ? t : -t;
		}
		if (side && rv_book() && RV.rtl)
			side = -side;
		if (side < 0)
			rv_go(RV.page - step);
		else if (side > 0)
			rv_go(RV.page + step);
		else
			RV.menu = RV.menu_just = true, RV.menu_sel = 0;
	}
}

static void rv_menu(void)
{
	IconId ic[8];
	const char *lab[8];
	u32 col[8];
	int act[8], n = 0;
	static char pg[64];
	ic[n] = IC_FLIP, col[n] = T.text, act[n++] = 0;
	lab[n - 1] = RV.orient == 0 ? _("Leer como un libro (consola de lado)") : _("Girar al otro lado");
	if (RV.orient) // (y volver a horizontal, siempre a mano)
		ic[n] = IC_BOOK, lab[n] = _("Leer en horizontal"), col[n] = T.text, act[n++] = 9;
	if (rv_book()) {
		ic[n] = IC_UNDO, lab[n] = RV.rtl ? _("Leer de izquierda a derecha") : _("Leer de derecha a izquierda (manga)"), col[n] = T.text, act[n++] = 1;
		ic[n] = RV.autorot ? IC_CHECK : IC_FLIP, lab[n] = RV.autorot ? _("Girar solo: activado") : _("Girar solo al girar la consola"), col[n] = T.text, act[n++] = 8;
	}
	if (!RV.reflow && !rv_book()) {
		if (RV.zoom < 3)
			ic[n] = IC_SEARCH, lab[n] = _("Ampliar"), col[n] = T.text, act[n++] = 2;
		if (RV.zoom > 0)
			ic[n] = IC_GRID, lab[n] = _("Ajustar a la pantalla"), col[n] = T.text, act[n++] = 3;
	}
	if (RV.reflow) {
		ic[n] = IC_TEXTSIZE, lab[n] = _("Letra más grande"), col[n] = T.text, act[n++] = 4;
		ic[n] = IC_FONT, lab[n] = _("Letra más pequeña"), col[n] = T.text, act[n++] = 5;
	}
	if (RV.pages > 1) {
		snprintf(pg, sizeof pg, _("Ir a la página… (%d de %d)"), RV.page + 1, RV.pages);
		ic[n] = IC_BOOK, lab[n] = pg, col[n] = T.text, act[n++] = 6;
	}
	ic[n] = IC_CLOSE, lab[n] = _("Cerrar el documento"), col[n] = T.danger, act[n++] = 7;
	int r = sheet_list(&RV.menu, &RV.menu_just, &RV.menu_anim, &RV.menu_sel, RV.name, ic, lab, col, n);
	if (r < 0)
		return;
	switch (act[r]) {
	case 9:
		RV.orient = 0;
		RV.cx = -1;
		rv_drop_tex();
		rv_relayout(RV.page);
		break;
	case 0:
		if (RV.orient == 0)
			RV.orient = 1; // a libro (con el giro automático, el lado lo pone la consola)
		else
			RV.orient = RV.orient == 1 ? 2 : 1, RV.autorot = false; // al otro lado, a mano: si no, el sensor lo devolvería
		RV.cx = -1;
		rv_drop_tex();
		rv_relayout(RV.page);
		break;
	case 1: RV.rtl = !RV.rtl; break;
	case 8: RV.autorot = !RV.autorot; break;
	case 2:
	case 3:
		RV.zoom = act[r] == 2 ? RV.zoom + 1 : 0;
		RV.cx = -1;
		break;
	case 4:
	case 5:
		RV.em = clampf(RV.em + (act[r] == 4 ? 1 : -1), 8, 20);
		rv_relayout(RV.page);
		break;
	case 6: RV.want_page = true; break;
	default: {
		int ret = RV.ret_app;
		rv_close();
		if (ret != AP_READER && ret != U.app)
			hm_launch(ret);
	}
	}
}

static bool rv_is_open(void) { return RV.open; }

static bool dl_count_cb(const FsxEntry *e, void *u)
{
	(void)e;
	(*(int *)u)++;
	return true;
}

// Descargas: desde un enlace, un QR o el enlace copiado; y, la última, lo ya descargado (la carpeta, como en Archivos)
static void dl_bottom(void)
{
	tab_header(_("Descargas"), false);
	if ((g_kdown & KEY_B) && !g_dialog.open) {
		hm_show();
		return;
	}
	static int sel;
	if (DLV_n < 0) {
		DLV_n = 0;
		fsx_list(DL_DIR, false, dl_count_cb, &DLV_n);
	}
	const char *cl = clip_text();
	bool clu = !strncasecmp(cl, "http://", 7) || !strncasecmp(cl, "https://", 8);
	char cnt[64];
	if (DLV_n == 0)
		util_strlcpy(cnt, _("Sin archivos · /Downloads"), sizeof cnt);
	else
		snprintf(cnt, sizeof cnt, DLV_n == 1 ? _("1 elemento · /Downloads") : _("%d elementos · /Downloads"), DLV_n);
	struct { IconId ic; u32 col; const char *t, *s; int act; } C[4];
	int n = 0;
	C[n++] = (typeof(C[0])){IC_LINK, T.warn, _("Desde un enlace"), _("Escribe la dirección de un archivo o de un proyecto de GitHub"), 0};
	C[n++] = (typeof(C[0])){IC_CAMERA, T.ok, _("Escanear un código QR"), _("Con la cámara, como en FBI"), 1};
	if (clu)
		C[n++] = (typeof(C[0])){IC_DUP, T.accent2, _("Enlace del portapapeles"), cl, 2};
	C[n++] = (typeof(C[0])){IC_FOLDER, T.accent, _("Ver lo descargado"), cnt, 3};
	if (g_krepeat & KEY_DOWN)
		sel = (sel + 1) % n, U.buttons = true;
	if (g_krepeat & KEY_UP)
		sel = (sel + n - 1) % n, U.buttons = true;
	if (sel >= n)
		sel = 0;
	int run = -1;
	float y = VIEW_Y + 8;
	for (int i = 0; i < n; i++, y += 45) {
		bool hl = touch_in(10, y, BOT_W - 20, 40) || (U.buttons && sel == i);
		draw_rrect(10, y, BOT_W - 20, 40, 13, hl ? T.surface2 : T.surface);
		if (U.buttons && sel == i)
			draw_rrect_outline(10, y, BOT_W - 20, 40, 13, 1.5f, T.accent);
		draw_circle(34, y + 20, 13, col_alpha(C[i].col, 0.18f));
		draw_icon(C[i].ic, 34, y + 20, 14, C[i].col);
		text_draw_clip(C[i].t, 58, y + 3, BOT_W - 80, TS_BODY, T.text);
		text_draw_clip(C[i].s, 58, y + 21, BOT_W - 80, TS_SUB, T.text3);
		if (!g_dialog.open && tap(10, y, BOT_W - 20, 40))
			run = C[i].act, sel = i;
	}
	if (!g_dialog.open && (g_kdown & KEY_A))
		run = C[sel].act;
	ds_refresh();
	{ // la cola: X o tocando aquí
		char q[80];
		snprintf(q, sizeof q, _("%s Cola de descargas (%d)"), GLYPH_X, DS.n);
		float qw = text_width(q, TS_SUB) + 24, qx = (BOT_W - qw) / 2, qy = SCR_H - 26;
		draw_rrect(qx, qy, qw, 22, 11, touch_in(qx, qy - 2, qw, 26) ? T.surface2 : T.surface);
		text_draw_center(q, BOT_W / 2, qy + (22 - text_lineh(TS_SUB)) / 2, TS_SUB, T.accent);
		if (!g_dialog.open && (tap(qx, qy - 2, qw, 26) || (g_kdown & KEY_X))) {
			DLQ = true;
			g_kdown &= ~KEY_X;
			return;
		}
	}
	if (run == 0)
		U.want_dlurl = true;
	else if (run == 1)
		qrs_open();
	else if (run == 2) {
		char url[1024];
		util_strlcpy(url, cl, sizeof url);
		if (dl_add(url, dl_dest_dir(url), "", DLA_INSTALL | DLA_STREAM, G.console, NULL) == 0)
			toast_icon(_("Descargando el enlace del portapapeles"), IC_DOWNLOAD, T.accent);
		else
			toast_icon(_("No se pudo descargar ese enlace"), IC_CLOSE, T.danger);
	} else if (run == 3) {
		g_kdown &= ~(KEY_A | KEY_B);
		fsx_mkdirs(DL_DIR);
		F.ret_from[0] = F.ret_to[0] = 0;
		f_load(DL_DIR);
		DLV = true;
	}
}

// la cola de descargas: cada una con cómo va; A, sus opciones (pausar, reanudar, cancelar, reintentar, su carpeta…)
static void dlq_bottom(void)
{
	tab_header(_("Cola de descargas"), true);
	if (!g_dialog.open && !DS.open && (tap(0, 0, 170, VIEW_Y) || (g_kdown & KEY_B))) {
		DLQ = false;
		return;
	}
	ds_refresh();
	static List l;
	int nd = DS.n;
	const float RH = 40;
	list_update(&l, nd, RH);
	if (!nd) {
		empty_state(IC_DOWNLOAD, _("La cola está vacía"), _("Lo que descargues desde un enlace, un QR o la Nube aparecerá aquí. Puedes pausarlo, reanudarlo o cancelarlo."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int r = 0; r < nd; r++) {
		float y = VIEW_Y + r * RH - l.off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		const DlItem *d = &DS.it[nd - 1 - r]; // (la más reciente arriba)
		bool t = row(&l, r, y, RH);
		if (!U.buttons && l.sel == r)
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		if (!DS.open && (t || activate(&l, r))) {
			DS.id = d->id;
			DS.open = DS.just = true, DS.sel = 0;
		}
		u32 sc;
		char st[160];
		dl_status(d, st, sizeof st, &sc);
		u32 ic = d->st == DL_FAIL ? T.danger : d->st == DL_DONE ? T.ok : T.accent;
		draw_rrect(14, y + (RH - 26) / 2, 26, 26, 8, col_alpha(ic, 0.16f));
		draw_icon(d->st == DL_FAIL ? IC_CLOSE : d->st == DL_DONE ? IC_CHECK : d->st == DL_PAUSED ? IC_CLOCK : IC_DOWNLOAD, 27, y + RH / 2, 14, ic);
		g_clip_ell = l.sel != r;
		text_draw_clip(d->name[0] ? d->name : d->host, 50, y + 5, BOT_W - 64, TS_BODY, T.text);
		text_draw_clip(st, 50, y + 22, BOT_W - 64, TS_SUB, sc);
		g_clip_ell = false;
		if ((d->st == DL_RUN || d->st == DL_PAUSED) && d->total) {
			draw_rect(50, y + RH - 4, BOT_W - 64, 2, T.surface2);
			draw_rect(50, y + RH - 4, (BOT_W - 64) * (float)((double)d->done / d->total), 2, d->st == DL_PAUSED ? T.text3 : T.accent);
		}
	}
	gfx_clip_off();
}

// de lo descargado, atrás: a las tarjetas de Descargas
static void dl_view_close(void)
{
	DLV = false;
	DLV_n = -1;
	ms_end();
}

static void draw_bottom(void)
{
	gfx_target(g_bot, T.bg);
	g_clip_ell = false;
	if (U.splash >= 0) {
		splash_bot(1);
		return;
	}
	if (UN.anim > 0.99f || guard_on()) {
		un_card();
		return;
	}
	if (br_is_open()) {
		br_bottom();
		dialog_draw();
		return;
	}
	if (rv_is_open()) {
		rv_bottom();
		dialog_draw();
		return;
	}
	if (CLU.open) {
		cl_open_poll();
		// encima de la nube: abriendo un archivo, la hoja del ZIP o el reproductor; lo de debajo no recibe nada
		bool over = CLU.info.opening || ZS.open || MU.open || (VW.cloud && VW.kind && (CLU.om || g_dialog.open));
		u32 kd = g_kdown, kr = g_krepeat;
		Touch tc = g_touch;
		if (over) {
			g_kdown = g_krepeat = 0;
			memset(&g_touch, 0, sizeof g_touch);
		}
		hdr_frame(!over && !CLU.om && !CLU.hm && !CLU.mo && !(VW.cloud && VW.kind));
		if (VW.cloud && VW.kind) {
			vw_bottom();
			if (over)
				g_kdown = kd, g_krepeat = kr, g_touch = tc;
			cl_options();
		} else {
			cl_bottom();
			if (over)
				g_kdown = kd, g_krepeat = kr, g_touch = tc;
		}
		zs_draw();
		music_sheet();
		cl_open_overlay();
		qa_draw();
		dialog_draw();
		return;
	}
	if (hm_is_open()) {
		hm_bottom();
		if (U.fade > 0) {
			draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.bg, U.fade));
			splash_bot(U.fade);
		}
		return;
	}
	// con un menú, la tarjeta de instalar o el visor encima, lo de debajo (pestaña y barra) no recibe nada
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	bool viewer = U.tab == TAB_FILES && VW.kind;
	bool overlay = QA.open || DVS.open || UL.open || FM.open || PM.open || TY.open || VS.open || MA.open || MC.open || TS.open || HS.open || XS.open || ZS.open || DS.open || QRS.open || TT.open || MU.open || U.ins_pend || viewer;
	if (overlay) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	} else if (mu_bar_shown() && tc.sy >= SCR_H - TABBAR_H - MU_BAR_H - 4 && tc.sy < SCR_H - TABBAR_H) // la barrita de la música: lo de debajo no se toca
		memset(&g_touch, 0, sizeof g_touch);
	hdr_frame(!overlay);
	if (viewer) {
		if (!FM.open && !TY.open && !U.ins_pend && !g_dialog.open)
			g_kdown = kd, g_krepeat = kr, g_touch = tc;
		vw_bottom();
	} else if (U.app == AP_MUSIC)
		ml_bottom();
	else if (U.app == AP_SHARE)
		sh_bottom();
	else if (U.app == AP_READER)
		rl_bottom();
	else if (U.app == AP_DL && !DLV && DLQ)
		dlq_bottom();
	else if (U.app == AP_DL && !DLV)
		dl_bottom();
	else if (U.tab == TAB_FILES) {
		tab_files();
		f_pick_bar();
	} else if (U.tab == TAB_ACTIVITY)
		tab_activity();
	else if (U.tab == TAB_DEVICES)
		tab_devices();
	else if (U.page == PAGE_LANG)
		settings_lang();
	else if (U.page == PAGE_ABOUT)
		settings_about();
	else if (U.page == PAGE_PIN)
		settings_pin();
	else if (U.page == PAGE_UPDATE)
		settings_update();
	else if (U.page == PAGE_CONN)
		settings_conn();
	else
		settings_main();
	bool lf = g_touch.long_fired; // una pulsación larga ya usada no vuelve a contar al soltar
	g_kdown = kd, g_krepeat = kr, g_touch = tc, g_touch.long_fired |= lf;
	if (U.nav_eat) { // lo que llevó a otra carpeta no sigue en las hojas y barras de después
		g_kdown &= ~(KEY_A | KEY_B | KEY_X | KEY_Y);
		g_touch.up = false;
	}
	U.nav_eat = false;
	if (!overlay && !g_dialog.open)
		music_bar(); // con la entrada de verdad (lo de debajo no la recibió)
	if (U.tab == TAB_FILES) {
		f_menu();
		pm_draw();
		ty_draw();
		vs_draw();
		ma_draw();
		mc_draw();
		ts_draw();
		hs_draw();
		xs_draw();
		zs_draw();
		tt_draw();
	}
	ds_draw();
	if (U.tab == TAB_DEVICES)
		dvs_draw();
	if (U.tab == TAB_SETTINGS)
		ul_draw();
	music_sheet();
	qa_draw();
	qrs_draw();
	install_card();
	un_card();
	dialog_draw();
	if (U.fade > 0) {
		draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.bg, U.fade));
		splash_bot(U.fade);
	}
}

// ------------------------------------------------------------ lógica
static void handle_dialog(int ctx, bool ok)
{
	WHERE("contestar un diálogo");
	if (ctx == DLG_SHARE) { // otra consola: vincularse o que le envíe una app (también al decir que no)
		share_answer(U.dlg_arg, ok);
		return;
	}
	if (!ok)
		return;
	if (ctx == DLG_TXTDISCARD) {
		vw_close();
		return;
	}
	if (ctx == DLG_TXTOVER) {
		vw_save(true);
		return;
	}
	state_lock();
	if (ctx == DLG_REVOKE) {
		device_revoke(U.dlg_arg);
	} else if (ctx == DLG_NEWPIN) {
		pin_regen();
	} else if (ctx == DLG_CLEAR) {
		G.hist_n = G.hist_head = 0;
		G.dirty_cfg = true;
	} else if (ctx == DLG_EXIT) {
		U.quit = true;
	} else if (ctx == DLG_UPDATE) {
		upd_install();
	} else if (ctx == DLG_FDELMULTI) {
		for (int i = 0; i < F.n; i++) {
			if (!ms_has(F.e[i].name))
				continue;
			char full[700];
			f_full(i, full, sizeof full);
			if (trash_put(full, G.console, (u8)mark_get(full)) == 0) {
				mark_deleted(full);
				event_add(EV_DELETE, F.e[i].name, G.console, 0);
			}
		}
	} else if (ctx == DLG_CLDEL) {
		if (U.dlg_arg >= 0 && U.dlg_arg < CLU.n) {
			if (VW.cloud && VW.kind && !strcmp(VW.citem.id, CLU.it[U.dlg_arg].id))
				vw_close(); // (el que se estaba viendo)
			cloud_delete(&CLU.it[U.dlg_arg]);
		}
	} else if (ctx == DLG_MLDEL) { // canciones: una (del_path) o todas las sin datos
		int done = 0;
		if (ML.del_path[0]) {
			if (trash_put(ML.del_path, G.console, 0) == 0)
				mlib_forget(ML.del_path), done++;
		} else
			for (int k = 0; k < ML.n; k++) {
				char p[256];
				util_strlcpy(p, ML.lib[ML.ix[k]].path, sizeof p);
				if (trash_put(p, G.console, 0) == 0)
					mlib_forget(p), done++;
			}
		char m[64];
		snprintf(m, sizeof m, done == 1 ? _("1 canción a la papelera") : _("%d canciones a la papelera"), done);
		toast_icon(m, IC_TRASH, T.text2);
	} else if (ctx == DLG_CLDELN) {
		for (int i = 0; i < CLU.n; i++)
			if (CLU.ms && CLU.mark[i])
				cloud_delete(&CLU.it[i]);
		CLU.ms = false;
	} else if (ctx == DLG_CLOUT) {
		cloud_disconnect();
		toast_icon(_("OneDrive desconectado"), IC_LOGOUT, T.text2);
	} else if (ctx == DLG_UPDRESTART) {
		aptSetChainloaderToSelf(); // vuelve a abrir Cloud3DS, ya con la versión nueva
		U.quit = true;
	} else if (ctx == DLG_UPDFILE) {
		if (U.dlg_arg >= 0 && U.dlg_arg < UL.n)
			upd_install_file(UL.f[U.dlg_arg].path);
	} else if (ctx == DLG_DLINSECURE) {
		dl_op((u32)U.dlg_arg, DL_INSECURE);
	} else if (ctx == DLG_UNINSTALL && U.dlg_arg >= 0 && U.dlg_arg < TI_n) {
		const TitleInfo *t = &TI[U.dlg_arg];
		if (t->size + t->upd_size + t->dlc_size >= (1ull << 30)) { // lo grande se confirma dos veces
			char msg[200];
			snprintf(msg, sizeof msg, _("«%s» ocupa más de 1 GB y habría que volver a descargarlo o instalarlo. Esta acción no se puede deshacer."), t->title);
			dialog_open(DLG_UNINSTALL2, _("¿Continuar?"), msg, _("Desinstalar"), true);
		} else
			ctx = DLG_UNINSTALL2;
	}
	if ((ctx == DLG_UNUPD || ctx == DLG_UNDLC || ctx == DLG_EXTDEL) && U.dlg_arg >= 0 && U.dlg_arg < TI_n && !UN.busy) {
		ctx = DLG_UNINSTALL2; // (la misma pantalla de espera, con lo suyo)
		UN.kind = g_dialog.ctx == DLG_UNUPD ? UN_UPD : g_dialog.ctx == DLG_UNDLC ? UN_DLC : UN_EXT;
	} else if (ctx == DLG_UNINSTALL2)
		UN.kind = UN_ALL;
	if (ctx == DLG_UNINSTALL2 && U.dlg_arg >= 0 && U.dlg_arg < TI_n && !UN.busy) {
		const TitleInfo *t = &TI[U.dlg_arg];
		UN.tid = t->tid;
		util_strlcpy(UN.name, t->title, sizeof UN.name);
		UN.size[0] = t->size, UN.size[1] = t->upd_size, UN.size[2] = t->dlc_size;
		UN.has[0] = true, UN.has[1] = t->has_upd || t->upd_size, UN.has[2] = t->dlc > 0;
		u16 *px = malloc(48 * 48 * 2);
		if (px && title_icon(t->tid, px))
			tex_from_rgb565(&UN.icon, px, 48, 48);
		else
			tex_free(&UN.icon);
		free(px);
		UN.part = UN.kind == UN_UPD ? 1 : UN.kind == UN_DLC ? 2 : UN.kind == UN_EXT ? 3 : 0, UN.done = false, UN.t0 = util_ms();
		UN.ti = *t;
		if (UN.kind == UN_EXT)
			UN.size[0] = TT.ext > 0 ? (u64)TT.ext : 0;
		s32 prio = 0x30;
		svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
		UN.thr = threadCreate(un_thread, NULL, 32 * 1024, prio + 1, -2, false);
		if (UN.thr)
			UN.busy = true; // (la pantalla de espera bloquea HOME)
		else
			toast_icon(_("No se pudo desinstalar"), IC_CLOSE, T.danger);
	} else if (ctx == DLG_TRASHEMPTY) {
		trash_empty();
		G.fsrev++;
	} else if (ctx == DLG_TRASHDEL) {
		const FEnt *e = f_selected();
		if (e)
			trash_purge(e->tid);
		G.fsrev++;
	} else if (ctx == DLG_FDELETE) {
		char full[700];
		f_full(FM.idx, full, sizeof full);
		if (trash_put(full, G.console, (u8)mark_get(full)) == 0) {
			mark_deleted(full);
			event_add(EV_DELETE, F.e[FM.idx].name, G.console, 0);
		}
	}
	G.rev++;
	memcpy(&V, &G, sizeof G);
	s_hist_n = V.hist_n;
	s_hist_head = V.hist_head;
	state_unlock();
	if (ctx == DLG_NEWPIN)
		toast_icon(_("Nuevo PIN y código QR generados"), IC_KEY, T.accent2);
	if (ctx == DLG_FDELMULTI)
		ms_end();
	if (ctx == DLG_FDELETE || ctx == DLG_FDELMULTI || ctx == DLG_TRASHEMPTY || ctx == DLG_TRASHDEL)
		f_reload();

}

static void watch_events(void)
{
	static u32 fs_seen;
	if (V.fsrev != fs_seen) { // algo cambió en la SD (desde la web o aquí): la carpeta abierta se vuelve a leer
		fs_seen = V.fsrev;
		F.need_reload = true;
	}
	if (V.hist_n == s_hist_n && V.hist_head == s_hist_head)
		return;
	if (s_events_seen && V.hist_n > 0) {
		const Event *e = ev(0);
		char msg[160];
		if (e->kind == EV_PAIRED) {
			snprintf(msg, sizeof msg, _("Vinculado: %s"), e->device);
			toast_icon(msg, IC_LINK, T.accent2);
		} else if (e->kind == EV_PIN_LOCK) {
			toast_icon(_("Acceso bloqueado por intentos fallidos"), IC_LOCK, T.warn);
		} else if (e->kind == EV_DELETE) {
			snprintf(msg, sizeof msg, _("Eliminado: %s"), e->name);
			toast_icon(msg, IC_TRASH, T.danger);
		} else if (e->kind == EV_UPLOAD_FAIL) {
			snprintf(msg, sizeof msg, _("Transferencia incompleta: %s"), e->name);
			toast_icon(msg, IC_CLOSE, T.danger);
		} else if (e->kind == EV_INSTALL) {
			snprintf(msg, sizeof msg, _("Instalado: %s"), e->name);
			toast_icon(msg, IC_CHECK, T.ok);
		}
		// el texto abierto en la consola se cambió desde otro dispositivo: si aquí no hay cambios, se recarga
		if ((e->kind == EV_EDIT || e->kind == EV_UPLOAD) && VW.kind == VW_TXT && !VW.cloud && !VW.doc.dirty && strcmp(e->device, V.console) &&
		    !strcmp(e->name, F.e[VW.idx].name)) {
			int cur = VW.cur, top = VW.top;
			vw_load(VW.idx);
			VW.cur = cur, VW.top = top;
			toast_icon(_("Actualizado desde otro dispositivo"), IC_REFRESH, T.accent);
		}
	}
	s_hist_n = V.hist_n;
	s_hist_head = V.hist_head;
}

static void lid_and_sleep(void)
{
	bool busy = xfer_active() > 0 || upd_busy() || ins_busy() || music_playing(); // con música, como Radio Neon: sigue con la tapa cerrada
	aptSetSleepAllowed(!busy);
	// tapa cerrada transfiriendo: apaga la luz y sigue
	u8 open = 1;
	PTMU_GetShellState(&open);
	bool want_off = busy && !open;
	if (want_off != U.lid_off) {
		if (R_SUCCEEDED(gspLcdInit())) {
			if (want_off)
				GSPLCD_PowerOffAllBacklights();
			else
				GSPLCD_PowerOnAllBacklights();
			gspLcdExit();
		}
		U.lid_off = want_off;
	}
}

// salir siempre pregunta; si hay transferencias lo dice bien claro (y se actualiza mientras está abierto)
static void exit_text(char *msg, size_t n, bool *danger)
{
	const Xfer *x = NULL;
	int cnt = 0;
	for (int i = 0; i < MAX_XFERS; i++)
		if (V.xfers[i].st == XS_RUNNING && cnt++ == 0)
			x = &V.xfers[i];
	*danger = cnt > 0;
	if (!cnt) {
		util_strlcpy(msg, _("Tu nube dejará de estar disponible en el móvil y en el PC hasta que vuelvas a abrir Cloud3DS."), n);
		return;
	}
	char name[64];
	util_strlcpy(name, x->name, sizeof name);
	int pct = x->total ? (int)((double)x->done * 100 / x->total) : 0;
	const char *dir = x->kind == XF_UPLOAD ? _("recibiendo") : x->kind == XF_COPY ? _("copiando") : x->kind == XF_INSTALL ? _("instalando") : x->kind == XF_ZIP ? _("comprimiendo") : x->kind == XF_UNZIP ? _("descomprimiendo") : x->kind == XF_FETCH ? _("descargando") : _("enviando");
	if (cnt == 1)
		snprintf(msg, n, _("Hay 1 transferencia en curso (%s «%s», %d%%). Si sales, se cancelará."), dir, name, pct);
	else
		snprintf(msg, n, _("Hay %d transferencias en curso (%s «%s», %d%%, y %d más). Si sales, se cancelarán."), cnt, dir,
		         name, pct, cnt - 1);
}

static void ask_exit(void)
{
	if (upd_busy() || ins_busy()) {
		toast_icon(upd_busy() ? _("Espera a que termine la actualización") : _("Espera a que termine la instalación"), IC_DOWNLOAD, T.warn);
		return;
	}
	char msg[300];
	bool danger;
	exit_text(msg, sizeof msg, &danger);
	dialog_open(DLG_EXIT, _("¿Salir de Cloud3DS?"), msg, _("Salir"), danger);
}

static void exit_refresh(void)
{
	if (!g_dialog.open || g_dialog.ctx != DLG_EXIT)
		return;
	exit_text(g_dialog.msg, sizeof g_dialog.msg, &g_dialog.danger);
}

static void update(void)
{
	// presentación: unos 2 s, o hasta pulsar algo (a partir de medio segundo)
	if (U.splash >= 0) {
		U.splash += g_dt;
		if (U.splash > 2.0f || (U.splash > 0.5f && ((g_kdown & ~KEY_TOUCH) || g_touch.down))) {
			U.splash = -1;
			U.fade = 1;
		}
		return;
	}
	un_poll();
	tl_poll();
	static bool home_blocked;
	bool g = guard_on();
	if (g != home_blocked) // HOME a mitad de instalar o borrar sería peligroso
		aptSetHomeAllowed(!g), home_blocked = g;
	if (g) { // nada de debajo responde hasta que termine (tampoco al dibujar)
		// salvo cancelar una descarga que se instala según llega: AM descarta lo instalado a medias
		static bool cancel_sent;
		if (!ins_streaming())
			cancel_sent = false;
		else if (!UN.busy && !cancel_sent && ((g_kdown & KEY_B) || (g_touch.up && !g_touch.drag && g_touch.sy > SCR_H - 64))) {
			dl_cancel_current();
			cancel_sent = true;
			toast_icon(_("Cancelando la descarga…"), IC_CLOSE, T.text2);
		}
		g_kdown = g_kheld = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
		return;
	}
	if (U.fade > 0)
		U.fade = fmaxf(0, U.fade - g_dt * 3.5f);
	// la ventana del QR no se queda: en cuanto se hace otra cosa, vuelve a la tira
	if (!V.qr_min && ((g_kdown & ~KEY_TOUCH) || g_touch.down || U.ins_pend || V.srv_off || QA.open || CLU.open || br_is_open()))
		qr_show(false);
	if (g_kdown & ~KEY_TOUCH)
		U.buttons = true;
	if (g_touch.down)
		U.buttons = false;
	if (ZQ.want) {
		ZQ.want = false;
		char def[256], nm[256];
		if (ZQ.n == 1) { // «carpeta.zip» o «archivo.zip» (sin su extensión)
			util_strlcpy(def, ZQ.names[0], sizeof def - 4);
			char *dot = strrchr(def, '.');
			bool dir = false;
			u64 sz;
			char full[800];
			snprintf(full, sizeof full, "%s%s%s", ZQ.base, strcmp(ZQ.base, "/") ? "/" : "", ZQ.names[0]);
			fsx_stat(full, &dir, &sz);
			if (dot && dot != def && !dir)
				*dot = 0;
		} else {
			const char *b = strrchr(ZQ.base, '/');
			util_strlcpy(def, b && b[1] ? b + 1 : "Cloud3DS", sizeof def - 4);
		}
		strcat(def, ".zip");
		if (kb_input(_("Nombre del ZIP"), def, nm, sizeof nm - 4, false, false) && nm[0]) {
			size_t l = strlen(nm);
			if (l < 4 || strcasecmp(nm + l - 4, ".zip"))
				strcat(nm, ".zip");
			if (strpbrk(nm, "\\/:*?\"<>|"))
				toast_icon(_("El nombre no es válido"), IC_CLOSE, T.danger);
			else {
				int r = zipjob_create(ZQ.base, ZQ.names, ZQ.n, nm, G.console);
				if (r != ZIP_OK)
					toast_icon(zip_err_text(r), IC_CLOSE, T.danger);
				else {
					toast_icon(_("Comprimiendo…"), IC_ARCHIVE, T.warn);
					ms_end();
					f_build_view();
				}
			}
		}
	}
	{ // un ZIP terminó: se dice cómo fue
		static u32 seen;
		ZipJob zj;
		zipjob_get(&zj);
		if (zj.id != seen && (zj.st == ZJ_DONE || zj.st == ZJ_FAIL)) {
			seen = zj.id;
			const char *nm = strrchr(zj.result, '/');
			char msg[300];
			if (zj.st == ZJ_DONE)
				snprintf(msg, sizeof msg, zj.create ? _("ZIP creado: %s") : _("Extraído en %s"), zj.create && nm ? nm + 1 : zj.result);
			toast_icon(zj.st == ZJ_DONE ? msg : zip_err_text(zj.err), zj.st == ZJ_DONE ? IC_CHECK : IC_CLOSE, zj.st == ZJ_DONE ? T.ok : T.danger);
			F.need_reload = true;
		}
	}
	{ // una captura nueva llegó a la web: a los pocos segundos, a cuántos dispositivos
		static u32 seen_rev;
		static u64 at;
		if (V.shot_rev != seen_rev) {
			seen_rev = V.shot_rev;
			at = osGetTime() + 3500;
		}
		if (at && osGetTime() >= at) {
			at = 0;
			if (V.shot_seen > 0) {
				char m[80];
				snprintf(m, sizeof m, V.shot_seen == 1 ? _("Captura enviada a 1 dispositivo") : _("Captura enviada a %d dispositivos"), V.shot_seen);
				toast_icon(m, IC_CAMERA, T.accent);
			}
		}
	}
	hash_get(&HJ);
	if (HJ.st == HASH_DONE && HJ.id != HS.seen) { // terminó: si no se está mirando, un aviso
		HS.seen = HJ.id;
		if (!HS.open || U.tab != TAB_FILES)
			toast_icon(_("MD5 y SHA-256 listos"), IC_SHIELD, T.ok);
	}
	if (HS.want_cmp) {
		HS.want_cmp = false;
		char v[160];
		if (kb_input(_("Pega o escribe el MD5 o el SHA-256 esperado"), "", v, sizeof v, false, false) && v[0]) {
			int r = hash_compare(&HJ, v);
			if (r == HCHK_NONE)
				toast_icon(_("No es un MD5 (32 caracteres) ni un SHA-256 (64 caracteres)"), IC_CLOSE, T.warn);
			else {
				HS.cmp = r;
				toast_icon(r == HCHK_OK ? _("Coincide") : _("No coincide"), r == HCHK_OK ? IC_CHECK : IC_CLOSE, r == HCHK_OK ? T.ok : T.danger);
			}
		}
	}
	{ // escribir desde el PC: abrir el documento, aplicar lo que llega y publicar cómo queda
		char lp[700];
		if (live_take_open(lp, sizeof lp)) {
			if (VW.kind == VW_TXT && strcmp(VW.path, lp) && vw_dirty()) {
				toast_icon(_("Guarda o cierra el documento abierto para escribir desde el PC"), IC_NOTE, T.warn);
				live_publish(lp, "", 0, false, false);
			} else if (VW.kind != VW_TXT || strcmp(VW.path, lp)) {
				char dir[700], nm[256];
				util_strlcpy(dir, lp, sizeof dir);
				char *sl = strrchr(dir, '/');
				util_strlcpy(nm, sl ? sl + 1 : lp, sizeof nm);
				if (sl == dir)
					dir[1] = 0;
				else if (sl)
					*sl = 0;
				if (VW.kind)
					vw_close();
				U.tab = TAB_FILES;
				f_load(dir);
				for (int i = 0; i < F.nv; i++)
					if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm)) {
						F.sel = i;
						vw_load(F.v[i].idx);
					}
			}
		}
		static int sig_pos = -1, sig_n = -1, sig_cur = -1, sig_saved = -2;
		static char sig_path[700];
		if (live_active(NULL, 0)) {
			bool here = VW.kind == VW_TXT && VW.doc.ro == TXT_RW && !VW.prot;
			LiveOp op;
			while (live_pop(&op)) {
				if (here && op.i == -1 && op.cur >= 0 && op.cur < VW.doc.n) // solo se movió el cursor
					VW.cur = op.cur;
				else if (here && op.i >= 0 && op.i <= VW.doc.n && op.d >= 0 && op.i + op.d <= VW.doc.n && (op.text || VW.doc.n - op.d >= 1)) {
					txt_splice(&VW.doc, op.i, op.d, op.text);
					VW.cur = op.cur < 0 ? 0 : op.cur >= VW.doc.n ? VW.doc.n - 1 : op.cur;
					VW.hlv = 0;
					if (VW.cur < VW.top || VW.cur >= VW.top + 10) // que se vea por dónde va
						VW.top = VW.cur > 3 ? VW.cur - 3 : 0;
				}
				free(op.text);
			}
			if (here && live_want_save())
				vw_save(false);
			if (VW.kind == VW_TXT && (VW.doc.pos != sig_pos || VW.doc.n != sig_n || VW.cur != sig_cur || VW.doc.saved != sig_saved ||
			                          strcmp(sig_path, VW.path))) {
				sig_pos = VW.doc.pos, sig_n = VW.doc.n, sig_cur = VW.cur, sig_saved = VW.doc.saved;
				util_strlcpy(sig_path, VW.path, sizeof sig_path);
				char *t = txt_join(&VW.doc, 0, VW.doc.n);
				if (t)
					live_publish(VW.path, t, VW.cur, VW.doc.dirty, here);
				free(t);
			} else if (VW.kind != VW_TXT && sig_n != -2) { // se cerró en la consola
				sig_n = -2, sig_path[0] = 0;
				char lp2[700], who[4];
				(void)who;
				u32 rv;
				int cr;
				bool dd, ok2;
				free(live_get(0xFFFFFFFF, &rv, &cr, &dd, &ok2, lp2, sizeof lp2));
				live_publish(lp2, "", 0, false, false);
			}
		} else
			sig_n = -1;
	}
	music_get(&MU.s);
	if (U.want_netname) {
		U.want_netname = false;
		char nm[48];
		if (kb_input(_("Nombre en la red (letras, números y guiones)"), V.net_name, nm, 41, false, false) && nm[0]) {
			for (char *p = nm; *p; p++) // en la red, en minúsculas y con guiones en vez de espacios
				*p = *p == ' ' ? '-' : (char)tolower((unsigned char)*p);
			if (!remote_name_ok(nm))
				toast_icon(_("Solo letras sin acentos, números y guiones"), IC_CLOSE, T.danger);
			else {
				state_lock();
				util_strlcpy(G.net_name, nm, sizeof G.net_name);
				state_unlock();
				remote_changed();
				char m[96];
				snprintf(m, sizeof m, _("Nombre en la red: %s.local"), nm);
				toast_icon(m, IC_CHECK, T.ok);
			}
		}
	}
	if (U.want_web) {
		U.want_web = false;
		br_open_home();
	}
	if (RV.want_page) { // Lector: ir a una página
		RV.want_page = false;
		char t[16], ini[16];
		snprintf(ini, sizeof ini, "%d", RV.page + 1);
		if (kb_input(_("Número de página"), ini, t, sizeof t, false, false) && t[0])
			rv_go(atoi(t) - 1);
	}
	if (U.want_dlurl) {
		U.want_dlurl = false;
		char url[1024];
		const char *cl = clip_text(); // (un enlace copiado, por ejemplo en el Navegador)
		bool clu = !strncasecmp(cl, "http://", 7) || !strncasecmp(cl, "https://", 8);
		if (kb_input(_("Enlace para descargar (https://…)"), clu ? cl : "https://", url, sizeof url, false, false) && url[0]) {
			if (dl_add(url, dl_dest_dir(url), "", DLA_INSTALL | DLA_STREAM, G.console, NULL) == 0)
				toast_icon(_("Descargando en Descargas (/Downloads)"), IC_GLOBE, T.accent);
			else
				toast_icon(_("El enlace no es válido"), IC_CLOSE, T.danger);
		}
	}
	if (U.want_search) {
		U.want_search = false;
		char q[128];
		if (kb_input(_("Buscar en esta carpeta y sus subcarpetas"), SR.q, q, sizeof q, false, false) && q[0])
			sr_start(q);
	}
	if (U.want_tnew) {
		U.want_tnew = false;
		char nm[128], dst[700];
		if (kb_input(_("Nombre del documento"), "", nm, sizeof nm - 4, false, false) && nm[0]) {
			if (!strchr(nm, '.')) // sin extensión: .txt
				strcat(nm, ".txt");
			snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
			bool d;
			u64 sz;
			char sd[720];
			snprintf(sd, sizeof sd, "sdmc:%s", dst);
			FILE *f = NULL;
			if (strpbrk(nm, "\\/:*?\"<>|") || nm[strlen(nm) - 1] == '.' || nm[strlen(nm) - 1] == ' ')
				toast_icon(_("El nombre no es válido"), IC_CLOSE, T.danger);
			else if (fsx_stat(dst, &d, &sz) == 0)
				toast_icon(_("Ya existe un archivo con ese nombre"), IC_CLOSE, T.danger);
			else if (!(f = fopen(sd, "wb")))
				toast_icon(_("No se pudo crear el documento"), IC_CLOSE, T.danger);
			else {
				fclose(f);
				state_lock();
				event_add(EV_CREATE, nm, G.console, 0);
				state_unlock();
				f_reload_keep();
				for (int i = 0; i < F.nv; i++) // queda elegido y se abre para escribir
					if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm)) {
						F.sel = i;
						vw_load(F.v[i].idx);
					}
			}
		}
		return;
	}
	if (U.want_fnew) {
		U.want_fnew = false;
		char nm[128], dst[700];
		if (kb_input(_("Nombre de la carpeta"), "", nm, sizeof nm, false, false) && nm[0]) {
			bool d;
			u64 sz;
			snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
			if (strpbrk(nm, "\\/:*?\"<>|") || nm[strlen(nm) - 1] == '.' || nm[strlen(nm) - 1] == ' ')
				toast_icon(_("El nombre no es válido"), IC_CLOSE, T.danger);
			else if (fsx_stat(dst, &d, &sz) == 0)
				toast_icon(_("Ya existe un archivo con ese nombre"), IC_CLOSE, T.danger);
			else if (fsx_mkdirs(dst) < 0)
				toast_icon(_("No se pudo crear la carpeta"), IC_CLOSE, T.danger);
			else {
				state_lock();
				event_add(EV_MKDIR, nm, G.console, 0);
				state_unlock();
				f_reload_keep();
				for (int i = 0; i < F.nv; i++) // queda elegida la carpeta nueva
					if (F.v[i].kind == FV_ITEM && !strcmp(F.e[F.v[i].idx].name, nm))
						F.sel = i;
			}
		}
		return;
	}
	cq_tick();
	static u64 trash_next;
	if (osGetTime() >= trash_next) { // lo de más de unos días se va solo; basta mirarlo cada hora
		trash_next = osGetTime() + 3600 * 1000ull;
		trash_expire();
	}
	if (U.want_txt) {
		int m = U.want_txt;
		U.want_txt = 0;
		if (VW.kind != VW_TXT)
			return;
		const char *ln = VW.doc.ln[VW.cur];
		int chars = 0;
		for (const char *p = ln; *p; p++)
			chars += ((u8)*p & 0xC0) != 0x80;
		if (m == 4) { // buscar: desde la línea siguiente, dando la vuelta; sin distinguir mayúsculas
			char q[128];
			if (!kb_input(_("Buscar en el texto"), VW.find, q, sizeof q, false, false) || !q[0])
				return;
			util_strlcpy(VW.find, q, sizeof VW.find);
			for (int k = 1; k <= VW.doc.n; k++) {
				int i = (VW.cur + k) % VW.doc.n;
				const char *h = VW.doc.ln[i];
				for (; *h; h++)
					if (!strncasecmp(h, q, strlen(q)))
						break;
				if (*h) {
					VW.cur = i;
					return;
				}
			}
			toast_icon(_("No se encontró"), IC_SEARCH, T.text3);
			return;
		}
		if ((m == 1 || m == 3) && chars > 2000) { // el teclado del sistema no admite más
			toast_icon(_("Esta línea es demasiado larga para el teclado"), IC_CLOSE, T.warn);
			return;
		}
		char *buf = malloc(8192);
		char hint[48];
		if (!buf)
			return;
		if (m == 1) {
			snprintf(hint, sizeof hint, _("Línea %d"), VW.cur + 1);
			if (kb_input(hint, ln, buf, 8192, true, false) && !txt_set(&VW.doc, VW.cur, buf))
				toast_icon(_("No hay memoria suficiente para abrir este archivo"), IC_CLOSE, T.danger);
		} else if (m == 3) {
			// varias líneas de una vez: desde la elegida, las que quepan en el teclado (hasta 1800 caracteres)
			int cnt = 0, tot = 0;
			while (VW.cur + cnt < VW.doc.n && cnt < 40) {
				int c = 1;
				for (const char *p = VW.doc.ln[VW.cur + cnt]; *p; p++)
					c += ((u8)*p & 0xC0) != 0x80;
				if (cnt && tot + c > 1800)
					break;
				tot += c, cnt++;
			}
			char *blk = txt_join(&VW.doc, VW.cur, cnt);
			snprintf(hint, sizeof hint, _("Líneas %d a %d"), VW.cur + 1, VW.cur + cnt);
			if (blk && kb_input(hint, blk, buf, 8192, true, false) && strcmp(blk, buf) && !txt_replace(&VW.doc, VW.cur, cnt, buf))
				toast_icon(_("No hay memoria suficiente para abrir este archivo"), IC_CLOSE, T.danger);
			free(blk);
		} else if (kb_input(_("Línea nueva"), "", buf, 8192, true, false)) {
			if (txt_insert(&VW.doc, VW.cur + 1, buf))
				VW.cur++;
		}
		VW.hlv = 0;
		free(buf);
		return;
	}
	if (U.want_frename) {
		U.want_frename = false;
		char nm[128], full[700], dst[1100];
		const FEnt *e = &F.e[FM.idx];
		f_full(FM.idx, full, sizeof full);
		if (kb_input(_("Nuevo nombre"), e->name, nm, sizeof nm, false, false) && nm[0] && strcmp(nm, e->name)) {
			if (strpbrk(nm, "\\/:*?\"<>|") || nm[strlen(nm) - 1] == '.' || nm[strlen(nm) - 1] == ' ') {
				toast_icon(_("El nombre no es válido"), IC_CLOSE, T.danger);
				return;
			}
			snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
			bool d;
			u64 sz;
			char a[1100], b[1200];
			snprintf(a, sizeof a, "sdmc:%s", full);
			snprintf(b, sizeof b, "sdmc:%s", dst);
			if (strcasecmp(full, dst) && fsx_stat(dst, &d, &sz) == 0)
				toast_icon(_("Ya existe un archivo con ese nombre"), IC_CLOSE, T.danger);
			else if (rename(a, b) != 0)
				toast_icon(_("No se pudo renombrar"), IC_CLOSE, T.danger);
			else {
				state_lock();
				mark_moved(full, dst);
				event_add(EV_RENAME, nm, G.console, 0);
				G.rev++;
				state_unlock();
				f_reload();
			}
		}
		return;
	}
	if (SH.want_ip) { // Compartir: añadir una consola por su IP
		SH.want_ip = false;
		char ip[32];
		if (kb_input(_("IP de la otra consola (se muestra en su app Conectar)"), "192.168.", ip, sizeof ip, false, false) && ip[0]) {
			if (share_add_ip(ip))
				toast_icon(_("Conectando con esa IP…"), IC_DEVICES, T.accent);
			else
				toast_icon(_("Esa IP no es válida"), IC_CLOSE, T.danger);
		}
		return;
	}
	if (CLU.want_ren || CLU.want_mkdir) { // la nube: cambiar el nombre o carpeta nueva
		int i = CLU.want_ren - 1;
		bool mk = CLU.want_mkdir;
		CLU.want_ren = 0, CLU.want_mkdir = false;
		char nm[160];
		if (mk) {
			if (kb_input(_("Nombre de la carpeta nueva"), _("Carpeta nueva"), nm, sizeof nm, false, false) && nm[0] && !strpbrk(nm, "/\\:*?\"<>|")) {
				cloud_mkdir(cl_parent(), nm);
				toast_icon(_("Creando la carpeta en OneDrive"), IC_FOLDER, T.accent);
			}
		} else if (i >= 0 && i < CLU.n) {
			CloudItem it = CLU.it[i];
			if (kb_input(_("Nombre nuevo"), it.name, nm, sizeof nm, false, false) && nm[0] && strcmp(nm, it.name)) {
				if (strpbrk(nm, "/\\:*?\"<>|"))
					toast_icon(_("El nombre no puede llevar / \\ : * ? \" < > |"), IC_CLOSE, T.danger);
				else {
					cloud_rename(&it, nm);
					toast_icon(_("Cambiando el nombre en OneDrive"), IC_PEN, T.accent);
				}
			}
		}
		return;
	}
	if (U.want_rename >= 0) {
		int i = U.want_rename;
		U.want_rename = -1;
		char nm[48];
		// vacío: vuelve al nombre automático (el de la red o el modelo)
		if (V.dev[i].used && kb_input(_("Nombre del dispositivo (déjalo vacío para usar el nombre automático)"), V.dev[i].named == NAME_CUSTOM ? V.dev[i].name : "",
		                              nm, sizeof nm, false, false)) {
			state_lock();
			if (G.dev[i].used && !strcmp(G.dev[i].token, V.dev[i].token))
				device_rename(i, nm);
			memcpy(&V, &G, sizeof G);
			state_unlock();
			toast_icon(_("Nombre guardado"), IC_PEN, T.accent2);
		}
		return;
	}
	if (U.want_pin) {
		U.want_pin = false;
		char pin[8];
		if (kb_pin(_("PIN de 4 cifras"), pin, sizeof pin)) {
			state_lock();
			bool ok = pin_set(pin);
			memcpy(&V, &G, sizeof G);
			state_unlock();
			if (ok)
				toast_icon(_("PIN guardado"), IC_KEY, T.accent2);
		}
		return;
	}
	if (install_input())
		return;
	exit_refresh();
	int d = dialog_update();
	if (d >= 0) {
		handle_dialog(g_dialog.ctx, d == 1);
		return;
	}
	if (g_dialog.open)
		return;
	br_poll(); // el navegador cerrado a media carga: se suelta al acabar
	if (CLU.open && (g_kdown & KEY_SELECT) && !CLU.om && !CLU.hm && !CLU.mo && !(vw_cloud_open() && VW.kind == VW_TXT)) { // la nube también: SELECT, al inicio
		if (vw_cloud_open()) // (en el editor, SELECT es «Bloque»)
			vw_close();
		CLU.open = false;
		hm_show();
		return;
	}
	if (br_is_open() || CLU.open || rv_is_open()) // el navegador, la nube y el Lector usan todos los botones (L/R, START…)
		return;
	if (!hm_is_open() && U.app == AP_CONNECT && (U.tab == TAB_ACTIVITY || U.tab == TAB_DEVICES) && (g_kdown & (KEY_L | KEY_R | KEY_ZL | KEY_ZR)))
		U.tab = U.tab == TAB_ACTIVITY ? TAB_DEVICES : TAB_ACTIVITY;
	if (hm_is_open()) { // el inicio usa los suyos (L/R, la cruceta, A, Y); START, salir
		if ((g_kdown & KEY_START) && !MU.open && !QRS.open)
			ask_exit();
		return;
	}
	// en el editor de texto, L y R no cambian de pestaña: R edita varias líneas y B cierra
	bool editor = VW.kind == VW_TXT && U.tab == TAB_FILES;
	// atrás: de una subpágina a Ajustes (y la pantalla de arriba vuelve al QR)
	if (U.page != PAGE_MAIN && ((g_kdown & KEY_B) || tap(0, 0, 170, VIEW_Y))) {
		U.list[TAB_SETTINGS].sel = U.page == PAGE_LANG ? SET_LANG : U.page == PAGE_PIN ? SET_PIN : U.page == PAGE_UPDATE ? SET_UPDATE : U.page == PAGE_CONN ? SET_CONN : SET_ABOUT;
		U.page = PAGE_MAIN;
	}
	if ((g_kdown & KEY_START) && !(U.tab == TAB_FILES && VW.kind == VW_TXT)) // en el editor, START guarda
		ask_exit();
	if ((g_kdown & KEY_SELECT) && !editor) { // SELECT: al inicio
		QA.open = false;
		hm_show();
	}
}

static aptHookCookie hook;

static void apt_hook(APT_HookType t, void *p)
{
	(void)p;
	if (t == APTHOOK_ONWAKEUP)
		server_restart();
}

static void load_console_name(void)
{
	u16 name[0x10] = {0};
	char n8[48] = "";
	if (R_SUCCEEDED(cfguInit())) {
		CFGU_GetConfigInfoBlk2(0x1C, 0x000A0000, name);
		cfguExit();
	}
	util_utf16_to_utf8(n8, sizeof n8, name, 10);
	if (n8[0])
		snprintf(G.console, sizeof G.console, "3DS · %s", n8);
	else
		util_strlcpy(G.console, "Nintendo 3DS", sizeof G.console);
}

static void titles_reload(void)
{
	tw_forget();
	tl_start();
}

// cada vuelta: la lista nueva, cuando llega; no mientras haya algo abierto que apunte a uno de la anterior
static void tl_poll(void)
{
	if (!TL.run || !TL.done || g_dialog.open || TT.open || UN.busy)
		return;
	if (TL.thr) {
		threadJoin(TL.thr, U64_MAX);
		threadFree(TL.thr);
		TL.thr = NULL;
	}
	TL.run = false;
	const TitleInfo *s = tt_selected();
	u64 keep = s ? s->tid : 0;
	free(TI);
	TI = TL.res, TI_n = TL.n < 0 ? 0 : TL.n;
	TL.res = NULL;
	if (TL.again) {
		TL.again = false;
		tl_start();
	}
	if (!F.titles)
		return;
	ti_fill();
	for (int i = 0; keep && i < F.nv; i++)
		if (F.v[i].kind == FV_ITEM && F.e[F.v[i].idx].tid < (u32)TI_n && TI[F.e[F.v[i].idx].tid].tid == keep)
			F.sel = i;
	if (F.sel >= F.nv)
		F.sel = F.nv - 1;
	if (F.sel < 0)
		F.sel = 0;
	F.target = fminf(F.target, fmaxf(0, F.total - VIEW_H));
	F.info_for = -1;
}

int main(int argc, char **argv)
{
	osSetSpeedupEnable(true);
	romfsInit();
	ptmuInit();
	util_battery_init();
	gfx_init();
	state_init();
	config_load();
	i18n_apply(G.lang);
	theme_set(!G.light);
	load_console_name();
	fsx_init();
	trash_init();
	aptHook(&hook, apt_hook, NULL);
	tl_start(); // la lista de títulos, ya en su hilo: al abrir Títulos está lista
	hidSetRepeatParameters(18, 5);

	server_set_enabled(!G.srv_off); // apagado desde Ajustes: se queda apagado al volver a abrir
	server_start();
	upd_init(argc > 0 ? argv[0] : NULL);
	dl_init(); // después de upd_init, que prepara curl
	cloud_init(); // usa el servicio HTTP que arranca dl_init
	ins_init();
	share_init(); // (contesta a otras consolas aunque no se abra Compartir)
	if (G.upd_auto)
		upd_check();
	U.splash = 0;
	U.qr_anim = G.qr_min ? 0 : 1;
	U.tab = TAB_ACTIVITY; // se abre como siempre, en Actividad
	U.want_rename = -1;

	u64 last_save = util_ms();
	wd_start();
	for (;;) {
		WHERE("aptMainLoop (HOME o reposo)");
		g_wd_pause = true; // (en HOME o en reposo no se dibuja: no es quedarse parada)
		bool run = aptMainLoop();
		g_wd_pause = false;
		if (!run || U.quit)
			break;
		g_frames++;
		g_app_now = hm_is_open() ? -1 : U.app;
		WHERE("leer botones");
		input_update();
		gfx_tick();

		// la copia se rehace si algo cambió, durante las transferencias y cada segundo
		// (para que «conectado ahora» esté al día)
		u64 now = util_ms();
		bool dirty = false;
		WHERE("copiar el estado (espera al servidor)");
		// con la pantalla de espera, sin esperar a otro hilo: si el estado está ocupado, se copia en la vuelta siguiente
		if (guard_on() ? state_trylock() : (state_lock(), true)) {
			if (G.rev != s_rev || xfer_active() || now - s_copy_t > 1000) {
				memcpy(&V, &G, sizeof G);
				s_rev = G.rev;
				s_copy_t = now;
			}
			dirty = G.dirty_cfg;
			state_unlock();
		}

		WHERE("avisos");
		watch_events();
		s_events_seen = true;
		int upd_was = U.upd.st;
		WHERE("actualizaciones");
		upd_get(&U.upd);
		// actualizada desde un archivo o desde GitHub: se ofrece reiniciar ya (si se abrió como CIA)
		if (U.upd.st == UPD_DONE && upd_was != UPD_DONE && !envIsHomebrew() && !g_dialog.open)
			dialog_open(DLG_UPDRESTART, _("Cloud3DS se ha actualizado"), _("Se usará la nueva versión al volver a abrirla. ¿Reiniciar Cloud3DS ahora?"),
			            _("Reiniciar"), false);
		static int ins_tick;
		WHERE("cola de instalación");
		if (U.ins_pend || ins_busy() || ++ins_tick % 20 == 0) { // la cola solo se copia entera cuando hace falta
			int before = U.ins_pend;
			ins_refresh();
			if (U.ins_pend > before && before == 0)
				U.buttons = true; // se decide con A, X o B
		}
		if (U.upd.st == UPD_AVAILABLE && U.upd_seen != U.upd.rev && !(U.tab == TAB_SETTINGS && U.page == PAGE_UPDATE)) {
			char msg[80];
			snprintf(msg, sizeof msg, _("Nueva versión %s disponible en Ajustes"), U.upd.latest);
			toast_icon(msg, IC_DOWNLOAD, T.ok);
		}
		if (U.upd.st == UPD_AVAILABLE)
			U.upd_seen = U.upd.rev;
		WHERE("tapa y reposo");
		lid_and_sleep();
		WHERE("Compartir: avisos");
		sh_prompts();
		WHERE("update");
		update();

		WHERE("empezar a dibujar");
		gfx_frame_begin();
		WHERE("pantalla de arriba");
		draw_top();
		WHERE("pantalla de abajo");
		draw_bottom();
		WHERE("terminar de dibujar");
		gfx_frame_end();

		if (dirty && now - last_save > 3000 && !xfer_active() && !guard_on()) {
			WHERE("guardar la configuración");
			state_lock();
			config_save();
			state_unlock();
			last_save = now;
		}
	}

	wd_stop();
	if (UN.busy) { // apagando a mitad de desinstalar: se espera a que AM termine
		threadJoin(UN.thr, U64_MAX);
		threadFree(UN.thr);
		aptSetHomeAllowed(true);
	}
	br_exit();
	cloud_exit();
	music_exit();
	mlib_exit();
	share_exit();
	dl_exit();
	ops_exit();
	th_exit();
	tw_exit();
	if (TL.thr) {
		threadJoin(TL.thr, U64_MAX);
		threadFree(TL.thr);
	}
	ins_exit();
	upd_exit();
	tex_free(&U.ins_icon);
	server_stop();
	state_lock();
	config_save();
	state_unlock();
	if (U.lid_off && R_SUCCEEDED(gspLcdInit())) {
		GSPLCD_PowerOnAllBacklights();
		gspLcdExit();
	}
	aptUnhook(&hook);
	fsx_exit();
	gfx_exit();
	util_battery_exit();
	ptmuExit();
	romfsExit();
	return 0;
}
