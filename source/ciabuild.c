/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "ciabuild.h"
#include "cia_tpl.h"
#include "digest.h"
#include "fsx.h"
#include "i18n.h"
#include "util.h"

#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MU 0x200u        /* unidad de medida del NCCH */
#define BLK 0x1000u      /* bloque de la RomFS (IVFC) */
#define CODE_MAX (32u << 20)
#define HI_APP 0x00040000u

static u32 le32(const u8 *b) { return b[0] | b[1] << 8 | b[2] << 16 | (u32)b[3] << 24; }
static void wle16(u8 *b, u16 v) { b[0] = v, b[1] = v >> 8; }
static void wle32(u8 *b, u32 v) { b[0] = v, b[1] = v >> 8, b[2] = v >> 16, b[3] = v >> 24; }
static void wle64(u8 *b, u64 v) { wle32(b, (u32)v), wle32(b + 4, (u32)(v >> 32)); }
static void wbe16(u8 *b, u16 v) { b[0] = v >> 8, b[1] = v; }
static void wbe32(u8 *b, u32 v) { b[0] = v >> 24, b[1] = v >> 16, b[2] = v >> 8, b[3] = v; }
static void wbe64(u8 *b, u64 v) { wbe32(b, (u32)(v >> 32)), wbe32(b + 4, (u32)v); }
static u64 up(u64 x, u64 a) { return (x + a - 1) / a * a; }

static void sha(const void *d, size_t n, u8 *out)
{
	Sha256 s;
	sha256_init(&s);
	sha256_update(&s, d, n);
	sha256_final(&s, out);
}

/* ---------------------------------------------------------------- leer lo instalado */

// un archivo del NCCH de un título de la SD: type 0 la RomFS (su nivel 3), 2 un archivo del ExeFS por su nombre
static Result ncch_open(u64 tid, u32 type, const char *name, Handle *h)
{
	u32 ap[4] = {(u32)tid, (u32)(tid >> 32), MEDIATYPE_SD, 0};
	u32 fp[5] = {0, 0, type, 0, 0};
	if (name)
		memcpy(&fp[3], name, strnlen(name, 8));
	return FSUSER_OpenFileDirectly(h, ARCHIVE_SAVEDATA_AND_CONTENT, (FS_Path){PATH_BINARY, sizeof ap, ap}, (FS_Path){PATH_BINARY, sizeof fp, fp},
	                               FS_OPEN_READ, 0);
}

static bool exefs_file(u64 tid, const char *name, u8 **out, u32 *n)
{
	*out = NULL, *n = 0;
	Handle h;
	if (R_FAILED(ncch_open(tid, 2, name, &h)))
		return false;
	u64 sz = 0;
	bool ok = R_SUCCEEDED(FSFILE_GetSize(h, &sz)) && sz > 0 && sz <= CODE_MAX;
	u8 *b = ok ? malloc(sz) : NULL;
	u32 rd = 0;
	ok = b && R_SUCCEEDED(FSFILE_Read(h, &rd, 0, b, (u32)sz)) && rd == sz;
	FSFILE_Close(h);
	if (!ok) {
		free(b);
		return false;
	}
	*out = b, *n = (u32)sz;
	return true;
}

static void smdh_text(const u8 *smdh, int lang, int off, int chars, char *out, size_t n)
{
	const u8 *t = smdh + 8 + lang * 0x200 + off;
	if (!t[0] && !t[1])
		t = smdh + 8 + 0x200 + off; // sin ese idioma: el inglés
	u16 tmp[0x81] = {0};
	for (int i = 0; i < chars && i < 0x80; i++)
		tmp[i] = t[i * 2] | t[i * 2 + 1] << 8;
	util_utf16_to_utf8(out, n, tmp, chars);
}

static void smdh_icon(const u8 *smdh, u16 *px)
{
	const u16 *ic = (const u16 *)(smdh + 0x24C0);
	static const u8 tbl[8] = {0x00, 0x01, 0x04, 0x05, 0x10, 0x11, 0x14, 0x15};
	for (int y = 0; y < 48; y++)
		for (int x = 0; x < 48; x++)
			px[y * 48 + x] = ic[((y >> 3) * 6 + (x >> 3)) * 64 + (tbl[x & 7] | tbl[y & 7] << 1)];
}

