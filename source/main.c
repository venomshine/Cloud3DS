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

#define TOP_HDR 24
#define QR_X 16
#define QR_Y 30
#define QR_S 176
#define COL_X 210   // columna derecha de la pantalla superior
#define COL_W 176

#define ABOUT_TEXT "Tu nube local para Nintendo 3DS: maneja la tarjeta SD desde el móvil o el PC con solo escanear un QR. Sin apps, sin cables y sin configurar nada. Todo se queda en tu red: nada pasa por Internet."

// hojas
enum { SK_NONE, SK_DRAWER, SK_ACCESS, SK_DEVICES, SK_ACTIVITY, SK_SETTINGS, SK_LANG, SK_ABOUT };
// diálogos
enum { DLG_REVOKE = 1, DLG_NEWPIN, DLG_EXIT, DLG_CLEAR };
// elementos con id fijo
enum {
	ID_ACTIVITY = 1, ID_DEVICES, ID_SETTINGS, ID_ABOUT, ID_EXIT,
	ID_RW, ID_RO, ID_PROTECT, ID_ACCESS, ID_PIN, ID_LANG, ID_THEME, ID_CLEAR,
	ID_GITHUB, ID_DISCORD, ID_KOFI, ID_VERSION,
};

static Sheet S;
static int s_parent;          // hoja a la que se vuelve al cerrar la actual
static int s_dlg_arg;
static bool s_quit;
static float s_busy;          // 0 = datos de conexión, 1 = transferencia (columna derecha)
static u64 s_free, s_total;
static u64 s_space_t;
static bool s_lid_off;

static AppState V;            // copia del estado para dibujar sin el cerrojo
static u32 s_rev = ~0u;
static u64 s_copy_t;
static int s_hist_n, s_hist_head;
static bool s_events_seen;

// ------------------------------------------------------------ datos
static int online_count(void)
{
	int n = 0;
	u64 now = util_ms();
	for (int i = 0; i < MAX_DEVICES; i++)
		if (V.dev[i].used && V.dev[i].last_ms && now - V.dev[i].last_ms < 30000)
			n++;
	return n;
}

static int paired_count(void)
{
	int n = 0;
	for (int i = 0; i < MAX_DEVICES; i++)
		n += V.dev[i].used;
	return n;
}

static bool dev_online(const Device *d)
{
	return d->last_ms && util_ms() - d->last_ms < 30000;
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
	for (int i = 0; i < MAX_XFERS && n < max; i++) {
		const Xfer *x = &V.xfers[i];
		if (x->st == XS_RUNNING)
			out[n++] = x;
	}
	for (int i = 0; i < MAX_XFERS && n < max; i++) {
		const Xfer *x = &V.xfers[i];
		if (x->st != XS_FREE && x->st != XS_RUNNING && now - x->t_end < 2500)
			out[n++] = x;
	}
	return n;
}

static const Event *ev(int i) { return &V.hist[(V.hist_head - i + MAX_HISTORY) % MAX_HISTORY]; }

// texto, icono y color de un evento
static void ev_info(const Event *e, char *title, size_t tn, char *sub, size_t sn, IconId *ic, u32 *col)
{
	char sz[24];
	util_fmt_size(sz, sizeof sz, e->size);
	util_strlcpy(title, e->name, tn);
	*col = T.accent;
	switch (e->kind) {
	case EV_UPLOAD:
		*ic = IC_UPLOAD; *col = T.ok;
		snprintf(sub, sn, "%s · %s · %s", _("Recibido"), sz, e->device);
		break;
	case EV_DOWNLOAD:
		*ic = IC_DOWNLOAD;
		snprintf(sub, sn, "%s · %s · %s", _("Enviado"), sz, e->device);
		break;
	case EV_UPLOAD_FAIL:
		*ic = IC_CLOSE; *col = T.danger;
		snprintf(sub, sn, "%s · %s", _("No se completó"), e->device);
		break;
	case EV_MKDIR:
		*ic = IC_FOLDER; *col = col_hex(0xFDD663);
		snprintf(sub, sn, "%s · %s", _("Carpeta creada"), e->device);
		break;
	case EV_RENAME:
		*ic = IC_PEN; *col = T.text2;
		snprintf(sub, sn, "%s · %s", _("Renombrado"), e->device);
		break;
	case EV_DELETE:
		*ic = IC_TRASH; *col = T.danger;
		snprintf(sub, sn, "%s · %s", _("Eliminado"), e->device);
		break;
	case EV_PAIRED:
		*ic = IC_LINK; *col = col_hex(0xC58AF9);
		util_strlcpy(title, _("Nuevo dispositivo vinculado"), tn);
		util_strlcpy(sub, e->device, sn);
		break;
	case EV_REVOKED:
		*ic = IC_LOGOUT; *col = T.text3;
		util_strlcpy(title, _("Dispositivo expulsado"), tn);
		util_strlcpy(sub, e->device, sn);
		break;
	case EV_PIN_RESET:
		*ic = IC_LOCK; *col = col_hex(0xFDD663);
		util_strlcpy(title, _("PIN cambiado por seguridad"), tn);
		util_strlcpy(sub, _("Demasiados intentos fallidos"), sn);
		break;
	}
}

static IconId dev_icon(const Device *d)
{
	return d->kind == 0 ? IC_PHONE : d->kind == 2 ? IC_TABLET : IC_LAPTOP;
}

