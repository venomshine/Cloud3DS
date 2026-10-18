/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Servidor HTTP de la consola: página web + API de archivos.
 * Corre en su propio hilo; publica su estado en G (state.h). */

void server_start(void);
void server_stop(void);
void server_restart(void); /* p. ej. al despertar de la suspensión */
