# faad2

[FAAD2](https://github.com/knik0/faad2) 2.11.4, licencia **GPL-2.0 o posterior** (compatible con la GPL-3.0 de
Cloud3DS). Es el decodificador AAC de la música en streaming: SoundCloud entrega el audio como AAC por HLS.

No se guarda en el repositorio: `make` (desde la raíz de Cloud3DS) lo clona en esa versión, comprueba su commit y lo
compila para la 3DS en `lib/libfaad.a`, con `include/neaacdec.h`. En punto fijo (`FIXED_POINT`) y solo AAC-LC
(`LC_ONLY_DECODER`): más ligero y rápido en la consola.
