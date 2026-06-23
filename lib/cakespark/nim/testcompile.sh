#!/bin/bash
# Spike: does the Nim-generated CakeSpark C compile freestanding for the RP2350 M33?
set -u
D=/home/legitbox/kefyros-pico/lib/cakespark/nim/cnim
NIMLIB=/usr/lib/nim/lib
cd "$D" || { echo "no cnim dir at $D"; exit 1; }
echo "nimbase: $(ls "$NIMLIB/nimbase.h" 2>/dev/null || echo MISSING)"
echo "c files: $(ls -1 ./*.c 2>/dev/null | wc -l)"
mkdir -p obj
FLAGS="-mcpu=cortex-m33 -mthumb -mfloat-abi=softfp -mfpu=fpv5-sp-d16 -Os \
-ffreestanding -ffunction-sections -fdata-sections -fno-strict-aliasing \
-Wno-implicit-function-declaration -c -I. -I$NIMLIB"
ok=0; fail=0
for f in ./*.c; do
  if arm-none-eabi-gcc $FLAGS "$f" -o "obj/$(basename "$f").o" 2>/tmp/cerr; then
    ok=$((ok+1))
  else
    fail=$((fail+1)); echo "### FAIL: $(basename "$f")"; grep -m6 -E "error:|fatal|undefined reference" /tmp/cerr
  fi
done
echo "=== compiled OK=$ok FAIL=$fail ==="
echo "total .text+ size:"; du -ch obj/*.o 2>/dev/null | tail -1
