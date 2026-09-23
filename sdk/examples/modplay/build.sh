#!/bin/bash
# Build modplay.kx (windowed class-1 app). Needs arm-none-eabi-gcc on PATH.
# Install: copy modplay.kx, manifest.json and icon.png to /apps/modplay/ on the SD card,
# put .mod files in /kefyros/mods.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SDK="$(cd "$HERE/../.." && pwd)"
ROOT="$(cd "$SDK/.." && pwd)"
CC="$(command -v arm-none-eabi-gcc)"
PREFIX="${CC%gcc}"
LOAD_BASE=0x20074000
# softfp: FPU instructions for pocketmod's float mixer, soft-float calling convention
# like libkapi.
CFLAGS="-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -Os \
        -ffreestanding -fno-builtin -ffunction-sections -fdata-sections -Wall \
        -I$SDK -I$SDK/include"

"$CC" $CFLAGS -c "$HERE/modplay.c" -o "$HERE/modplay.o"
"$CC" -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -nostdlib \
      -Wl,--gc-sections -T "$SDK/app.ld" "$HERE/modplay.o" "$SDK/lib/libkapi.a" \
      -o "$HERE/modplay.elf" -lgcc
"${PREFIX}objcopy" -O binary "$HERE/modplay.elf" "$HERE/modplay.bin"
python3 "$ROOT/tools/mkkx.py" --elf "$HERE/modplay.elf" --bin "$HERE/modplay.bin" \
      --load-base "$LOAD_BASE" --flags 4 --prefix "$PREFIX" --out "$HERE/modplay.kx"
"${PREFIX}size" "$HERE/modplay.elf"
