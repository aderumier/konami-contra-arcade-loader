#!/bin/sh
# Build contra (replacement libacio.dll, 32-bit) with clang + lld + Wine's PE import libs.
set -e
cd "$(dirname "$0")"
mkdir -p build
OUT=build/libacio.dll
clang --target=i686-w64-windows-gnu -fuse-ld=lld -nostdlib -shared -O2 -Wall -Wno-unused-function \
    -ffreestanding -fno-stack-protector -mno-stack-arg-probe \
    -I/usr/include/wine/windows -I/usr/include/wine/msvcrt \
    src/contra.c src/libacio.def -o $OUT \
    -L/usr/lib/wine/i386-windows -lkernel32 -luser32 -lgdi32 -lmsvcrt \
    -Wl,--entry,_DllMain@12
echo built $OUT
