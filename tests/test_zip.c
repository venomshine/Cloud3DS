/*
 * Cloud3DS - pruebas de source/zip.c en el PC (sin la consola).
 *   gcc -Wall -fsanitize=address,undefined -DZIP_ROOT='""' -Itests/shim -Isource -o /tmp/test_zip tests/test_zip.c source/zip.c -lz
 *   /tmp/test_zip list    ZIP              → lo que trae
 *   /tmp/test_zip extract ZIP DESTINO      → lo extrae
 *   /tmp/test_zip create  ZIP BASE NOMBRE… → crea el ZIP con esos nombres de BASE
 * (las rutas son absolutas, como en la SD)
 */
#include "zip.h"
#include "fsx.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

int fsx_list(const char *path, bool want_mtime, FsxListCb cb, void *user)
{
	(void)want_mtime;
	DIR *d = opendir(path);
	if (!d)
		return -1;
	struct dirent *de;
	int n = 0;
	while ((de = readdir(d))) {
		if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
			continue;
		char p[2048];
		snprintf(p, sizeof p, "%s/%s", path, de->d_name);
		struct stat st;
		if (stat(p, &st))
			continue;
		FsxEntry e = {0};
		snprintf(e.name, sizeof e.name, "%s", de->d_name);
		e.dir = S_ISDIR(st.st_mode), e.size = e.dir ? 0 : (u64)st.st_size, e.mtime = (u64)st.st_mtime;
		n++;
		if (!cb(&e, user))
			break;
	}
	closedir(d);
	return n;
}
int fsx_stat(const char *path, bool *dir, u64 *size)
{
	struct stat st;
	if (stat(path, &st))
		return -1;
	*dir = S_ISDIR(st.st_mode), *size = (u64)st.st_size;
	return 0;
}
int fsx_mkdirs(const char *path)
{
	char p[2048];
	snprintf(p, sizeof p, "%s", path);
	for (char *s = p + 1; *s; s++)
		if (*s == '/') {
			*s = 0;
			mkdir(p, 0755);
			*s = '/';
		}
	return mkdir(p, 0755) == 0 || errno == EEXIST ? 0 : -1;
}
bool fsx_space(u64 *f, u64 *t) { *f = *t = 1ull << 40; return true; }
u64 fsx_mtime(const char *path) { struct stat st; return stat(path, &st) ? 0 : (u64)st.st_mtime; }

int main(int argc, char **argv)
{
	if (argc < 3)
		return 2;
	volatile bool cancel = false;
	volatile uint64_t done = 0;
	ZipProgress pr = {&cancel, &done};
	if (!strcmp(argv[1], "list") || !strcmp(argv[1], "extract")) {
		ZipDir z;
		int r = zip_read_dir(argv[2], &z);
		if (r) {
			printf("error %d\n", r);
			return 1;
		}
		if (!strcmp(argv[1], "list")) {
			printf("%d archivos, %d carpetas, %llu bytes%s%s\n", z.nfiles, z.ndirs, (unsigned long long)z.usize,
			       z.encrypted ? " cifrado" : "", z.unsupported ? " método raro" : "");
			char names[64][256];
			int cnt[64];
			uint64_t sz[64];
			bool dirs[64];
			int n = zip_top_level(&z, names, cnt, sz, dirs, 64);
			for (int i = 0; i < n; i++)
				printf("  %s%s %d %llu\n", names[i], dirs[i] ? "/" : "", cnt[i], (unsigned long long)sz[i]);
		} else {
			r = zip_extract(argv[2], &z, argv[3], &pr);
			printf("extraer: %d (%llu bytes)\n", r, (unsigned long long)done);
		}
		zip_dir_free(&z);
		return r ? 1 : 0;
	}
	if (!strcmp(argv[1], "create")) {
		int n = argc - 4;
		char (*names)[256] = calloc(n, 256);
		for (int i = 0; i < n; i++)
			snprintf(names[i], 256, "%s", argv[4 + i]);
		int r = zip_create(argv[2], argv[3], names, n, &pr);
		printf("crear: %d (%llu bytes)\n", r, (unsigned long long)done);
		free(names);
		return r ? 1 : 0;
	}
	return 2;
}
