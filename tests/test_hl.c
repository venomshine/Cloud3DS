/*
 * Cloud3DS - pruebas de source/hl.c (colores de código) en el PC.
 *   gcc -Wall -fsanitize=address,undefined -Isource -o /tmp/test_hl tests/test_hl.c source/hl.c && /tmp/test_hl
 */
#include "hl.h"
#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)

/* los tramos cubren la línea entera, sin huecos; devuelve el color del carácter en pos */
static int color_at(const char *ext, const char *const *lines, int line, int pos)
{
	int lang = hl_lang(ext), st = 0, c = -1;
	HlSpan sp[64];
	for (int l = 0; lines[l]; l++) {
		int n = hl_line(lang, lines[l], &st, sp, 64), at = 0;
		for (int i = 0; i < n; i++) {
			CHECK(sp[i].start == at);
			if (l == line && pos >= sp[i].start && pos < sp[i].start + sp[i].len)
				c = sp[i].c;
			at += sp[i].len;
		}
		CHECK(at == (int)strlen(lines[l]));
	}
	return c;
}

int main(void)
{
	const char *py[] = {"def f(x):  # nota", "    \"\"\"doc", "    sigue\"\"\" ; return 1", NULL};
	CHECK(color_at("py", py, 0, 0) == HC_KEY);   /* def */
	CHECK(color_at("py", py, 0, 4) == HC_TAG);   /* f( : función */
	CHECK(color_at("py", py, 0, 12) == HC_COM);  /* # nota */
	CHECK(color_at("py", py, 2, 6) == HC_STR);   /* dentro de """ de dos líneas */
	CHECK(color_at("py", py, 2, 22) == HC_NUM);  /* 1 */
	const char *c[] = {"#include <stdio.h>", "int x = 0x1F; /* a", "b */ char *s = \"hola\"; // c", NULL};
	CHECK(color_at("cpp", c, 0, 1) == HC_KEY);
	CHECK(color_at("cpp", c, 1, 0) == HC_KEY);
	CHECK(color_at("cpp", c, 1, 8) == HC_NUM);
	CHECK(color_at("cpp", c, 2, 0) == HC_COM);   /* sigue el comentario */
	CHECK(color_at("cpp", c, 2, 16) == HC_STR);
	CHECK(color_at("cpp", c, 2, 25) == HC_COM);
	const char *j[] = {"{\"k\": \"v\", \"n\": 2, \"t\": true}", NULL};
	CHECK(color_at("json", j, 0, 1) == HC_ATTR);
	CHECK(color_at("json", j, 0, 6) == HC_STR);
	CHECK(color_at("json", j, 0, 25) == HC_KEY);
	const char *h[] = {"<a href=\"x\">t</a><!-- c", "-->", NULL};
	CHECK(color_at("html", h, 0, 1) == HC_TAG);
	CHECK(color_at("html", h, 0, 3) == HC_ATTR);
	CHECK(color_at("html", h, 1, 0) == HC_COM);
	CHECK(hl_lang("TXT") == HL_NONE && hl_lang("Py") == HL_PY && hl_lang(NULL) == HL_NONE);
	/* línea larguísima: no se sale de los tramos */
	static char big[5000];
	for (int i = 0; i < 4999; i++)
		big[i] = i % 3 ? 'a' : '"';
	int st = 0;
	HlSpan sp[8];
	int n = hl_line(HL_C, big, &st, sp, 8), tot = 0;
	for (int i = 0; i < n; i++)
		tot += sp[i].len;
	CHECK(n <= 8 && tot == 4999);
	printf(fails ? "%d fallos\n" : "todo bien\n", fails);
	return fails != 0;
}
