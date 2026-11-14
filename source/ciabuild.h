/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#pragma once

#include "app.h"

/* Armar el instalador (CIA) de una app homebrew ya instalada en la SD, para pasarla a otra consola (Compartir).
 * El sistema no deja leer el contenido tal cual está en la SD, pero sí sus partes: el ExeFS y la RomFS (por el
 * archivo NCCH del título) y su cabecera de programa (por fs:REG). Con ellas se vuelve a montar el NCCH (sin cifrar,
 * con sus hashes: ExeFS, IVFC de la RomFS y cabecera) y el CIA (certificados públicos, ticket, TMD y metadatos).
 *
 * Solo homebrew: aplicaciones de la SD sin ticket personalizado (lo comprado en la eShop lo tiene), con su contenido
 * sin cifrar y sin clasificación por edades en su SMDH (todo lo comercial la lleva). */

typedef struct {
	u64  tid;
	u16  version;
	u64  size;          /* lo que ocupa instalado */
	char name[64], author[64];
	bool has_icon;
	u16  icon[48 * 48]; /* RGB565 */
} CbTitle;

int  cb_list(CbTitle **out);  /* las apps homebrew instaladas (malloc); cuántas */
bool cb_is_homebrew(u64 tid); /* comprobarlo otra vez, justo antes de enviar */

/* Arma el CIA en path. done/total: progreso (bytes); cancel: parar. 0 bien; si no, err dice por qué */
int cb_build(u64 tid, const char *path, volatile u64 *done, volatile u64 *total, volatile bool *cancel, char *err, size_t en);
