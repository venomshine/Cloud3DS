# Pruebas

Scripts usados para probar Cloud3DS en el emulador Azahar (núcleo libretro, sin pantalla) antes de cada entrega:

- `mkdata.py` rellena una SD emulada con capturas, fotos, CIAs y homebrew de ejemplo.
- `test_api.py` prueba la API: vinculación, listas, subidas (UTF-8, sobrescribir, cortadas), rangos, renombrar, borrar y protecciones.
- `web.js` / `web.sh` prueban la web en un móvil y un PC simulados con Playwright y sacan capturas.
- `slow_upload.py` sube a velocidad limitada, como por Wi-Fi real, para ver el progreso en la consola.
- `readme_shots.js` / `readme.sh` sacan las capturas del README (interfaz en inglés).

Las rutas apuntan al entorno donde se ejecutaron (`/home/user/tools`): ajústalas al tuyo.