// ------------------------------------------------------------ hojas
static void open_drawer(void)
{
	sheet_open(&S, SK_DRAWER, APP_NAME, true);
	s_parent = SK_NONE;
	char sub[96];
	snprintf(sub, sizeof sub, V.hist_n == 1 ? _("%d evento") : _("%d eventos"), V.hist_n);
	sheet_add(&S, IC_CLOCK, _("Actividad"), V.hist_n ? sub : _("Aún no hay actividad"), SR_CHEV, ID_ACTIVITY);
	int on = online_count(), pr = paired_count();
	if (pr)
		snprintf(sub, sizeof sub, _("%d conectados · %d vinculados"), on, pr);
	else
		snprintf(sub, sizeof sub, "%s", _("Ninguno vinculado todavía"));
	sheet_add(&S, IC_DEVICES, _("Dispositivos"), sub, SR_CHEV, ID_DEVICES);
	sheet_header(&S, "");
	sheet_add(&S, IC_GEAR, _("Ajustes"), NULL, SR_CHEV, ID_SETTINGS);
	snprintf(sub, sizeof sub, _("Versión %s · por %s"), APP_VERSION, "VENOMSHiNE");
	sheet_add(&S, IC_INFO, _("Acerca de Cloud3DS"), sub, SR_CHEV, ID_ABOUT);
	sheet_add(&S, IC_POWER, _("Salir"), NULL, SR_NONE, ID_EXIT);
	S.sel = 0;
}

static void open_access(int parent)
{
	sheet_open(&S, SK_ACCESS, _("Permisos"), false);
	s_parent = parent;
	if (parent)
		S.anim = 1;
	sheet_add(&S, IC_FOLDER, _("Lectura y escritura"), _("Pueden subir, renombrar y borrar"), V.readonly ? SR_NONE : SR_CHECK, ID_RW);
	sheet_add(&S, IC_LOCK, _("Solo lectura"), _("Solo pueden ver y descargar"), V.readonly ? SR_CHECK : SR_NONE, ID_RO);
	sheet_header(&S, "");
	sheet_add(&S, IC_SHIELD, _("Proteger carpetas del sistema"), _("Pide confirmación en Nintendo 3DS, luma…"),
	          V.protect_sys ? SR_ON : SR_OFF, ID_PROTECT);
	S.sel = V.readonly ? 1 : 0;
}

static void open_devices(int parent, int keep_sel)
{
	float sc = S.scroll;
	sheet_open(&S, SK_DEVICES, _("Dispositivos"), false);
	s_parent = parent;
	if (parent || keep_sel >= 0)
		S.anim = 1;
	for (int pass = 0; pass < 2; pass++) {
		bool first = true;
		for (int i = 0; i < MAX_DEVICES; i++) {
			const Device *d = &V.dev[i];
			if (!d->used || dev_online(d) != (pass == 0))
				continue;
			if (first)
				sheet_header(&S, pass == 0 ? _("Conectados ahora") : _("Vinculados"));
			first = false;
			char sub[96], when[32];
			if (pass == 0)
				snprintf(sub, sizeof sub, "%s · %s", _("Conectado"), d->ip);
			else {
				util_fmt_when(when, sizeof when, d->last_seen);
				snprintf(sub, sizeof sub, _("Visto: %s"), when);
			}
			SheetItem *it = sheet_add(&S, dev_icon(d), d->name, sub, SR_CHEV, 100 + i);
			it->icon_col = pass == 0 ? T.ok : T.text2;
		}
	}
	if (!paired_count()) {
		SheetItem *e = sheet_add(&S, -1, _("Ningún dispositivo vinculado"), _("Escanea el QR de arriba con tu móvil"), SR_NONE, -9);
		e->disabled = true;
	}
	if (keep_sel >= 0 && keep_sel < S.n) {
		S.sel = keep_sel;
		S.scroll = S.scroll_target = sc;
	} else {
		S.sel = -1;
		for (int i = 0; i < S.n; i++)
			if (!S.items[i].header && !S.items[i].disabled) {
				S.sel = i;
				break;
			}
	}
}

static void open_activity(int parent)
{
	sheet_open(&S, SK_ACTIVITY, _("Actividad"), false);
	s_parent = parent;
	if (parent)
		S.anim = 1;
	long today = (long)(util_unix_now() / 86400);
	int section = -1;
	for (int i = 0; i < V.hist_n; i++) {
		const Event *e = ev(i);
		int sec = (long)(e->when / 86400) == today ? 0 : 1;
		if (sec != section) {
			sheet_header(&S, sec == 0 ? _("Hoy") : _("Antes"));
			section = sec;
		}
		char title[128], sub[140];
		IconId ic = IC_INFO;
		u32 col;
		ev_info(e, title, sizeof title, sub, sizeof sub, &ic, &col);
		SheetItem *it = sheet_add(&S, ic, title, sub, SR_VALUE, i);
		it->icon_col = col;
		if (sec == 0)
			util_fmt_hm(it->value, sizeof it->value, e->when);
		else
			util_fmt_when(it->value, sizeof it->value, e->when);
	}
	if (!V.hist_n) {
		SheetItem *e = sheet_add(&S, -1, _("Aún no hay actividad"), _("Sube algo desde tu móvil o tu PC"), SR_NONE, -9);
		e->disabled = true;
	} else {
		sheet_header(&S, "");
		SheetItem *c = sheet_add(&S, IC_TRASH, _("Borrar el historial"), NULL, SR_NONE, ID_CLEAR);
		c->icon_col = T.danger;
	}
	S.sel = -1;
	for (int i = 0; i < S.n; i++)
		if (!S.items[i].header && !S.items[i].disabled) {
			S.sel = i;
			break;
		}
}

