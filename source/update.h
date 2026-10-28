/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Buscar e instalar actualizaciones desde las releases de GitHub. */

enum { UPD_IDLE, UPD_CHECKING, UPD_LATEST, UPD_AVAILABLE, UPD_DOWNLOADING, UPD_INSTALLING, UPD_DONE, UPD_ERROR };

typedef struct {
	int  st;
	u32  rev;          /* cambia con cada novedad */
	char latest[16];   /* «1.2.0» */
	char notes[1500];  /* notas de la release (Markdown) */
	bool has_file;     /* la release trae el archivo y el Cloud3DS.sha256 */
	u64  done, total;  /* progreso de la descarga o la instalación */
	char err[32];      /* offline, tls, rate, no_release, sha, am, io… */
} UpdInfo;

void upd_init(const char *self_path); /* argv[0] si se abrió como .3dsx */
void upd_check(void);
void upd_install(void);
void upd_get(UpdInfo *out);
bool upd_busy(void);
bool upd_newer(const char *a, const char *b); /* a > b */
void upd_exit(void);
