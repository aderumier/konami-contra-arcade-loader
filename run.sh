#!/bin/sh
# Run Contra: Evolution with the contra loader. Extra args go to the game (e.g. -full for fullscreen).
GAME="${GAME:-/home/aderumier/code/jurassicpark.wine/drive_c/game/Contra - Evolution}"
export WINEPREFIX="${WINEPREFIX:-/home/aderumier/code/jurassicpark.wine}"
export WINEDLLOVERRIDES="d3d9=n,b;libacio=n"
export WINEDEBUG="${WINEDEBUG:--all}"
cd "$GAME" && exec wine AMContra.exe "$@"