static void open_settings(int parent, int keep_sel)
{
	float sc = S.scroll, sct = S.scroll_target;
	sheet_open(&S, SK_SETTINGS, _("Ajustes"), false);
	s_parent = parent;
	if (parent || keep_sel >= 0)
		S.anim = 1;
	sheet_header(&S, _("Acceso"));
	SheetItem *it = sheet_add(&S, IC_LOCK, _("Permisos"), NULL, SR_VALUE, ID_ACCESS);
	snprintf(it->value, sizeof it->value, "%s", V.readonly ? _("Solo lectura") : _("Lectura y escritura"));
	sheet_add(&S, IC_SHIELD, _("Proteger carpetas del sistema"), _("Pide confirmación en Nintendo 3DS, luma…"),
	          V.protect_sys ? SR_ON : SR_OFF, ID_PROTECT);
	it = sheet_add(&S, IC_KEY, _("PIN nuevo"), _("Invalida el QR y el PIN actuales"), SR_VALUE, ID_PIN);
	snprintf(it->value, sizeof it->value, "%s", V.pin);
	sheet_header(&S, _("Apariencia"));
	it = sheet_add(&S, IC_GLOBE, _("Idioma"), NULL, SR_VALUE, ID_LANG);
	snprintf(it->value, sizeof it->value, "%s", lang_setting_name(V.lang));
	it = sheet_add(&S, IC_PALETTE, _("Tema"), NULL, SR_VALUE, ID_THEME);
	snprintf(it->value, sizeof it->value, "%s", V.light ? _("Claro") : _("Oscuro"));
	sheet_header(&S, _("Información"));
	char sub[80];
	snprintf(sub, sizeof sub, _("Versión %s · por %s"), APP_VERSION, "VENOMSHiNE");
	sheet_add(&S, IC_INFO, _("Acerca de Cloud3DS"), sub, SR_CHEV, ID_ABOUT);
	if (keep_sel >= 0) {
		S.sel = keep_sel;
		S.scroll = sc;
		S.scroll_target = sct;
	} else
		S.sel = 1;
}

static void open_lang(void)
{
	sheet_open(&S, SK_LANG, _("Idioma"), false);
	s_parent = SK_SETTINGS;
	S.anim = 1;
	for (int l = LSET_SYSTEM; l <= LSET_JA; l++)
		sheet_add(&S, l == LSET_SYSTEM ? IC_GEAR : IC_GLOBE, lang_setting_name(l),
		          l == LSET_SYSTEM ? _("Usa el idioma de la consola") : NULL, V.lang == l ? SR_CHECK : SR_NONE, l);
	S.sel = V.lang;
}

static void open_about(int parent)
{
	sheet_open(&S, SK_ABOUT, _("Acerca de Cloud3DS"), false);
	s_parent = parent;
	S.anim = 1;
	sheet_header(&S, _("Creado por VENOMSHiNE · elige uno para ver su QR"));
	SheetItem *a = sheet_add(&S, IC_CODE, "GitHub", "github.com/venomshine", SR_CHEV, ID_GITHUB);
	a->icon_col = T.text;
	SheetItem *b = sheet_add(&S, IC_CHAT, "Discord", _("Únete a la comunidad"), SR_CHEV, ID_DISCORD);
	b->icon_col = col_hex(0x8C9EFF);
	SheetItem *c = sheet_add(&S, IC_STAR, "Ko-fi", _("Invítame a un café"), SR_CHEV, ID_KOFI);
	c->icon_col = col_hex(0xFF8A80);
	sheet_header(&S, "");
	SheetItem *v = sheet_add(&S, IC_CLOUD, "Cloud3DS " APP_VERSION, _("Gracias por usarlo"), SR_NONE, ID_VERSION);
	v->icon_col = T.accent;
	S.sel = S.n - 1;   // la versión: arriba se ve el texto de presentación
}

static void reopen(int kind)
{
	switch (kind) {
	case SK_DRAWER: open_drawer(); S.anim = 1; break;
	case SK_SETTINGS: open_settings(SK_DRAWER, -1); break;
	default: break;
	}
}

static void set_access(bool readonly, bool protect)
{
	state_lock();
	G.readonly = readonly;
	G.protect_sys = protect;
	G.dirty_cfg = true;
	G.rev++;
	state_unlock();
}

static void handle_choice(int idx)
{
	SheetItem *it = &S.items[idx];
	int id = it->id;
	switch (S.kind) {
	case SK_DRAWER:
		if (id == ID_ACTIVITY) open_activity(SK_DRAWER);
		else if (id == ID_DEVICES) open_devices(SK_DRAWER, -1);
		else if (id == ID_SETTINGS) open_settings(SK_DRAWER, -1);
		else if (id == ID_ABOUT) open_about(SK_DRAWER);
		else if (id == ID_EXIT) { sheet_close(&S); s_quit = true; }
		break;
	case SK_ACCESS: {
		int keep = idx;
		if (id == ID_RW) set_access(false, V.protect_sys);
		else if (id == ID_RO) set_access(true, V.protect_sys);
		else if (id == ID_PROTECT) set_access(V.readonly, !V.protect_sys);
		V.readonly = G.readonly;
		V.protect_sys = G.protect_sys;
		int parent = s_parent;
		open_access(parent);
		S.anim = 1;
		S.sel = keep;
		if (id != ID_PROTECT)
			toast(V.readonly ? _("Solo lectura activado") : _("Lectura y escritura activadas"));
		break;
	}
	case SK_DEVICES:
		if (id >= 100) {
			const Device *d = &V.dev[id - 100];
			char msg[200];
			snprintf(msg, sizeof msg, _("%s tendrá que volver a escanear el QR o escribir el PIN."), d->name);
			s_dlg_arg = id - 100;
			dialog_open(DLG_REVOKE, _("¿Expulsar dispositivo?"), msg, _("Expulsar"), true);
		}
		break;
	case SK_ACTIVITY:
		if (id == ID_CLEAR)
			dialog_open(DLG_CLEAR, _("¿Borrar el historial?"), _("Se borra la lista de actividad. Tus archivos no se tocan."),
			            _("Borrar"), true);
		break;
	case SK_SETTINGS:
		if (id == ID_ACCESS) open_access(SK_SETTINGS);
		else if (id == ID_PROTECT) {
			set_access(V.readonly, !V.protect_sys);
			V.protect_sys = G.protect_sys;
			open_settings(s_parent, idx);
		} else if (id == ID_PIN)
			dialog_open(DLG_NEWPIN, _("¿Crear un PIN nuevo?"),
			            _("El PIN y el QR actuales dejarán de servir. Los dispositivos ya vinculados siguen conectados."),
			            _("Cambiar"), false);
		else if (id == ID_LANG) open_lang();
		else if (id == ID_THEME) {
			state_lock();
			G.light = !G.light;
			G.dirty_cfg = true;
			G.rev++;
			V.light = G.light;
			state_unlock();
			theme_set(!V.light);
			open_settings(s_parent, idx);
		} else if (id == ID_ABOUT) open_about(SK_SETTINGS);
		break;
	case SK_LANG:
		state_lock();
		G.lang = id;
		G.dirty_cfg = true;
		G.rev++;
		V.lang = id;
		state_unlock();
		i18n_apply(id);
		open_settings(SK_DRAWER, 5);
		break;
	default:
		break;
	}
}

