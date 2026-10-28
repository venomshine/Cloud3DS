/*
 * Cloud3DS - tu nube local para Nintendo 3DS
 * Copyright (C) 2026 VENOMSHiNE - https://github.com/venomshine/Cloud3DS
 *
 * Software libre bajo la GNU General Public License v3 con términos adicionales
 * de atribución (sección 7). Consulta LICENSE y NOTICE.md.
 */
#include "img.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_STDIO_WARNINGS
#include "stb_image.h"

/* la 3DS tiene poca memoria: una foto de más de 4 Mpx no se abre (14 MB en RGBA) */
#define MAX_PIXELS (4 * 1024 * 1024)

bool img_size(const char *path, int *w, int *h)
{
	char full[700];
	snprintf(full, sizeof full, "sdmc:%s", path);
	int c;
	return stbi_info(full, w, h, &c) != 0;
}

ImgResult img_load_fit(const char *path, int maxw, int maxh, u8 **rgba, int *w, int *h, int *ow, int *oh)
{
	char full[700];
	snprintf(full, sizeof full, "sdmc:%s", path);
	int iw, ih, c;
	*rgba = NULL;
	if (!stbi_info(full, &iw, &ih, &c))
		return IMG_BAD;
	*ow = iw, *oh = ih;
	if ((long long)iw * ih > MAX_PIXELS)
		return IMG_TOO_BIG;
	u8 *src = stbi_load(full, &iw, &ih, &c, 4);
	if (!src)
		return IMG_BAD;
	float k = 1;
	if (iw > maxw || ih > maxh)
		k = iw * maxh > ih * maxw ? (float)maxw / iw : (float)maxh / ih;
	int tw = iw * k + 0.5f, th = ih * k + 0.5f;
	if (tw < 1) tw = 1;
	if (th < 1) th = 1;
	if (tw == iw && th == ih) {
		*rgba = src, *w = iw, *h = ih;
		return IMG_OK;
	}
	/* reducción por promedio de área: más limpia que tomar un píxel suelto */
	u8 *dst = malloc((size_t)tw * th * 4);
	if (!dst) {
		stbi_image_free(src);
		return IMG_BAD;
	}
	for (int y = 0; y < th; y++) {
		int y0 = y * ih / th, y1 = (y + 1) * ih / th;
		if (y1 <= y0) y1 = y0 + 1;
		for (int x = 0; x < tw; x++) {
			int x0 = x * iw / tw, x1 = (x + 1) * iw / tw;
			if (x1 <= x0) x1 = x0 + 1;
			unsigned s[4] = {0}, n = 0;
			for (int yy = y0; yy < y1; yy++)
				for (int xx = x0; xx < x1; xx++, n++)
					for (int i = 0; i < 4; i++)
						s[i] += src[((size_t)yy * iw + xx) * 4 + i];
			for (int i = 0; i < 4; i++)
				dst[((size_t)y * tw + x) * 4 + i] = s[i] / n;
		}
	}
	stbi_image_free(src);
	*rgba = dst, *w = tw, *h = th;
	return IMG_OK;
}
