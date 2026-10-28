# Pruebas

Scripts usados para probar Cloud3DS en el emulador Azahar (núcleo libretro, sin pantalla) antes de cada entrega:

- `mkdata.py` rellena una SD emulada con capturas, fotos, CIAs y homebrew de ejemplo.
- `test_api.py` prueba la API: vinculación, listas, subidas (UTF-8, sobrescribir, cortadas), rangos, renombrar, borrar y protecciones.
- `web.js` / `web.sh` prueban la web en un móvil y un PC simulados con Playwright y sacan capturas.
- `slow_upload.py` sube a velocidad limitada, como por Wi-Fi real, para ver el progreso en la consola.
- `readme_shots.js` / `readme.sh` sacan las capturas del README (interfaz en inglés).
- `test_txt.c` prueba en el PC el editor de texto (`source/txt.c`): que guardar sin cambios deja el archivo idéntico (BOM, saltos de Windows, salto final), que Latin-1 y los binarios quedan en solo lectura y que un archivo enorme no se corta a mitad de un carácter. Cómo compilarlo, en su cabecera.

Las rutas apuntan al entorno donde se ejecutaron (`/home/user/tools`): ajústalas al tuyo.