static void handle_dialog(int ctx, bool ok)
{
	if (!ok)
		return;
	state_lock();
	switch (ctx) {
	case DLG_REVOKE: {
		Device *d = &G.dev[s_dlg_arg];
		if (d->used) {
			event_add(EV_REVOKED, "", d->name, 0);
			d->used = false;
			G.dirty_cfg = true;
		}
		break;
	}
	case DLG_NEWPIN:
		pin_regen();
		break;
	case DLG_CLEAR:
		G.hist_n = 0;
		G.hist_head = 0;
		G.dirty_cfg = true;
		break;
	case DLG_EXIT:
		s_quit = true;
		break;
	}
	G.rev++;
	memcpy(&V, &G, sizeof G);
	s_hist_n = V.hist_n;
	s_hist_head = V.hist_head;
	state_unlock();
	if (ctx == DLG_NEWPIN) {
		toast(_("PIN y QR nuevos listos"));
		if (S.open && S.kind == SK_SETTINGS) open_settings(s_parent, S.sel);
	}
	if (ctx == DLG_REVOKE && S.open && S.kind == SK_DEVICES) open_devices(s_parent, -1);
	if (ctx == DLG_CLEAR && S.open) sheet_close(&S);
}

// ------------------------------------------------------------ pantalla superior
static void draw_battery(float x, float y, int pct, bool charging)
{
	float w = 19, h = 10;
	draw_rrect_outline(x, y, w, h, 2.5f, 1.2f, T.text3);
	draw_rrect(x + w + 0.5f, y + 3, 2, h - 6, 1, T.text3);
	if (pct >= 0) {
		u32 c = charging ? T.accent : (pct <= 15 ? T.danger : (pct <= 30 ? col_hex(0xFDD663) : T.ok));
		draw_rrect(x + 2, y + 2, fmaxf(1.5f, (w - 4) * pct / 100.0f), h - 4, 1.2f, c);
	}
	if (charging)
		draw_icon(IC_BOLT, x + w / 2, y + h / 2, 9, T.bg);
}

static const char *status_label(void)
{
	const Xfer *xs[1];
	if (!V.ip) return _("Sin Wi-Fi");
	if (V.srv_error[0]) return _("Error");
	if (!V.running) return _("Preparando…");
	if (visible_xfers(xs, 1) && xs[0]->st == XS_RUNNING)
		return xs[0]->kind == XF_UPLOAD ? _("Recibiendo") : _("Enviando");
	if (V.readonly) return _("Solo lectura");
	return _("Lista");
}

static void draw_header_top(void)
{
	draw_rect(0, 0, TOP_W, TOP_HDR - 4, T.bg);
	draw_grad_v(0, TOP_HDR - 4, TOP_W, 6, T.bg, col_alpha(T.bg, 0));
	// derecha: fecha · hora   [batería] 87 %
	float rx = TOP_W - 10;
	bool chg = false;
	int pct = util_battery(&chg);
	if (pct >= 0) {
		char bp[8];
		snprintf(bp, sizeof bp, "%d%%", pct);
		text_draw_right(bp, rx, 5, 0.45f, T.text2);
		rx -= text_width(bp, 0.45f) + 4;
		draw_battery(rx - 22, 6, pct, chg);
		rx -= 22 + 10;
	}
	char clk[8];
	util_fmt_now(clk, sizeof clk);
	text_draw_right(clk, rx, 5, 0.45f, T.text2);
	rx -= text_width(clk, 0.45f) + 5;
	draw_circle(rx - 1.5f, 12, 1.3f, T.text3);
	rx -= 6;
	char day[32];
	util_fmt_today(day, sizeof day);
	text_draw_right(day, rx, 5, 0.45f, T.text3);
	rx -= text_width(day, 0.45f) + 8;
	// izquierda: Cloud3DS + estado (se recorta para no pisar la fecha)
	text_draw(APP_NAME, 10, 3, 0.55f, T.text);
	float mx = 10 + text_width(APP_NAME, 0.55f) + 6;
	if (rx - 8 - mx > 20)
		text_draw_clip(status_label(), mx, 5, rx - 8 - mx, 0.45f, T.text3);
	const Xfer *xs[1];
	if (visible_xfers(xs, 1) && xs[0]->st == XS_RUNNING) {
		float p = g_time * 1.2f;
		draw_grad_h(0, TOP_HDR - 4, TOP_W, 2, col_alpha(brand_grad(p), 0.9f), col_alpha(brand_grad(p + 0.5f), 0.9f));
	}
}

static void bg_glow(float w)
{
	float t = g_time * 0.15f;
	draw_grad4(0, 140, w, 100, col_alpha(brand_grad(t), 0), col_alpha(brand_grad(t + 0.3f), 0),
	           col_alpha(brand_grad(t), 0.14f), col_alpha(brand_grad(t + 0.3f), 0.14f));
}

