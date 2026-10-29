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
#include "i18n.h"
#include "ops.h"
#include "update.h"
#include "install.h"
#include "img.h"
#include "txt.h"
#include <time.h>
#include <stdlib.h>
#include <strings.h>
#include "server.h"
#include "state.h"
#include "ui.h"
#include "util.h"

#include <arpa/inet.h>
#include <math.h>
#include <stdio.h>
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
#define TABBAR_H 42
#define VIEW_Y   32
#define VIEW_H   (SCR_H - TABBAR_H - VIEW_Y)
#define QR_PAD   24 // el botón del QR sobresale de la barra
#define MINI_H   30 // tira de arriba con el QR cerrado

#define ABOUT_TEXT "Tu nube local para Nintendo 3DS. Gestiona la tarjeta SD desde el móvil o el PC con solo escanear un código QR: sin apps, sin cables y sin configuración. Tus archivos no salen de tu red local; solo se consulta GitHub para buscar actualizaciones."

enum { TAB_FILES, TAB_ACTIVITY, TAB_DEVICES, TAB_SETTINGS, TAB_COUNT };
enum { PAGE_MAIN, PAGE_LANG, PAGE_ABOUT, PAGE_PIN, PAGE_UPDATE };
enum { PIN_CUSTOM, PIN_RANDOM, PIN_COUNT };
enum { SET_READONLY, SET_PROTECT, SET_PIN, SET_LANG, SET_THEME, SET_UPDATE, SET_ABOUT, SET_EXIT, SET_COUNT };
enum { AB_APP, AB_GITHUB, AB_DISCORD, AB_KOFI, AB_COUNT };
enum { DLG_REVOKE = 1, DLG_NEWPIN, DLG_EXIT, DLG_CLEAR, DLG_UPDATE, DLG_FDELETE, DLG_FEXISTS, DLG_TXTDISCARD, DLG_TXTOVER };

typedef struct {
	float off, target;
	int sel;
} List;