// todo lo comercial trae alguna clasificación por edades activa (bit 7); el homebrew, ninguna
static bool smdh_rated(const u8 *smdh)
{
	for (int i = 0; i < 16; i++)
		if (smdh[0x2008 + i] & 0x80)
			return true;
	return false;
}

// el contenido principal cifrado (CDN) es de la eShop o de un cartucho; un error al preguntarlo no decide (false)
static bool content_encrypted(u64 tid)
{
	u32 n = 0;
	AM_ContentInfo ci[4];
	if (R_FAILED(AM_ListExistingContentInfos(&n, ci, 0, 4, tid, MEDIATYPE_SD)))
		return false;
	for (u32 i = 0; i < n && i < 4; i++)
		if (ci[i].index == 0)
			return ci[i].type & AM_CONTENT_ENCRYPTED;
	return false;
}

// los títulos con ticket personalizado (comprados en la eShop): no se comparten
static int personalized(u64 **out)
{
	*out = NULL;
	AM_TicketInfo *ti = malloc(sizeof *ti * 512);
	u32 n = 0;
	if (!ti || R_FAILED(AM_GetPersonalizedTicketInfos(&n, ti, 512))) {
		free(ti);
		return 0;
	}
	u64 *ids = malloc(sizeof *ids * (n ? n : 1));
	for (u32 i = 0; ids && i < n; i++)
		ids[i] = ti[i].titleId;
	free(ti);
	*out = ids;
	return ids ? (int)n : 0;
}

static bool in_list(const u64 *l, int n, u64 v)
{
	for (int i = 0; i < n; i++)
		if (l[i] == v)
			return true;
	return false;
}

// ¿homebrew? (con AM ya abierto)
static bool homebrew_am(u64 tid, const u64 *pers, int npers, u8 *smdh_out)
{
	if ((u32)(tid >> 32) != HI_APP || in_list(pers, npers, tid) || content_encrypted(tid))
		return false;
	u8 *smdh = NULL;
	u32 n = 0;
	if (!exefs_file(tid, "icon", &smdh, &n) || n < 0x36C0 || memcmp(smdh, "SMDH", 4) || smdh_rated(smdh)) {
		free(smdh);
		return false;
	}
	if (smdh_out)
		memcpy(smdh_out, smdh, 0x36C0);
	free(smdh);
	return true;
}

int cb_list(CbTitle **out)
{
	*out = NULL;
	if (R_FAILED(amInit()))
		return 0;
	u32 n = 0;
	u64 *ids = NULL, *pers = NULL;
	AM_TitleEntry *info = NULL;
	int cnt = 0, np = personalized(&pers);
	u8 *smdh = malloc(0x36C0);
	if (smdh && R_SUCCEEDED(AM_GetTitleCount(MEDIATYPE_SD, &n)) && n && (ids = malloc(n * sizeof *ids)) && (info = malloc(n * sizeof *info)) &&
	    R_SUCCEEDED(AM_GetTitleList(&n, MEDIATYPE_SD, n, ids)) && R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, n, ids, info))) {
		CbTitle *t = calloc(n, sizeof *t);
		int lang = g_lang == LANG_JA ? 0 : g_lang == LANG_ES ? 5 : 1;
		for (u32 i = 0; t && i < n; i++) {
			if (!homebrew_am(ids[i], pers, np, smdh))
				continue;
			CbTitle *x = &t[cnt++];
			x->tid = ids[i], x->version = info[i].version, x->size = info[i].size;
			smdh_text(smdh, lang, 0x80, 0x80, x->name, sizeof x->name);
			if (!x->name[0])
				smdh_text(smdh, lang, 0, 0x40, x->name, sizeof x->name);
			smdh_text(smdh, lang, 0x180, 0x40, x->author, sizeof x->author);
			smdh_icon(smdh, x->icon);
			x->has_icon = true;
			if (!x->name[0])
				snprintf(x->name, sizeof x->name, "%016llX", (unsigned long long)x->tid);
		}
		*out = t;
		if (!t)
			cnt = 0;
	}
	free(smdh), free(ids), free(info), free(pers);
	amExit();
	return cnt;
}

