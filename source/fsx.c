/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "fsx.h"
#include "util.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

static FS_Archive sd;
static bool sd_open;

void fsx_init(void)
{
	sd_open = R_SUCCEEDED(FSUSER_OpenArchive(&sd, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));
}

void fsx_exit(void)
{
	if (sd_open)
		FSUSER_CloseArchive(sd);
	sd_open = false;
}

/* UTF-8 -> UTF-16 para FS_Path */
static int to16(u16 *out, int max, const char *s)
{
	int o = 0;
	const u8 *p = (const u8 *)s;
	while (*p && o < max - 2) {
		u32 c;
		if (*p < 0x80)
			c = *p++;
		else if ((*p & 0xE0) == 0xC0 && p[1]) {
			c = (*p & 0x1F) << 6 | (p[1] & 0x3F);
			p += 2;
		} else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) {
			c = (*p & 0x0F) << 12 | (p[1] & 0x3F) << 6 | (p[2] & 0x3F);
			p += 3;
		} else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) {
			c = (*p & 0x07) << 18 | (p[1] & 0x3F) << 12 | (p[2] & 0x3F) << 6 | (p[3] & 0x3F);
			p += 4;
		} else
			return -1;
		if (c >= 0x10000) {
			c -= 0x10000;
			out[o++] = 0xD800 | (c >> 10);
			out[o++] = 0xDC00 | (c & 0x3FF);
		} else
			out[o++] = c;
	}
	out[o] = 0;
	return o;
}

static FS_Path mkpath(u16 *buf, int max, const char *p)
{
	int n = to16(buf, max, p);
	if (n < 0)
		n = 0, buf[0] = 0;
	return (FS_Path){PATH_UTF16, (n + 1) * 2, buf};
}

static u64 fat_to_unix(u64 t)
{
	/* ControlArchive devuelve milisegundos desde 2000-01-01 (como sdmc_getmtime) */
	return t / 1000 + 946684800ull;
}

u64 fsx_mtime(const char *path)
{
	u16 b[512];
	int n = to16(b, 512, path);
	if (n < 0)
		return 0;
	u64 t = 0;
	if (R_FAILED(FSUSER_ControlArchive(sd, ARCHIVE_ACTION_GET_TIMESTAMP, b, (n + 1) * 2, &t, sizeof t)))
		return 0;
	return t ? fat_to_unix(t) : 0;
}

int fsx_list(const char *path, bool want_mtime, FsxListCb cb, void *user)
{
	if (!sd_open)
		return -1;
	u16 pb[512];
	Handle d;
	if (R_FAILED(FSUSER_OpenDirectory(&d, sd, mkpath(pb, 512, path))))
		return -1;
	static FS_DirectoryEntry ents[32];
	FsxEntry e;
	char full[1024];
	int total = 0;
	bool go = true;
	while (go) {
		u32 got = 0;
		if (R_FAILED(FSDIR_Read(d, &got, 32, ents)) || got == 0)
			break;
		for (u32 i = 0; i < got && go; i++) {
			util_utf16_to_utf8(e.name, sizeof e.name, ents[i].name, 0x106);
			e.dir = ents[i].attributes & FS_ATTRIBUTE_DIRECTORY;
			e.size = e.dir ? 0 : ents[i].fileSize;
			e.mtime = 0;
			if (want_mtime) {
				snprintf(full, sizeof full, "%s%s%s", path, path[strlen(path) - 1] == '/' ? "" : "/", e.name);
				e.mtime = fsx_mtime(full);
			}
			total++;
			go = cb(&e, user);
		}
	}
	FSDIR_Close(d);
	return total;
}

