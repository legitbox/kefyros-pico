#!/usr/bin/env python3
# mkkx.py — pack a freestanding app .bin/.elf into a Kefyros .kx executable.
# Header layout matches `struct kx_header` in sdk/kapi.h (32 bytes, little-endian).
import argparse, struct, subprocess, zlib

def nm_addr(prefix, elf, sym):
    out = subprocess.check_output([prefix + 'nm', elf], text=True)
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[2] == sym:
            return int(p[0], 16)
    raise SystemExit(f"mkkx: symbol '{sym}' not found in {elf}")

def bss_size(prefix, elf):
    out = subprocess.check_output([prefix + 'size', '-A', elf], text=True)
    for line in out.splitlines():
        p = line.split()
        if len(p) >= 2 and p[0] == '.bss':
            return int(p[1])
    return 0

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--elf', required=True)
    ap.add_argument('--bin', required=True)
    ap.add_argument('--load-base', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--prefix', default='arm-none-eabi-')
    ap.add_argument('--flags', type=lambda x: int(x, 0), default=0)
    ap.add_argument('--stack', type=lambda x: int(x, 0), default=0)
    a = ap.parse_args()

    load_base = int(a.load_base, 0)
    entry_off = nm_addr(a.prefix, a.elf, 'app_main') - load_base
    if entry_off < 0:
        raise SystemExit("mkkx: app_main resolved below load_base")
    bss = bss_size(a.prefix, a.elf)
    image = open(a.bin, 'rb').read()
    crc = zlib.crc32(image) & 0xffffffff

    hdr = struct.pack('<4sHHIIIIII', b'KX01', 1, a.flags, load_base,
                      entry_off, len(image), bss, a.stack, crc)
    assert len(hdr) == 32, len(hdr)
    with open(a.out, 'wb') as f:
        f.write(hdr)
        f.write(image)
    print(f"mkkx: wrote {a.out}  load_base=0x{load_base:08x} entry_off=0x{entry_off:x} "
          f"image={len(image)}B bss={bss}B crc=0x{crc:08x} total={32+len(image)}B")

if __name__ == '__main__':
    main()