bool cb_is_homebrew(u64 tid)
{
	if (R_FAILED(amInit()))
		return false;
	u64 *pers = NULL;
	int np = personalized(&pers);
	bool ok = homebrew_am(tid, pers, np, NULL);
	free(pers);
	amExit();
	return ok;
}

// Luma3DS: svcControlService (0xB0) con SERVICEOP_STEAL_CLIENT_SESSION (0): una sesión de un servicio que ya no admite
// más (la comparte con quien la tenga). Solo si es Luma (GetSystemInfo 0x10000 solo existe en Luma): en otro sistema,
// esa llamada no existe y colgaría la consola
static bool is_luma(void)
{
	s64 v = 0;
	bool ok = R_SUCCEEDED(svcGetSystemInfo(&v, 0x10000, 0));
	if (ok) // (la versión: mayor, menor y revisión en los bytes 3, 2 y 1)
		util_log("compartir: Luma3DS %u.%u.%u", (unsigned)(v >> 24 & 0xFF), (unsigned)(v >> 16 & 0xFF), (unsigned)(v >> 8 & 0xFF));
	return ok;
}
// la llamada, como en Luma3DS (csvc.s): una función aparte, para que op, out y name vayan en r0, r1 y r2 sin depender del
// compilador (con ensamblador en línea, la sesión llegaba vacía aunque Luma respondía 0)
__asm__(".section .text.luma_control_service, \"ax\", %progbits\n"
        ".arm\n"
        ".global luma_control_service\n"
        ".type luma_control_service, %function\n"
        ".align 2\n"
        "luma_control_service:\n"
        "    svc 0xB0\n"
        "    bx lr\n"
        ".previous\n");
Result luma_control_service(u32 op, Handle *out, const char *name);
static Result luma_steal_session(Handle *out, const char *name)
{
	return luma_control_service(0, out, name); // SERVICEOP_STEAL_CLIENT_SESSION
}

// la cabecera del programa (ExHeader_Info, 0x400), como la pide loader: el programa se registra en pxi:pm (Process9, que
// descifra el NCCH) y se le pide su cabecera. (fs:REG solo sirve para lo que se carga desde el PC: con un título de la
// SD respondía C8804465, «no encontrado».) La sesión de pxi:pm la tiene loader: si no hay otra libre, se usa la suya a
// través de Luma3DS
static bool get_exheader(u64 tid, u8 *exh, char *err, size_t en)
{
	srvSetBlockingPolicy(true); // (no bloquear: si no hay sesión libre, error al momento)
	Result r = pxiPmInit();
	srvSetBlockingPolicy(false);
	bool own = R_SUCCEEDED(r), stolen = false;
	if (!own && is_luma()) {
		Handle h = 0;
		Result r2 = luma_steal_session(&h, "pxi:pm");
		util_log("compartir: pxi:pm ocupado (%08lX); sesión compartida por Luma3DS: %08lX, handle %08lX", (unsigned long)r, (unsigned long)r2,
		         (unsigned long)h);
		if (R_SUCCEEDED(r2) && h) {
			*pxiPmGetSessionHandle() = h;
			stolen = true, r = 0;
		}
	}
	if (own || stolen) {
		FS_ProgramInfo pi;
		memset(&pi, 0, sizeof pi);
		pi.programId = tid, pi.mediaType = MEDIATYPE_SD;
		u64 ph = 0;
		if (R_SUCCEEDED(r = PXIPM_RegisterProgram(&ph, &pi, &pi))) { // (sin actualización: la misma, como hace pm)
			r = PXIPM_GetProgramInfo((ExHeader_Info *)exh, ph);
			PXIPM_UnregisterProgram(ph);
		} else
			util_log("compartir: PXIPM_RegisterProgram: %08lX", (unsigned long)r);
		if (own)
			pxiPmExit();
		else {
			svcCloseHandle(*pxiPmGetSessionHandle());
			*pxiPmGetSessionHandle() = 0;
		}
	}
	util_log("compartir: cabecera de %016llX por pxi:pm: %08lX", (unsigned long long)tid, (unsigned long)r);
	if (R_SUCCEEDED(r))
		return true;
#ifdef TEST_CA_SD
	{ // (solo en las pruebas con el emulador, que no tiene pxi:pm: la cabecera, de un archivo)
		char p[96];
		snprintf(p, sizeof p, APP_DIR "/prueba_exh_%016llX.bin", (unsigned long long)tid);
		FILE *f = fopen(p, "rb");
		bool ok = f && fread(exh, 1, 0x400, f) == 0x400;
		if (f)
			fclose(f);
		if (ok)
			return true;
	}
#endif
	snprintf(err, en, _("No se pudo leer la cabecera de la app (pxi:pm: %08lX)."), (unsigned long)r);
	return false;
}

