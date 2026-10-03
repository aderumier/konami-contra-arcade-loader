#!/bin/sh
# Install contra into the game dir: back up the real libacio.dll once, copy ours over it, DXVK app-local.
set -e
GAME="${GAME:-/home/aderumier/code/jurassicpark.wine/drive_c/game/Contra - Evolution}"
HERE="$(dirname "$0")"
[ -f "$GAME/libacio.dll.orig" ] || cp "$GAME/libacio.dll" "$GAME/libacio.dll.orig"
cp "$HERE/build/libacio.dll" "$GAME/libacio.dll"
rm -f "$GAME/d3d9.dll"; cp "$HERE/dxvk/d3d9.dll" "$GAME/d3d9.dll"; chmod 644 "$GAME/d3d9.dll"
[ -f "$GAME/contra.ini" ] || cp "$HERE/contra.ini" "$GAME/contra.ini"
echo installed
