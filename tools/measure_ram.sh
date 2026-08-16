#!/bin/bash
# measure_ram.sh — static SRAM picture: firmware sections + the KAPI arena + heap headroom.
ROOT=/home/legitbox/kefyros-pico
TARGET="${1:-$ROOT/build-pimoroni/kefyros.elf}"
case "$TARGET" in /*) ;; *) TARGET="$ROOT/$TARGET" ;; esac
BDIR="$(dirname "$TARGET")"
CC="$(grep -m1 '^CMAKE_C_COMPILER:FILEPATH=' "$BDIR/CMakeCache.txt" 2>/dev/null | cut -d= -f2)"
[ -x "$CC" ] || CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"

echo "=== kefyros.elf sections (text=flash, data+bss=SRAM) ==="
"${PREFIX}size" "$TARGET"

echo
echo "=== current arena size ==="
grep -n 'KAPI_ARENA_BYTES' "$ROOT/port/kapi.c" | head -1

echo
echo "=== map files present ==="
ls -1 "$BDIR"/*.map 2>/dev/null || echo "(none)"

echo
echo "=== RAM region + heap/stack symbols (from .elf via nm) ==="
"${PREFIX}nm" "$TARGET" | grep -iE '__StackLimit|__stack|__bss_end__|__end__|__HeapLimit|heap|g_kapi_arena' | sort
