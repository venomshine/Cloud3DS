#include "i18n.h"
#include "app.h"

#include <string.h>

typedef struct {
	const char *es, *en, *ja;
} TrEntry;

#include "i18n_table.h"

#define HSIZE 1024

static Lang cur = LANG_ES;
static short htab[HSIZE];

static u32 hash(const char *s)
{
	u32 h = 2166136261u;
	while (*s)
		h = (h ^ (u8)*s++) * 16777619u;
	return h;
}

void i18n_init(int setting)
{
	if (setting > 0 && setting <= LANG_COUNT) {
		cur = (Lang)(setting - 1);
	} else {
		u8 l = CFG_LANGUAGE_EN;
		if (R_SUCCEEDED(cfguInit())) {
			CFGU_GetSystemLanguage(&l);
			cfguExit();
		}
		cur = l == CFG_LANGUAGE_ES ? LANG_ES : l == CFG_LANGUAGE_JP ? LANG_JA : LANG_EN;
	}
	memset(htab, 0xFF, sizeof htab);
	for (int i = 0; i < (int)ARRAY_LEN(TR_TABLE); i++) {
		u32 h = hash(TR_TABLE[i].es) & (HSIZE - 1);
		while (htab[h] >= 0)
			h = (h + 1) & (HSIZE - 1);
		htab[h] = i;
	}
}

Lang i18n_lang(void)
{
	return cur;
}

const char *i18n_tr(const char *es)
{
	if (cur == LANG_ES)
		return es;
	u32 h = hash(es) & (HSIZE - 1);
	while (htab[h] >= 0) {
		const TrEntry *e = &TR_TABLE[htab[h]];
		if (strcmp(e->es, es) == 0) {
			const char *r = cur == LANG_EN ? e->en : e->ja;
			return r && *r ? r : es;
		}
		h = (h + 1) & (HSIZE - 1);
	}
	return es;
}
