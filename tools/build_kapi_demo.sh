#!/bin/bash
# Build a class-1 .kx linked at KAPI ABI 1's stable arena address.
# No firmware build or reflash is required. Usage: build_kapi_demo.sh <appdir>
set -e
ROOT=/home/legitbox/kefyros-pico
SDK="$ROOT/sdk"
APP="${1:-$SDK/examples/demo}"
NAME="$(basename "$APP")"

CC=/usr/bin/arm-none-eabi-gcc
PREFIX="${CC%gcc}"

ARENA=0x20074000
echo "stable KAPI arena base: $ARENA"

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

CFLAGS="-mcpu=cortex-m33 -mthumb -Os -ffreestanding -fno-builtin -fno-exceptions -ffunction-sections -fdata-sections -Wall -I$SDK"
"$CC" $CFLAGS -c "$APP/$NAME.c" -o "$APP/$NAME.o"
"$CC" -mcpu=cortex-m33 -mthumb -nostdlib -Wl,--gc-sections -T /tmp/kapi_app.ld "$APP/$NAME.o" -o "$APP/$NAME.elf" -lgcc
"${PREFIX}objcopy" -O binary "$APP/$NAME.elf" "$APP/$NAME.bin"
python3 "$ROOT/tools/mkkx.py" --elf "$APP/$NAME.elf" --bin "$APP/$NAME.bin" --load-base "$ARENA" --prefix "$PREFIX" --out "$APP/$NAME.kx"
echo "--- $NAME.kx ---"; ls -l "$APP/$NAME.kx"
