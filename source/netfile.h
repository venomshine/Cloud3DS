/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Archivos de Internet que se abren sin guardarlos en la SD: «nube:/<n>/<nombre>» es un disco de solo lectura (con
 * fopen, open y stat, como cualquier otro) cuyo contenido está en memoria y va llegando en un hilo, de principio a fin.
 * Leer lo que aún no ha llegado espera; si queda lejos de lo que se está bajando (el final de un PDF o de un ZIP, por
 * ejemplo), se pide ese trozo aparte, por rangos. Se guardan los últimos NF_SLOTS: el más viejo que nadie tenga
 * abierto se libera al pedir otro. */

#define NF_SLOTS 3

void nf_init(void);
/* empieza a traer url (de size bytes): en path, su ruta «nube:/…». false si no hay memoria para él entero */
bool nf_open_url(const char *url, u64 size, const char *name, char *path, size_t pn);
/* cómo va: lo que llegó seguido desde el principio; done: entero; failed: no se pudo */
bool nf_progress(const char *path, u64 *got, u64 *total, bool *done, bool *failed);
bool nf_is(const char *path); /* ¿es una ruta «nube:»? */
