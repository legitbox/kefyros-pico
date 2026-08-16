#!/bin/bash
# build_libkapi.sh — compile the KAPI standard library into sdk/lib/libkapi.a.
# Apps link this (-lkapi) to get malloc/printf/string/math forwarded through the kapi vtable.
set -e
ROOT=/home/legitbox/kefyros-pico
SDK="$ROOT/sdk"

CC="$(command -v arm-none-eabi-gcc || true)"
[ -x "$CC" ] || CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"

# -fno-builtin + -fno-tree-loop-distribute-patterns: stop the compiler turning our
# memcpy/memset loops back into calls to themselves.
CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions \
        -fno-tree-loop-distribute-patterns -ffunction-sections -fdata-sections \
        -Wall -I$SDK/include -I$SDK"

OBJS=""
for f in kapi_rt kapi_math kapi_printf; do
    "$CC" $CFLAGS -c "$SDK/lib/$f.c" -o "$SDK/lib/$f.o"
    OBJS="$OBJS $SDK/lib/$f.o"
done
rm -f "$SDK/lib/libkapi.a"
"${PREFIX}ar" rcs "$SDK/lib/libkapi.a" $OBJS
echo "libkapi.a built:"; ls -l "$SDK/lib/libkapi.a"
