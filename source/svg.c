/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
/* Imágenes SVG para el navegador (logos, iconos) con NanoSVG (zlib) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define NANOSVG_IMPLEMENTATION
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#include "svg.h"

bool svg_render(const char *data, size_t n, int maxw, int maxh, unsigned char **rgba, int *w, int *h)
{
	*rgba = NULL;
	char *txt = malloc(n + 1); // NanoSVG escribe en el texto mientras lo lee
	if (!txt)
		return false;
	memcpy(txt, data, n);
	txt[n] = 0;
	NSVGimage *im = nsvgParse(txt, "px", 96);
	free(txt);
	if (!im)
		return false;
	float iw = im->width > 0 ? im->width : 24, ih = im->height > 0 ? im->height : 24;
	float k = fminf((float)maxw / iw, (float)maxh / ih);
	if (k <= 0)
		k = 1;
	int ow = (int)fmaxf(1, iw * k), oh = (int)fmaxf(1, ih * k);
	unsigned char *px = calloc((size_t)ow * oh, 4);
	NSVGrasterizer *r = px ? nsvgCreateRasterizer() : NULL;
	if (r) {
		nsvgRasterize(r, im, 0, 0, k, px, ow, oh, ow * 4);
		nsvgDeleteRasterizer(r);
	}
	nsvgDelete(im);
	if (!r) {
		free(px);
		return false;
	}
	*rgba = px, *w = ow, *h = oh;
	return true;
}
