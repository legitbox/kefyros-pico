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

# libxm: 8-bit samples to save heap, fixed rate, no names/timing/muting API.
XMFLAGS="-std=c2x -ffast-math -DNDEBUG -w -I$HERE/libxm \
        -DXM_VERBOSE=0 -DXM_LINEAR_INTERPOLATION=1 -DXM_RAMPING=1 -DXM_LIBXM_DELTA_SAMPLES=0 \
        -DXM_STRINGS=0 -DXM_TIMING_FUNCTIONS=0 -DXM_MUTING_FUNCTIONS=0 -DXM_SAMPLE_RATE=32768 \
        -DXM_MICROSTEP_BITS=12 -DXM_PANNING_TYPE=8 -DXM_LOOPING_TYPE=2 \
        -DXM_DISABLED_EFFECTS=0ULL -DXM_DISABLED_VOLUME_EFFECTS=0ULL -DXM_DISABLED_FEATURES=0ULL"

"$CC" $CFLAGS -c "$HERE/modplay.c" -o "$HERE/modplay.o"
for f in xm load play; do "$CC" $CFLAGS $XMFLAGS -c "$HERE/libxm/$f.c" -o "$HERE/xm_$f.o"; done
"$CC" -mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -nostdlib \
      -Wl,--gc-sections -T "$SDK/app.ld" "$HERE/modplay.o" "$HERE"/xm_*.o "$SDK/lib/libkapi.a" \
      -o "$HERE/modplay.elf" -lm -lgcc
"${PREFIX}objcopy" -O binary "$HERE/modplay.elf" "$HERE/modplay.bin"
python3 "$ROOT/tools/mkkx.py" --elf "$HERE/modplay.elf" --bin "$HERE/modplay.bin" \
      --load-base "$LOAD_BASE" --flags 4 --prefix "$PREFIX" --out "$HERE/modplay.kx"
"${PREFIX}size" "$HERE/modplay.elf"
