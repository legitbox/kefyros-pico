#!/bin/bash
# Cross-build the Kefyros PicoCalc firmware (RP2350). Run in WSL.
# Targets: pico2w (default), pimoroni (Pimoroni Pico Plus 2 W).
set -e
export PICO_SDK_PATH=/home/legitbox/pico-sdk
cd /home/legitbox/kefyros-pico

TARGET="${1:-pico2w}"
case "$TARGET" in
    pico2w)    BOARD="pico2_w" ;;
    pimoroni)  BOARD="pimoroni_pico_plus2_w_rp2350" ;;
    *) echo "Unknown target '$TARGET' (use: pico2w or pimoroni)" >&2; exit 2 ;;
esac

# toolchain sanity
command -v cmake          >/dev/null || { echo "MISSING cmake";          exit 1; }
command -v arm-none-eabi-gcc >/dev/null || { echo "MISSING arm-none-eabi-gcc"; exit 1; }
command -v ninja >/dev/null && GEN="-G Ninja" || GEN=""

BUILD_DIR="build-${TARGET}"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
cmake $GEN -DCMAKE_BUILD_TYPE=Release -DPICO_PLATFORM=rp2350 -DPICO_BOARD="$BOARD" .. 2>&1 | tail -25
echo "=== building ==="
cmake --build . -j"$(nproc)" 2>&1 | tail -60
echo "=== artifacts ==="
ls -la kefyros.uf2 kefyros.elf 2>/dev/null && echo "BUILD_OK target=$TARGET board=$BOARD" || echo NO_UF2
