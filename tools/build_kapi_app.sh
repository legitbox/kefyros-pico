#!/bin/bash
# build_kapi_app.sh — build a class-1 .kx that links the KAPI standard library (libkapi).
# Compiles every .c in <appdir>, links at the live kernel arena address, packs the .kx.
# Run AFTER build.sh (needs build/kefyros.elf for the arena address). Usage: <appdir>
set -e
ROOT=/home/legitbox/kefyros-pico
SDK="$ROOT/sdk"
APP="${1:?usage: build_kapi_app.sh <appdir>}"
NAME="$(basename "$APP")"

CC="$(grep -m1 '^CMAKE_C_COMPILER:FILEPATH=' "$ROOT/build/CMakeCache.txt" | cut -d= -f2)"
[ -x "$CC" ] || CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"

ARENA="$("${PREFIX}nm" "$ROOT/build/kefyros.elf" | awk '$3=="g_kapi_arena"{print "0x"$1; exit}')"
[ -n "$ARENA" ] || { echo "ERROR: g_kapi_arena not found in kefyros.elf"; exit 1; }
echo "kernel arena base: $ARENA"

bash "$ROOT/tools/build_libkapi.sh"

cat > /tmp/kapi_app.ld <<EOF
ENTRY(app_main)
MEMORY { ARENA (rwx) : ORIGIN = $ARENA, LENGTH = 64K }
SECTIONS {
  . = ORIGIN(ARENA);
  .text : { KEEP(*(.text.app_main)) *(.text*) *(.rodata*) } > ARENA
  .data : { *(.data*) } > ARENA
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > ARENA
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
EOF

CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions \
        -ffunction-sections -fdata-sections -Wall -I$SDK/include -I$SDK"

OBJS=""
for src in "$APP"/*.c; do
    o="${src%.c}.o"
    "$CC" $CFLAGS -c "$src" -o "$o"
    OBJS="$OBJS $o"
done

"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -Wl,--gc-sections -T /tmp/kapi_app.ld \
      $OBJS -L"$SDK/lib" -o "$APP/$NAME.elf" -lkapi -lgcc
"${PREFIX}objcopy" -O binary "$APP/$NAME.elf" "$APP/$NAME.bin"
python3 "$ROOT/tools/mkkx.py" --elf "$APP/$NAME.elf" --bin "$APP/$NAME.bin" \
      --load-base "$ARENA" --prefix "$PREFIX" --out "$APP/$NAME.kx"
echo "--- size ---"; "${PREFIX}size" "$APP/$NAME.elf"
echo "--- $NAME.kx ---"; ls -l "$APP/$NAME.kx"
