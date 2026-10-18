/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 * Interfaz basada en Gemi3DS de VENOMSHiNE - https://github.com/venomshine/Gemi3DS
 */
// Cloud3DS - bucle principal e interfaz de la consola
#include "app.h"
#include "fsx.h"
#include "i18n.h"
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

#define ABOUT_TEXT "Tu nube local para Nintendo 3DS: maneja la tarjeta SD desde el móvil o el PC con solo escanear un QR. Sin apps, sin cables y sin configurar nada. Todo se queda en tu red: nada pasa por Internet."

enum { TAB_ACTIVITY, TAB_DEVICES, TAB_SETTINGS, TAB_COUNT };
enum { PAGE_MAIN, PAGE_LANG, PAGE_ABOUT };
enum { SET_READONLY, SET_PROTECT, SET_PIN, SET_LANG, SET_THEME, SET_ABOUT, SET_EXIT, SET_COUNT };
enum { AB_GITHUB, AB_DISCORD, AB_KOFI, AB_VERSION, AB_COUNT };
enum { DLG_REVOKE = 1, DLG_NEWPIN, DLG_EXIT, DLG_CLEAR };

typedef struct {
	float off, target;
	int sel;
} List;

static struct {
	int tab, page;
	float tab_anim;
	List list[TAB_COUNT + 2];   // una por pestaña y por subpágina de Ajustes
	bool buttons;                // la última interacción fue con botones (se ve la selección)
	float busy;                  // 0 = datos de conexión, 1 = transferencia
	float t_ro, t_prot;          // animación de los interruptores
	int dlg_arg;
	bool quit;
	float splash;                // segundos de presentación (-1 = terminada)
	float fade;                  // fundido de la presentación a la app (1 -> 0)
	bool lid_off;
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
	case EV_UPLOAD:
		*ic = IC_UPLOAD; *col = T.ok;
		snprintf(sub, sn, "%s · %s · %s", _("Recibido"), sz, e->device);
		break;
	case EV_DOWNLOAD:
		*ic = IC_DOWNLOAD; *col = T.accent;
		snprintf(sub, sn, "%s · %s · %s", _("Enviado"), sz, e->device);
		break;
	case EV_UPLOAD_FAIL:
		*ic = IC_CLOSE; *col = T.danger;
		snprintf(sub, sn, "%s · %s", _("No se completó"), e->device);
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
		util_strlcpy(title, _("Dispositivo expulsado"), tn);
		util_strlcpy(sub, e->device, sn);
		break;
	default:
		*ic = IC_LOCK; *col = T.warn;
		util_strlcpy(title, _("PIN cambiado por seguridad"), tn);
		util_strlcpy(sub, _("Demasiados intentos fallidos"), sn);
		break;
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

static void draw_header(void)
{
	draw_cloud(18, 11, 8, 1);
	text_draw(APP_NAME, 32, 3, 0.5f, T.text);
	// derecha: Wi-Fi  fecha · hora  [batería] 87 %
	float rx = TOP_W - 10;
	bool chg = false;
	int pct = util_battery(&chg);
	char bp[8];
	snprintf(bp, sizeof bp, "%d%%", pct);
	text_draw_right(bp, rx, 4, TS_HDR, T.text2);
	rx -= text_width(bp, TS_HDR) + 4;
	draw_battery(rx - 22, 6, pct, chg);
	rx -= 22 + 10;
	char clk[8], day[32];
	util_fmt_now(clk, sizeof clk);
	text_draw_right(clk, rx, 4, TS_HDR, T.text2);
	rx -= text_width(clk, TS_HDR) + 5;
	draw_circle(rx - 1.5f, 11, 1.3f, T.text3);
	rx -= 6;
	util_fmt_today(day, sizeof day);
	text_draw_right(day, rx, 4, TS_HDR, T.text3);
	rx -= text_width(day, TS_HDR) + 10;
	int bars = V.ip ? osGetWifiStrength() : 0;
	for (int i = 0; i < 3; i++) {
		float h = 4 + i * 3;
		draw_rrect(rx - 13 + i * 5, 16 - h, 3, h, 1, i < bars ? T.text2 : T.divider);
	}
	const Xfer *xs[1];
	if (visible_xfers(xs, 1) && xs[0]->st == XS_RUNNING) {
		float p = g_time * 1.2f;
		draw_grad_h(0, HDR_H, TOP_W, 2, col_alpha(brand_grad(p), 0.9f), col_alpha(brand_grad(p + 0.5f), 0.9f));
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

static void draw_status(void)
{
	float y = QR_Y + QR_S - 12;
	int on = online_count();
	if (on) {
		char s[48];
		snprintf(s, sizeof s, on == 1 ? _("%d dispositivo conectado") : _("%d dispositivos conectados"), on);
		draw_circle(COL_X + 4, y + 7, 4, T.ok);
		text_draw_clip(s, COL_X + 14, y, COL_W - 14, TS_SUB, T.ok);
	} else {
		int dots = (int)(g_time * 2) % 4;
		char w[48];
		snprintf(w, sizeof w, _("Esperando conexión%.*s"), dots, "...");
		text_draw_clip(w, COL_X, y, COL_W, TS_SUB, T.text3);
	}
}

static void connect_col(float a, float dy)
{
	float x = COL_X, y = QR_Y + dy;
	text_draw_clip(_("Escanea con tu móvil"), x, y + 2, COL_W, 0.55f, col_alpha(T.text, a));
	text_wrap_lines(_("La cámara abre tu nube al instante. Mismo Wi-Fi que la consola."), COL_W, TS_SUB, 3, x, y + 24,
	                col_alpha(T.text3, a), true);
	draw_rect(x, y + 66, COL_W, 1, col_alpha(T.divider, a));
	text_draw_clip(_("O escribe en el navegador:"), x, y + 74, COL_W, TS_SUB, col_alpha(T.text3, a));
	char url[48];
	url_text(url, sizeof url, false);
	float s = 0.64f;
	while (s > 0.44f && text_width(url, s) > COL_W)
		s -= 0.02f;
	text_draw(url, x, y + 90, s, col_alpha(T.accent, a));
	text_draw(_("PIN"), x, y + 132, TS_SUB, col_alpha(T.text3, a));
	for (int i = 0; i < 4; i++) {
		float bx = x + 30 + i * 31, by = y + 122;
		draw_rrect(bx, by, 27, 32, 8, col_alpha(T.surface2, a));
		char d[2] = {V.pin[i], 0};
		text_draw_center(d, bx + 13.5f, by + (32 - text_lineh(0.62f)) / 2, 0.62f, col_alpha(T.text, a));
	}
}

static void xfer_col(float a, float dy)
{
	const Xfer *xs[3];
	int n = visible_xfers(xs, 3);
	if (!n)
		return;
	const Xfer *x = xs[0];
	float tx = COL_X, y = QR_Y + dy;
	bool up = x->kind == XF_UPLOAD;
	const char *head = up ? _("Recibiendo") : _("Enviando");
	u32 col = up ? T.ok : T.accent;
	IconId ic = up ? IC_UPLOAD : IC_DOWNLOAD;
	if (x->st == XS_DONE) {
		head = _("Completado");
		col = T.ok;
		ic = IC_CHECK;
	} else if (x->st != XS_RUNNING) {
		head = _("No se completó");
		col = T.danger;
		ic = IC_CLOSE;
	}
	draw_circle(tx + 13, y + 13, 13, col_alpha(col, a));
	draw_icon(ic, tx + 13, y + 13, 15, col_alpha(0xFFFFFFFF, a));
	text_draw_clip(head, tx + 34, y + 4, COL_W - 34, 0.55f, col_alpha(T.text, a));
	text_wrap_lines(x->name, COL_W, TS_BODY, 2, tx, y + 34, col_alpha(T.text, a), true);
	char sub[96];
	snprintf(sub, sizeof sub, up ? _("de %s") : _("a %s"), x->device);
	text_draw_clip(sub, tx, y + 66, COL_W, TS_SUB, col_alpha(T.text3, a));

	float frac = x->st == XS_DONE ? 1 : x->total ? (float)((double)x->done / x->total) : 0;
	brand_bar(tx, y + 86, COL_W, 8, frac);
	char d[24], t[24], line[80], pc[12];
	util_fmt_size(d, sizeof d, x->done);
	util_fmt_size(t, sizeof t, x->total);
	snprintf(line, sizeof line, _("%s de %s"), d, t);
	text_draw_clip(line, tx, y + 100, COL_W - 40, TS_SUB, col_alpha(T.text2, a));
	snprintf(pc, sizeof pc, "%d%%", (int)(frac * 100));
	text_draw_right(pc, tx + COL_W, y + 99, TS_BODY, col_alpha(T.accent, a));
	if (x->st == XS_RUNNING && x->speed > 0) {
		char sp[24], eta[24];
		util_fmt_speed(sp, sizeof sp, x->speed);
		util_fmt_eta(eta, sizeof eta, (x->total - x->done) / x->speed);
		snprintf(line, sizeof line, _("%s · quedan %s"), sp, eta);
		text_draw_clip(line, tx, y + 116, COL_W, TS_SUB, col_alpha(T.text3, a));
	}
	if (n > 1) {
		char more[48];
		snprintf(more, sizeof more, _("y %d más"), n - 1);
		text_draw_clip(more, tx, y + 132, COL_W, TS_SUB, col_alpha(T.text3, a));
	}
}

// subpágina «Acerca de»: la tarjeta muestra el enlace elegido (al salir vuelve el QR de conexión)
static void about_card(void)
{
	card();
	int sel = U.list[TAB_COUNT + 1].sel;
	if (sel >= AB_GITHUB && sel <= AB_KOFI) {
		static const char *const NAMES[3] = {"GitHub", "Discord", "Ko-fi"};
		const char *desc[3] = {_("Código, novedades y nuevas versiones de mis proyectos."),
		                       _("Únete a la comunidad: dudas, ideas y avisos de actualizaciones."),
		                       _("Si te gusta Cloud3DS, invítame a un café. ¡Ayuda muchísimo!")};
		draw_qr(LINKS[sel], QR_X, QR_Y, QR_S, 1);
		text_draw(NAMES[sel], COL_X, QR_Y + 2, 0.66f, T.text);
		text_wrap_lines(desc[sel], COL_W, 0.42f, 5, COL_X, QR_Y + 30, T.text2, true);
		text_draw_clip(_("Escanéalo con tu móvil"), COL_X, QR_Y + 120, COL_W, TS_SUB, T.text3);
		text_wrap_lines(LINKS[sel] + 8, COL_W, TS_SUB, 3, COL_X, QR_Y + 136, T.accent, true);
	} else {
		draw_cloud(QR_X + QR_S / 2 - 6, QR_Y + 62, 34, 1);
		float w = text_width(APP_NAME, 0.9f);
		text_draw_grad(APP_NAME, QR_X + (QR_S - w) / 2, QR_Y + 102, 0.9f, g_time * 0.1f, 1);
		char vb[80];
		snprintf(vb, sizeof vb, _("v%s · hecho por %s"), APP_VERSION, "VENOMSHiNE");
		float vw = text_width(vb, TS_SUB);
		text_draw_clip(vb, QR_X + (QR_S - fminf(vw, QR_S)) / 2, QR_Y + 136, QR_S, TS_SUB, T.text2);
		text_wrap_lines(_(ABOUT_TEXT), COL_W, 0.42f, 10, COL_X, QR_Y + 2, T.text2, true);
		text_draw_clip(_("Interfaz basada en Gemi3DS"), COL_X, QR_Y + QR_S - 12, COL_W, TS_SUB, T.text3);
	}
}

// ------------------------------------------------------------ presentación
static void splash_top(float alpha)
{
	float t = U.splash >= 0 ? U.splash : 2.0f;
	float a = ease_out_cubic(t / 0.9f);
	// halo con los colores de la marca
	for (int i = 0; i < 6; i++) {
		float r = 30 + i * 22 * a;
		draw_circle(200, 98, r, col_alpha(brand_grad(i / 6.0f + t * 0.2f), 0.05f * a * alpha));
	}
	float sc = ease_out_back(clampf(t / 0.8f, 0, 1));
	if (sc > 0.01f)
		draw_cloud(196, 96, 44 * sc, alpha);
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

static void draw_top(void)
{
	gfx_target(g_top, T.bg);
	if (U.splash >= 0) {
		splash_top(1);
		return;
	}
	draw_header();
	if (U.tab == TAB_SETTINGS && U.page == PAGE_ABOUT) {
		about_card();
	} else if (!V.ip) {
		card_message(IC_WIFI_OFF, T.text3, _("Sin conexión Wi-Fi"),
		             _("Conecta la consola a tu Wi-Fi. Si no tienes router, usa el punto de acceso de tu móvil."));
	} else if (V.srv_error[0]) {
		card_message(IC_INFO, T.warn, _("No se pudo abrir el servidor"),
		             _("Cierra otras apps de red (como FTPD) y vuelve a abrir Cloud3DS."));
	} else if (!V.running) {
		card();
		draw_arc(TOP_W / 2, CARD_Y + 80, 16, 3, g_time * 6, g_time * 6 + 4.2f, T.accent);
		text_draw_center(_("Preparando tu nube…"), TOP_W / 2, CARD_Y + 114, TS_BODY, T.text3);
	} else {
		card();
		char url[64], qr[96];
		url_text(url, sizeof url, true);
		snprintf(qr, sizeof qr, "%s/#k=%s", url, V.qr_key);
		draw_qr(qr, QR_X, QR_Y, QR_S, 1);
		const Xfer *xs[1];
		bool busy = visible_xfers(xs, 1) > 0;
		U.busy = approach(U.busy, busy ? 1 : 0, 9);
		float b = ease_in_out(U.busy);
		if (b < 0.99f)
			connect_col(1 - b, -b * 10);
		if (b > 0.01f)
			xfer_col(b, (1 - b) * 10);
		draw_status();
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
	return &U.list[U.tab];
}

static void list_update(List *l, int count, float rh)
{
	float max = count * rh - VIEW_H;
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
	if (V.stat_up || V.stat_down) {
		char a[24], b[24];
		util_fmt_size(a, sizeof a, V.stat_up);
		util_fmt_size(b, sizeof b, V.stat_down);
		float x = BOT_W - 14 - text_width(b, TS_SUB);
		text_draw(b, x, 11, TS_SUB, T.text3);
		draw_icon(IC_DOWNLOAD, x - 9, 16, 12, T.accent);
		x -= 26 + text_width(a, TS_SUB);
		text_draw(a, x, 11, TS_SUB, T.text3);
		draw_icon(IC_UPLOAD, x - 9, 16, 12, T.ok);
	}
	int n = V.hist_n;
	const float RH = 40;
	int rows = n ? n + 1 : 0;   // + «Borrar el historial»
	list_update(l, rows, RH);
	if (!n) {
		empty_state(IC_CLOUD, _("Todo listo"), _("Sube algo desde tu móvil o tu PC y aparecerá aquí."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < rows; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		bool t = row(l, i, y, RH);
		if (i == n) {
			row_icon(28, y + RH / 2, IC_TRASH, T.danger);
			text_draw(_("Borrar el historial"), 50, y + (RH - text_lineh(TS_BODY)) / 2, TS_BODY, T.danger);
			if (t || activate(l, i))
				dialog_open(DLG_CLEAR, _("¿Borrar el historial?"), _("Se borra la lista de actividad. Tus archivos no se tocan."),
				            _("Borrar"), true);
			continue;
		}
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
	dialog_open(DLG_REVOKE, _("¿Expulsar dispositivo?"), msg, _("Expulsar"), true);
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
		empty_state(IC_DEVICES, _("Sin dispositivos"), _("Escanea el QR de arriba con tu móvil para vincularlo."));
		return;
	}
	gfx_clip(0, VIEW_Y, BOT_W, VIEW_H);
	for (int i = 0; i < n; i++) {
		float y = VIEW_Y + i * RH - l->off;
		if (y + RH < VIEW_Y || y > VIEW_Y + VIEW_H)
			continue;
		const Device *d = &V.dev[idx[i]];
		bool on = dev_online(d);
		// botón Expulsar a la derecha (se comprueba antes que la fila)
		float bw = 74, bh = 26, bx = BOT_W - 14 - bw, by = y + (RH - bh) / 2;
		bool bt = !g_dialog.open && tap(bx, by, bw, bh) && g_touch.sy >= VIEW_Y;
		if (!bt)
			row(l, i, y, RH);
		draw_circle(28, y + RH / 2, 13, T.surface2);
		draw_icon(dev_icon(d), 28, y + RH / 2, 16, on ? T.ok : T.text3);
		if (on) {
			draw_circle(38, y + RH / 2 + 9, 4.5f, T.bg);
			draw_circle(38, y + RH / 2 + 9, 3, T.ok);
		}
		text_draw_clip(d->name, 50, y + 7, bx - 58, TS_BODY, T.text);
		char sub[64], when[24];
		if (on)
			snprintf(sub, sizeof sub, "%s · %s", _("Conectado ahora"), d->ip);
		else {
			util_fmt_when(when, sizeof when, d->last_seen);
			snprintf(sub, sizeof sub, _("Visto: %s"), when);
		}
		text_draw_clip(sub, 50, y + 24, bx - 58, TS_SUB, on ? T.ok : T.text3);
		bool bp = touch_in(bx, by, bw, bh);
		draw_rrect(bx, by, bw, bh, bh / 2, col_alpha(T.danger, bp ? 0.35f : 0.16f));
		text_draw_center(_("Expulsar"), bx + bw / 2, by + (bh - text_lineh(TS_SUB)) / 2, TS_SUB, T.danger);
		if (bt || activate(l, i))
			ask_revoke(idx[i]);
	}
	gfx_clip_off();
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

static void settings_action(int i)
{
	switch (i) {
	case SET_READONLY:
		set_flag(&G.readonly);
		toast_icon(V.readonly ? _("Solo lectura activado") : _("Lectura y escritura activadas"), IC_LOCK, T.warn);
		break;
	case SET_PROTECT:
		set_flag(&G.protect_sys);
		break;
	case SET_PIN:
		dialog_open(DLG_NEWPIN, _("¿Crear un PIN nuevo?"),
		            _("El PIN y el QR actuales dejarán de servir. Los dispositivos ya vinculados siguen conectados."),
		            _("Cambiar"), false);
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
	case SET_ABOUT:
		U.page = PAGE_ABOUT;
		U.list[TAB_COUNT + 1].sel = AB_DISCORD;
		U.list[TAB_COUNT + 1].off = U.list[TAB_COUNT + 1].target = 0;
		break;
	case SET_EXIT:
		U.quit = true;
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
			title = _("Solo lectura"); sub = _("Solo se podrá ver y descargar"); ic = IC_LOCK; col = T.warn;
			break;
		case SET_PROTECT:
			title = _("Proteger carpetas del sistema"); sub = _("Pide confirmación en Nintendo 3DS, luma…");
			ic = IC_SHIELD; col = T.danger;
			break;
		case SET_PIN:
			title = _("PIN nuevo"); sub = _("Invalida el QR y el PIN actuales"); ic = IC_KEY; col = T.accent2; val = V.pin;
			break;
		case SET_LANG:
			title = _("Idioma"); ic = IC_GLOBE; chev = true;
			snprintf(vbuf, sizeof vbuf, "%s", lang_setting_name(V.lang));
			val = vbuf;
			break;
		case SET_THEME:
			title = _("Tema"); ic = IC_PALETTE; col = T.accent2; val = V.light ? _("Claro") : _("Oscuro");
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

static void settings_about(void)
{
	List *l = cur_list();
	tab_header(_("Acerca de Cloud3DS"), true);
	const float RH = 40;
	list_update(l, AB_COUNT, RH);
	static const IconId IC[AB_COUNT] = {IC_CODE, IC_CHAT, IC_HEART, IC_CLOUD};
	const u32 COL[AB_COUNT] = {col_hex(0x6E7681), col_hex(0x5865F2), col_hex(0xFF5E5B), T.accent};
	const char *title[AB_COUNT] = {"GitHub", "Discord", "Ko-fi", "Cloud3DS " APP_VERSION};
	const char *sub[AB_COUNT] = {_("Código y nuevas versiones"), _("Únete a la comunidad"), _("Invítame a un café"),
	                             _("Gracias por usarlo")};
	for (int i = 0; i < AB_COUNT; i++) {
		float y = VIEW_Y + i * RH - l->off;
		row(l, i, y, RH);
		row_icon(28, y + RH / 2, IC[i], COL[i]);
		text_draw_clip(title[i], 50, y + 5, 230, TS_BODY, T.text);
		text_draw_clip(sub[i], 50, y + 22, 230, TS_SUB, T.text3);
		if (i == l->sel)
			draw_icon(IC_QR, BOT_W - 26, y + RH / 2, 15, T.accent);
	}
}

static void draw_tabbar(void)
{
	float y = SCR_H - TABBAR_H;
	draw_rect(0, y, BOT_W, TABBAR_H, T.surface);
	draw_rect(0, y, BOT_W, 1, T.divider);
	float tw = BOT_W / (float)TAB_COUNT;
	U.tab_anim = approach(U.tab_anim, U.tab, 16);
	draw_rrect(U.tab_anim * tw + tw / 2 - 16, y + 2, 32, 3, 1.5f, T.accent);
	static const IconId ics[TAB_COUNT] = {IC_CLOCK, IC_DEVICES, IC_GEAR};
	const char *names[TAB_COUNT] = {_("Actividad"), _("Dispositivos"), _("Ajustes")};
	for (int i = 0; i < TAB_COUNT; i++) {
		float k = clampf(1 - fabsf(U.tab_anim - i), 0, 1);
		u32 c = col_lerp(T.text3, T.accent, k);
		draw_icon(ics[i], i * tw + tw / 2, y + 15, 17, c);
		float lw = fminf(text_width(names[i], 0.38f), tw - 8);
		text_draw_clip(names[i], i * tw + (tw - lw) / 2, y + 25, tw - 8, 0.38f, c);
		if (!g_dialog.open && tap(i * tw, y, tw, TABBAR_H)) {
			U.tab = i;
			U.page = PAGE_MAIN;
		}
	}
}

static void draw_bottom(void)
{
	gfx_target(g_bot, T.bg);
	if (U.splash >= 0) {
		splash_bot(1);
		return;
	}
	if (U.tab == TAB_ACTIVITY)
		tab_activity();
	else if (U.tab == TAB_DEVICES)
		tab_devices();
	else if (U.page == PAGE_LANG)
		settings_lang();
	else if (U.page == PAGE_ABOUT)
		settings_about();
	else
		settings_main();
	draw_tabbar();
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
	state_lock();
	if (ctx == DLG_REVOKE) {
		Device *d = &G.dev[U.dlg_arg];
		if (d->used) {
			event_add(EV_REVOKED, "", d->name, 0);
			d->used = false;
			G.dirty_cfg = true;
		}
	} else if (ctx == DLG_NEWPIN) {
		pin_regen();
	} else if (ctx == DLG_CLEAR) {
		G.hist_n = G.hist_head = 0;
		G.dirty_cfg = true;
	} else if (ctx == DLG_EXIT) {
		U.quit = true;
	}
	G.rev++;
	memcpy(&V, &G, sizeof G);
	s_hist_n = V.hist_n;
	s_hist_head = V.hist_head;
	state_unlock();
	if (ctx == DLG_NEWPIN)
		toast_icon(_("PIN y QR nuevos listos"), IC_KEY, T.accent2);
}

static void watch_events(void)
{
	if (V.hist_n == s_hist_n && V.hist_head == s_hist_head)
		return;
	if (s_events_seen && V.hist_n > 0) {
		const Event *e = ev(0);
		char msg[160];
		if (e->kind == EV_PAIRED) {
			snprintf(msg, sizeof msg, _("Vinculado: %s"), e->device);
			toast_icon(msg, IC_LINK, T.accent2);
		} else if (e->kind == EV_PIN_RESET) {
			toast_icon(_("PIN cambiado por seguridad"), IC_LOCK, T.warn);
		} else if (e->kind == EV_DELETE) {
			snprintf(msg, sizeof msg, _("Eliminado: %s"), e->name);
			toast_icon(msg, IC_TRASH, T.danger);
		} else if (e->kind == EV_UPLOAD_FAIL) {
			snprintf(msg, sizeof msg, _("No se completó: %s"), e->name);
			toast_icon(msg, IC_CLOSE, T.danger);
		}
	}
	s_hist_n = V.hist_n;
	s_hist_head = V.hist_head;
}

static void lid_and_sleep(void)
{
	bool busy = xfer_active() > 0;
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

static void ask_exit(void)
{
	dialog_open(DLG_EXIT, _("¿Salir de Cloud3DS?"), _("Hay transferencias en curso y se cancelarán."), _("Salir"), true);
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
	int d = dialog_update();
	if (d >= 0) {
		handle_dialog(g_dialog.ctx, d == 1);
		return;
	}
	if (g_dialog.open)
		return;
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
		U.list[TAB_SETTINGS].sel = U.page == PAGE_LANG ? SET_LANG : SET_ABOUT;
		U.page = PAGE_MAIN;
	}
	if (g_kdown & KEY_START) {
		if (xfer_active())
			ask_exit();
		else
			U.quit = true;
	}
	if (U.quit && xfer_active()) {
		U.quit = false;
		ask_exit();
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

int main(void)
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
	U.splash = 0;

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
