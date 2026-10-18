#pragma once

#include "app.h"

/* Operaciones de la SD con el servicio FS directamente (más rápido que
 * stat() por archivo). Las rutas son UTF-8 relativas a la raíz: "/3ds/x". */

typedef struct {
	char name[256];
	bool dir;
	u64  size;
	u64  mtime; /* unix, 0 si no se pidió */
} FsxEntry;

typedef bool (*FsxListCb)(const FsxEntry *e, void *user); /* false = parar */

void fsx_init(void);
void fsx_exit(void);
int  fsx_list(const char *path, bool want_mtime, FsxListCb cb, void *user); /* <0 error */
int  fsx_stat(const char *path, bool *dir, u64 *size);                       /* 0 ok, -1 no existe */
int  fsx_rmrf(const char *path);
int  fsx_mkdirs(const char *path); /* crea también los padres */
bool fsx_space(u64 *free_b, u64 *total_b);
u64  fsx_mtime(const char *path);

/* Valida una ruta del cliente: absoluta, sin "..", sin caracteres que FAT
 * no admite. Devuelve false si no es válida. */
bool fsx_path_ok(const char *p);
bool fsx_name_ok(const char *n);
bool fsx_is_system(const char *p); /* /Nintendo 3DS, /luma, boot.firm... */
