#!/bin/bash
# Regenerate the CakeSpark C sources for the firmware build.
# Run in WSL whenever lib/cakespark/src changes. Output: cnim/*.c (committed,
# compiled by the main CMake build alongside the rest of the firmware).
#   --os:any --mm:arc -d:useMalloc : freestanding, allocations via the C heap
#   --cpu:arm -d:cakesparkIntBits=32 : 32-bit M33 target
#   -d:danger : drop runtime checks / error-formatting machinery (smaller)
set -e
cd /home/legitbox/kefyros-pico/lib/cakespark/nim
rm -rf cnim
nim c --compileOnly --app:staticlib --noMain \
  --mm:arc -d:useMalloc --os:any --cpu:arm \
  -d:danger -d:cakesparkIntBits=32 \
  --nimcache:./cnim --path:../src \
  cake_embed.nim
# nimbase.h is #included by every generated .c; vendor it next to them so the
# firmware build needs only this dir on its include path (no system Nim dependency).
cp "$(find /usr/lib/nim /usr/share/nim -name nimbase.h 2>/dev/null | head -1)" cnim/
echo "generated $(ls cnim/*.c | wc -l) C files (+ nimbase.h)"
