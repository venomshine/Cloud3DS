#pragma once

typedef enum { LANG_ES = 0, LANG_EN, LANG_JA, LANG_COUNT } Lang;

/* 0 = sistema; si no, Lang + 1 */
void i18n_init(int setting);
Lang i18n_lang(void);

/* La clave es el texto en español; devuelve la traducción del idioma actual */
const char *i18n_tr(const char *es);
#define _(s) i18n_tr(s)
