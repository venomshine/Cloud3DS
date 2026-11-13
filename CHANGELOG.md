# Cambios

## 1.2.0 — novedades y arreglos

- Nueva interfaz en la consola: un inicio con cada función como una aplicación (Archivos, Títulos, Conectar, Descargas, Nube, Navegador, Música y Ajustes), con lo más útil de la elegida en la pantalla de arriba y dos tamaños de icono. Cada aplicación ocupa las dos pantallas; SELECT vuelve siempre al inicio y B, desde la pantalla principal de una aplicación. Las aplicaciones se comunican por el portapapeles (archivos copiados y enlaces).
- Pantalla de espera en las operaciones delicadas: instalar un CIA (también mientras se descarga o al actualizar Cloud3DS), desinstalar y borrar datos bloquean los botones, la pantalla táctil y HOME hasta terminar. Una descarga que se está instalando se puede cancelar sin riesgo.
- Descargas de Internet en la consola: desde la web, escribiendo un enlace o escaneando un QR (como en FBI), con cola, pausa y reanudación, comprobación de MD5 o SHA-256, y releases de GitHub. Van por el servicio HTTP del sistema, como FBI, y comprueban el certificado (si no se puede, se puede descargar sin comprobarlo tras un aviso).
- Los CIA pedidos desde la consola se instalan mientras se descargan, sin guardarlos en la SD.
- Títulos: juegos y aplicaciones de la SD con su icono, versión y lo que ocupan, ordenables por tamaño, con un resumen del espacio de la SD. Copias de la partida en el formato de Checkpoint, desinstalar solo la actualización o los DLC, borrar los datos extra (con copia previa y sin tocar los que comparta otro juego) y desinstalar.
- Nube: OneDrive en la consola, gestionado como Archivos: copiar, cortar, cambiar el nombre, eliminar y crear carpetas, con un mismo portapapeles para la SD y la nube (subir y descargar archivos o carpetas enteras pegando con X). Las capturas nuevas se suben solas.
- Navegador web (experimental), con favoritos e historial.
- Papelera (con recuperación durante 7 días), búsqueda en toda la SD, selección múltiple, MD5 y SHA-256, crear y extraer ZIP y descargar carpetas como ZIP.
- Reproductor de música (MP3, OGG y WAV) que sigue sonando con la tapa cerrada, y capturas de pantalla que aparecen al instante en la web.
- Escribir desde el teclado del PC en el editor de la consola (edición en directo).
- cloud3ds.local e instalación remota desde las apps de PC para FBI. Vincular por la IP vale también para el nombre .local y al revés.
- Actualizar Cloud3DS desde un archivo de la SD (Ajustes → Actualizaciones → Instalar desde un archivo) o subiendo su CIA desde la web, además de desde GitHub.
- Arreglos de manejo: una pulsación ya no activa lo que queda debajo al cambiar de carpeta; atrás vuelve a donde se abrió una categoría; Y y X cierran los menús que abren; un CIA se abre con dos toques.
- Textos revisados en la consola y en la web, con traducción completa al inglés y al japonés.

## 1.1.0 — novedades y arreglos

- Actualizaciones: Cloud3DS busca versiones nuevas en GitHub al abrirse (se puede desactivar) y desde Ajustes → Actualizaciones. Descarga la nueva, comprueba su SHA-256 y la instala; solo ofrece versiones más nuevas que la instalada.
- Copiar y mover dentro de la SD desde la web («Mover a…» y «Copiar a…», también con varios a la vez), con un selector de carpetas.
- Si un archivo ya existe al subir, copiar o mover: reemplazar (o combinar carpetas), renombrar, conservar ambos, omitir o cancelar todo, con «Aplicar a todos».
- Las carpetas del sistema salen aparte en la web, en un grupo plegable, y cualquier carpeta o archivo se puede destacar con uno de seis colores.
- Los dispositivos aparecen con su nombre real cuando la red lo da («Mi iPhone», «DESKTOP-ABC123») y se pueden renombrar desde la consola o desde la web.
- La tarjeta del QR se minimiza con SELECT o con el botón del QR de la barra de pestañas, y deja libre la pantalla de arriba.
- Textos revisados en la consola y en la web para que sean más claros y consistentes.
- Wi-Fi al abrir: si la red de la consola no estaba lista al arrancar, la app se quedaba en «Sin conexión Wi-Fi» para siempre. Ahora lo reintenta sola cada pocos segundos, distingue «Iniciando la red», «Conectado al Wi-Fi, esperando IP» y «Sin conexión Wi-Fi», y lo apunta todo en `net.log`.
- Flechas de transferencia en el sentido correcto: lo que la consola recibe lleva ↓ y lo que envía, ↑ (tarjeta de transferencias, historial y totales de Actividad).
- Salir siempre pide confirmación, con START o desde Ajustes. Si hay transferencias, el diálogo dice cuántas y cuál, y que se cancelarán.
- «Partidas» en la web abre las copias de Checkpoint (`/3ds/Checkpoint/saves`) o JKSM (`/JKSV`), y si no hay ninguna explica cómo hacerlas.
- Japonés: los textos de la consola ya no se salen de su sitio (pantalla principal y «Acerca de»), con traducciones más cortas donde se cortaban.
- Al confirmar un diálogo con A ya no se vuelve a abrir.

## 1.0.0 — primera versión

- Servidor web en la consola: web para el móvil y el PC servida comprimida desde la propia 3DS.
- Vinculación con QR (entra directamente) o con PIN de 4 cifras que no cambia entre arranques (se puede elegir uno propio o pedir uno al azar), con bloqueo cada vez más largo tras varios intentos fallidos. Dispositivos recordados, que se pueden expulsar desde la consola.
- Explorador de la SD: accesos rápidos, ruta navegable, lista o cuadrícula con miniaturas, búsqueda y orden por nombre, fecha o tamaño.
- Subidas de archivos y carpetas (botón o arrastrar y soltar) con cola, progreso, velocidad, tiempo restante y cancelación. Conflictos: reemplazar, conservar ambos u omitir.
- Descargas con soporte de rangos (los vídeos se pueden adelantar), visor de fotos, vídeos y texto.
- Crear carpetas, renombrar y borrar (también varios a la vez).
- Seguridad: modo solo lectura, subidas atómicas (no quedan restos si se corta), avisos de falta de espacio y del límite de 4 GB de FAT32.
- Consola: presentación animada; QR, dirección y PIN siempre a la vista con las transferencias en directo al lado; pestañas de Actividad, Dispositivos y Ajustes; «Acerca de» con los QR de GitHub, Discord y Ko-fi; fecha, hora, Wi-Fi y batería. Tema oscuro y claro, en español, inglés y japonés.
- Al expulsar un dispositivo desde la consola, su navegador muestra que lo desconectaron.
- Carpetas del sistema protegidas (con candado en la web): no se pueden borrar ni renombrar mientras la protección esté activa.
- No se suspende mientras transfiere y apaga la pantalla si se cierra la tapa.
- Se distribuye como `.cia` (con banner y sonido en el menú HOME) y `.3dsx`.