static void draw_qr_card(void)
{
	char url[64], qr[96];
	if (!V.ip || V.srv_error[0] || !V.running) {
		draw_rrect(QR_X, QR_Y, QR_S, QR_S, 14, T.surface);
		if (!V.ip) {
			draw_icon(IC_WIFI_OFF, QR_X + QR_S / 2, QR_Y + 58, 44, T.text3);
			text_draw_center(_("Sin conexión Wi-Fi"), QR_X + QR_S / 2, QR_Y + 96, 0.5f, T.text2);
			text_wrap_lines(_("Conecta la consola a tu Wi-Fi o al punto de acceso de tu móvil. El código aparecerá solo."),
			                QR_S - 24, 0.4f, 4, QR_X + 12, QR_Y + 120, T.text3, true);
		} else if (V.srv_error[0]) {
			draw_icon(IC_INFO, QR_X + QR_S / 2, QR_Y + 58, 40, T.danger);
			text_draw_center(_("No se pudo abrir el servidor"), QR_X + QR_S / 2, QR_Y + 96, 0.45f, T.text2);
			text_wrap_lines(_("Cierra otras apps de red (como FTPD) y vuelve a abrir Cloud3DS."), QR_S - 24, 0.4f, 4,
			                QR_X + 12, QR_Y + 120, T.text3, true);
		} else {
			draw_arc(QR_X + QR_S / 2, QR_Y + QR_S / 2 - 8, 14, 3, g_time * 6, g_time * 6 + 4.2f, T.accent);
			text_draw_center(_("Preparando tu nube…"), QR_X + QR_S / 2, QR_Y + QR_S / 2 + 16, 0.45f, T.text2);
		}
		return;
	}
	url_text(url, sizeof url, true);
	snprintf(qr, sizeof qr, "%s/#k=%s", url, V.qr_key);
	float pulse = 0.5f + 0.5f * sinf(g_time * 2);
	draw_rrect(QR_X - 3, QR_Y - 3, QR_S + 6, QR_S + 6, 16, col_alpha(brand_grad(g_time * 0.1f), 0.35f + 0.25f * pulse));
	draw_qr(qr, QR_X, QR_Y, QR_S, 1);
	text_draw_center(_("Escanéalo con la cámara del móvil"), QR_X + QR_S / 2, QR_Y + QR_S + 6, 0.4f, T.text3);
}

static void draw_status_line(float y)
{
	int on = online_count();
	if (!V.ip || !V.running)
		return;
	if (on) {
		char s[48];
		snprintf(s, sizeof s, on == 1 ? _("%d dispositivo conectado") : _("%d dispositivos conectados"), on);
		draw_circle(COL_X + 4, y + 8, 4, T.ok);
		text_draw_clip(s, COL_X + 14, y + 1, COL_W - 14, 0.45f, T.ok);
	} else {
		int dots = (int)(g_time * 2) % 4;
		char w[48];
		snprintf(w, sizeof w, _("Esperando conexión%.*s"), dots, "...");
		text_draw(w, COL_X, y + 1, 0.45f, T.text3);
	}
}

// columna derecha: cómo conectarse
static void draw_connect_col(float a, float dy)
{
	float tx = COL_X, y0 = QR_Y + dy;
	if (!V.ip) {
		text_draw(_("Conecta tu"), tx, y0, 0.72f, col_alpha(T.text, a));
		text_draw_grad(_("consola"), tx, y0 + 22, 0.72f, g_time * 0.1f, a);
		text_wrap_lines(_("Cloud3DS funciona en tu red local: el móvil o el PC tienen que estar en el mismo Wi-Fi."),
		                COL_W, 0.42f, 5, tx, y0 + 58, col_alpha(T.text2, a), true);
		return;
	}
	text_draw(_("Abre tu"), tx, y0, 0.72f, col_alpha(T.text, a));
	text_draw_grad(_("nube local"), tx, y0 + 22, 0.72f, g_time * 0.1f, a);
	text_draw(_("O escribe en el navegador:"), tx, y0 + 58, 0.42f, col_alpha(T.text3, a));
	char url[48];
	url_text(url, sizeof url, false);
	float s = 0.62f;
	while (s > 0.44f && text_width(url, s) > COL_W)
		s -= 0.02f;
	text_draw(url, tx, y0 + 74, s, col_alpha(T.accent, a));
	text_draw(_("PIN"), tx, y0 + 108, 0.42f, col_alpha(T.text3, a));
	for (int i = 0; i < 4; i++) {
		float bx = tx + 30 + i * 30, by = y0 + 102;
		draw_rrect(bx, by, 26, 28, 8, col_alpha(T.surface2, a));
		char d[2] = {V.pin[i], 0};
		text_draw_center(d, bx + 13, by + 5, 0.62f, col_alpha(T.text, a));
	}
}

