/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Busca en la red el nombre real de los dispositivos vinculados (mDNS,
 * NetBIOS o el router) en un hilo propio. Necesita soc:U ya iniciado. */

void names_start(void);
void names_stop(void);
