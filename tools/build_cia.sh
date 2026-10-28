#!/bin/sh
# Empaqueta Cloud3DS.cia a partir de Cloud3DS.elf (compila antes con make).
# Necesita bannertool y makerom (rutas por variables si no están en el PATH).
set -e
cd "$(dirname "$0")/.."
BANNERTOOL=${BANNERTOOL:-bannertool}
MAKEROM=${MAKEROM:-makerom}
VER=$(sed -n 's/^#define APP_VERSION *"\(.*\)"/\1/p' source/app.h)
MAJ=$(echo "$VER" | cut -d. -f1); MIN=$(echo "$VER" | cut -d. -f2); PAT=$(echo "$VER" | cut -d. -f3)
NUM=$(( (MAJ << 10) | (MIN << 4) | PAT ))
mkdir -p build
"$BANNERTOOL" makebanner -i cia/banner.png -a cia/banner.wav -o build/banner.bnr >/dev/null
"$BANNERTOOL" makesmdh -s "Cloud3DS" -l "Cloud3DS · tu nube local" -p "VENOMSHiNE" -i cia/icon.png -o build/icon.icn >/dev/null
"$MAKEROM" -f cia -o Cloud3DS.cia -elf Cloud3DS.elf -rsf cia/cloud3ds.rsf -icon build/icon.icn \
	-banner build/banner.bnr -exefslogo -target t -ver "$NUM"
echo "Cloud3DS.cia v$VER (-ver $NUM)"
# lista de SHA-256 para la release: la app la usa para comprobar las actualizaciones
sha256sum Cloud3DS.cia Cloud3DS.3dsx > Cloud3DS.sha256
echo "Cloud3DS.sha256 (súbelo a la release junto al .cia y el .3dsx)"
