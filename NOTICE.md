# Cloud3DS — aviso de copyright y términos adicionales

Copyright (C) 2026 VENOMSHiNE — https://github.com/venomshine/Cloud3DS

Cloud3DS es software libre: puedes redistribuirlo y/o modificarlo bajo los términos de la
**GNU General Public License versión 3** (archivo [LICENSE](LICENSE)), publicada por la Free Software Foundation.

Se distribuye con la esperanza de que sea útil, pero **SIN NINGUNA GARANTÍA**; ni siquiera la garantía implícita
de COMERCIABILIDAD o IDONEIDAD PARA UN PROPÓSITO PARTICULAR. Consulta la GNU General Public License para más detalles.

## Términos adicionales (sección 7 de la GPL-3.0)

Conforme a la sección 7 de la GNU GPL v3, se añaden estos términos a todo el código de Cloud3DS:

1. **Atribución obligatoria (sección 7b).** Toda obra que incluya o se base en código de Cloud3DS, total o parcialmente,
   debe mostrar de forma visible el aviso:

   > Basado en Cloud3DS de VENOMSHiNE — https://github.com/venomshine/Cloud3DS

   - en el README o la documentación principal del proyecto, y
   - en la propia aplicación, en una pantalla de créditos o «Acerca de» si la tiene.

   Este aviso no puede eliminarse ni ocultarse.

2. **Origen y versiones modificadas (sección 7c).** No se puede presentar el código como obra propia ni ocultar su origen.
   Las versiones modificadas deben indicar claramente que lo son y no pueden presentarse como la versión original de Cloud3DS.

3. **Nombres (sección 7e).** Esta licencia no concede derecho a usar los nombres «Cloud3DS» ni «VENOMSHiNE» para
   promocionar obras derivadas ni para dar a entender que el autor las respalda, más allá del aviso de atribución anterior.

## Código de terceros

Las siguientes partes conservan sus propias licencias:

- `lib/cJSON.*` — [cJSON](https://github.com/DaveGamble/cJSON), licencia MIT.
- `lib/qrcodegen.*` — [QR Code generator](https://github.com/nayuki/QR-Code-generator) de Project Nayuki, licencia MIT.
- `lib/stb_image.h` — [stb_image](https://github.com/nothings/stb) de Sean Barrett, dominio público o licencia MIT (a elegir).
- [libctru, citro2d y citro3d](https://github.com/devkitPro) (enlazadas al compilar), licencia zlib.
- [libcurl](https://curl.se/) (enlazada al compilar), licencia curl (tipo MIT).
- [Mbed TLS](https://github.com/Mbed-TLS/mbedtls) (enlazada al compilar), licencia Apache-2.0.
- `romfs/certs/cacert.pem` — [certificados raíz de Mozilla](https://curl.se/docs/caextract.html), licencia MPL-2.0.

---

## English summary

Cloud3DS is free software licensed under the **GNU GPL v3** (see [LICENSE](LICENSE)), with these additional terms under section 7:
(b) any work that uses or is based on Cloud3DS code must visibly show *"Based on Cloud3DS by VENOMSHiNE — https://github.com/venomshine/Cloud3DS"*
in its README/documentation and in the app's credits or About screen, if it has one;
(c) the origin must not be misrepresented, and modified versions must be clearly marked as such;
(e) no rights are granted to use the names "Cloud3DS" or "VENOMSHiNE" to promote derivative works.
Third-party code in `lib/` keeps its own license.
