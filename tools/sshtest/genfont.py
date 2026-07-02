#!/usr/bin/env python3
# genfont.py — convert Spleen 6x12 (BDF) into ui/font_term6x12.c for the Term app.
#
# Output: const uint8_t kf_font6x12[0x91][12], indexed directly by the internal
# glyph value used in port/vt100.h. Each row is one byte, MSB-first, where bit7
# is the leftmost of 6 columns (bit7..bit2 used, bit1..bit0 zero). This is the
# native layout for the row-compose renderer in apps/term.c.
#
# ASCII 0x20-0x7E come straight from Spleen. The VT_GL_* special glyphs
# (0x80-0x90) are pulled from Spleen's Unicode coverage when present, else
# synthesized as clean centered box-drawing lines.
import sys, os

CELL_W, CELL_H = 6, 12
ASCENT = 9   # FONTBOUNDINGBOX 6 12 0 -3  ->  descent 3, ascent 9

def parse_bdf(path):
    glyphs = {}   # codepoint -> [12 bytes]
    with open(path, "r", encoding="latin-1") as f:
        lines = f.read().splitlines()
    i = 0
    while i < len(lines):
        if lines[i].startswith("STARTCHAR"):
            cp = None; bbx = None; bitmap = []
            i += 1
            while i < len(lines) and not lines[i].startswith("ENDCHAR"):
                ln = lines[i]
                if ln.startswith("ENCODING"):
                    cp = int(ln.split()[1])
                elif ln.startswith("BBX"):
                    p = ln.split(); bbx = (int(p[1]), int(p[2]), int(p[3]), int(p[4]))
                elif ln == "BITMAP":
                    i += 1
                    while i < len(lines) and not lines[i].startswith("ENDCHAR"):
                        bitmap.append(lines[i].strip()); i += 1
                    break
                i += 1
            if cp is not None and bbx is not None:
                glyphs[cp] = render_cell(bbx, bitmap)
        i += 1
    return glyphs

