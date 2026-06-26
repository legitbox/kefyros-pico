# Kefyros Paint — design plan (V1)

A keyboard-driven **pixel-art editor** for the PicoCalc / RP2350 Kefyros build.
Icon: amber pencil (`/kefyros/icons/paint.png`, from Downloads `paint.png`).

## Locked spec (from user)
- **Purpose:** general pixel-art doodling (not tied to wallpapers or game tiles — but PNG
  export makes the art reusable as either).
- **Color model:** **16-color fixed indexed palette** (editable slots). Slot 0 = background.
- **Canvas size:** **preset squares** — 16, 32, 64, 128, 144 — chosen at New-canvas time.
- **V1 tools:** pencil, eraser, bucket fill, line, rectangle, ellipse, eyedropper, undo/redo.

## Hard platform facts (the design is shaped by these)
- Screen 320x320; persistent OS top bar = 24px → content area **320 x 296**.
- No mouse/touch. Input = **QWERTY raw keys** grabbed with `kf_grab_input(1)` (same model as
  editor.c / calc.c; poll fn in the superloop). Modifiers (Ctrl/Shift/Alt/Sym) available.
- **Heap is the constraint** (~180KB shared malloc). Per [[sram-heap-budget]]: allocate all
  working buffers on open, free on close — a fat static buffer here would silently OOM music/3D.
- SD via POSIX (`/kefyros/...`); LVGL PNG **decode** already in the build (lodepng).

## Data model
- **Art buffer = LVGL canvas in `LV_COLOR_FORMAT_I4`** (4bpp indexed). 144x144 worst case =
  **10,368 B** + a 16-entry palette. The canvas buffer *is* the document (no separate copy).
- 16-color RGB palette held alongside (`lv_canvas_set_palette`). Default = a balanced general
  palette (PICO-8-style: black, dark-blue, dark-purple, dark-green, brown, dark-grey, light-grey,
  white, red, orange, yellow, green, blue, indigo, pink, peach). Every slot editable (RGB).
- Slot 0 is the **background**: shown as a checkerboard in-editor; exported as either that color
  or transparent (PNG export toggle).

## Rendering — the one technical risk to spike first
- Display = the native-res I4 canvas **scaled by an integer zoom** with **antialias OFF**
  (`lv_image_set_antialias(false)`) for crisp nearest-neighbour pixels; pan by moving the object.
  This avoids ever allocating a full-screen framebuffer (a 320x320 RGB565 fb = 185KB would blow
  the heap — see calc_graph.c's strip trick for why we don't).
- **RISK / SPIKE:** confirm LVGL 9 scales an **indexed (I4)** canvas crisply. If it won't, fall
  back to an **RGB565 canvas at native art res** (144² = 41KB) for display + keep the I4 array as
  the saveable source of truth. Validate this in the first hour before building tools on top.
- Integer zoom levels auto-fit on New: 144→2x (288px), 128→2x, 64→4x, 32→8x, 16→16x; `+`/`-`
  change zoom, pan with Shift+arrows when the view exceeds 296px.

## Memory budget — SRAM holds only the live data; undo history lives in PSRAM
- **SRAM (malloc-on-open / free-on-close):** the I4 art canvas (~11KB) + one preview/restore
  scratch (~11KB) = **~21KB total**. This is all LVGL ever dereferences directly.
- **WHY the canvas can't be in PSRAM:** PSRAM is a PIO-SPI **block store, not memory-mapped**
  (`kf_psram_read/write`, no real pointers). LVGL renders/scales the canvas by dereferencing the
  buffer pointer every frame — so the live canvas MUST be SRAM. It's only ~11KB, so that's free.
- **Undo/redo → PSRAM, full snapshots (no delta ring needed).** History is accessed by our own
  code, not LVGL, so PSRAM is the ideal home. Store a 10KB snapshot per op; 8MB ÷ 10KB ≈ **~800
  levels** = effectively unlimited. Cost per op ≈ a 10KB quad-QSPI write ≈ **~0.5ms** (undo is a
  rare keypress — imperceptible). `kf_psram_alloc` a ring on open, `kf_psram_free_to` on close
  (shared LIFO bump allocator, same pattern as the GB ROM cache).
