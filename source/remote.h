/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"
#include <poll.h>

/* Otras formas de llegar a la consola, dentro del bucle del servidor:
 *  - mDNS: responde a «<nombre>.local» con la IP de la consola (RFC 6762).
 *  - FBI remoto: el puerto 5000 acepta lo que mandan las apps de PC para FBI
 *    (4 bytes con la longitud y los enlaces, uno por línea). Los enlaces van a
 *    la cola de descargas (a /cias, con la tarjeta de instalar) y la respuesta
 *    de un byte se manda al terminar, que es cuando esas apps cierran. */

#define REMOTE_FBI_PORT 5000

void remote_open(u32 ip, int web_port, bool mdns, bool fbi, const char *name);
void remote_close(void);
int  remote_fds(struct pollfd *pf, int max);     /* añade los suyos; cuántos */
void remote_events(struct pollfd *pf, int n);    /* los mismos, tras poll */
void remote_tick(void);                          /* respuestas pendientes a FBI */
bool remote_fbi_ready(void);
bool remote_mdns_ready(void);
bool remote_name_ok(const char *n);              /* letras, números y guiones; 1-40 */
