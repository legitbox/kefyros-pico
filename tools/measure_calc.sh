#!/bin/bash
# measure_calc.sh — gross footprint of the calc compute core compiled against libkapi.
# These 7 files are the part that ports to KAPI UNMODIFIED (pure C, no LVGL/kernel deps).
set -e
ROOT=/home/legitbox/kefyros-pico
CC="$(grep -m1 '^CMAKE_C_COMPILER:FILEPATH=' "$ROOT/build/CMakeCache.txt" | cut -d= -f2)"
[ -x "$CC" ] || CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"
CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions -ffunction-sections -fdata-sections -Wall -I$ROOT/apps -I$ROOT/sdk/include -I$ROOT/sdk"

OUT=/tmp/calcmeas
mkdir -p "$OUT"; rm -f "$OUT"/*.o

for f in calc_eval calc_parse calc_exact calc_num calc_bignum calc_sym calc_solve; do
    "$CC" $CFLAGS -c "$ROOT/apps/$f.c" -o "$OUT/$f.o"
done
echo "--- per-file (text data bss dec) ---"
"${PREFIX}size" "$OUT"/*.o
echo "--- TOTAL ---"
"${PREFIX}size" -t "$OUT"/*.o | tail -1
echo "ALL 7 COMPUTE FILES COMPILED CLEAN against libkapi shims"
