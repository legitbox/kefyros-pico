#!/bin/bash
# Cross-build the Kefyros PicoCalc firmware (RP2350). Run in WSL.
set -e
export PICO_SDK_PATH=/home/legitbox/pico-sdk
cd /home/legitbox/kefyros-pico

# toolchain sanity
command -v cmake          >/dev/null || { echo "MISSING cmake";          exit 1; }
command -v arm-none-eabi-gcc >/dev/null || { echo "MISSING arm-none-eabi-gcc"; exit 1; }
command -v ninja >/dev/null && GEN="-G Ninja" || GEN=""

mkdir -p build
cd build
cmake $GEN -DCMAKE_BUILD_TYPE=Release -DPICO_PLATFORM=rp2350 -DPICO_BOARD=pico2_w .. 2>&1 | tail -25
echo "=== building ==="
cmake --build . -j"$(nproc)" 2>&1 | tail -60
echo "=== artifacts ==="
ls -la kefyros.uf2 kefyros.elf 2>/dev/null && echo BUILD_OK || echo NO_UF2
