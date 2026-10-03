#!/bin/sh
# Restore the real libacio.dll and remove our files.
set -e
GAME="${GAME:-/home/aderumier/code/jurassicpark.wine/drive_c/game/Contra - Evolution}"
[ -f "$GAME/libacio.dll.orig" ] && mv "$GAME/libacio.dll.orig" "$GAME/libacio.dll"
rm -f "$GAME/d3d9.dll" "$GAME/contra.log"
echo uninstalled