int fsx_stat(const char *path, bool *dir, u64 *size)
{
	if (strcmp(path, "/") == 0) {
		*dir = true;
		*size = 0;
		return 0;
	}
	u16 pb[512];
	FS_Path fp = mkpath(pb, 512, path);
	Handle h;
	if (R_SUCCEEDED(FSUSER_OpenFile(&h, sd, fp, FS_OPEN_READ, 0))) {
		u64 s = 0;
		FSFILE_GetSize(h, &s);
		FSFILE_Close(h);
		*dir = false;
		*size = s;
		return 0;
	}
	if (R_SUCCEEDED(FSUSER_OpenDirectory(&h, sd, fp))) {
		FSDIR_Close(h);
		*dir = true;
		*size = 0;
		return 0;
	}
	return -1;
}

int fsx_rmrf(const char *path)
{
	bool dir;
	u64 sz;
	if (fsx_stat(path, &dir, &sz) < 0)
		return -1;
	u16 pb[512];
	FS_Path fp = mkpath(pb, 512, path);
	Result r = dir ? FSUSER_DeleteDirectoryRecursively(sd, fp) : FSUSER_DeleteFile(sd, fp);
	return R_SUCCEEDED(r) ? 0 : -1;
}

int fsx_mkdirs(const char *path)
{
	char tmp[1024];
	util_strlcpy(tmp, path, sizeof tmp);
	u16 pb[512];
	for (char *p = tmp + 1;; p++) {
		if (*p == '/' || *p == 0) {
			char c = *p;
			*p = 0;
			FSUSER_CreateDirectory(sd, mkpath(pb, 512, tmp), 0);
			*p = c;
			if (!c)
				break;
		}
	}
	bool dir;
	u64 sz;
	return fsx_stat(path, &dir, &sz) == 0 && dir ? 0 : -1;
}

bool fsx_space(u64 *free_b, u64 *total_b)
{
	FS_ArchiveResource r;
	if (R_FAILED(FSUSER_GetArchiveResource(&r, SYSTEM_MEDIATYPE_SD)))
		return false;
	*free_b = (u64)r.clusterSize * r.freeClusters;
	*total_b = (u64)r.clusterSize * r.totalClusters;
	return true;
}

bool fsx_name_ok(const char *n)
{
	size_t l = strlen(n);
	if (l == 0 || l > 255 || !util_utf8_valid(n))
		return false;
	if (strcmp(n, ".") == 0 || strcmp(n, "..") == 0)
		return false;
	for (const u8 *p = (const u8 *)n; *p; p++)
		if (*p < 0x20 || strchr("\\/:*?\"<>|", *p))
			return false;
	/* FAT no admite nombres que acaban en punto o espacio */
	if (n[l - 1] == '.' || n[l - 1] == ' ')
		return false;
	return true;
}

bool fsx_path_ok(const char *p)
{
	if (p[0] != '/' || strlen(p) > 900 || !util_utf8_valid(p))
		return false;
	if (strcmp(p, "/") == 0)
		return true;
	char tmp[1024];
	util_strlcpy(tmp, p + 1, sizeof tmp);
	size_t l = strlen(tmp);
	if (l && tmp[l - 1] == '/')
		tmp[l - 1] = 0;
	char *save = NULL;
	for (char *t = strtok_r(tmp, "/", &save); t; t = strtok_r(NULL, "/", &save))
		if (!fsx_name_ok(t))
			return false;
	return strstr(p, "//") == NULL;
}

bool fsx_is_system(const char *p)
{
	/* carpetas y archivos que, si se tocan, pueden dejar la consola o el homebrew sin arrancar.
	 * /luma y /3ds solo cuentan ellas mismas: lo que hay dentro (capturas, apps) se puede gestionar */
	static const char *const TREE[] = {"/Nintendo 3DS", "/boot.firm", "/boot.3dsx", "/gm9", "/3ds/Cloud3DS",
	                                   "/luma/payloads", "/luma/sysmodules", "/luma/titles", "/luma/config.ini"};
	if (strcasecmp(p, "/luma") == 0 || strcasecmp(p, "/3ds") == 0)
		return true;
	for (size_t i = 0; i < ARRAY_LEN(TREE); i++) {
		size_t l = strlen(TREE[i]);
		if (strncasecmp(p, TREE[i], l) == 0 && (p[l] == 0 || p[l] == '/'))
			return true;
	}
	return false;
}