def render_cell(bbx, bitmap):
    w, h, xoff, yoff = bbx
    cell = [0] * CELL_H
    top = ASCENT - (yoff + h)   # empty rows above the glyph bitmap
    for r, hexrow in enumerate(bitmap[:h]):
        if not hexrow:
            continue
        val = int(hexrow, 16)
        # BDF pads each row to a whole number of bytes; the leftmost pixel is the
        # MSB of the first byte. For w<=8 there is exactly one byte.
        byte = (val >> (8 * ((w + 7)//8 - 1))) & 0xFF if w > 8 else val & 0xFF
        # shift so BBX col0 lands at cell column xoff (bit7 = col0)
        if xoff >= 0:
            row = (byte >> xoff) & 0xFC
        else:
            row = (byte << (-xoff)) & 0xFC
        tr = top + r
        if 0 <= tr < CELL_H:
            cell[tr] |= row
    return cell

# --- procedural box-drawing fallbacks (6 wide, 12 tall) ---
VCOL = 0x20   # column 2 (bit5) — the vertical stroke
HROW = 6      # middle row for horizontal strokes
FULL = 0xFC   # all 6 columns
LEFT_HALF  = 0xE0   # cols 0..2
RIGHT_HALF = 0x3C   # cols 2..5

def synth(kind):
    c = [0]*CELL_H
    if kind == 'hline':
        c[HROW] = FULL
    elif kind == 'vline':
        for r in range(CELL_H): c[r] = VCOL
    elif kind == 'ul':      # ┌
        c[HROW] = RIGHT_HALF
        for r in range(HROW, CELL_H): c[r] |= VCOL
    elif kind == 'ur':      # ┐
        c[HROW] = LEFT_HALF
        for r in range(HROW, CELL_H): c[r] |= VCOL
    elif kind == 'll':      # └
        for r in range(0, HROW+1): c[r] |= VCOL
        c[HROW] |= RIGHT_HALF
    elif kind == 'lr':      # ┘
        for r in range(0, HROW+1): c[r] |= VCOL
        c[HROW] |= LEFT_HALF
    elif kind == 'ltee':    # ├
        for r in range(CELL_H): c[r] = VCOL
        c[HROW] |= RIGHT_HALF
    elif kind == 'rtee':    # ┤
        for r in range(CELL_H): c[r] = VCOL
        c[HROW] |= LEFT_HALF
    elif kind == 'ttee':    # ┬
        c[HROW] = FULL
        for r in range(HROW, CELL_H): c[r] |= VCOL
    elif kind == 'btee':    # ┴
        for r in range(0, HROW+1): c[r] |= VCOL
        c[HROW] |= FULL
    elif kind == 'cross':   # ┼
        for r in range(CELL_H): c[r] = VCOL
        c[HROW] |= FULL
    elif kind == 'diamond':
        c[4] = 0x10; c[5] = 0x38; c[6] = 0x7C; c[7] = 0x38; c[8] = 0x10
    elif kind == 'checker':
        for r in range(CELL_H): c[r] = 0xA8 if (r & 1) == 0 else 0x54
    elif kind == 'degree':
        c[0] = 0x30; c[1] = 0x48 & 0xFC; c[2] = 0x30
    elif kind == 'plusminus':
        c[4] = 0x20; c[5] = 0x20; c[6] = 0xF8 & 0xFC; c[7] = 0x20; c[9] = 0xF8 & 0xFC
    elif kind == 'bullet':
        c[6] = 0x30
    return c

# internal glyph index -> (unicode codepoint or None, synth-kind)
SPECIAL = {
    0x80: (0x25C6, 'diamond'),
    0x81: (0x2592, 'checker'),
    0x82: (0x00B0, 'degree'),
    0x83: (0x00B1, 'plusminus'),
    0x84: (0x2518, 'lr'),
    0x85: (0x2510, 'ur'),
    0x86: (0x250C, 'ul'),
    0x87: (0x2514, 'll'),
    0x88: (0x253C, 'cross'),
    0x89: (0x2500, 'hline'),
    0x8A: (0x251C, 'ltee'),
    0x8B: (0x2524, 'rtee'),
    0x8C: (0x2534, 'btee'),
    0x8D: (0x252C, 'ttee'),
    0x8E: (0x2502, 'vline'),
    0x8F: (0x00B7, 'bullet'),
    0x90: (0xFFFD, None),   # replacement -> Spleen U+FFFD or '?'
}

def main():
    here = os.path.dirname(os.path.abspath(__file__))
    bdf = os.path.join(here, "spleen", "spleen-6x12.bdf")
    out = os.path.abspath(os.path.join(here, "..", "..", "ui", "font_term6x12.c"))
    g = parse_bdf(bdf)

    table = [[0]*CELL_H for _ in range(0x91)]
    # ASCII
    for cp in range(0x20, 0x7F):
        table[cp] = g.get(cp, g.get(ord('?'), [0]*CELL_H))
    # specials
    notes = []
    for idx, (cp, kind) in SPECIAL.items():
        if cp is not None and cp in g:
            table[idx] = g[cp]; notes.append("%02X<-U+%04X" % (idx, cp))
        elif kind:
            table[idx] = synth(kind); notes.append("%02X<-synth(%s)" % (idx, kind))
        else:
            table[idx] = g.get(ord('?'), [0]*CELL_H); notes.append("%02X<-'?'" % idx)

    with open(out, "w") as f:
        f.write("// ui/font_term6x12.c — 6x12 terminal font for the Term app.\n")
        f.write("// GENERATED by tools/sshtest/genfont.py from Spleen 6x12 (BSD-2-Clause,\n")
        f.write("// Copyright (c) 2018-2023 Frederic Cambus). Do not edit by hand.\n")
        f.write("// Indexed by the internal glyph value from port/vt100.h; each row is one\n")
        f.write("// byte, MSB-first, bit7 = leftmost of 6 columns (bits 7..2 used).\n")
        f.write("#include <stdint.h>\n\n")
        f.write("const uint8_t kf_font6x12[0x91][12] = {\n")
        for i in range(0x91):
            row = table[i]
            f.write("  [0x%02X] = {%s}," % (i, ",".join("0x%02X" % b for b in row)))
            if 0x20 <= i < 0x7F:
                ch = chr(i) if i != ord('\\') else '\\\\'
                f.write("  /* '%s' */" % ch)
            f.write("\n")
        f.write("};\n")
    print("wrote", out)
    print("specials:", " ".join(notes))

if __name__ == "__main__":
    main()
