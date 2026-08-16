#!/bin/bash
# build_calc_kx.sh — build the calc compute-core milestone .kx.
# Compiles the 7 UNMODIFIED calc compute files from apps/ + the calc_app.c entry, links
# libkapi at the stable ABI-1 arena, packs sdk/examples/calc/calc.kx.
set -e
ROOT=/home/legitbox/kefyros-pico
SDK="$ROOT/sdk"
APP="$SDK/examples/calc"
NAME=calc

CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"

ARENA=0x20074000
echo "stable KAPI arena base: $ARENA"

bash "$ROOT/tools/build_libkapi.sh"

cat > /tmp/kapi_app.ld <<EOF
ENTRY(app_main)
MEMORY { ARENA (rwx) : ORIGIN = $ARENA, LENGTH = 48K }
SECTIONS {
  . = ORIGIN(ARENA);
  .text : { KEEP(*(.text.app_main)) *(.text*) *(.rodata*) } > ARENA
  .data : { *(.data*) } > ARENA
  .bss (NOLOAD) : { __bss_start = .; *(.bss*) *(COMMON) __bss_end = .; } > ARENA
  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
EOF

CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions \
        -ffunction-sections -fdata-sections -Wall -I$ROOT/apps -I$SDK/include -I$SDK"

CORE="calc_eval calc_parse calc_exact calc_num calc_bignum calc_sym calc_solve"
OBJS=""
for f in $CORE; do "$CC" $CFLAGS -c "$ROOT/apps/$f.c" -o "/tmp/$f.o"; OBJS="$OBJS /tmp/$f.o"; done
"$CC" $CFLAGS -c "$APP/calc_app.c" -o "/tmp/calc_app.o"; OBJS="$OBJS /tmp/calc_app.o"

"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -Wl,--gc-sections -T /tmp/kapi_app.ld \
      $OBJS -L"$SDK/lib" -o "$APP/$NAME.elf" -lkapi -lgcc
"${PREFIX}objcopy" -O binary "$APP/$NAME.elf" "$APP/$NAME.bin"
echo "--- linked size (text+data+bss = arena footprint) ---"
"${PREFIX}size" "$APP/$NAME.elf"
python3 "$ROOT/tools/mkkx.py" --elf "$APP/$NAME.elf" --bin "$APP/$NAME.bin" \
      --load-base "$ARENA" --prefix "$PREFIX" --out "$APP/$NAME.kx"
echo "--- $NAME.kx ---"; ls -l "$APP/$NAME.kx"
