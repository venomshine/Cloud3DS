/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
// Cloud3DS - traducciones. La tabla se genera con tools/gen_i18n.py -> i18n_table.h
#include <3ds.h>
#include <string.h>
#include <stdio.h>
#include "i18n.h"

typedef struct { const char *es, *en, *ja; } TrEntry;
#include "i18n_table.h"

int g_lang = LANG_ES;
#define HSIZE 2048
static short s_hash[HSIZE];
static bool s_built;

static unsigned hash_str(const char *s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

static void build(void) {
    memset(s_hash, -1, sizeof(s_hash));
    int n = (int)(sizeof(TR_TAB) / sizeof(TR_TAB[0]));
    for (int i = 0; i < n; i++) {
        unsigned h = hash_str(TR_TAB[i].es) & (HSIZE - 1);
        while (s_hash[h] >= 0) h = (h + 1) & (HSIZE - 1);
        s_hash[h] = (short)i;
    }
    s_built = true;
}

const char *tr(const char *es) {
    if (!es || g_lang == LANG_ES || !*es) return es;
    if (!s_built) build();
    unsigned h = hash_str(es) & (HSIZE - 1);
    while (s_hash[h] >= 0) {
        const TrEntry *e = &TR_TAB[s_hash[h]];
        if (!strcmp(e->es, es)) {
            const char *t = g_lang == LANG_JA ? e->ja : e->en;
            return t && *t ? t : es;
        }
        h = (h + 1) & (HSIZE - 1);
    }
    return es;
}

static int system_lang(void) {
    u8 l = CFG_LANGUAGE_EN;
    if (R_SUCCEEDED(cfguInit())) { CFGU_GetSystemLanguage(&l); cfguExit(); }
    if (l == CFG_LANGUAGE_JP) return LANG_JA;
    if (l == CFG_LANGUAGE_ES) return LANG_ES;
    return LANG_EN;
}

void i18n_apply(int setting) {
    switch (setting) {
    case LSET_ES: g_lang = LANG_ES; break;
    case LSET_EN: g_lang = LANG_EN; break;
    case LSET_JA: g_lang = LANG_JA; break;
    default: g_lang = system_lang(); break;
    }
}

const char *lang_setting_name(int setting) {
    static const char *names[] = {"Español", "English", "日本語"};
    static char buf[48];
    switch (setting) {
    case LSET_ES: return names[0];
    case LSET_EN: return names[1];
    case LSET_JA: return names[2];
    default: snprintf(buf, sizeof(buf), "%s (%s)", tr("Sistema"), names[system_lang()]); return buf;
    }
}
