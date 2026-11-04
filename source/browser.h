/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Navegador ligero (prototipo). Lee el HTML con hubbub y libdom (NetSurf, MIT) y lo maqueta
 * él mismo: títulos, párrafos, listas, citas, código, tablas como filas y enlaces que se tocan.
 * Sin JavaScript. Las imágenes se ven como un marcador con su texto alternativo. */

void br_open_home(void);                 /* la página de inicio: favoritos e historial */
void br_open_url(const char *url);       /* http:// o https:// */
void br_open_file(const char *sd_path);  /* un .html de la SD */
bool br_is_open(void);
void br_top(void);                        /* pantalla de arriba */
void br_bottom(void);                     /* pantalla de abajo: la página, con su entrada */
void br_exit(void);
void br_poll(void);                       /* cada fotograma: lo pedido mientras cargaba */