// ¿el .code viene comprimido (como está en el ExeFS) o ya descomprimido? El comprimido acaba con su pie (LZ inverso)
static bool code_compressed(const u8 *d, u32 n)
{
	if (n < 8)
		return false;
	u32 a = le32(d + n - 8), extra = le32(d + n - 4), hl = a >> 24, cl = a & 0xFFFFFF;
	return hl >= 8 && hl <= 0x0B && cl >= hl && cl <= n && extra > 0 && extra < CODE_MAX;
}

/* ---------------------------------------------------------------- armar */

typedef struct {
	FILE *f;
	volatile u64 *done;
	volatile bool *cancel;
} Out;

static bool wr_at(Out *o, u64 off, const void *d, size_t n)
{
	return fseek(o->f, (long)off, SEEK_SET) == 0 && fwrite(d, 1, n, o->f) == n;
}

int cb_build(u64 tid, const char *path, volatile u64 *done, volatile u64 *total, volatile bool *cancel, char *err, size_t en)
{
	err[0] = 0;
	*done = 0, *total = 1;
	int ret = -1;
	u8 *exh = calloc(1, 0x800), *smdh = malloc(0x36C0), *hdr = calloc(1, MU), *exefs_h = calloc(1, MU);
	u8 *file[4] = {0};
	u32 fsz[4] = {0};
	u8 *l2 = NULL, *l1 = NULL, *buf = NULL;
	Handle rh = 0;
	Out o = {NULL, done, cancel};
	char prod[16] = "CTR-H-SHAR";
	u16 version = 0;
	if (!exh || !smdh || !hdr || !exefs_h) {
		snprintf(err, en, "%s", _("No hay memoria suficiente"));
		goto out;
	}
	// lo de AM: que siga siendo homebrew, su versión y su código de producto
	if (R_FAILED(amInit())) {
		snprintf(err, en, "%s", _("No se pudo abrir el gestor de títulos"));
		goto out;
	}
	{
		u64 *pers = NULL;
		int np = personalized(&pers);
		bool hb = homebrew_am(tid, pers, np, smdh);
		free(pers);
		AM_TitleEntry te;
		u64 id = tid;
		if (R_SUCCEEDED(AM_GetTitleInfo(MEDIATYPE_SD, 1, &id, &te)))
			version = te.version;
		char pc[16] = {0};
		if (R_SUCCEEDED(AM_GetTitleProductCode(MEDIATYPE_SD, tid, pc)) && pc[0])
			memcpy(prod, pc, sizeof prod);
		amExit();
		if (!hb) {
			snprintf(err, en, "%s", _("Solo se pueden compartir apps homebrew"));
			goto out;
		}
	}
	if (!get_exheader(tid, exh, err, en))
		goto out;
	// el ExeFS, archivo a archivo (.code tal cual lo da el sistema: si ya viene descomprimido, se marca así)
	static const char *const NAMES[4] = {".code", "banner", "icon", "logo"};
	for (int i = 0; i < 4; i++)
		exefs_file(tid, NAMES[i], &file[i], &fsz[i]);
	if (!file[0]) {
		snprintf(err, en, "%s", _("No se pudo leer el programa de la app"));
		goto out;
	}
	if ((exh[0xD] & 1) && !code_compressed(file[0], fsz[0]))
		exh[0xD] &= ~1;
	// la descripción de acceso: la misma que la cabecera (su firma la acepta el sistema con los parches de firma)
	memcpy(exh + 0x600, exh + 0x200, 0x200);
	u64 exefs_size = MU;
	{
		int k = 0;
		for (int i = 0; i < 4; i++) {
			if (!file[i])
				continue;
			u8 *e = exefs_h + k * 16;
			memcpy(e, NAMES[i], strlen(NAMES[i]));
			wle32(e + 8, (u32)(exefs_size - MU));
			wle32(e + 12, fsz[i]);
			sha(file[i], fsz[i], exefs_h + 0xC0 + (9 - k) * 32);
			exefs_size += up(fsz[i], MU);
			k++;
		}
	}
	// la RomFS (su nivel 3); si no tiene, sin RomFS
	u64 l3 = 0;
	if (R_SUCCEEDED(ncch_open(tid, 0, NULL, &rh)) && (R_FAILED(FSFILE_GetSize(rh, &l3)) || !l3)) {
		FSFILE_Close(rh);
		rh = 0, l3 = 0;
	}
	const u64 exefs_off = 0xA00, romfs_off = rh ? up(exefs_off + exefs_size, BLK) : 0;
	u64 n3 = up(l3, BLK) / BLK, l2s = n3 * 32, n2 = up(l2s, BLK) / BLK, l1s = n2 * 32, n1 = up(l1s, BLK) / BLK, ms = n1 * 32;
	u64 lv2log = up(l1s, BLK), lv3log = up(lv2log + l2s, BLK);
	u64 p3 = up(0x60 + ms, BLK), p1 = up(p3 + l3, BLK), p2 = up(p1 + l1s, BLK), romfs_size = rh ? up(p2 + l2s, BLK) : 0;
	u64 ncch_size = rh ? romfs_off + romfs_size : exefs_off + exefs_size;
	// el CIA: cabecera, certificados, ticket, TMD, contenido y metadatos (cada parte alineada a 64)
	const u64 o_cert = up(0x2020, 64), o_tik = up(o_cert + sizeof CIA_CERTS, 64), o_tmd = up(o_tik + sizeof CIA_TIK, 64),
	          o_con = up(o_tmd + sizeof CIA_TMD, 64), o_meta = up(o_con + ncch_size, 64);
	*total = l3 * 2 + ncch_size + exefs_size;
	char sd[300];
	snprintf(sd, sizeof sd, "sdmc:%s", path);
	if (!(o.f = fopen(sd, "w+b"))) {
		snprintf(err, en, "%s", _("No se pudo crear el archivo en la SD"));
		goto out;
	}
	setvbuf(o.f, NULL, _IOFBF, 64 * 1024);
	buf = malloc(BLK * 16);
	if (!buf) {
		snprintf(err, en, "%s", _("No hay memoria suficiente"));
		goto out;
	}
	// RomFS: nivel 3 (con su hash por bloque), nivel 1, nivel 2 y su cabecera IVFC con el hash maestro
	u8 rfs_hdr[MU];
	memset(rfs_hdr, 0, sizeof rfs_hdr);
	if (rh) {
		if (!(l2 = calloc(1, up(l2s, BLK))) || !(l1 = calloc(1, up(l1s, BLK)))) {
			snprintf(err, en, "%s", _("La app es demasiado grande para la memoria libre"));
			goto out;
		}
		for (u64 off = 0; off < l3;) {
			if (*cancel)
				goto out;
			u32 want = (u32)(l3 - off < BLK * 16 ? l3 - off : BLK * 16), rd = 0;
			if (R_FAILED(FSFILE_Read(rh, &rd, off, buf, want)) || rd != want) {
				snprintf(err, en, "%s", _("No se pudo leer la RomFS de la app"));
				goto out;
			}
			memset(buf + want, 0, up(want, BLK) - want);
			for (u32 b = 0; b < up(want, BLK); b += BLK)
				sha(buf + b, BLK, l2 + (off + b) / BLK * 32);
			if (!wr_at(&o, o_con + romfs_off + p3 + off, buf, want)) {
				snprintf(err, en, "%s", _("No se pudo escribir en la SD. Comprueba el espacio disponible."));
				goto out;
			}
			off += want;
			*done += want;
		}
		for (u64 b = 0; b < n2; b++)
			sha(l2 + b * BLK, BLK, l1 + b * 32);
		u8 *m = rfs_hdr + 0x60;
		for (u64 b = 0; b < n1 && 0x60 + (b + 1) * 32 <= MU; b++)
			sha(l1 + b * BLK, BLK, m + b * 32);
		if (0x60 + ms > MU) { // (no cabe en la región de hash: más de ~13 MB de nivel 1… no pasa con tamaños reales)
			snprintf(err, en, "%s", _("La app es demasiado grande"));
			goto out;
		}
		memcpy(rfs_hdr, "IVFC", 4);
		wle32(rfs_hdr + 4, 0x10000);
		wle32(rfs_hdr + 8, (u32)ms);
		const u64 lo[3] = {0, lv2log, lv3log}, ls[3] = {l1s, l2s, l3};
		for (int i = 0; i < 3; i++) {
			wle64(rfs_hdr + 0x0C + i * 24, lo[i]);
			wle64(rfs_hdr + 0x14 + i * 24, ls[i]);
			wle32(rfs_hdr + 0x1C + i * 24, 12);
		}
		wle32(rfs_hdr + 0x54, 0x5C);
		if (!wr_at(&o, o_con + romfs_off, rfs_hdr, MU) || !wr_at(&o, o_con + romfs_off + p1, l1, l1s) || !wr_at(&o, o_con + romfs_off + p2, l2, l2s)) {
			snprintf(err, en, "%s", _("No se pudo escribir en la SD. Comprueba el espacio disponible."));
			goto out;
		}
		// relleno hasta el final de la RomFS
		static const u8 z[16] = {0};
		if (fseek(o.f, (long)(o_con + romfs_off + romfs_size - sizeof z), SEEK_SET) || fwrite(z, 1, sizeof z, o.f) != sizeof z)
			goto out;
	}
	// ExeFS
	if (!wr_at(&o, o_con + exefs_off, exefs_h, MU))
		goto out;
	{
		u64 at = o_con + exefs_off + MU;
		for (int i = 0; i < 4; i++) {
			if (!file[i])
				continue;
			if (!wr_at(&o, at, file[i], fsz[i]))
				goto out;
			*done += fsz[i];
			at += up(fsz[i], MU);
		}
		static const u8 z[MU] = {0};
		u64 end = o_con + exefs_off + exefs_size, cur = (u64)ftell(o.f);
		if (end > cur && fwrite(z, 1, end - cur, o.f) != end - cur)
			goto out;
	}
	// cabecera NCCH (sin cifrar) y la del programa
	memcpy(hdr + 0x100, "NCCH", 4);
	wle32(hdr + 0x104, (u32)(ncch_size / MU));
	wle64(hdr + 0x108, tid);
	memcpy(hdr + 0x110, "00", 2);
	wle16(hdr + 0x112, 2);
	wle64(hdr + 0x118, tid);
	memcpy(hdr + 0x150, prod, strnlen(prod, 16));
	sha(exh, 0x400, hdr + 0x160);
	wle32(hdr + 0x180, 0x400);
	hdr[0x188 + 4] = 1;    // CTR
	hdr[0x188 + 5] = 0x03; // datos + ejecutable
	hdr[0x188 + 7] = 0x05; // sin cifrar (FixedCryptoKey | NoCrypto, como makerom)
	wle32(hdr + 0x1A0, (u32)(exefs_off / MU));
	wle32(hdr + 0x1A4, (u32)(exefs_size / MU));
	wle32(hdr + 0x1A8, 1);
	sha(exefs_h, MU, hdr + 0x1C0);
	if (rh) {
		wle32(hdr + 0x1B0, (u32)(romfs_off / MU));
		wle32(hdr + 0x1B4, (u32)(romfs_size / MU));
		wle32(hdr + 0x1B8, 1);
		sha(rfs_hdr, MU, hdr + 0x1E0);
	}
	if (!wr_at(&o, o_con, hdr, MU) || !wr_at(&o, o_con + MU, exh, 0x800))
		goto out;
	// el hash del contenido entero (se vuelve a leer lo escrito)
	u8 chash[32];
	{
		fflush(o.f);
		Sha256 s;
		sha256_init(&s);
		if (fseek(o.f, (long)o_con, SEEK_SET))
			goto out;
		for (u64 left = ncch_size; left;) {
			if (*cancel)
				goto out;
			size_t want = left < BLK * 16 ? (size_t)left : BLK * 16;
			if (fread(buf, 1, want, o.f) != want) {
				snprintf(err, en, "%s", _("No se pudo leer lo escrito en la SD"));
				goto out;
			}
			sha256_update(&s, buf, want);
			left -= want;
			*done += want;
		}
		sha256_final(&s, chash);
	}
	// ticket y TMD (de la plantilla, con lo de esta app) y los metadatos (dependencias e icono)
	u8 tik[sizeof CIA_TIK], tmd[sizeof CIA_TMD], *meta = calloc(1, 0x3AC0);
	if (!meta)
		goto out;
	memcpy(tik, CIA_TIK, sizeof tik);
	wbe64(tik + 0x1D0, 0x0004000000000000ull | (tid & 0xFFFFFFFFull));
	wbe64(tik + 0x1DC, tid);
	wbe16(tik + 0x1E6, version);
	memcpy(tmd, CIA_TMD, sizeof tmd);
	wbe64(tmd + 0x18C, tid);
	u64 save = 0;
	memcpy(&save, exh + 0x1C0, 8);
	wle32(tmd + 0x19A, (u32)save);
	wbe16(tmd + 0x1DC, version);
	wbe16(tmd + 0x1DE, 1);
	u8 *cc = tmd + 0xB04;
	memset(cc, 0, 0x30);
	wbe32(cc, 0);         // id del contenido
	wbe16(cc + 4, 0);     // índice
	wbe16(cc + 6, 0);     // sin cifrar
	wbe64(cc + 8, ncch_size);
	memcpy(cc + 16, chash, 32);
	u8 *cir = tmd + 0x204;
	memset(cir, 0, 64 * 0x24);
	wbe16(cir, 0), wbe16(cir + 2, 1);
	sha(cc, 0x30, cir + 4);
	sha(cir, 64 * 0x24, tmd + 0x1E4);
	memcpy(meta, exh + 0x40, 0x180); // dependencias
	memcpy(meta + 0x400, smdh, 0x36C0);
	u8 ch[0x2020];
	memset(ch, 0, sizeof ch);
	wle32(ch, 0x2020);
	wle32(ch + 8, sizeof CIA_CERTS);
	wle32(ch + 12, sizeof tik);
	wle32(ch + 16, sizeof tmd);
	wle32(ch + 20, 0x3AC0);
	wle64(ch + 24, ncch_size);
	ch[0x20] = 0x80; // contenido 0
	bool ok = wr_at(&o, 0, ch, sizeof ch) && wr_at(&o, o_cert, CIA_CERTS, sizeof CIA_CERTS) && wr_at(&o, o_tik, tik, sizeof tik) &&
	          wr_at(&o, o_tmd, tmd, sizeof tmd) && wr_at(&o, o_meta, meta, 0x3AC0);
	free(meta);
	if (!ok || fflush(o.f)) {
		snprintf(err, en, "%s", _("No se pudo escribir en la SD. Comprueba el espacio disponible."));
		goto out;
	}
	*done = *total;
	ret = 0;
	util_log("compartir: CIA de %016llX armado (%llu bytes, RomFS %llu)", (unsigned long long)tid, (unsigned long long)(o_meta + 0x3AC0),
	         (unsigned long long)l3);
out:
	if (rh)
		FSFILE_Close(rh);
	if (o.f)
		fclose(o.f);
	if (ret) {
		if (!err[0] && *cancel)
			snprintf(err, en, "%s", _("Operación cancelada"));
		else if (!err[0])
			snprintf(err, en, "%s", _("No se pudo escribir en la SD. Comprueba el espacio disponible."));
		char sd2[300];
		snprintf(sd2, sizeof sd2, "sdmc:%s", path);
		remove(sd2);
		util_log("compartir: no se pudo armar el CIA de %016llX: %s", (unsigned long long)tid, err);
	}
	for (int i = 0; i < 4; i++)
		free(file[i]);
	free(exh), free(smdh), free(hdr), free(exefs_h), free(l2), free(l1), free(buf);
	return ret;
}
