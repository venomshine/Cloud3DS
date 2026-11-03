# qjs

[QuickJS-ng](https://github.com/quickjs-ng/quickjs) (licencia MIT, ver `LICENSE`), el motor de JavaScript
que usa el navegador de Cloud3DS. Commit `e22ce4d`.

Solo el motor: `quickjs.c`, `libregexp.c`, `libunicode.c`, `dtoa.c` y sus cabeceras, sin cambios.
No se usa `quickjs-libc` (archivos, procesos…): lo que una página puede hacer lo da `source/jsdom.c`.
