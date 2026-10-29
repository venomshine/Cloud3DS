/*
 * Cloud3DS - pruebas de source/digest.c en el PC (sin la consola).
 *   gcc -O2 -Wall -fsanitize=address,undefined -Isource -o /tmp/test_digest tests/test_digest.c source/digest.c
 *   /tmp/test_digest archivo trozo   → «md5 sha256» del archivo, leído en trozos de ese tamaño
 */
#include "digest.h"
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc < 3) {
		puts("uso: test_digest archivo trozo");
		return 2;
	}
	FILE *f = fopen(argv[1], "rb");
	if (!f)
		return 1;
	size_t k = (size_t)atoi(argv[2]);
	unsigned char *b = malloc(k);
	Md5 m;
	Sha256 s;
	md5_init(&m);
	sha256_init(&s);
	size_t n;
	while ((n = fread(b, 1, k, f)) > 0) {
		md5_update(&m, b, n);
		sha256_update(&s, b, n);
	}
	fclose(f);
	free(b);
	unsigned char d1[16], d2[32];
	char h1[33], h2[65];
	md5_final(&m, d1);
	sha256_final(&s, d2);
	digest_hex(d1, 16, h1);
	digest_hex(d2, 32, h2);
	printf("%s %s\n", h1, h2);
	return 0;
}
