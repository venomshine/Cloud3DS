/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - idiomas de la interfaz (español, inglés, japonés)
#pragma once
enum { LANG_ES, LANG_EN, LANG_JA };
enum { LSET_SYSTEM, LSET_ES, LSET_EN, LSET_JA };   // valor guardado en la configuración
extern int g_lang;                                  // idioma efectivo
void i18n_apply(int setting);                      // aplica el ajuste (0 = el de la consola)
const char *tr(const char *es);                    // texto en el idioma actual (la clave es el español)
const char *lang_setting_name(int setting);        // "Sistema (English)", "Español"…
#define _(s) tr(s)
#define N_(s) s                                    // se traduce después, con tr()