static struct {
	int tab, page;
	float tab_anim;
	List list[TAB_COUNT + 4];   // una por pestaña y por subpágina de Ajustes
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
		snprintf(sub, sn, "%s · %s", _("Eliminado"), e->device);
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
		return _("GitHub ha limitado las consultas por ahora. Inténtalo de nuevo en unos minutos.");
	if (!strcmp(e, "no_release"))
		return _("Todavía no hay versiones publicadas.");
	if (!strcmp(e, "sha"))
		return _("El archivo descargado no coincide con su SHA-256 y no se ha instalado.");
	if (!strcmp(e, "am"))
		return _("La consola no pudo instalar la actualización. Prueba de nuevo o instálala con FBI.");
	if (!strcmp(e, "io"))
		return _("No se pudo escribir en la tarjeta SD.");
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
		text_wrap_lines(_("Esta versión no incluye el archivo de comprobación (Cloud3DS.sha256), así que no se puede instalar desde aquí."),
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
	text_wrap_lines(_("Cloud3DS busca versiones nuevas en GitHub. Antes de instalar, comprueba que el archivo descargado es el original (SHA-256)."),
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
	return !r->self && !(r->inst_ver >= 0 && r->inst_ver > r->ver);
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
		util_strlcpy(out, _("Es Cloud3DS: actualízala desde Ajustes → Actualizaciones."), n);
		*col = T.warn;
	} else if (r->inst_ver < 0) {
		snprintf(out, n, _("Versión %s · no está instalado"), a);
	} else if (r->inst_ver == r->ver) {
		snprintf(out, n, _("Versión %s · ya instalada (se reinstalará)"), a);
	} else if (r->inst_ver > r->ver) {
		snprintf(out, n, _("Versión %s · ya tienes una más nueva (%s). La consola no deja bajar de versión."), a,
		         ins_ver_text((u16)r->inst_ver, b, sizeof b));
		*col = T.warn;
	} else {
		snprintf(out, n, _("Versión %s · actualiza la %s instalada"), a, ins_ver_text((u16)r->inst_ver, b, sizeof b));
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
	float y0 = CARD_Y + MINI_H + 8, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 16, W = CARD_W - 32;
	float ix = X, iy = y0 + 14;
	draw_rrect(ix - 2, iy - 2, 76, 76, 16, col_alpha(T.surface2, a));
	if (U.ins_icon.ok)
		draw_texture(&U.ins_icon, ix + 2, iy + 2, 68, 68, a);
	else
		draw_icon(IC_GEM, ix + 36, iy + 36, 34, col_alpha(T.text3, a));
	float tx = ix + 88, tw = X + W - tx;
	text_draw_clip(_("Instalar en la consola"), tx, iy, tw, TS_SUB, col_alpha(T.accent, a));
	if (U.ins_pend > 1) {
		char more[48];
		snprintf(more, sizeof more, _("y %d más esperando"), U.ins_pend - 1);
		text_draw_right(more, X + W, iy, TS_SUB, col_alpha(T.text3, a));
	}
	text_wrap_lines(r->title, tw, TS_TITLE, 2, tx, iy + 16, col_alpha(T.text, a), true);
	char line[160], sz[24];
	util_fmt_size(sz, sizeof sz, r->size);
	snprintf(line, sizeof line, "%s%s%s · %s", r->publisher, r->publisher[0] ? " · " : "", sz, r->file);
	text_draw_clip(line, tx, iy + 56, tw, TS_SUB, col_alpha(T.text3, a));
	float y = iy + 84;
	draw_rect(X, y, W, 1, col_alpha(T.divider, a));
	u32 col;
	ins_version_line(r, line, sizeof line, &col);
	int nl = text_wrap_lines(line, W, TS_BODY, 2, X, y + 8, col_alpha(col, a), true);
	bool n3ds = false;
	APT_CheckNew3DS(&n3ds);
	if (r->n3ds_only && !n3ds && nl < 2)
		text_draw_clip(_("Este juego es solo para New 3DS."), X, y + 8 + text_lineh(TS_BODY), W, TS_BODY, col_alpha(T.warn, a));
	snprintf(line, sizeof line, _("ID %016llX · pedido desde %s"), (unsigned long long)r->tid, r->device);
	text_draw_clip(line, X, y0 + h - 22, W, TS_SUB, col_alpha(T.text3, a));
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
// (que lo borra); si ya estaba en la SD, solo «Instalar» o «No instalar»
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
	b[n++] = (InsBtn){GLYPH_A, U.ins_pend > 1 ? _("Instalar todos") : _("Instalar"), 0xFFFFFFFF, T.ok, can, INS_ACCEPT, KEY_A};
	if (up) {
		b[n++] = (InsBtn){GLYPH_X, _("Solo copiar a la SD"), T.text, T.surface2, true, INS_KEEP, KEY_X};
		b[n++] = (InsBtn){GLYPH_B, _("Cancelar la copia"), T.danger, col_alpha(T.danger, 0.16f), true, INS_REJECT, KEY_B};
	} else {
		b[n++] = (InsBtn){GLYPH_B, _("No instalar"), T.text, T.surface2, true, INS_REJECT, KEY_B};
	}
	return n;
}

static void install_card(void)
{
	U.ins_anim = approach(U.ins_anim, U.ins_pend ? 1 : 0, 14);
	if (U.ins_anim < 0.01f)
		return;
	const InsReq *r = ins_first_pending();
	float a = ease_out_cubic(U.ins_anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	float y = 26 + (1 - a) * 30;
	draw_rrect(10, y + 3, BOT_W - 20, SCR_H - y - 10, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, SCR_H - y - 10, 16, T.surface);
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
	InsBtn b[3];
	int n = ins_buttons(b);
	for (int k = 0; k < n; k++)
		ins_button(y + 76 + k * 44, b[k].glyph, b[k].label, b[k].fg, b[k].bg, b[k].on);
}

// botones de la tarjeta; devuelve true si se ha decidido algo
static bool install_input(void)
{
	if (!U.ins_pend || g_dialog.open)
		return false;
	InsBtn b[3];
	int n = ins_buttons(b), what = -1;
	for (int k = 0; k < n && what < 0; k++)
		if (b[k].on && ((g_kdown & b[k].key) || tap(INS_BX, 26 + 76 + k * 44, INS_BW, 36)))
			what = b[k].what;
	if (what < 0)
		return true; // la tarjeta tapa lo de debajo: no pasa nada más
	bool up = false;
	for (int i = 0; i < U.ins_n; i++)
		up |= U.ins[i].st == INS_PENDING && U.ins[i].uploaded;
	ins_decide(-1, what);
	ins_refresh();
	// el toque que decidió no sigue a lo de debajo (el botón del QR está justo bajo el último)
	g_kdown &= ~(KEY_A | KEY_B | KEY_X);
	g_touch.up = false;
	if (what == INS_ACCEPT)
		toast_icon(_("Instalando…"), IC_DOWNLOAD, T.ok);
	else if (what == INS_KEEP)
		toast_icon(_("Copiado a la SD sin instalar"), IC_FOLDER, T.accent);
	else if (up)
		toast_icon(_("Copia cancelada: se borró de la SD"), IC_TRASH, T.text3);
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
	int nl = text_wrap_lines(_("Ábrelo con la cámara. Tu móvil debe estar en la misma red Wi-Fi."), COL_W, TS_SUB, 3, x,
	                         y + 24, col_alpha(T.text3, a), true);
	// en japonés las líneas son más altas: la raya y lo que sigue bajan lo que haga falta (hasta el PIN)
	float dy2 = fmaxf(0, y + 24 + nl * text_lineh(TS_SUB) + 4 - (y + 66));
	draw_rect(x, y + 66 + dy2, COL_W, 1, col_alpha(T.divider, a));
	text_draw_clip(_("O escribe en el navegador:"), x, y + 74 + dy2, COL_W, TS_SUB, col_alpha(T.text3, a));
	char url[48];
	url_text(url, sizeof url, false);
	float s = 0.64f;
	while (s > 0.44f && text_width(url, s) > COL_W)
		s -= 0.02f;
	text_draw(url, x, y + 74 + dy2 + fmaxf(16, text_lineh(TS_SUB) + 2), s, col_alpha(T.accent, a));
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
	}
	if (x->st == XS_DONE) {
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
	text_draw_clip(x->name, x0, y + 32, w, TS_BODY, col_alpha(T.text, a));
	snprintf(line, sizeof line, x->kind != XF_DOWNLOAD ? _("de %s") : _("a %s"), x->device);
	text_draw_clip(line, x0, y + 52, w, TS_SUB, col_alpha(T.text3, a));
	brand_bar(x0, y + 72, w, 8, frac);
	util_fmt_size(d, sizeof d, x->done);
	util_fmt_size(t, sizeof t, x->total);
	snprintf(line, sizeof line, _("%s de %s"), d, t);
	text_draw_clip(line, x0, y + 86, w / 2, TS_SUB, col_alpha(T.text2, a));
	if (x->st == XS_RUNNING && x->speed > 0) {
		char sp[24], eta[24];
		util_fmt_speed(sp, sizeof sp, x->speed);
		util_fmt_eta(eta, sizeof eta, (x->total - x->done) / x->speed);
		snprintf(line, sizeof line, _("%s · quedan %s"), sp, eta);
		text_draw_right(line, x0 + w, y + 86, TS_SUB, col_alpha(T.text3, a));
	}
	if (n > 1) {
		snprintf(line, sizeof line, _("y %d más"), n - 1);
		text_draw_clip(line, x0, y + 104, w, TS_SUB, col_alpha(T.text3, a));
	}
}


// tira compacta: dirección, PIN y estado; el resto de la pantalla queda libre
static void xfer_head(const Xfer *x, const char **head, u32 *col, IconId *ic);

static void mini_strip(float a)
{
	// una sola línea: dirección y estado a la izquierda, PIN a la derecha
	float y = CARD_Y, cy = y + MINI_H / 2;
	draw_icon(IC_QR, CARD_X + 18, cy, 13, col_alpha(T.accent, a));
	char url[48], pin[16];
	url_text(url, sizeof url, false);
	const float sc = 0.52f;
	float lx = CARD_X + 34;
	text_draw(url, lx, cy - text_lineh(sc) / 2, sc, col_alpha(T.accent, a));
	snprintf(pin, sizeof pin, "%s", V.pin);
	float pw = text_width(pin, sc) + 16, px = CARD_X + CARD_W - 10 - pw;
	draw_rrect(px, y + 5, pw, MINI_H - 10, (MINI_H - 10) / 2, col_alpha(T.surface2, a));
	text_draw_center(pin, px + pw / 2, cy - text_lineh(sc) / 2, sc, col_alpha(T.text, a));
	float lw = text_width(_("PIN"), TS_SUB);
	text_draw(_("PIN"), px - 6 - lw, cy - text_lineh(TS_SUB) / 2, TS_SUB, col_alpha(T.text3, a));
	int on = online_count();
	if (on) {
		float sx = lx + text_width(url, sc) + 14;
		char n[8];
		snprintf(n, sizeof n, "%d", on);
		draw_icon(IC_DEVICES, sx + 8, cy, 14, col_alpha(T.ok, a));
		text_draw(n, sx + 19, cy - text_lineh(sc) / 2, sc, col_alpha(T.ok, a));
		lx = sx + 19 + text_width(n, sc);
	} else {
		lx += text_width(url, sc);
	}
	// lo que está en curso (instalar, copiar, recibir…): qué es y cuánto va, con una barra abajo
	const Xfer *xs[1];
	if (visible_xfers(xs, 1)) {
		const Xfer *x = xs[0];
		const char *hd;
		u32 col;
		IconId ic;
		xfer_head(x, &hd, &col, &ic);
		float f = x->st == XS_DONE ? 1 : x->total ? clampf((float)((double)x->done / x->total), 0, 1) : 0;
		char t[96];
		if (x->st == XS_RUNNING)
			snprintf(t, sizeof t, "%s %d%%", hd, (int)(f * 100));
		else
			util_strlcpy(t, hd, sizeof t);
		float x0 = lx + 16, x1 = px - 10 - lw - 8;
		if (x1 - x0 > 40) {
			draw_icon(ic, x0 + 7, cy, 12, col_alpha(col, a));
			text_draw_clip(t, x0 + 17, cy - text_lineh(TS_SUB) / 2, x1 - x0 - 17, TS_SUB, col_alpha(col, a));
		}
		draw_rect(CARD_X + 12, y + MINI_H - 4, CARD_W - 24, 2, col_alpha(T.surface2, a));
		draw_rect(CARD_X + 12, y + MINI_H - 4, (CARD_W - 24) * f, 2, col_alpha(col, a));
	}
}

// debajo de la tira: las transferencias a lo ancho o una pista para volver a abrir el QR
// debajo de la tira, con el QR cerrado: toda la cola (transferencias, copias e instalaciones)
static void mini_body(float a)
{
	if (a <= 0.01f)
		return;
	float y = CARD_Y + MINI_H + 10, h = SCR_H - 8 - y;
	const Xfer *xs[MAX_XFERS];
	int nx = visible_xfers(xs, MAX_XFERS);
	const InsReq *wait[MAX_INS];
	int nw = 0;
	for (int i = 0; i < U.ins_n; i++)
		if (U.ins[i].st == INS_PENDING || U.ins[i].st == INS_QUEUED)
			wait[nw++] = &U.ins[i];
	if (!nx && !nw) {
		draw_halo(TOP_W / 2, y + 44, 70, 0.25f * a);
		draw_cloud(TOP_W / 2, y + 44, 22, a);
		text_draw_center(_("Tu nube está lista"), TOP_W / 2, y + 76, TS_BODY, col_alpha(T.text2, a));
		text_draw_center(_("Pulsa SELECT para ver el código QR"), TOP_W / 2, y + 98, TS_SUB, col_alpha(T.text3, a));
		return;
	}
	if (nx == 1 && !nw) { // una sola: a lo grande
		draw_rrect(CARD_X, y + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
		draw_rrect(CARD_X, y, CARD_W, h, 16, col_alpha(T.surface, a));
		xfer_wide(CARD_X + 16, y + 14, CARD_W - 32, a);
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
		text_draw_clip(wait[i]->st == INS_PENDING ? _("Esperando a que lo confirmes") : _("En cola para instalar"), X + 30, ry + 17, W - 30,
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
		const char *desc[3] = {_("Código fuente y nuevas versiones. Si encuentras un error, abre un issue y lo revisaré."),
		                       _("Únete a la comunidad: soporte, ideas y novedades de cada versión."),
		                       _("Si Cloud3DS te resulta útil, puedes invitarme a un café. Me ayuda a seguir mejorándolo.")};
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
		snprintf(vb, sizeof vb, _("v%s · hecho por %s"), APP_VERSION, "VENOMSHiNE");
		text_draw_center(vb, LX, 178, TS_SUB, T.text2);
		text_draw(_("Gracias por usar!"), X, 18, TS_TITLE, T.text);
		// los dos textos tienen que caber antes de la licencia: si no (japonés), se reduce un poco la letra
		const char *t1 = _(ABOUT_TEXT), *t2 = _("¿Algún problema? Escríbeme por GitHub o Discord.");
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
	text_draw_center(_("hecho por VENOMSHiNE"), 160, 214, TS_SUB, col_alpha(T.text3, ta));
}

static void files_top(float a);
static void vw_top(void);
static bool vw_is_open(void);
static bool device_top(float a);

static void draw_top(void)
{
	gfx_target(g_top, T.bg);
	if (U.splash >= 0) {
		splash_top(1);
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
	if (about) {
		about_top();
	} else if (U.tab == TAB_SETTINGS && U.page == PAGE_UPDATE) {
		update_top();
	} else if (!strcmp(V.srv_error, "soc")) {
		// la red de la consola (soc:U) no arrancó: no es culpa del Wi-Fi, y se reintenta sola
		char msg[160];
		snprintf(msg, sizeof msg, _("El servicio de red de la consola no responde (error %08lX). Reintentando automáticamente…"),
		         (unsigned long)V.net_err);
		card_message(IC_REFRESH, T.warn, _("Iniciando la red"), msg);
	} else if (!V.ip && V.wifi == 3) {
		card_message(IC_WIFI, T.accent, _("Conectado al Wi-Fi"), _("Esperando a que el router asigne una dirección IP…"));
	} else if (!V.ip) {
		card_message(IC_WIFI_OFF, T.text3, _("Sin conexión Wi-Fi"),
		             _("Conecta la consola a una red Wi-Fi. Si no tienes router, puedes usar el punto de acceso de tu móvil."));
	} else if (V.srv_error[0]) {
		card_message(IC_INFO, T.warn, _("No se pudo iniciar el servidor"),
		             _("Cierra otras aplicaciones de red (como FTPD) y vuelve a abrir Cloud3DS."));
	} else if (!V.running) {
		card();
		draw_arc(TOP_W / 2, CARD_Y + 80, 16, 3, g_time * 6, g_time * 6 + 4.2f, T.accent);
		text_draw_center(_("Preparando tu nube…"), TOP_W / 2, CARD_Y + 114, TS_BODY, T.text3);
	} else {
		// la tarjeta se encoge hasta la tira compacta (SELECT o el botón del QR abajo)
		U.qr_anim = approach(U.qr_anim, V.qr_min ? 0 : 1, 10);
		float k = ease_in_out(U.qr_anim), full = clampf(k * 1.6f - 0.6f, 0, 1), mini = clampf(1 - k * 1.6f, 0, 1);
		float h = MINI_H + (CARD_H - MINI_H) * k;
		draw_rrect(CARD_X, CARD_Y + 3, CARD_W, h, 16, T.shadow);
		draw_rrect(CARD_X, CARD_Y, CARD_W, h, 16, T.surface);
		gfx_clip(CARD_X, CARD_Y, CARD_W, h);
		if (full > 0.01f) {
			char url[64], qr[96];
			url_text(url, sizeof url, true);
			snprintf(qr, sizeof qr, "%s/#k=%s", url, V.qr_key);
			// con el QR abierto, solo el QR y cómo conectarse: lo demás se ve con el QR cerrado
			draw_qr(qr, QR_X, QR_Y, QR_S, full);
			connect_col(full, 0);
			draw_status(COL_X, QR_Y + QR_S - 12, COL_W, full);
		}
		if (mini > 0.01f)
			mini_strip(mini);
		gfx_clip_off();
		if (mini > 0.01f && U.ins_pend)
			install_top(mini);
		else if (U.tab == TAB_FILES && mini > 0.01f)
			files_top(mini);
		else if (!(U.tab == TAB_DEVICES && mini > 0.01f && device_top(mini)))
			mini_body(mini);
	}
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
	return &U.list[U.tab];
}

static void list_update(List *l, int count, float rh)
{
	float max = count * rh + QR_PAD - VIEW_H; // al final, sitio para que la última fila no quede bajo el botón del QR
	if (max < 0)
		max = 0;
	if (g_touch.held && g_touch.drag && g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H) {
		l->target -= g_touch.y - g_touch.py;
		l->off = l->target;
	}
	if (U.buttons && !g_dialog.open) {
		if (g_krepeat & KEY_DOWN) l->sel++;
		if (g_krepeat & KEY_UP) l->sel--;
		if (l->sel >= count) l->sel = count - 1;
		if (l->sel < 0) l->sel = 0;
		if (g_krepeat & (KEY_UP | KEY_DOWN)) {
			float y = l->sel * rh;
			if (y < l->target) l->target = y;
			if (y + rh > l->target + VIEW_H) l->target = y + rh - VIEW_H;
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

static void tab_header(const char *title, bool back)
{
	float x = 14;
	if (back) {
		if (touch_in(0, 0, 170, VIEW_Y))
			draw_rrect(6, 4, fminf(text_width(title, TS_TITLE), 190) + 36, 24, 12, T.surface);
		draw_icon(IC_CHEV_LEFT, 20, 16, 14, T.text2);
		x = 32;
	}
	text_draw_clip(title, x, 7, 190, TS_TITLE, T.text);
}

static void empty_state(IconId ic, const char *title, const char *text)
{
	float cy = VIEW_Y + VIEW_H / 2 - 26;
	draw_circle(BOT_W / 2, cy, 24, T.surface);
	draw_icon(ic, BOT_W / 2, cy, 24, T.text3);
	text_draw_center(title, BOT_W / 2, cy + 32, TS_BODY, T.text);
	wrap_center(text, BOT_W / 2, cy + 52, 260, TS_SUB, 3, T.text3);
}

static void tab_activity(void)
{
	List *l = cur_list();
	tab_header(_("Actividad"), false);
	float hx = BOT_W - 14;
	if (V.hist_n) {
		// borrar el historial: siempre a mano, arriba a la derecha (también con Y)
		float cx = BOT_W - 24, cy = 16;
		bool p = touch_in(cx - 14, cy - 14, 28, 28);
		draw_circle(cx, cy, 12, p ? col_alpha(T.danger, 0.35f) : T.surface);
		draw_icon(IC_TRASH, cx, cy, 14, T.danger);
		if (!g_dialog.open && (tap(cx - 14, cy - 14, 28, 28) || (g_kdown & KEY_Y)))
			dialog_open(DLG_CLEAR, _("¿Borrar el historial?"), _("Se borrará la lista de actividad. Tus archivos no se modifican."),
			            _("Borrar"), true);
		hx = cx - 20;
	}
	if (V.stat_up || V.stat_down) {
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
	int rows = n;
	list_update(l, rows, RH);
	if (!n) {
		empty_state(IC_CLOUD, _("Todo listo"), _("Aquí verás lo que subas o descargues desde tu móvil o tu PC."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < rows; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		row(l, i, y, RH);
		const Event *e = ev(i);
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
}

static void ask_revoke(int d)
{
	char msg[200];
	snprintf(msg, sizeof msg, _("%s tendrá que volver a escanear el QR o escribir el PIN."), V.dev[d].name);
	U.dlg_arg = d;
	dialog_open(DLG_REVOKE, _("¿Desvincular este dispositivo?"), msg, _("Desvincular"), true);
}

static void tab_devices(void)
{
	List *l = cur_list();
	int idx[MAX_DEVICES];
	int n = device_order(idx);
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
		// a la derecha, Renombrar (lápiz) y Desvincular; se comprueban antes que la fila
		float bw = 74, bh = 26, bx = BOT_W - 14 - bw, by = y + (RH - bh) / 2;
		float ex = bx - 8 - bh;
		bool vis = g_touch.sy >= VIEW_Y && !g_dialog.open;
		bool bt = vis && tap(bx, by, bw, bh);
		bool et = vis && tap(ex, by, bh, bh);
		bool rt = false;
		if (!bt && !et)
			rt = row(l, i, y, RH);
		if (!U.buttons && !strcmp(U.dev_tok, d->token))
			draw_rrect(6, y + 1, BOT_W - 12, RH - 2, 10, T.surface);
		draw_circle(28, y + RH / 2, 13, T.surface2);
		draw_icon(dev_icon(d), 28, y + RH / 2, 16, on ? T.ok : T.text3);
		if (on) {
			draw_circle(38, y + RH / 2 + 9, 4.5f, T.bg);
			draw_circle(38, y + RH / 2 + 9, 3, T.ok);
		}
		text_draw_clip(d->name, 50, y + 7, ex - 58, TS_BODY, T.text);
		// debajo, el estado y el modelo si el nombre ya no lo dice («iPhone de Mario» · iPhone · Safari)
		char sub[128], st[64], when[24];
		if (on)
			util_strlcpy(st, _("Conectado ahora"), sizeof st);
		else {
			util_fmt_when(when, sizeof when, d->last_seen);
			snprintf(st, sizeof st, _("Visto: %s"), when);
		}
		if (strcmp(d->name, d->model) && d->model[0])
			snprintf(sub, sizeof sub, "%s · %s", st, d->model);
		else
			snprintf(sub, sizeof sub, "%s · %s", st, d->ip);
		text_draw_clip(sub, 50, y + 24, ex - 58, TS_SUB, on ? T.ok : T.text3);
		bool ep = touch_in(ex, by, bh, bh);
		draw_circle(ex + bh / 2, by + bh / 2, bh / 2, ep ? T.divider : T.surface2);
		draw_icon(IC_PEN, ex + bh / 2, by + bh / 2, 13, T.text2);
		bool bp = touch_in(bx, by, bw, bh);
		draw_rrect(bx, by, bw, bh, bh / 2, col_alpha(T.danger, bp ? 0.35f : 0.16f));
		text_draw_center(_("Desvincular"), bx + bw / 2, by + (bh - text_lineh(TS_SUB)) / 2, TS_SUB, T.danger);
		// con botones, el elegido es el resaltado; Y renombra y X desvincula
		bool mine = U.buttons && !g_dialog.open && l->sel == i;
		if (bt || (mine && (g_kdown & KEY_X)))
			ask_revoke(idx[i]);
		else if (et || (mine && (g_kdown & KEY_Y)))
			U.want_rename = idx[i];
		if (rt || mine)
			util_strlcpy(U.dev_tok, d->token, sizeof U.dev_tok);
	}
	gfx_clip_off();
}

// ------------------------------------------------------------ explorador (pestaña Archivos)
#define F_MAX 2000
typedef struct {
	char name[128];
	bool dir, sys;
	u8 color;
	u64 size, mtime;
} FEnt;
enum { FV_CHIPS, FV_ITEM, FV_SYSHDR };
typedef struct {
	u8 kind;
	short idx;
	float y, h;
} FView;
enum { CH_HOME, CH_SHOTS, CH_PHOTOS, CH_CIAS, CH_HB, CH_SAVES, CH_COUNT };
static const char *const CHIP_PATH[CH_COUNT] = {"/", "/luma/screenshots", "/DCIM", "/cias", "/3ds", "@saves"};
static const char *const SAVE_DIRS[3] = {"/3ds/Checkpoint/saves", "/JKSV/Saves", "/JKSV"};

static struct {
	char path[512];
	FEnt *e;
	int n;
	bool err, truncated;
	FView v[F_MAX + 2];
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
	char preview[600]; // principio de un texto, para verlo arriba
} F = {.path = "/", .sys_open = true, .info_for = -1, .sel = 1};

static const char *chip_name(int i)
{
	switch (i) {
	case CH_HOME: return _("Inicio");
	case CH_SHOTS: return _("Capturas");
	case CH_PHOTOS: return _("Fotos");
	case CH_CIAS: return _("CIAs");
	case CH_HB: return _("Homebrew");
	default: return _("Partidas");
	}
}

static IconId chip_icon(int i)
{
	static const IconId IC[CH_COUNT] = {IC_CLOUD, IC_CAMERA, IC_IMAGE, IC_GEM, IC_CODE, IC_STAR};
	return IC[i];
}

static void f_full(int i, char *out, size_t n)
{
	snprintf(out, n, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", F.e[i].name);
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
} FType;
static const FType FTYPES[] = {
	{"png", FK_IMG}, {"jpg", FK_IMG}, {"jpeg", FK_IMG}, {"bmp", FK_IMG}, {"gif", FK_IMG}, {"tga", FK_IMG},
	{"mpo", FK_IMG, N_("Foto 3D (MPO)")},
	{"mp4", FK_VIDEO}, {"mov", FK_VIDEO}, {"avi", FK_VIDEO}, {"mkv", FK_VIDEO}, {"webm", FK_VIDEO}, {"3gp", FK_VIDEO},
	{"moflex", FK_VIDEO, N_("Vídeo de 3DS (MOFLEX)")},
	{"mp3", FK_AUDIO}, {"wav", FK_AUDIO}, {"ogg", FK_AUDIO}, {"m4a", FK_AUDIO}, {"flac", FK_AUDIO}, {"aac", FK_AUDIO},
	{"opus", FK_AUDIO}, {"mid", FK_AUDIO}, {"midi", FK_AUDIO},
	{"bcstm", FK_AUDIO, N_("Sonido de 3DS (BCSTM)")}, {"bcwav", FK_AUDIO, N_("Sonido de 3DS (BCWAV)")},
	{"cia", FK_APP, N_("Programa instalable (CIA)")}, {"3dsx", FK_APP, N_("Homebrew (3DSX)")},
	{"3ds", FK_APP, N_("Copia de un cartucho de 3DS")}, {"cci", FK_APP, N_("Copia de un cartucho de 3DS")},
	{"cxi", FK_APP, N_("Programa de 3DS (CXI)")}, {"firm", FK_APP, N_("Firmware (FIRM)")},
	{"tik", FK_APP, N_("Ticket (TIK)")}, {"smdh", FK_APP, N_("Icono de un programa (SMDH)")},
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
	{"py", FK_TEXT}, {"sh", FK_TEXT}, {"c", FK_TEXT}, {"h", FK_TEXT}, {"js", FK_TEXT}, {"css", FK_TEXT},
	{"html", FK_TEXT}, {"htm", FK_TEXT},
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

static int f_cmp(const void *a, const void *b)
{
	const FEnt *x = a, *y = b;
	if (x->dir != y->dir)
		return y->dir - x->dir;
	if (!!x->color != !!y->color)
		return !!y->color - !!x->color;
	return strcasecmp(x->name, y->name);
}

// filas que se dibujan: accesos rápidos, lo del usuario y, aparte, lo del sistema
static void f_build_view(void)
{
	F.nv = 0;
	float y = 0;
	F.v[F.nv++] = (FView){FV_CHIPS, -1, y, 44};
	y += 44;
	int nsys = 0;
	for (int i = 0; i < F.n; i++) {
		if (F.e[i].sys) {
			nsys++;
			continue;
		}
		F.v[F.nv++] = (FView){FV_ITEM, (short)i, y, 36};
		y += 36;
	}
	if (nsys) {
		F.v[F.nv++] = (FView){FV_SYSHDR, (short)nsys, y, 28};
		y += 28;
		if (F.sys_open)
			for (int i = 0; i < F.n; i++)
				if (F.e[i].sys) {
					F.v[F.nv++] = (FView){FV_ITEM, (short)i, y, 36};
					y += 36;
				}
	}
	F.total = y + QR_PAD;
}

static void f_load(const char *path)
{
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
	F.err = fsx_list(path, true, f_collect, NULL) < 0;
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
	qsort(F.e, F.n, sizeof *F.e, f_cmp);
	f_build_view();
	if (!same) {
		F.sel = F.nv > 1 ? 1 : 0;
		F.off = F.target = 0;
	}
	if (F.sel >= F.nv)
		F.sel = F.nv - 1;
	F.info_for = -1;
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
	f_load(p);
}

static void f_up(void)
{
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

static const FEnt *f_selected(void)
{
	return F.sel >= 0 && F.sel < F.nv && F.v[F.sel].kind == FV_ITEM ? &F.e[F.v[F.sel].idx] : NULL;
}

// SMDH de un .3dsx (nombre, autor e icono), si lo lleva
static bool read_3dsx(const char *path, char *title, size_t tn, char *pub, size_t pn, Texture *icon)
{
	char sd[720];
	snprintf(sd, sizeof sd, "sdmc:%s", path);
	FILE *f = fopen(sd, "rb");
	if (!f)
		return false;
	u8 h[0x28];
	bool ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "3DSX", 4) && (h[4] | h[5] << 8) > 32;
	u8 *smdh = NULL;
	if (ok) {
		u32 off = h[32] | h[33] << 8 | h[34] << 16 | (u32)h[35] << 24, sz = h[36] | h[37] << 8 | h[38] << 16 | (u32)h[39] << 24;
		ok = sz >= 0x36C0 && (smdh = malloc(0x36C0)) && !fseek(f, off, SEEK_SET) && fread(smdh, 1, 0x36C0, f) == 0x36C0 &&
		     !memcmp(smdh, "SMDH", 4);
	}
	fclose(f);
	if (ok) {
		int l = g_lang == LANG_JA ? 0 : g_lang == LANG_ES ? 5 : 1;
		const u8 *ti = smdh + 8 + l * 0x200;
		if (!ti[0] && !ti[1])
			ti = smdh + 8 + 0x200;
		u16 tmp[0x81] = {0};
		for (int i = 0; i < 0x40; i++)
			tmp[i] = ti[i * 2] | ti[i * 2 + 1] << 8;
		util_utf16_to_utf8(title, tn, tmp, 0x40);
		for (int i = 0; i < 0x40; i++)
			tmp[i] = ti[0x180 + i * 2] | ti[0x180 + i * 2 + 1] << 8;
		util_utf16_to_utf8(pub, pn, tmp, 0x40);
		static u16 px[48 * 48];
		const u16 *ic = (const u16 *)(smdh + 0x24C0);
		static const u8 tbl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
		for (int y = 0; y < 48; y++)
			for (int x = 0; x < 48; x++)
				px[y * 48 + x] = ic[((y >> 3) * 6 + (x >> 3)) * 64 + (tbl[x & 7] | tbl[y & 7] << 1)];
		tex_from_rgb565(icon, px, 48, 48);
	}
	free(smdh);
	return ok;
}

static bool count_cb(const FsxEntry *e, void *u)
{
	(void)e;
	return ++*(int *)u < 100000;
}

static bool f_is_img(const FEnt *e);
static bool f_is_txt(const FEnt *e);

// lee lo que hace falta para la pantalla de arriba: icono y datos del CIA o .3dsx, foto en pequeño…
static void f_info_refresh(void)
{
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
		int n = 0;
		if (fsx_list(full, false, count_cb, &n) >= 0)
			F.dir_count = n;
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

// mover o copiar: carpeta de destino que se está eligiendo
static struct {
	int mode;          // 0 nada, FA_MOVE o FA_COPY: eligiendo carpeta de destino
	char src[700], name[128];
	bool src_dir;
} FP;

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
} VW;

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

static void vw_close(void)
{
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
		dialog_open(DLG_TXTDISCARD, _("¿Salir sin guardar?"), _("Los cambios que hiciste en este archivo se perderán."), _("Salir sin guardar"), true);
	else
		vw_close();
}

static void vw_load(int idx)
{
	char full[700];
	f_full(idx, full, sizeof full);
	tex_free(&VW.tex);
	txt_free(&VW.doc);
	VW.idx = idx;
	VW.t = 0;
	if (f_is_img(&F.e[idx])) {
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
	VW.drag = 0;
	util_strlcpy(VW.path, full, sizeof VW.path);
	int r = txt_load(&VW.doc, full);
	if (r != TXT_OK) {
		toast_icon(r == TXT_ERR_MEM ? _("No hay memoria para abrir este archivo") : _("No se pudo abrir este archivo"), IC_CLOSE, T.danger);
		return;
	}
	VW.kind = VW_TXT;
	VW.prot = V.protect_sys && fsx_is_system(full);
}

// fotos de la carpeta, para pasar con ← →
static void vw_step(int d)
{
	if (VW.kind != VW_IMG)
		return;
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

static int vt_rows(int i, float w)
{
	static char b[2400];
	vt_line(i, b, sizeof b);
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
		int r = text_wrap_lines(b, w, TS_SUB, VT_MAXROWS, 0, 0, 0, false);
		bool here = i == VW.cur;
		if (here)
			draw_rect(0, y - 1, TOP_W, r * lh + 2, col_alpha(T.accent, 0.16f));
		char num[12];
		snprintf(num, sizeof num, "%d", i + 1);
		text_draw_right(num, gw - 8, y, TS_SUB, here ? T.accent : T.text3);
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
			text_draw_center(VW.res == IMG_TOO_BIG ? _("Esta foto es demasiado grande para verla en la consola") : _("No se pudo abrir esta foto"),
			                 TOP_W / 2, SCR_H / 2 - 8, TS_BODY, 0xFFAAAAAA);
		// pie con el nombre, que se desvanece a los 3 s
		float a = clampf((3.5f - VW.t) / 0.5f, 0, 1);
		if (a > 0.01f) {
			draw_rect(0, SCR_H - 26, TOP_W, 26, col_alpha(0xFF000000, 0.55f * a));
			char cap[200];
			if (VW.res == IMG_OK)
				snprintf(cap, sizeof cap, "%s · %d × %d", F.e[VW.idx].name, VW.ow, VW.oh);
			else
				util_strlcpy(cap, F.e[VW.idx].name, sizeof cap);
			text_draw_center_clip(cap, TOP_W / 2, SCR_H - 21, TOP_W - 20, TS_SUB, col_alpha(0xFFFFFFFF, a));
		}
		return;
	}
	vt_draw();
}

// botón grande con icono y texto; true si se tocó
static bool vw_button(float x, float y, float w, float h, IconId ic, const char *label, bool on)
{
	bool p = on && touch_in(x, y, w, h);
	draw_rrect(x, y, w, h, 12, p ? T.surface2 : T.surface);
	u32 c = on ? T.text2 : col_alpha(T.text3, 0.5f);
	draw_icon(ic, x + w / 2, y + h / 2 - 7, 15, c);
	text_draw_center_clip(label, x + w / 2, y + h / 2 + 4, w - 6, TS_SUB, on ? T.text3 : col_alpha(T.text3, 0.5f));
	return on && tap(x, y, w, h);
}

static void vw_save(bool force);

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
		util_strlcpy(sub, VW.doc.cut ? _("Solo lectura: archivo muy grande, se ve el principio") : _("Solo lectura: archivo muy grande"), sizeof sub), sc = T.warn;
	else if (VW.doc.dirty)
		snprintf(sub, sizeof sub, _("Línea %d de %d · Sin guardar"), VW.cur + 1, n), sc = T.warn;
	else
		snprintf(sub, sizeof sub, _("Línea %d de %d"), VW.cur + 1, n);
	text_draw_clip(sub, 60, 32, BOT_W - 74, TS_SUB, sc);
	// las líneas de alrededor: tocar una la elige; tocar la elegida la edita; arrastrar desplaza
	const float LY = 58, RH = 21;
	const int LR = 4;
	int first = VW.cur - 1;
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
		text_draw_right(num, 16 + gw, y + 3, TS_SUB, i == VW.cur ? T.accent : T.text3);
		vt_line(i, b, 400);
		text_draw_clip(b, 24 + gw, y + 3, BOT_W - 48 - gw, TS_SUB, T.text);
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
	// botones
	const float BY = LY + LR * RH + 10, BH = 40;
	char t1[48], t2[48], t3[48], t4[48];
	int hit = -1;
	if (ed) {
		snprintf(t1, sizeof t1, "%s %s", GLYPH_A, _("Editar"));
		snprintf(t2, sizeof t2, "%s %s", GLYPH_X, _("Añadir"));
		util_strlcpy(t3, _("Borrar"), sizeof t3);
		snprintf(t4, sizeof t4, "%s %s", GLYPH_Y, _("Opciones"));
		const char *T4[4] = {t1, t2, t3, t4};
		static const IconId I4[4] = {IC_PEN, IC_PLUS, IC_TRASH, IC_MENU};
		float bw = (BOT_W - 20 - 3 * 6) / 4.0f;
		for (int k = 0; k < 4; k++)
			if (vw_button(10 + k * (bw + 6), BY, bw, BH, I4[k], T4[k], true))
				hit = k;
	} else {
		snprintf(t1, sizeof t1, "%s %s", GLYPH_DPAD, _("Anterior"));
		snprintf(t2, sizeof t2, "%s %s", GLYPH_DPAD, _("Siguiente"));
		snprintf(t4, sizeof t4, "%s %s", GLYPH_Y, _("Opciones"));
		float bw = (BOT_W - 20 - 2 * 6) / 3.0f;
		if (vw_button(10, BY, bw, BH, IC_UP, _("Página anterior"), true))
			hit = 10;
		if (vw_button(10 + bw + 6, BY, bw, BH, IC_DOWN, _("Página siguiente"), true))
			hit = 11;
		if (vw_button(10 + 2 * (bw + 6), BY, bw, BH, IC_MENU, t4, true))
			hit = 3;
	}
	// abajo: guardar (si se puede) y cerrar
	float cy = BY + BH + 6, ch = SCR_H - 6 - cy;
	char cl[48], sv[48];
	snprintf(cl, sizeof cl, "%s %s", GLYPH_B, _("Cerrar"));
	snprintf(sv, sizeof sv, "START %s", _("Guardar"));
	bool save = false, close = false;
	if (ed) {
		float hw = (BOT_W - 26) / 2;
		bool dirty = VW.doc.dirty;
		bool sp = dirty && touch_in(10, cy, hw, ch);
		draw_rrect(10, cy, hw, ch, ch / 2, dirty ? (sp ? col_lerp(T.accent, T.text, 0.15f) : T.accent) : T.surface);
		text_draw_center(sv, 10 + hw / 2, cy + (ch - text_lineh(TS_BODY)) / 2, TS_BODY, dirty ? 0xFFFFFFFF : col_alpha(T.text3, 0.6f));
		save = dirty && tap(10, cy, hw, ch);
		bool cp = touch_in(16 + hw, cy, hw, ch);
		draw_rrect(16 + hw, cy, hw, ch, ch / 2, cp ? T.surface2 : T.surface);
		text_draw_center(cl, 16 + hw + hw / 2, cy + (ch - text_lineh(TS_BODY)) / 2, TS_BODY, T.text);
		close = tap(16 + hw, cy, hw, ch);
	} else {
		bool cp = touch_in(10, cy, BOT_W - 20, ch);
		draw_rrect(10, cy, BOT_W - 20, ch, ch / 2, cp ? T.surface2 : T.surface);
		text_draw_center(cl, BOT_W / 2, cy + (ch - text_lineh(TS_BODY)) / 2, TS_BODY, T.text);
		close = tap(10, cy, BOT_W - 20, ch);
	}
	// botones de la consola
	int page = vt_visible() - 1;
	if (g_krepeat & KEY_UP)
		VW.cur--;
	if (g_krepeat & KEY_DOWN)
		VW.cur++;
	if ((g_krepeat & KEY_LEFT) || hit == 10)
		VW.cur -= page, VW.top -= page;
	if ((g_krepeat & KEY_RIGHT) || hit == 11)
		VW.cur += page, VW.top += page;
	VW.cur = VW.cur < 0 ? 0 : VW.cur >= n ? n - 1 : VW.cur;
	VW.top = VW.top < 0 ? 0 : VW.top > VW.cur ? VW.cur : VW.top;
	if (ed && (hit == 0 || edit >= 0 || (g_kdown & KEY_A)))
		U.want_txt = 1;
	else if (ed && (hit == 1 || (g_kdown & KEY_X)))
		U.want_txt = 2;
	else if (ed && hit == 2) {
		txt_remove(&VW.doc, VW.cur);
		if (VW.cur >= VW.doc.n)
			VW.cur = VW.doc.n - 1;
		toast_icon(_("Línea borrada"), IC_TRASH, T.text3);
	} else if (hit == 3 || (g_kdown & KEY_Y)) {
		g_kdown &= ~KEY_Y;
		f_menu_open();
	} else if (save || (ed && VW.doc.dirty && (g_kdown & KEY_START))) {
		vw_save(false);
	} else if (close || (g_kdown & KEY_B)) {
		vw_try_close();
	}
	g_kdown &= ~KEY_START; // en el editor, START guarda: no sale de la app
}

static void vw_bottom(void)
{
	draw_rect(0, 0, BOT_W, SCR_H, T.bg);
	const FEnt *e = &F.e[VW.idx];
	IconId ic;
	u32 col;
	f_kind(e, &ic, &col);
	draw_rrect(14, 14, 36, 36, 10, col_alpha(col, 0.18f));
	draw_icon(ic, 32, 32, 18, col);
	text_draw_clip(e->name, 60, 14, BOT_W - 74, TS_BODY, T.text);
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
		f_menu_open();
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
		dialog_open(DLG_TXTOVER, _("El archivo cambió mientras lo editabas"),
		            _("Se modificó desde otro dispositivo. Si guardas, esos cambios se perderán."), _("Guardar igualmente"), true);
		return;
	}
	if (txt_save(&VW.doc, VW.path) != TXT_OK) {
		toast_icon(_("No se pudo guardar"), IC_CLOSE, T.danger);
		return;
	}
	if (fsx_stat(VW.path, &d, &sz) == 0)
		VW.doc.size = sz;
	state_lock();
	event_add(EV_EDIT, F.e[VW.idx].name, G.console, sz);
	G.rev++;
	state_unlock();
	toast_icon(_("Guardado"), IC_CHECK, T.ok);
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
	} else if (f_is_img(e) || f_is_txt(e)) {
		vw_load(F.v[vi].idx);
	} else if (!strcasecmp(f_ext(e->name), "cia") && strlen(full) < 256) {
		char err[24] = "";
		if (ins_request(full, G.console, false, err, sizeof err) < 0)
			toast_icon(!strcmp(err, "system_title") ? _("Por seguridad, no se instalan títulos del sistema") : !strcmp(err, "already") ? _("Ya está esperando en la consola")
			                                                                                                  : _("La consola no reconoce este CIA"),
			           IC_CLOSE, T.danger);
		else
			ins_refresh(); // sale la tarjeta de instalar, con su icono y su versión
	} else {
		f_menu_open();
	}
	return true;
}

static void f_new_folder(void)
{
	U.want_fnew = true;
}

static void tab_files(void)
{
	if (!F.e)
		f_load("/");
	if (F.need_reload)
		f_reload_keep();
	// cabecera: atrás, la carpeta actual y, a la derecha, «nueva carpeta»
	bool root = !strcmp(F.path, "/");
	const char *nm = F.nosaves ? _("Partidas") : root ? "SD" : strrchr(F.path, '/') + 1;
	tab_header(nm, !root || F.nosaves);
	float nx = BOT_W - 24, ny = 16;
	bool np = touch_in(nx - 14, ny - 14, 28, 28);
	if (!F.missing && !F.nosaves) {
		draw_circle(nx, ny, 12, np ? T.surface2 : T.surface);
		draw_icon(IC_FOLDER, nx - 1, ny + 1, 13, T.accent);
		draw_circle(nx + 6, ny - 5, 5, T.accent);
		draw_icon(IC_PLUS, nx + 6, ny - 5, 7, 0xFFFFFFFF);
		char cnt[32];
		snprintf(cnt, sizeof cnt, F.n == 1 ? _("1 elemento") : _("%d elementos"), F.n);
		text_draw_right(cnt, nx - 20, 11, TS_SUB, T.text3);
		if (!g_dialog.open && (tap(nx - 14, ny - 14, 28, 28) || (U.buttons && !FP.mode && (g_kdown & KEY_X)))) {
			f_new_folder();
			return;
		}
	}
	if ((!root || F.nosaves) && !g_dialog.open && tap(0, 0, 170, VIEW_Y)) {
		if (F.nosaves)
			f_reload_keep();
		else
			f_up();
		return;
	}
	// desplazamiento con el dedo o siguiendo la selección
	float max = fmaxf(0, F.total - VIEW_H);
	if (g_touch.held && g_touch.drag && g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H) {
		F.target -= g_touch.y - g_touch.py;
		F.off = F.target;
	}
	if (U.buttons && !g_dialog.open && (g_krepeat & (KEY_UP | KEY_DOWN))) {
		F.sel += (g_krepeat & KEY_DOWN) ? 1 : -1;
		F.sel = F.sel < 0 ? 0 : F.sel >= F.nv ? F.nv - 1 : F.sel;
		const FView *v = &F.v[F.sel];
		if (v->y < F.target)
			F.target = v->y;
		if (v->y + v->h > F.target + VIEW_H)
			F.target = v->y + v->h - VIEW_H;
	}
	if (U.buttons && !g_dialog.open && F.sel == 0 && (g_krepeat & (KEY_LEFT | KEY_RIGHT)))
		F.chip = (F.chip + ((g_krepeat & KEY_RIGHT) ? 1 : CH_COUNT - 1)) % CH_COUNT;
	F.target = clampf(F.target, 0, max);
	F.off = approach(F.off, F.target, 18);

	bool inview = g_touch.sy >= VIEW_Y && g_touch.sy < VIEW_Y + VIEW_H && !g_dialog.open;
	int open = -1, menu = -1; // fila que se abre o cuyo menú se pide
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < F.nv; i++) {
		const FView *v = &F.v[i];
		float y = VIEW_Y + v->y - F.off;
		if (y + v->h < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		if (v->kind == FV_CHIPS) {
			float cw = (BOT_W - 12) / (float)CH_COUNT;
			for (int c = 0; c < CH_COUNT; c++) {
				float cx = 6 + c * cw;
				bool here = c == CH_SAVES ? (F.nosaves || strstr(F.path, "/Checkpoint/saves") || !strncasecmp(F.path, "/JKSV", 5))
				                          : !F.nosaves && !strcasecmp(F.path, CHIP_PATH[c]);
				bool hl = here || (U.buttons && F.sel == 0 && F.chip == c) || (inview && touch_in(cx, y + 2, cw, 40));
				if (hl)
					draw_rrect(cx + 2, y + 3, cw - 4, 38, 10, here ? col_alpha(T.accent, 0.18f) : T.surface);
				draw_icon(chip_icon(c), cx + cw / 2, y + 15, 16, here ? T.accent : T.text2);
				float lw = fminf(text_width(chip_name(c), 0.32f), cw - 4);
				text_draw_clip(chip_name(c), cx + (cw - lw) / 2, y + 26, cw - 4, 0.32f, here ? T.accent : T.text3);
				if (inview && tap(cx, y + 2, cw, 40))
					f_open_chip(c);
			}
			if (U.buttons && F.sel == 0 && !g_dialog.open && (g_kdown & KEY_A))
				f_open_chip(F.chip);
			continue;
		}
		bool pressed = inview && touch_in(6, y, BOT_W - 12, v->h);
		bool selected = i == F.sel && (U.buttons || f_selected());
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
		// a la derecha, «⋮» para las opciones (como en la web); se comprueba antes que la fila
		float mx = BOT_W - 30;
		bool mp = inview && touch_in(mx - 4, y, 34, v->h);
		if (pressed || selected)
			draw_rrect(6, y + 1, BOT_W - 12, v->h - 2, 10, selected ? col_alpha(T.accent, 0.14f) : T.surface);
		if (e->color) // destacada: una raya de su color a la izquierda, como en la web
			draw_rrect(6, y + 5, 3, v->h - 10, 1.5f, col);
		draw_rrect(14, y + 5, 26, 26, 8, col_alpha(col, 0.18f));
		draw_icon(ic, 27, y + 18, 15, col);
		if (e->sys && V.protect_sys) {
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
		text_draw_clip(e->name, 48, y + 3, mx - 58, TS_BODY, e->color ? col : T.text);
		text_draw_clip(sub, 48, y + 19, mx - 58, TS_SUB, T.text3);
		if (mp)
			draw_circle(mx + 12, y + v->h / 2, 12, T.surface2);
		for (int d = -1; d <= 1; d++)
			draw_circle(mx + 12, y + v->h / 2 + d * 5, 1.6f, T.text3);
		if (inview && tap(mx - 4, y, 34, v->h))
			menu = i;
		else if (inview && long_press(6, y, mx - 10, v->h))
			menu = i;
		else if (inview && tap(6, y, mx - 10, v->h))
			open = i;
	}
	gfx_clip_off();
	float my = VIEW_Y + 44 - F.off;
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
		wrap_center(_("Las partidas de los juegos van cifradas dentro de «Nintendo 3DS». Haz una copia con Checkpoint o JKSM y aparecerá aquí."),
		            BOT_W / 2, my + 14, BOT_W - 40, TS_SUB, 4, T.text3);
	} else if (F.err || F.n == 0) {
		text_draw_center(F.err ? _("No se pudo abrir esta carpeta") : _("Carpeta vacía"), BOT_W / 2, my + 40, TS_BODY, T.text3);
	}
	if (U.buttons && !g_dialog.open && F.sel > 0 && F.v[F.sel].kind == FV_ITEM) {
		if (g_kdown & KEY_A)
			open = F.sel;
		else if (g_kdown & KEY_Y)
			menu = F.sel;
	}
	if (U.buttons && !g_dialog.open && (g_kdown & KEY_B)) {
		if (F.nosaves)
			f_reload_keep();
		else
			f_up();
	}
	if (menu >= 0) {
		F.sel = menu;
		f_menu_open();
	} else if (open >= 0) {
		f_open_item(open);
	}
}

// ---- menú de un archivo o carpeta
enum { FA_INSTALL, FA_RENAME, FA_MOVE, FA_COPY, FA_DELETE, FA_COLORS, FA_ASTEXT };
static struct {
	bool open;
	float anim;
	int idx;     // elemento de F.e
	int acts[8], n, sel, color;
	bool just;   // se acaba de abrir: el toque o el botón que lo abrió no cuenta
} FM;

static void f_menu_open(void)
{
	const FEnt *e = f_selected();
	if (!e)
		return;
	FM.idx = F.v[F.sel].idx;
	FM.n = 0;
	bool locked = e->sys && V.protect_sys;
	if (!e->dir && !strcasecmp(f_ext(e->name), "cia"))
		FM.acts[FM.n++] = FA_INSTALL;
	if (!e->dir && !f_is_img(e) && !f_is_txt(e) && !vw_is_open()) // cualquier archivo se puede mirar como texto
		FM.acts[FM.n++] = FA_ASTEXT;
	if (!locked) {
		FM.acts[FM.n++] = FA_RENAME;
		FM.acts[FM.n++] = FA_MOVE;
	}
	FM.acts[FM.n++] = FA_COPY;
	if (!locked)
		FM.acts[FM.n++] = FA_DELETE;
	FM.acts[FM.n++] = FA_COLORS;
	FM.sel = 0;
	FM.color = e->color;
	FM.open = true;
	FM.just = true;
}

static const char *fa_name(int a)
{
	switch (a) {
	case FA_INSTALL: return _("Instalar en la consola");
	case FA_RENAME: return _("Renombrar");
	case FA_MOVE: return _("Mover a…");
	case FA_COPY: return _("Copiar a…");
	case FA_DELETE: return _("Eliminar");
	case FA_ASTEXT: return _("Abrir como texto");
	default: return _("Destacar");
	}
}

static IconId fa_icon(int a)
{
	static const IconId IC[7] = {IC_DOWNLOAD, IC_PEN, IC_FOLDER, IC_PLUS, IC_TRASH, IC_STAR, IC_NOTE};
	return IC[a];
}

static void f_reload(void)
{
	char p[512];
	util_strlcpy(p, F.path, sizeof p);
	f_load(p);
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
	// renombrar, mover, copiar o borrar lo que se está viendo: primero se cierra el visor
	if (vw_is_open() && a != FA_INSTALL) {
		if (vw_dirty()) {
			toast_icon(_("Guarda o descarta los cambios primero"), IC_INFO, T.warn);
			return;
		}
		vw_close();
	}
	if (a == FA_INSTALL) {
		char err[24] = "";
		int id = strlen(full) < 256 ? ins_request(full, G.console, false, err, sizeof err) : -1;
		if (id < 0) {
			toast_icon(!strcmp(err, "system_title") ? _("Por seguridad, no se instalan títulos del sistema") : !strcmp(err, "already") ? _("Ya está esperando en la consola")
			                                                                                                  : _("La consola no reconoce este CIA"),
			           IC_CLOSE, T.danger);
			return;
		}
		// elegido aquí mismo: no hace falta la tarjeta de confirmación, salvo que no se pueda
		InsReq r;
		ins_peek(full, &r);
		if (r.self || (r.inst_ver >= 0 && r.inst_ver > r.ver)) {
			ins_decide(id, INS_KEEP);
			toast_icon(r.self ? _("Cloud3DS se actualiza desde Ajustes → Actualizaciones") : _("Ya tienes una versión más nueva instalada"), IC_INFO, T.warn);
			return;
		}
		ins_decide(id, INS_ACCEPT);
		toast_icon(_("Instalando…"), IC_DOWNLOAD, T.ok);
	} else if (a == FA_RENAME) {
		U.want_frename = true;
	} else if (a == FA_DELETE) {
		char msg[200];
		snprintf(msg, sizeof msg, e->dir ? _("Se eliminarán «%s» y todo su contenido. Esta acción no se puede deshacer.")
		                                 : _("Se eliminará «%s» de la tarjeta SD. Esta acción no se puede deshacer."),
		         e->name);
		dialog_open(DLG_FDELETE, _("¿Eliminar?"), msg, _("Eliminar"), true);
	} else if (a == FA_MOVE || a == FA_COPY) {
		FP.mode = a;
		util_strlcpy(FP.src, full, sizeof FP.src);
		util_strlcpy(FP.name, e->name, sizeof FP.name);
		FP.src_dir = e->dir;
		toast_icon(_("Ve a la carpeta de destino y pulsa X"), IC_FOLDER, T.accent);
	}
}

static void f_menu(void)
{
	FM.anim = approach(FM.anim, FM.open ? 1 : 0, 16);
	if (FM.anim < 0.01f)
		return;
	float a = ease_out_cubic(FM.anim);
	draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.dim, a));
	const float RH = 32;
	float h = 40 + FM.n * RH + 8, y = SCR_H - h - 6 + (1 - a) * 40;
	draw_rrect(10, y + 3, BOT_W - 20, h, 16, col_alpha(T.shadow, a));
	draw_rrect(10, y, BOT_W - 20, h, 16, T.surface);
	if (!FM.open)
		return;
	const FEnt *e = &F.e[FM.idx];
	text_draw_center_clip(e->name, BOT_W / 2, y + 12, BOT_W - 50, TS_BODY, T.text);
	static const u32 MK[7] = {0, 0xFF4444EF, 0xFF1673F9, 0xFF08B3EA, 0xFF5EC522, 0xFFF6823B, 0xFFF755A8};
	int run = -1;
	// el mismo toque o botón que abrió el menú no elige nada: ese frame no hay entrada
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	bool fresh = FM.just;
	FM.just = false;
	if (fresh) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	if (U.buttons) {
		if (g_krepeat & KEY_DOWN) FM.sel = (FM.sel + 1) % FM.n;
		if (g_krepeat & KEY_UP) FM.sel = (FM.sel + FM.n - 1) % FM.n;
	}
	for (int i = 0; i < FM.n; i++) {
		float ry = y + 36 + i * RH;
		int act = FM.acts[i];
		bool hl = (U.buttons && FM.sel == i) || touch_in(16, ry, BOT_W - 32, RH);
		if (hl)
			draw_rrect(16, ry + 1, BOT_W - 32, RH - 2, 9, T.surface2);
		u32 c = act == FA_DELETE ? T.danger : act == FA_INSTALL ? T.ok : T.text;
		draw_icon(fa_icon(act), 34, ry + RH / 2, 14, c);
		if (act == FA_COLORS) {
			text_draw(fa_name(act), 50, ry + (RH - text_lineh(TS_BODY)) / 2, TS_BODY, c);
			if (U.buttons && FM.sel == i && (g_krepeat & (KEY_LEFT | KEY_RIGHT)))
				FM.color = (FM.color + ((g_krepeat & KEY_RIGHT) ? 1 : 6)) % 7;
			for (int k = 0; k < 7; k++) {
				float sx = BOT_W - 30 - (6 - k) * 21, sy = ry + RH / 2;
				if (FM.color == k)
					draw_circle(sx, sy, 9.5f, T.text);
				draw_circle(sx, sy, 8, k ? MK[k] : T.surface2);
				if (!k)
					draw_line(sx - 4, sy + 4, sx + 4, sy - 4, 1.4f, T.text3);
				if (tap(sx - 10, ry, 20, RH)) {
					FM.color = k;
					run = FA_COLORS;
				}
			}
			if (U.buttons && FM.sel == i && (g_kdown & KEY_A))
				run = FA_COLORS;
		} else {
			text_draw(fa_name(act), 50, ry + (RH - text_lineh(TS_BODY)) / 2, TS_BODY, c);
			if (tap(16, ry, BOT_W - 32, RH) || (U.buttons && FM.sel == i && (g_kdown & KEY_A)))
				run = act;
		}
	}
	if (g_kdown & KEY_B || (g_touch.up && !g_touch.drag && g_touch.sy < y))
		FM.open = false;
	if (run == FA_COLORS) {
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

// ---- mover o copiar: se elige la carpeta navegando y se confirma con X o con el botón
static void f_pick_bar(void)
{
	if (!FP.mode)
		return;
	float y = VIEW_Y + VIEW_H - QR_PAD - 34, w = BOT_W - 20; // por encima del botón del QR
	draw_rrect(10, y + 2, w, 30, 15, T.shadow);
	draw_rrect(10, y, w, 30, 15, T.accent);
	char t[96];
	snprintf(t, sizeof t, "%s %s", GLYPH_X, FP.mode == FA_MOVE ? _("Mover aquí") : _("Copiar aquí"));
	text_draw_center(t, BOT_W / 2 - 30, y + (30 - text_lineh(0.46f)) / 2, 0.46f, 0xFFFFFFFF);
	draw_rect(BOT_W - 70, y + 6, 1, 18, col_alpha(0xFFFFFFFF, 0.4f));
	text_draw_center(_("Cancelar"), BOT_W - 42, y + (30 - text_lineh(TS_SUB)) / 2, TS_SUB, 0xFFFFFFFF);
	bool cancel = tap(BOT_W - 70, y, 60, 30);
	bool go = (g_kdown & KEY_X) || (!cancel && tap(10, y, w - 60, 30));
	if (cancel) {
		FP.mode = 0;
		return;
	}
	if (!go)
		return;
	char dst[1100];
	snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", FP.name);
	if (FP.src_dir && (!strcasecmp(F.path, FP.src) || (!strncasecmp(F.path, FP.src, strlen(FP.src)) && F.path[strlen(FP.src)] == '/'))) {
		toast_icon(_("No puedes meter una carpeta dentro de sí misma"), IC_CLOSE, T.danger);
		return;
	}
	if (V.protect_sys && fsx_is_system(F.path) && strcmp(F.path, "/")) {
		toast_icon(_("Carpeta protegida: elige otra"), IC_LOCK, T.warn);
		return;
	}
	bool d;
	u64 sz;
	if (fsx_stat(dst, &d, &sz) == 0) {
		if (FP.mode == FA_MOVE && !strcasecmp(dst, FP.src)) {
			FP.mode = 0; // ya está aquí
			return;
		}
		char msg[200];
		snprintf(msg, sizeof msg, _("Ya existe «%s» aquí. Se guardará con otro nombre, como «%s (2)»."), FP.name, FP.name);
		dialog_open(DLG_FEXISTS, _("Ya existe"), msg, _("Conservar ambos"), false);
		return;
	}
	f_pick_do(dst);
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
	const float LW = 74;
	text_draw_clip(label, x, y, LW - 6, TS_SUB, col_alpha(T.text3, a));
	text_draw_clip(value, x + LW, y, w - LW, TS_SUB, col_alpha(vc, a));
	return y + 15;
}

// pantalla de arriba en Archivos con el QR cerrado: la información de lo elegido
static void files_top(float a)
{
	f_info_refresh();
	const FEnt *e = f_selected();
	float y0 = CARD_Y + MINI_H + 8, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	if (!e) {
		draw_icon(IC_FOLDER, TOP_W / 2, y0 + h / 2 - 14, 30, col_alpha(T.text3, a));
		text_draw_center(_("Elige un archivo para ver su información"), TOP_W / 2, y0 + h / 2 + 8, TS_BODY, col_alpha(T.text3, a));
		return;
	}
	IconId ic;
	u32 col;
	f_kind(e, &ic, &col);
	// a la izquierda, la vista previa: la foto, el icono del programa o el del tipo
	const float PS = 120;
	float py = y0 + 12;
	draw_rrect(X, py, PS, PS, 14, col_alpha(T.surface2, a));
	if (F.thumb.ok)
		draw_texture(&F.thumb, X + (PS - F.thumb.w) / 2, py + (PS - F.thumb.h) / 2, F.thumb.w, F.thumb.h, a);
	else if (F.cia_icon.ok)
		draw_texture(&F.cia_icon, X + 12, py + 12, 96, 96, a);
	else if (F.preview[0]) { // las primeras líneas, como una hoja
		gfx_clip(X, py, PS, PS);
		text_wrap_lines(F.preview, PS - 14, 0.34f, 12, X + 7, py + 6, col_alpha(T.text2, a), true);
		gfx_clip_off();
	} else
		draw_icon(ic, X + PS / 2, py + PS / 2, 48, col_alpha(col, a));
	// a la derecha, los datos
	float rx = X + PS + 14, rw = X + W - rx, y = py;
	const char *title = F.cia_ok && F.cia->title[0] ? F.cia->title : F.app_title[0] ? F.app_title : e->name;
	int nl = text_wrap_lines(title, rw, TS_TITLE, 2, rx, y, col_alpha(e->color ? col : T.text, a), true);
	y += nl * text_lineh(TS_TITLE) + 2;
	const char *pub = F.cia_ok ? F.cia->publisher : F.app_pub;
	if (pub[0]) {
		text_draw_clip(pub, rx, y, rw, TS_SUB, col_alpha(T.text2, a));
		y += 16;
	}
	y += 4;
	char v[160], b[48];
	f_type_text(e, v, sizeof v);
	y = info_row(rx, y, rw, _("Tipo"), v, e->sys ? T.warn : T.text2, a);
	if (e->dir) {
		if (F.dir_count >= 0) {
			snprintf(v, sizeof v, F.dir_count == 1 ? _("1 elemento") : _("%d elementos"), F.dir_count);
			y = info_row(rx, y, rw, _("Contenido"), v, T.text2, a);
		}
	} else {
		util_fmt_size(b, sizeof b, e->size);
		if (e->size >= 1024)
			snprintf(v, sizeof v, _("%s (%llu bytes)"), b, (unsigned long long)e->size);
		else
			util_strlcpy(v, b, sizeof v);
		y = info_row(rx, y, rw, _("Tamaño"), v, T.text2, a);
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
		u32 vc;
		char vl[160];
		ins_version_line(F.cia, vl, sizeof vl, &vc);
		y = info_row(rx, y, rw, _("Versión"), vl, vc, a);
		snprintf(v, sizeof v, "%016llX", (unsigned long long)F.cia->tid);
		y = info_row(rx, y, rw, _("ID"), v, T.text2, a);
	}
	// abajo: la ruta y qué hacen los botones
	float by = y0 + h - 22;
	draw_rect(X, by - 6, W, 1, col_alpha(T.divider, a));
	bool can_open = e->dir || f_is_img(e) || f_is_txt(e) || F.cia_ok;
	char hint[96];
	if (can_open)
		snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_A, _("Abrir"), GLYPH_Y, _("Opciones"));
	else
		snprintf(hint, sizeof hint, "%s %s", GLYPH_Y, _("Opciones"));
	float hw = text_width(hint, TS_SUB);
	text_draw_right(hint, X + W, by, TS_SUB, col_alpha(T.text2, a));
	char full[700];
	f_full(F.v[F.sel].idx, full, sizeof full);
	text_draw_clip(full, X, by, W - hw - 12, TS_SUB, col_alpha(T.text3, a));
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
	float y0 = CARD_Y + MINI_H + 8, h = SCR_H - 8 - y0;
	draw_rrect(CARD_X, y0 + 3, CARD_W, h, 16, col_alpha(T.shadow, a));
	draw_rrect(CARD_X, y0, CARD_W, h, 16, col_alpha(T.surface, a));
	const float X = CARD_X + 14, W = CARD_W - 28;
	// icono grande con el estado, y al lado el nombre y el modelo
	float cx = X + 24, cy = y0 + 36;
	draw_circle(cx, cy, 24, col_alpha(on ? col_alpha(T.ok, 0.18f) : T.surface2, a));
	draw_icon(dev_icon(d), cx, cy, 28, col_alpha(on ? T.ok : T.text3, a));
	float rx = X + 62, rw = X + W - rx;
	char v[96], when[24];
	text_draw_clip(d->name, rx, y0 + 14, rw, TS_TITLE, col_alpha(T.text, a));
	if (!on && d->last_seen) {
		util_fmt_when(when, sizeof when, d->last_seen);
		snprintf(v, sizeof v, _("Visto: %s"), when);
	}
	text_draw_clip(on ? _("Conectado ahora") : d->last_seen ? v : _("Desconectado"), rx, y0 + 14 + text_lineh(TS_TITLE) + 2, rw, TS_SUB,
	               col_alpha(on ? T.ok : T.text3, a));
	float y = y0 + 74;
	const char *kind = d->kind == 0 ? _("Móvil") : d->kind == 2 ? _("Tableta") : _("Ordenador");
	if (d->model[0])
		snprintf(v, sizeof v, "%s · %s", kind, d->model);
	else
		util_strlcpy(v, kind, sizeof v);
	y = info_row(X, y, W, _("Tipo"), v, T.text2, a);
	if (d->ip[0])
		y = info_row(X, y, W, on ? _("IP") : _("Última IP"), d->ip, T.text2, a);
	const char *src = d->named == NAME_CUSTOM ? _("Puesto a mano") : d->named == NAME_FOUND ? _("Dado por la red") : _("Según el navegador");
	y = info_row(X, y, W, _("Nombre"), src, T.text2, a);
	if (d->created) {
		f_fmt_date(v, sizeof v, d->created);
		y = info_row(X, y, W, _("Vinculado"), v, T.text2, a);
	}
	float by = y0 + h - 22;
	draw_rect(X, by - 6, W, 1, col_alpha(T.divider, a));
	char hint[96];
	snprintf(hint, sizeof hint, "%s %s   %s %s", GLYPH_Y, _("Renombrar"), GLYPH_X, _("Desvincular"));
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

static void settings_main(void)
{
	List *l = cur_list();
	tab_header(_("Ajustes"), false);
	const float RH = 44;
	list_update(l, SET_COUNT, RH);
	U.t_ro = approach(U.t_ro, V.readonly ? 1 : 0, 14);
	U.t_prot = approach(U.t_prot, V.protect_sys ? 1 : 0, 14);
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
			title = _("Solo lectura"); sub = _("Los dispositivos solo podrán ver y descargar"); ic = IC_LOCK; col = T.warn;
			break;
		case SET_PROTECT:
			title = _("Proteger carpetas del sistema"); sub = _("Impide borrar Nintendo 3DS, 3ds, luma…");
			ic = IC_SHIELD; col = T.danger;
			break;
		case SET_PIN:
			title = _("PIN"); sub = _("Se mantiene hasta que lo cambies"); ic = IC_KEY; col = T.accent2; val = V.pin;
			chev = true;
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
			title = _("Salir"); sub = _("O pulsa START"); ic = IC_POWER; col = T.danger;
			break;
		}
		row_icon(28, y + RH / 2, ic, col);
		float right = BOT_W - 16;
		if (i == SET_READONLY || i == SET_PROTECT) {
			toggle(right - 36, y + RH / 2 - 10, i == SET_READONLY ? U.t_ro : U.t_prot);
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
		text_draw_clip(i == PIN_CUSTOM ? _("Elegir mi PIN") : _("PIN aleatorio nuevo"), 50, y + 6, 230, TS_BODY, T.text);
		text_draw_clip(i == PIN_CUSTOM ? _("Elige cuatro cifras fáciles de recordar") : _("La consola genera uno al azar"),
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

static void settings_update(void)
{
	List *l = cur_list();
	tab_header(_("Actualizaciones"), true);
	const float RH = 44;
	list_update(l, 2, RH);
	U.t_upd = approach(U.t_upd, V.upd_auto ? 1 : 0, 14);
	bool busy = U.upd.st == UPD_DOWNLOADING || U.upd.st == UPD_INSTALLING || U.upd.st == UPD_CHECKING;
	for (int i = 0; i < 2; i++) {
		float y = VIEW_Y + i * RH - l->off;
		bool t = row(l, i, y, RH);
		const char *title, *sub;
		char tb[64];
		IconId ic;
		u32 col;
		if (i == 1) {
			title = _("Buscar al abrir la app");
			sub = _("Consulta GitHub en segundo plano");
			ic = IC_CLOCK, col = T.accent2;
			toggle(BOT_W - 52, y + RH / 2 - 10, U.t_upd);
		} else if (U.upd.st == UPD_AVAILABLE && U.upd.has_file) {
			snprintf(tb, sizeof tb, _("Actualizar a la versión %s"), U.upd.latest);
			title = tb, sub = _("Se descarga, se comprueba y se instala");
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
		text_draw_clip(title, 50, y + 6, i == 1 ? BOT_W - 120 : BOT_W - 66, TS_BODY, busy && i == 0 ? T.text3 : T.text);
		text_draw_clip(sub, 50, y + 23, i == 1 ? BOT_W - 120 : BOT_W - 66, TS_SUB, T.text3);
		if (!(t || activate(l, i)))
			continue;
		if (i == 1) {
			set_flag(&G.upd_auto);
		} else if (busy) {
			continue;
		} else if (U.upd.st == UPD_AVAILABLE && U.upd.has_file) {
			char msg[200];
			snprintf(msg, sizeof msg, _("Se descargará la versión %s desde GitHub, se comprobará y se instalará. No apagues la consola mientras tanto."),
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
	const char *sub[AB_COUNT] = {_("Gracias por usar!"), _("Código fuente e informes de errores"), _("Comunidad y ayuda"),
	                             _("Invítame a un café")};
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

static void qr_toggle(void)
{
	state_lock();
	G.qr_min = !G.qr_min;
	G.dirty_cfg = true;
	G.rev++;
	memcpy(&V, &G, sizeof G);
	state_unlock();
}

// barra de abajo: dos pestañas a cada lado y, en el centro, el botón del QR (como SELECT)
#define QR_FAB 52.0f
static float tab_x(int i)
{
	float tw = (BOT_W - QR_FAB) / (float)TAB_COUNT;
	return i * tw + (i >= TAB_COUNT / 2 ? QR_FAB : 0);
}

static void draw_tabbar(void)
{
	float y = SCR_H - TABBAR_H;
	float tw = (BOT_W - QR_FAB) / (float)TAB_COUNT;
	draw_rect(0, y, BOT_W, TABBAR_H, T.surface);
	draw_rect(0, y, BOT_W, 1, T.divider);
	// la raya de la pestaña elegida se desliza (y salta el hueco del QR)
	U.tab_anim = approach(U.tab_anim, U.tab, 16);
	float fa = U.tab_anim, xa = floorf(fa) < TAB_COUNT - 1 ? tab_x((int)floorf(fa)) + (tab_x((int)floorf(fa) + 1) - tab_x((int)floorf(fa))) * (fa - floorf(fa))
	                                                        : tab_x(TAB_COUNT - 1);
	draw_rrect(xa + tw / 2 - 16, y + 2, 32, 3, 1.5f, T.accent);
	static const IconId ics[TAB_COUNT] = {IC_FOLDER, IC_CLOCK, IC_DEVICES, IC_GEAR};
	const char *names[TAB_COUNT] = {_("Archivos"), _("Actividad"), _("Dispositivos"), _("Ajustes")};
	for (int i = 0; i < TAB_COUNT; i++) {
		float k = clampf(1 - fabsf(U.tab_anim - i), 0, 1), x = tab_x(i);
		u32 c = col_lerp(T.text3, T.accent, k);
		draw_icon(ics[i], x + tw / 2, y + 15, 17, c);
		float lw = fminf(text_width(names[i], 0.36f), tw - 4);
		text_draw_clip(names[i], x + (tw - lw) / 2, y + 25, tw - 4, 0.36f, c);
		if (!g_dialog.open && tap(x, y, tw, TABBAR_H)) {
			U.tab = i;
			U.page = PAGE_MAIN;
		}
	}
	// botón del QR: redondo y elevado sobre la barra, con el degradado de la marca si está abierto
	float cx = BOT_W / 2, cy = y + 8, r = 21;
	bool open = !V.qr_min, p = touch_in(cx - r - 4, cy - r - 4, 2 * r + 8, SCR_H - (cy - r - 4));
	draw_circle(cx, cy + 2, r + 3, T.shadow);
	draw_circle(cx, cy, r + 3, T.bg);
	if (open) {
		draw_circle(cx, cy, r, brand_grad(g_time * 0.05f));
		draw_circle(cx, cy, r - 2, col_lerp(T.accent, T.accent2, 0.5f));
	} else {
		draw_circle(cx, cy, r, p ? T.surface2 : T.surface);
		draw_arc(cx, cy, r, 1.5f, 0, 6.2832f, T.divider);
	}
	draw_icon(IC_QR, cx, cy, 20, open ? 0xFFFFFFFF : T.text2);
	if (p && g_touch.down)
		draw_circle(cx, cy, r, col_alpha(0xFFFFFFFF, 0.15f));
	if (!g_dialog.open && tap(cx - r - 4, cy - r - 4, 2 * r + 8, SCR_H - (cy - r - 4)))
		qr_toggle();
}

static void draw_bottom(void)
{
	gfx_target(g_bot, T.bg);
	if (U.splash >= 0) {
		splash_bot(1);
		return;
	}
	// con un menú, la tarjeta de instalar o el visor encima, lo de debajo (pestaña y barra) no recibe nada
	u32 kd = g_kdown, kr = g_krepeat;
	Touch tc = g_touch;
	bool viewer = U.tab == TAB_FILES && VW.kind;
	if (FM.open || U.ins_pend || viewer) {
		g_kdown = g_krepeat = 0;
		memset(&g_touch, 0, sizeof g_touch);
	}
	if (viewer) {
		if (!FM.open && !U.ins_pend && !g_dialog.open)
			g_kdown = kd, g_krepeat = kr, g_touch = tc;
		vw_bottom();
	} else if (U.tab == TAB_FILES) {
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
	else
		settings_main();
	if (!viewer)
		draw_tabbar();
	g_kdown = kd, g_krepeat = kr, g_touch = tc;
	if (U.tab == TAB_FILES)
		f_menu();
	install_card();
	dialog_draw();
	if (U.fade > 0) {
		draw_rect(0, 0, BOT_W, SCR_H, col_alpha(T.bg, U.fade));
		splash_bot(U.fade);
	}
}

// ------------------------------------------------------------ lógica
static void handle_dialog(int ctx, bool ok)
{
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
	} else if (ctx == DLG_FDELETE) {
		char full[700];
		f_full(FM.idx, full, sizeof full);
		if (fsx_rmrf(full) == 0) {
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
	if (ctx == DLG_FDELETE)
		f_reload();
	if (ctx == DLG_FEXISTS) {
		char dir[512], dst[1100];
		util_strlcpy(dir, F.path, sizeof dir);
		ops_free_name(dst, sizeof dst, dir, FP.name);
		f_pick_do(dst);
	}
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
		if ((e->kind == EV_EDIT || e->kind == EV_UPLOAD) && VW.kind == VW_TXT && !VW.doc.dirty && strcmp(e->device, V.console) &&
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
	bool busy = xfer_active() > 0 || upd_busy() || ins_busy();
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
		util_strlcpy(msg, _("Tu nube dejará de estar disponible en el móvil y en el PC hasta que vuelvas a abrirla."), n);
		return;
	}
	char name[64];
	util_strlcpy(name, x->name, sizeof name);
	int pct = x->total ? (int)((double)x->done * 100 / x->total) : 0;
	const char *dir = x->kind == XF_UPLOAD ? _("recibiendo") : x->kind == XF_COPY ? _("copiando") : x->kind == XF_INSTALL ? _("instalando") : _("enviando");
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
	if (U.fade > 0)
		U.fade = fmaxf(0, U.fade - g_dt * 3.5f);
	if (g_kdown & ~KEY_TOUCH)
		U.buttons = true;
	if (g_touch.down)
		U.buttons = false;
	if (U.want_fnew) {
		U.want_fnew = false;
		char nm[128], dst[700];
		if (kb_input(_("Nombre de la carpeta"), "", nm, sizeof nm, false, false) && nm[0]) {
			bool d;
			u64 sz;
			snprintf(dst, sizeof dst, "%s%s%s", F.path, strcmp(F.path, "/") ? "/" : "", nm);
			if (strpbrk(nm, "\\/:*?\"<>|") || nm[strlen(nm) - 1] == '.' || nm[strlen(nm) - 1] == ' ')
				toast_icon(_("Ese nombre no es válido"), IC_CLOSE, T.danger);
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
	if (U.want_txt) {
		int m = U.want_txt;
		U.want_txt = 0;
		if (VW.kind != VW_TXT)
			return;
		const char *ln = VW.doc.ln[VW.cur];
		int chars = 0;
		for (const char *p = ln; *p; p++)
			chars += ((u8)*p & 0xC0) != 0x80;
		if (m == 1 && chars > 2000) { // el teclado del sistema no admite más
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
				toast_icon(_("No hay memoria para abrir este archivo"), IC_CLOSE, T.danger);
		} else if (kb_input(_("Línea nueva"), "", buf, 8192, true, false)) {
			if (txt_insert(&VW.doc, VW.cur + 1, buf))
				VW.cur++;
		}
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
				toast_icon(_("Ese nombre no es válido"), IC_CLOSE, T.danger);
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
	if (U.want_rename >= 0) {
		int i = U.want_rename;
		U.want_rename = -1;
		char nm[48];
		// vacío: vuelve al nombre automático (el de la red o el modelo)
		if (V.dev[i].used && kb_input(_("Nombre del dispositivo (vacío: automático)"), V.dev[i].named == NAME_CUSTOM ? V.dev[i].name : "",
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
	if ((g_kdown & KEY_SELECT) && U.ins_pend) // el QR se abre y se cierra siempre, también con la tarjeta de instalar
		qr_toggle();
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
	if ((g_kdown & (KEY_R | KEY_ZR | KEY_L | KEY_ZL)) && VW.kind && U.tab == TAB_FILES) {
		if (vw_dirty()) { // con cambios sin guardar no se cambia de pestaña sin preguntar
			vw_try_close();
			g_kdown &= ~(KEY_R | KEY_ZR | KEY_L | KEY_ZL);
		} else {
			vw_close();
		}
	}
	if (g_kdown & (KEY_R | KEY_ZR)) {
		U.tab = (U.tab + 1) % TAB_COUNT;
		U.page = PAGE_MAIN;
	}
	if (g_kdown & (KEY_L | KEY_ZL)) {
		U.tab = (U.tab + TAB_COUNT - 1) % TAB_COUNT;
		U.page = PAGE_MAIN;
	}
	// atrás: de una subpágina a Ajustes (y la pantalla de arriba vuelve al QR)
	if (U.page != PAGE_MAIN && ((g_kdown & KEY_B) || tap(0, 0, 170, VIEW_Y))) {
		U.list[TAB_SETTINGS].sel = U.page == PAGE_LANG ? SET_LANG : U.page == PAGE_PIN ? SET_PIN : U.page == PAGE_UPDATE ? SET_UPDATE : SET_ABOUT;
		U.page = PAGE_MAIN;
	}
	if ((g_kdown & KEY_START) && !(U.tab == TAB_FILES && VW.kind == VW_TXT)) // en el editor, START guarda
		ask_exit();
	if (g_kdown & KEY_SELECT)
		qr_toggle();
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
	aptHook(&hook, apt_hook, NULL);
	hidSetRepeatParameters(18, 5);

	server_start();
	upd_init(argc > 0 ? argv[0] : NULL);
	ins_init();
	if (G.upd_auto)
		upd_check();
	U.splash = 0;
	U.qr_anim = G.qr_min ? 0 : 1;
	U.tab = TAB_ACTIVITY; // se abre como siempre, en Actividad
	U.want_rename = -1;

	u64 last_save = util_ms();
	while (aptMainLoop() && !U.quit) {
		input_update();
		gfx_tick();

		// la copia se rehace si algo cambió, durante las transferencias y cada segundo
		// (para que «conectado ahora» esté al día)
		state_lock();
		u64 now = util_ms();
		if (G.rev != s_rev || xfer_active() || now - s_copy_t > 1000) {
			memcpy(&V, &G, sizeof G);
			s_rev = G.rev;
			s_copy_t = now;
		}
		bool dirty = G.dirty_cfg;
		state_unlock();

		watch_events();
		s_events_seen = true;
		upd_get(&U.upd);
		static int ins_tick;
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
		lid_and_sleep();
		update();

		gfx_frame_begin();
		draw_top();
		draw_bottom();
		gfx_frame_end();

		if (dirty && now - last_save > 3000 && !xfer_active()) {
			state_lock();
			config_save();
			state_unlock();
			last_save = now;
		}
	}

	ops_exit();
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