// columna derecha: transferencia en curso (el QR sigue a la vista)
static void draw_xfer_col(float a, float dy)
{
	const Xfer *xs[3];
	int n = visible_xfers(xs, 3);
	if (!n)
		return;
	const Xfer *x = xs[0];
	float tx = COL_X, y = QR_Y + dy;
	bool up = x->kind == XF_UPLOAD;
	const char *head;
	u32 hc = T.text;
	IconId ic = up ? IC_UPLOAD : IC_DOWNLOAD;
	if (x->st == XS_RUNNING)
		head = up ? _("Recibiendo") : _("Enviando");
	else if (x->st == XS_DONE) {
		head = _("Completado");
		hc = T.ok;
		ic = IC_CHECK;
	} else {
		head = _("No se completó");
		hc = T.danger;
		ic = IC_CLOSE;
	}
	draw_icon(ic, tx + 9, y + 10, 18, col_alpha(hc, a));
	text_draw(head, tx + 24, y + 1, 0.6f, col_alpha(hc, a));
	text_wrap_lines(x->name, COL_W, 0.5f, 2, tx, y + 28, col_alpha(T.text, a), true);
	char sub[96];
	snprintf(sub, sizeof sub, up ? _("de %s") : _("a %s"), x->device);
	text_draw_clip(sub, tx, y + 64, COL_W, 0.42f, col_alpha(T.text3, a));

	// barra fina con el degradado de la marca, que la transferencia va descubriendo
	float frac = x->total ? (float)((double)x->done / x->total) : (x->st == XS_DONE ? 1 : 0);
	if (x->st == XS_DONE)
		frac = 1;
	float bx = tx, by = y + 88, bw = COL_W, bh = 5;
	draw_rrect(bx, by, bw, bh, bh / 2, col_alpha(T.surface2, a));
	if (frac > 0) {
		float fw = bw * frac;
		gfx_clip(bx, by, fw, bh);
		for (int k = 0; k < 16; k++)
			draw_grad_h(bx + bw * k / 16, by, bw / 16 + 1, bh, col_alpha(brand_grad(k / 16.0f * 0.6f), a),
			            col_alpha(brand_grad((k + 1) / 16.0f * 0.6f), a));
		gfx_clip_off();
		draw_circle(bx + fw, by + bh / 2, 3.2f, col_alpha(brand_grad(frac * 0.6f), a));
	}
	char d[24], t[24], line[80], pc[12];
	util_fmt_size(d, sizeof d, x->done);
	util_fmt_size(t, sizeof t, x->total);
	snprintf(line, sizeof line, _("%s de %s"), d, t);
	text_draw_clip(line, tx, by + 10, COL_W - 44, 0.42f, col_alpha(T.text2, a));
	snprintf(pc, sizeof pc, "%d%%", (int)(frac * 100));
	text_draw_right(pc, tx + COL_W, by + 8, 0.5f, col_alpha(brand_grad(0.3f), a));
	if (x->st == XS_RUNNING && x->speed > 0) {
		char sp[24], eta[24];
		util_fmt_speed(sp, sizeof sp, x->speed);
		util_fmt_eta(eta, sizeof eta, (x->total - x->done) / x->speed);
		snprintf(line, sizeof line, _("%s · quedan %s"), sp, eta);
		text_draw_clip(line, tx, by + 28, COL_W, 0.42f, col_alpha(T.text3, a));
	}
	if (n > 1) {
		char more[48];
		snprintf(more, sizeof more, _("y %d más"), n - 1);
		text_draw(more, tx, by + 46, 0.42f, col_alpha(T.text3, a));
	}
}

// información de la hoja abierta: ocupa la columna derecha y el QR sigue a la vista
static bool draw_sheet_context_top(void)
{
	if (!S.open)
		return false;
	SheetItem *it = sheet_selected(&S);
	float a = ease_out_cubic(S.anim);
	if (S.kind == SK_ABOUT) {
		draw_rect(0, TOP_HDR, TOP_W, SCR_H - TOP_HDR, col_alpha(T.bg, a));
		const char *url = NULL, *name = NULL, *desc = NULL;
		if (it && it->id == ID_GITHUB) { url = LINK_GITHUB; name = "GitHub"; desc = _("Código, novedades y nuevas versiones de mis proyectos."); }
		if (it && it->id == ID_DISCORD) { url = LINK_DISCORD; name = "Discord"; desc = _("Únete a la comunidad: dudas, ideas y avisos de actualizaciones."); }
		if (it && it->id == ID_KOFI) { url = LINK_KOFI; name = "Ko-fi"; desc = _("Si te gusta Cloud3DS, invítame a un café. ¡Ayuda muchísimo!"); }
		if (url) {
			draw_qr(url, QR_X, QR_Y, QR_S, a);
			float tx = COL_X;
			text_draw(name, tx, QR_Y + 4, 0.75f, col_alpha(T.text, a));
			text_wrap_lines(desc, COL_W, 0.45f, 5, tx, QR_Y + 34, col_alpha(T.text2, a), true);
			text_draw(_("Escanéalo con tu teléfono"), tx, QR_Y + 130, 0.42f, col_alpha(T.text3, a));
			text_wrap_lines(url + 8, COL_W, 0.4f, 3, tx, QR_Y + 150, col_alpha(T.accent, a), true);
		} else {
			draw_cloud(44, 62, 20, a);
			text_draw_grad(APP_NAME, 78, 42, 1.0f, g_time * 0.1f, a);
			char vb[80];
			snprintf(vb, sizeof vb, _("v%s · hecho por %s"), APP_VERSION, "VENOMSHiNE");
			text_draw(vb, 80, 74, 0.47f, col_alpha(T.text2, a));
			text_wrap_lines(_(ABOUT_TEXT), 364, 0.44f, 6, 18, 102, col_alpha(T.text2, a), true);
			text_draw(_("Interfaz basada en Gemi3DS"), 18, 206, 0.42f, col_alpha(T.text3, a));
		}
		return true;
	}
	if (!it || (S.kind != SK_DEVICES && S.kind != SK_ACTIVITY && S.kind != SK_SETTINGS && S.kind != SK_ACCESS))
		return false;
	// panel en la columna derecha
	float x = COL_X - 6, y = QR_Y - 2, w = TOP_W - x - 8, h = QR_S + 6;
	draw_rect(x - 4, y - 4, w + 12, h + 30, col_alpha(T.bg, a));
	draw_rrect(x, y, w, h, 14, col_alpha(T.surface, a));
	float tx = x + 12, tw = w - 24;
	if (S.kind == SK_DEVICES && it->id >= 100) {
		const Device *d = &V.dev[it->id - 100];
		draw_icon(dev_icon(d), tx + 9, y + 20, 18, col_alpha(dev_online(d) ? T.ok : T.text2, a));
		text_draw_clip(d->name, tx + 24, y + 11, tw - 24, 0.55f, col_alpha(T.text, a));
		char l1[80], when[32];
		util_fmt_when(when, sizeof when, d->created);
		snprintf(l1, sizeof l1, _("Vinculado: %s"), when);
		text_draw_clip(l1, tx, y + 42, tw, 0.42f, col_alpha(T.text2, a));
		if (dev_online(d))
			snprintf(l1, sizeof l1, "%s", _("Conectado ahora"));
		else {
			util_fmt_when(when, sizeof when, d->last_seen);
			snprintf(l1, sizeof l1, _("Visto: %s"), when);
		}
		text_draw_clip(l1, tx, y + 60, tw, 0.42f, col_alpha(dev_online(d) ? T.ok : T.text2, a));
		snprintf(l1, sizeof l1, "IP %s", d->ip);
		text_draw_clip(l1, tx, y + 78, tw, 0.42f, col_alpha(T.text3, a));
		text_wrap_lines(_("Toca para expulsarlo: tendrá que volver a escanear el QR o escribir el PIN."), tw, 0.4f, 3, tx,
		                y + 112, col_alpha(T.text3, a), true);
	} else if (S.kind == SK_ACTIVITY && it->id >= 0 && it->id < V.hist_n) {
		const Event *e = ev(it->id);
		char title[128], sub[140], when[32];
		IconId ic = IC_INFO;
		u32 col;
		ev_info(e, title, sizeof title, sub, sizeof sub, &ic, &col);
		draw_icon(ic, tx + 9, y + 20, 18, col_alpha(col, a));
		util_fmt_when(when, sizeof when, e->when);
		text_draw_clip(when, tx + 24, y + 13, tw - 24, 0.45f, col_alpha(T.text3, a));
		int nl = text_wrap_lines(title, tw, 0.5f, 3, tx, y + 40, col_alpha(T.text, a), true);
		text_wrap_lines(sub, tw, 0.42f, 4, tx, y + 44 + nl * text_lineh(0.5f), col_alpha(T.text2, a), true);
	} else {
		const char *t = it->label, *d = "";
		if (it->id == ID_ACCESS || it->id == ID_RW || it->id == ID_RO)
			d = _("Con «Solo lectura», los dispositivos pueden ver y descargar, pero no subir, renombrar ni borrar nada.");
		else if (it->id == ID_PROTECT)
			d = _("Antes de borrar o cambiar Nintendo 3DS, luma/payloads, boot.firm y otras carpetas importantes, la web pide confirmación.");
		else if (it->id == ID_PIN)
			d = _("Crea un PIN y un QR nuevos. Los dispositivos ya vinculados siguen conectados; los nuevos necesitarán el PIN nuevo.");
		else if (it->id == ID_LANG)
			d = _("Idioma de la consola. La web usa el idioma de tu móvil o tu PC.");
		else if (it->id == ID_THEME)
			d = _("Tema oscuro o claro para la consola. La web sigue el tema de tu móvil o tu PC.");
		else if (it->id == ID_ABOUT)
			d = _("Versión, créditos y enlaces a la comunidad.");
		else
			return true;
		text_draw_clip(t, tx, y + 12, tw, 0.55f, col_alpha(T.text, a));
		text_wrap_lines(d, tw, 0.43f, 8, tx, y + 40, col_alpha(T.text2, a), true);
	}
	return true;
}