- Net SRAM peak while open ≈ **~21KB**, all freed on exit; undo depth bounded only by PSRAM.

## UX / keyboard map (modal, single-hand friendly)
- **Arrows** move the pixel cursor. **Space/Enter** = apply current tool at cursor.
- Tools: `P` pencil · `E` eraser · `F` fill · `L` line · `R` rect · `O` ellipse · `I` eyedropper.
- Color: `[` / `]` cycle active slot; `Tab` opens the palette+tool **HUD overlay** to pick directly;
  number keys `1`-`9`,`0` jump to slots 1-10 (Shift for 11-16).
- Zoom/pan: `+` / `-` zoom, `Shift+arrows` pan. `G` toggle pixel grid. `H` toggle help/HUD.
- History: `Ctrl+Z` undo · `Ctrl+Y` (or `U`/`Shift+U`) redo.
- Two-point tools (line/rect/ellipse): pick tool → move to P1 → Enter anchors → move (live
  preview via restore-from-scratch each step) → Enter commits · Esc cancels. Shift = filled shape.
- Files: `F1` Save · `F2` Save As · `F3` Open · `F4` New (size picker) · `F5` Export PNG ·
  `F6` Edit palette · `Esc` Quit (unsaved-changes guard like editor.c: press again to discard).

## File formats
- **Native `.kpx`** (round-trips palette + size + indices): small header
  `magic 'KPX1' | u16 w | u16 h | u8 bpp(4) | 16×RGB888 palette | I4 pixel data`. ~10KB. Stored in
  `/kefyros/paint/`.
- **PNG export** (`F5`) → `/kefyros/paint/<name>.png`, RGB (or RGBA if slot-0-transparent). lodepng
  is present for decode; enable its **encoder** (cheap) — or fall back to writing an uncompressed
  PNG (stored zlib blocks) / a trivial BMP if enabling encode is awkward. Makes art usable as a
  wallpaper or icon immediately.
- New dir constant `KF_PAINT "/kefyros/paint"` (mkdir-on-demand like KF_NOTES/KF_WALLS).

## Integration points
- New files: `apps/paint.c` (+ `apps/paint_png.c` if PNG encode is split out). CMake auto-globs
  `apps/*.c` — no CMake edit needed.
- `kefyros.h`: declare `void app_paint_open(void);` and `void paint_poll(void);`; add `KF_PAINT`.
- `ui/launcher.c` `dapps[]`: add `{ "paint", "Paint", app_paint_open, 12 }`.
- Superloop: call `paint_poll()` beside `calc_poll()`/`editor_poll()` (no-op unless open).
- Copy Downloads `paint.png` → SD `/kefyros/icons/paint.png` (48px; launcher scales it).
- Host-first where possible: the fill/line/rect/ellipse rasterisers + .kpx (de)serialise + PNG
  encode are board-agnostic → unit-test them with plain gcc under `tools/paint_test/` before flashing.

## Build phases (each ends flashable / testable)
1. **Skeleton + render spike** — app opens, grabs keys, I4 canvas + palette, integer-zoom scaled
   view, cursor + pencil + eraser, grid/HUD toggle. *Resolves the indexed-scaling risk.*
2. **Color + fill** — palette HUD (`Tab`), slot cycle/number keys, eyedropper, bucket fill
   (scanline flood). Palette edit screen (`F6`).
3. **Shapes** — line (Bresenham), rectangle, ellipse (midpoint); outline + filled; live preview.
4. **Undo/redo** — PSRAM snapshot ring (`kf_psram_alloc`/`free_to`) wired into every mutating op.
5. **Persistence** — `.kpx` save/load/open-picker, New-canvas size picker, unsaved guard.
6. **PNG export** — encoder + slot-0-transparent toggle; drop into `/kefyros/paint/`.

## Defaults chosen (override anytime)
- Save dir `/kefyros/paint/`; PNG export same dir. Default palette = PICO-8 set, fully editable.
- Slot 0 = background, opaque by default, optional transparent-on-PNG-export.
- Redo on `Ctrl+Y`. New-canvas default size = 64. Default tool = pencil, default color = slot 7 (white).
