# mupdf

[MuPDF](https://mupdf.com) 1.26.9, de Artifex, con licencia **AGPL-3.0** (ver `src/…/COPYING` tras descargarlo).
Lo usa el Lector de Cloud3DS para abrir PDF, CBZ (manga y cómics), EPUB, DOCX y TXT.

No se guarda en el repositorio: `make` (desde la raíz de Cloud3DS) lo descarga de mupdf.com, comprueba su SHA-256
y lo compila para la 3DS en `lib/` (`libmupdf.a` y `libmupdf-third.a`) con sus cabeceras en `include/`.

Ajustes para la 3DS:
- Sin JavaScript, códigos de barras, OCR, escritura de DOCX/ODT, XPS, perfiles de color ni brotli; sin las fuentes de
  otros alfabetos (quedan las básicas y la CJK básica). Ver `OPTS` en el `Makefile`.
- `fix3ds.h` se incluye en cada archivo: newlib define `quad` como macro (choca con un campo de MuPDF) y no trae
  `timegm` (la da Cloud3DS en `source/reader.c`).
- zlib es la de devkitPro (Cloud3DS ya la usa); el resto de librerías van incluidas en MuPDF.