static void draw_top(void)
{
	gfx_target(g_top, T.bg);
	bg_glow(TOP_W);
	bool about = S.open && S.kind == SK_ABOUT;
	if (!about)
		draw_qr_card();
	const Xfer *xs[1];
	bool busy = visible_xfers(xs, 1) > 0;
	s_busy = approach(s_busy, busy ? 1 : 0, 9);
	float b = ease_in_out(s_busy);
	if (!draw_sheet_context_top()) {
		if (b < 0.99f)
			draw_connect_col(1 - b, -b * 10);
		if (b > 0.01f)
			draw_xfer_col(b, (1 - b) * 10);
		draw_status_line(QR_Y + QR_S - 14);
	}
	draw_header_top();
	toast_draw_top();
}

// ------------------------------------------------------------ pantalla inferior
static void draw_top_bar(void)
{
	if (ui_icon_button(IC_MENU, 19, 19, 15, T.text2, 0) && !g_dialog.open && !S.open)
		open_drawer();
	const char *lbl = V.readonly ? _("Solo lectura") : _("Lectura y escritura");
	float w = text_width(lbl, 0.5f) + 50;
	if (w > 200)
		w = 200;
	float x = 160 - w / 2;
	bool p = touch_in(x, 4, w, 30);
	draw_rrect(x, 7, w, 24, 12, p ? T.pill_hi : T.surface);
	draw_icon(V.readonly ? IC_LOCK : IC_FOLDER, x + 15, 19, 13, V.readonly ? col_hex(0xFDD663) : T.accent);
	text_draw_clip(lbl, x + 26, 12, w - 44, 0.5f, T.text);
	draw_icon(IC_CHEV_DOWN, x + w - 13, 19, 11, T.text2);
	if (tap(x, 4, w, 30) && !g_dialog.open && !S.open)
		open_access(SK_NONE);
	// dispositivos, con los conectados en un globo
	if (ui_icon_button(IC_DEVICES, 301, 19, 15, T.text2, 0) && !g_dialog.open && !S.open)
		open_devices(SK_NONE, -1);
	int on = online_count();
	if (on) {
		char n[12];
		snprintf(n, sizeof n, "%d", on > 9 ? 9 : on);
		draw_circle(311, 10, 6.5f, T.ok);
		text_draw_center(n, 311, 4.5f, 0.38f, T.bg);
	}
}

static void stat_tile(int i, IconId ic, u32 icc, const char *label, const char *val)
{
	float x = 10 + (i % 2) * 154, y = 62 + (i / 2) * 54;
	float app = clampf(g_time * 3 - i * 0.15f, 0, 1);
	bool p = touch_in(x, y, 146, 46);
	draw_rrect(x, y + (1 - ease_out_cubic(app)) * 8, 146, 46, 14, col_alpha(p ? T.pill_hi : T.surface, app));
	draw_icon(ic, x + 21, y + 23, 18, col_alpha(icc, app));
	text_draw_clip(label, x + 38, y + 7, 102, 0.42f, col_alpha(T.text3, app));
	text_draw_clip(val, x + 38, y + 21, 102, 0.55f, col_alpha(T.text, app));
}

