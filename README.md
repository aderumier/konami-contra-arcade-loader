# konami-contra-arcade-loader

Standalone loader to run Konami **Contra: Evolution** (arcade, `AMContra.exe`, KC1 2011011801) under Wine
without TeknoParrot.

It is a replacement `libacio.dll`: the game imports it, so dropping ours next to the exe gets the loader
into the process. No injector is needed.

## What it does

- **Fixes the boot crash (a Wine bug).** The game calls `GetCharABCWidthsA(hdc, c, c, &abc)` with a
  Shift-JIS font and a 2-byte `c`. Wine turns the 2 bytes into 2 WCHARs and writes 2 `ABC` structs,
  smashing the stack (access violation at `0x43506d`). The loader gives Wine a scratch buffer and copies
  back only one entry.
- **Clock:** reports the operator clock as set and uses the PC clock. This avoids boot error 5-2503
  "The clock is un-setting up".
- **Keyboard remap:** rewrites the game's DirectInput keyboard table at load time.
- **COM1 control panel emulation:** answers the board init and sends coin events (keys 5 / 6).
- **USB HID I/O fallback** (VID 055c PID b3ab): presented as a quiet fake device.
- Ships DXVK 3.1.1 x32 `d3d9.dll`, installed next to the exe.

## Default controls

| | P1 | P2 |
|---|---|---|
| Start | 1 | 2 |
| Move | W A S D | Arrows |
| Buttons | E R T (+ I) | F G H (+ Home) |
| Coin | 5 | 6 |

F1 = Service (also adds a service credit), F2 = Test. Gamepads work through the game's own
DirectInput joystick support; the XInput Back button inserts a coin.

In the clock screen, values change with P1 up/down (W/S) and fields move with A/D.

## Build / install / run

Builds with clang + lld against Wine's PE import libraries (no mingw needed):

```sh
./build.sh                       # -> build/libacio.dll
GAME=/path/to/"Contra - Evolution" ./install.sh   # backs up libacio.dll once as libacio.dll.orig
WINEPREFIX=/path/to/prefix GAME=... ./run.sh      # windowed; add -full for fullscreen
./uninstall.sh                   # restores the original libacio.dll
```

Settings are in `contra.ini` (copied into the game directory on install). The log is `contra.log` in
the game directory.

## contra.ini

- `[general] clock_set=1`: report the clock as set.
- `[keyboard] aN=<DIK>`: rebind game action N. Actions: 0/1 start, 2-9 move (even = P1, odd = P2),
  10-17 buttons, 20 test, 21 service.
- `[keys] coin1/coin2`: Windows VK codes for the coin keys.
- `[pad] coin1/coin2`: XInput button masks for the coin keys.
- Debug: `debug=1` logs file opens, `sample=N` logs main-thread EIP every N seconds, and `count=1`
  logs input task call rates.

## Notes

- The ACIO wrapper the exe links against is never called by this build. The real panel is COM1:
  the game sends `0x90` (init) and the board pushes 2-byte events `0xBx code`, where bit 2 means pressed.
- Known quirk: a single tap moves the test-menu cursor twice (the key-trigger flag lasts 2 frames).
