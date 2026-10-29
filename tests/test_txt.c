/*
 * Cloud3DS - pruebas de source/txt.c en el PC (sin la consola).
 *   gcc -Wall -fsanitize=address,undefined -DTXT_ROOT='""' -Isource -o /tmp/test_txt tests/test_txt.c source/txt.c
 *   mkdir /tmp/tt && /tmp/test_txt /tmp/tt
 */
#include "txt.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
static int fails;
#define CHECK(c) do { if (!(c)) { printf("FALLO %s:%d %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
static void wr(const char *p, const char *b, size_t n) { FILE *f = fopen(p, "wb"); fwrite(b, 1, n, f); fclose(f); }
static char *rd(const char *p, size_t *n) { FILE *f = fopen(p, "rb"); static char b[1<<16]; *n = fread(b, 1, sizeof b, f); fclose(f); return b; }
int main(int argc, char **argv) {
	if (argc < 2) { puts("uso: test_txt carpeta_vacia"); return 2; }
	char p[512]; snprintf(p, sizeof p, "%s/a.txt", argv[1]);
	TxtDoc d; size_t n; char *b;
	// CRLF + BOM + final newline: guardar sin cambios = idéntico
	const char *s1 = "\xEF\xBB\xBFhola\r\nmundo ñ\r\n\r\nfin\r\n";
	wr(p, s1, strlen(s1));
	CHECK(txt_load(&d, p) == 0); CHECK(d.n == 4); CHECK(d.crlf && d.bom && d.final_nl && d.ro == TXT_RW);
	CHECK(!strcmp(d.ln[1], "mundo ñ")); CHECK(d.ln[2][0] == 0);
	CHECK(txt_save(&d, p) == 0); b = rd(p, &n); CHECK(n == strlen(s1) && !memcmp(b, s1, n));
	// editar con salto dentro: se parte en dos líneas
	CHECK(txt_set(&d, 0, "uno\ndos")); CHECK(d.n == 5 && !strcmp(d.ln[0], "uno") && !strcmp(d.ln[1], "dos") && !strcmp(d.ln[2], "mundo ñ"));
	CHECK(txt_insert(&d, 5, "nueva")); txt_remove(&d, 3);
	CHECK(txt_save(&d, p) == 0); b = rd(p, &n);
	const char *e1 = "\xEF\xBB\xBFuno\r\ndos\r\nmundo ñ\r\nfin\r\nnueva\r\n"; CHECK(n == strlen(e1) && !memcmp(b, e1, n));
	txt_free(&d);
	// LF sin salto final
	const char *s2 = "a\nb"; wr(p, s2, 3);
	CHECK(txt_load(&d, p) == 0); CHECK(d.n == 2 && !d.crlf && !d.final_nl && !d.bom);
	CHECK(txt_save(&d, p) == 0); b = rd(p, &n); CHECK(n == 3 && !memcmp(b, s2, 3)); txt_free(&d);
	// vacío
	wr(p, "", 0); CHECK(txt_load(&d, p) == 0); CHECK(d.n == 1 && d.ln[0][0] == 0 && d.ro == TXT_RW);
	txt_set(&d, 0, "x"); CHECK(txt_save(&d, p) == 0); b = rd(p, &n); CHECK(n == 1 && b[0] == 'x'); txt_free(&d);
	// Latin-1: solo lectura y se ve bien
	wr(p, "ma\xF1" "ana\n", 7); CHECK(txt_load(&d, p) == 0); CHECK(d.ro == TXT_RO_ENC && !strcmp(d.ln[0], "mañana"));
	CHECK(txt_save(&d, p) != 0); b = rd(p, &n); CHECK(n == 7); txt_free(&d);
	// binario
	wr(p, "AB\0\x01\xFFxy", 7); CHECK(txt_load(&d, p) == 0); CHECK(d.ro == TXT_RO_BIN && !strcmp(d.ln[0], "AB...xy")); txt_free(&d);
	// grande: solo lectura, recortado sin partir un carácter
	char *big = malloc(TXT_VIEW_MAX + 10); memset(big, 'a', TXT_VIEW_MAX + 10); memcpy(big + TXT_VIEW_MAX - 1, "ñ", 2);
	wr(p, big, TXT_VIEW_MAX + 10); CHECK(txt_load(&d, p) == 0); CHECK(d.ro == TXT_RO_BIG && d.cut && d.n == 1 && strlen(d.ln[0]) == TXT_VIEW_MAX - 1); txt_free(&d);
	memset(big, 'b', TXT_EDIT_MAX + 1); wr(p, big, TXT_EDIT_MAX + 1); CHECK(txt_load(&d, p) == 0); CHECK(d.ro == TXT_RO_BIG && !d.cut); txt_free(&d);
	// UTF-8 inválido corto
	wr(p, "\xC3", 1); CHECK(txt_load(&d, p) == 0); CHECK(d.ro == TXT_RO_ENC); txt_free(&d);
	free(big);
	// bloque: se editan varias líneas a la vez
	wr(p, "a\nb\nc\nd\n", 8); CHECK(txt_load(&d, p) == 0);
	char *j = txt_join(&d, 1, 2); CHECK(j && !strcmp(j, "b\nc")); free(j);
	CHECK(txt_replace(&d, 1, 2, "X\nY\nZ")); CHECK(d.n == 5 && !strcmp(d.ln[1], "X") && !strcmp(d.ln[3], "Z") && !strcmp(d.ln[4], "d"));
	CHECK(txt_replace(&d, 0, 5, "solo")); CHECK(d.n == 1 && !strcmp(d.ln[0], "solo"));
	CHECK(!txt_replace(&d, 0, 2, "x"));
	CHECK(txt_save(&d, p) == 0); b = rd(p, &n); CHECK(n == 5 && !memcmp(b, "solo\n", 5)); txt_free(&d);
	// deshacer y rehacer: cada cambio vuelve exactamente a como estaba
	wr(p, "uno\ndos\ntres\n", 13); CHECK(txt_load(&d, p) == 0); CHECK(!d.dirty && !txt_can_undo(&d));
	CHECK(txt_set(&d, 1, "DOS")); CHECK(txt_insert(&d, 3, "cuatro")); txt_remove(&d, 0); CHECK(txt_move(&d, 0, 1));
	CHECK(d.n == 3 && !strcmp(d.ln[0], "tres") && !strcmp(d.ln[1], "DOS") && !strcmp(d.ln[2], "cuatro") && d.dirty);
	CHECK(txt_undo(&d) == 0); CHECK(!strcmp(d.ln[0], "DOS") && !strcmp(d.ln[1], "tres"));
	CHECK(txt_undo(&d) == 0); CHECK(d.n == 4 && !strcmp(d.ln[0], "uno"));
	CHECK(txt_undo(&d) == 3); CHECK(d.n == 3);
	CHECK(txt_undo(&d) == 1); CHECK(!strcmp(d.ln[1], "dos") && !d.dirty && !txt_can_undo(&d) && txt_undo(&d) == -1);
	CHECK(txt_redo(&d) == 1 && txt_redo(&d) == 3 && d.n == 4 && d.dirty);
	CHECK(txt_save(&d, p) == 0 && !d.dirty);
	CHECK(txt_undo(&d) == 3 && d.dirty); CHECK(txt_redo(&d) == 3 && !d.dirty); // vuelve a lo guardado
	CHECK(txt_undo(&d) == 3); CHECK(txt_set(&d, 0, "nuevo")); CHECK(!txt_can_redo(&d) && d.dirty); // un cambio nuevo borra lo de rehacer
	txt_free(&d);
	// la única línea: borrarla la deja vacía, y deshacer la recupera
	wr(p, "x", 1); CHECK(txt_load(&d, p) == 0); txt_remove(&d, 0); CHECK(d.n == 1 && d.ln[0][0] == 0);
	CHECK(txt_undo(&d) == 0 && !strcmp(d.ln[0], "x")); txt_free(&d);
	// muchos cambios: el historial tiene tope y no se rompe
	wr(p, "a", 1); CHECK(txt_load(&d, p) == 0);
	for (int k = 0; k < 450; k++) { char t[16]; snprintf(t, sizeof t, "v%d", k); txt_set(&d, 0, t); }
	int u = 0; while (txt_undo(&d) >= 0) u++;
	CHECK(u == 200 && !strcmp(d.ln[0], "v249") && d.dirty); txt_free(&d);
	printf(fails ? "%d fallos\n" : "todo bien\n", fails);
	return fails != 0;
}
