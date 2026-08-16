#!/bin/bash
# Build hello.kx — proves the KAPI app toolchain end-to-end (compile -> link -> pack).
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT=/home/legitbox/kefyros-pico
SDK="$ROOT/sdk"

# KAPI apps are independent of a particular firmware build.
CC="$(command -v arm-none-eabi-gcc || true)"
[ -x "$CC" ] || CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"        # e.g. /usr/bin/arm-none-eabi-
echo "toolchain: $CC"

LOAD_BASE=0x20074000
CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions \
        -ffunction-sections -fdata-sections -Wall -I$SDK"

"$CC" $CFLAGS -c "$HERE/hello.c" -o "$HERE/hello.o"
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -Wl,--gc-sections \
      -T "$SDK/app.ld" "$HERE/hello.o" -o "$HERE/hello.elf" -lgcc
"${PREFIX}objcopy" -O binary "$HERE/hello.elf" "$HERE/hello.bin"

python3 "$ROOT/tools/mkkx.py" --elf "$HERE/hello.elf" --bin "$HERE/hello.bin" \
      --load-base "$LOAD_BASE" --prefix "$PREFIX" --out "$HERE/hello.kx"

echo "--- header (first 32 bytes) ---"
xxd "$HERE/hello.kx" | head -2
