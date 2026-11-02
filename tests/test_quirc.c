/*
 * Cloud3DS - prueba en el PC de la lectura de QR (quirc) con imágenes como las de la cámara.
 *   gcc -O2 -Wall -Ilib -o /tmp/test_quirc tests/test_quirc.c lib/quirc*.c lib/qrcodegen.c -lm
 *   /tmp/test_quirc
 */
#include "qrcodegen.h"
#include "quirc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

/* QR de texto, ampliado a escala s, en (ox, oy) de una imagen 400×240 gris con ruido */
static int try_read(const char *text, int s, int ox, int oy, int noise, char *out, size_t n)
{
	uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
	if (!qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_MEDIUM, 1, 40, qrcodegen_Mask_AUTO, true))
		return -1;
	int sz = qrcodegen_getSize(qr);
	struct quirc *q = quirc_new();
	quirc_resize(q, 400, 240);
	int w, h;
	uint8_t *img = quirc_begin(q, &w, &h);
	srand(1234);
	for (int y = 0; y < h; y++)
		for (int x = 0; x < w; x++) {
			int mx = (x - ox) / s - 4, my = (y - oy) / s - 4; // 4 módulos de margen blanco
			int dark = x >= ox && y >= oy && mx >= 0 && my >= 0 && mx < sz && my < sz && qrcodegen_getModule(qr, mx, my);
			int v = dark ? 40 : 210;
			v += noise ? rand() % (2 * noise + 1) - noise : 0;
			img[y * w + x] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
		}
	quirc_end(q);
	int r = -2;
	for (int i = 0; i < quirc_count(q); i++) {
		struct quirc_code c;
		struct quirc_data d;
		quirc_extract(q, i, &c);
		if (quirc_decode(&c, &d) == QUIRC_SUCCESS) {
			snprintf(out, n, "%.*s", d.payload_len, (char *)d.payload);
			r = 0;
			break;
		}
	}
	quirc_destroy(q);
	return r;
}

int main(void)
{
	const char *t[] = {"https://github.com/BernardoGiordano/Checkpoint/releases/download/v3.10.1/Checkpoint.cia",
	                   "https://example.com/a.cia\nhttps://example.com/b.zip", "hola, esto no es un enlace"};
	for (int i = 0; i < 3; i++)
		for (int s = 2; s <= 5; s++) {
			char out[2048] = "";
			int r = try_read(t[i], s, 30 + s * 3, 10, 25, out, sizeof out);
			int ok = r == 0 && !strcmp(out, t[i]);
			printf("texto %d, escala %d: %s\n", i, s, ok ? "leído" : "no");
		}
	// como en la cámara: muchas posiciones; lo que cuenta es que la mayoría se lea
	for (int s = 2; s <= 5; s++) {
		int ok = 0, tot = 0;
		for (int ox = 0; ox < 160; ox += 7)
			for (int oy = 0; oy < 60; oy += 11) {
				char out[2048] = "";
				if (45 * s + oy > 240 || 45 * s + ox > 400)
					continue;
				tot++;
				ok += try_read(t[0], s, ox, oy, 25, out, sizeof out) == 0 && !strcmp(out, t[0]);
			}
		printf("escala %d: %d de %d\n", s, ok, tot);
		if (s >= 3 && ok < tot)
			fails++;
	}
	printf("fallos: %d\n", fails);
	return fails != 0;
}
