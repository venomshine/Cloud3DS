#pragma once

#include "app.h"

/* Servidor HTTP de la consola: página web + API de archivos.
 * Corre en su propio hilo; publica su estado en G (state.h). */

void server_start(void);
void server_stop(void);
void server_restart(void); /* p. ej. al despertar de la suspensión */