static void draw_home_bot(void)
{
	u64 now = util_ms();
	if (now - s_space_t > 5000 || !s_space_t) {
		s_space_t = now;
		fsx_space(&s_free, &s_total);
	}
	text_draw_center(_("Hoy en tu nube"), 160, 43, 0.45f, T.text3);
	char v[32];
	snprintf(v, sizeof v, "%d", online_count());
	stat_tile(0, IC_DEVICES, col_hex(0x8AB4F8), _("Conectados"), v);
	util_fmt_size(v, sizeof v, s_free);
	stat_tile(1, IC_FOLDER, col_hex(0xFDD663), _("Espacio libre"), v);
	util_fmt_size(v, sizeof v, V.stat_up);
	stat_tile(2, IC_UPLOAD, col_hex(0x81C995), _("Recibido"), v);
	util_fmt_size(v, sizeof v, V.stat_down);
	stat_tile(3, IC_DOWNLOAD, col_hex(0xF28B82), _("Enviado"), v);
	if (!g_dialog.open && !S.open) {
		if (tap(10, 62, 146, 46))
			open_devices(SK_NONE, -1);
		else if (tap(164, 62, 146, 46) || tap(10, 116, 300, 46))
			open_activity(SK_NONE);
	}
}

// barra inferior: lo último que pasó (abre la actividad)
static void draw_last_bar(void)
{
	bool p = touch_in(8, 184, 304, 46);
	draw_rrect(8, 184, 304, 46, 23, p ? T.pill_hi : T.pill);
	if (!T.dark)
		draw_rrect_outline(8, 184, 304, 46, 23, 1, T.divider);
	if (!V.hist_n) {
		draw_icon(IC_CLOCK, 31, 207, 17, T.text3);
		text_draw_clip(_("Aquí verás lo que suban o descarguen"), 52, 199, 228, 0.5f, T.text3);
	} else {
		const Event *e = ev(0);
		char title[128], sub[140], hm[16];
		IconId ic = IC_INFO;
		u32 col;
		ev_info(e, title, sizeof title, sub, sizeof sub, &ic, &col);
		draw_icon(ic, 31, 207, 17, col);
		util_fmt_hm(hm, sizeof hm, e->when);
		float hw = text_width(hm, 0.42f);
		text_draw_clip(title, 52, 191, 230 - hw - 8, 0.5f, T.text);
		text_draw_right(hm, 282, 193, 0.42f, T.text3);
		text_draw_clip(sub, 52, 208, 230, 0.42f, T.text3);
	}
	draw_icon(IC_CHEV_RIGHT, 297, 207, 12, T.text3);
	if (tap(8, 184, 304, 46) && !g_dialog.open && !S.open)
		open_activity(SK_NONE);
}

static void draw_bottom(void)
{
	gfx_target(g_bot, T.bg);
	draw_top_bar();
	draw_home_bot();
	draw_last_bar();
	sheet_draw(&S);
	dialog_draw();
}

// ------------------------------------------------------------ lógica
static void watch_events(void)
{
	if (V.hist_n == s_hist_n && V.hist_head == s_hist_head)
		return;
	if (s_events_seen && V.hist_n > 0) {
		const Event *e = ev(0);
		char msg[160];
		if (e->kind == EV_PAIRED) {
			snprintf(msg, sizeof msg, _("Vinculado: %s"), e->device);
			toast(msg);
		} else if (e->kind == EV_PIN_RESET) {
			toast(_("PIN cambiado por seguridad"));
		} else if (e->kind == EV_DELETE) {
			snprintf(msg, sizeof msg, _("Eliminado: %s"), e->name);
			toast(msg);
		} else if (e->kind == EV_UPLOAD_FAIL) {
			snprintf(msg, sizeof msg, _("No se completó: %s"), e->name);
			toast(msg);
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
	if (want_off != s_lid_off) {
		if (R_SUCCEEDED(gspLcdInit())) {
			if (want_off)
				GSPLCD_PowerOffAllBacklights();
			else
				GSPLCD_PowerOnAllBacklights();
			gspLcdExit();
		}
		s_lid_off = want_off;
	}
}

static void ask_exit(void)
{
	if (xfer_active())
		dialog_open(DLG_EXIT, _("¿Salir de Cloud3DS?"), _("Hay transferencias en curso y se cancelarán."), _("Salir"), true);
	else
		s_quit = true;
}

static void update(void)
{
	int d = dialog_update();
	if (d >= 0) {
		handle_dialog(g_dialog.ctx, d == 1);
		return;
	}
	if (g_dialog.open)
		return;
	if (S.open) {
		int r = sheet_update(&S);
		if (r >= 0)
			handle_choice(r);
		else if (r == -2 && s_parent != SK_NONE)
			reopen(s_parent);
		return;
	}
	if (g_kdown & KEY_X) open_drawer();
	else if (g_kdown & KEY_Y) open_devices(SK_NONE, -1);
	else if (g_kdown & KEY_A) open_activity(SK_NONE);
	else if (g_kdown & KEY_R) open_access(SK_NONE);
	else if (g_kdown & KEY_START) ask_exit();
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

	u64 last_save = util_ms();
	while (aptMainLoop() && !s_quit) {
		input_update();
		gfx_tick();

		// la copia se rehace si algo cambió, durante las transferencias y cada segundo
		// (para que «conectado ahora» esté al día)
		state_lock();
		u64 tnow = util_ms();
		if (G.rev != s_rev || xfer_active() || tnow - s_copy_t > 1000) {
			memcpy(&V, &G, sizeof G);
			s_rev = G.rev;
			s_copy_t = tnow;
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

		u64 now = util_ms();
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
	if (s_lid_off && R_SUCCEEDED(gspLcdInit())) {
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
