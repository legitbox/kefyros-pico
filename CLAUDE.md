# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

Kefyros is bare-metal firmware for a **PicoCalc** handheld built on a **Pico 2 W (RP2350)**. It is an LVGL 9.2 PDA with an amber-CRT theme: a launcher plus apps (calculator, file browser, text editor, FLAC music player, electronics tools, WiFi manager, an HTML-only web browser "Spineko", wallpaper/settings). **Code-only firmware** — all art, sounds, and notes live on the SD card under `/kefyros/...`, nothing is baked into the binary.

`STATUS.md` is the running engineering log (phase-by-phase: PSRAM, WiFi, browser). Read it for current state, known gotchas, and hardware-validation notes — it is more current than this file on in-progress work.

## Build / flash / stage

Builds run **inside WSL Ubuntu** (the repo lives in WSL at `/home/legitbox/kefyros-pico`; pico-sdk at `/home/legitbox/pico-sdk`).

```bash
bash build.sh          # -> build/kefyros.uf2   (CMake + Ninja, Release)
```

- From the Windows side / non-WSL shells, invoke through WSL: `wsl.exe -d Ubuntu -- bash -lc 'bash /home/legitbox/kefyros-pico/build.sh'`. The `wsl.exe` boundary strips `$VARS` — pass literal args or use script files, not env expansion. The plain Bash tool here is Git-Bash, **not** WSL.
- **Flash:** put the Pico in BOOTSEL (it mounts as Windows `F:`), copy `kefyros.uf2`. Re-enter BOOTSEL via POWER→BOOTSEL or a power-cycle.
- **SD card** is Windows `E:` (FAT32). Stage art/notes with `tools/stage_sd.py` (writes the `/kefyros` tree to WSL `/mnt/e`), then copy to the real card. `tools/bake_icons.py` builds the icon PNGs. The card physically moves between PC and PicoCalc.
- There are **no unit tests** in this repo — verification is flash-to-hardware. STATUS.md tracks what has been confirmed on real HW vs. only "builds".

### Debugging crashes
A fault drops to an amber **panic screen** showing PC + stack. Resolve addresses with `arm-none-eabi-addr2line -e build/<snapshot>.elf 0x<PC>`. Per-flash snapshot ELFs are kept (e.g. `build/kefyros-wifi.elf`) — match the ELF to the firmware that crashed. Check heap headroom with `arm-none-eabi-size` and `nm | grep __HeapLimit/__end__` (capacity = `__HeapLimit` − `__end__`). Most historical crashes were **OOM**, not logic bugs.

## Architecture

### Two-core display pipeline (the central design constraint)
- **Core 0** runs everything: LVGL, the superloop, all app logic. Its LVGL `flush_cb` does **not** touch SPI — it hands the dirty rect + pixel buffer to **Core 1** through a single-slot mailbox and returns immediately.
- **Core 1** (`disp_core1_main`, launched in `main.c` *after* `disp_init`) spins on that mailbox and does the actual ILI9488 SPI+DMA blit, then signals `flush_ready`. LVGL won't call `flush_cb` again until then.
- Anything that changes `clk_sys` (the dynamic clock switch) must first `disp_pause_core1()` so no blit is in flight while peripheral clocks shift, then `disp_resume_core1()`.

### Boot order is load-bearing (`src/main.c`)
`clock_init()` (sets final clk_sys so every peripheral is configured against it) → `lv_init()` → `disp_init()` → launch Core 1 → mount SD (non-fatal) → keyboard → audio → PSRAM → indev → tick → splash → wallpaper decoder → launcher → topbar. CYW43 WiFi is **not** brought up at boot — see clock tradeoff below.

### Superloop
`main()` ends in an infinite loop: `uart_poll()` (keyboard) → per-app raw-key pumps (`calc_poll`/`electronics_poll`/`editor_poll`/`music_poll`/`browser_poll`, each a no-op unless that app has grabbed input) → `kf_net_poll()` → `lv_timer_handler()` → `sleep_ms(2)`. **New apps that need raw keys or background work add a `*_poll()` here** and a matching grab.

### HAL / public API
`kefyros.h` is the single framework header — every `port/*` module exposes its API there (display, keyboard/STM32 link, indev, SD storage, PSRAM, dynamic clock, audio, WiFi/net, fault, sys, launcher/app framework). Read it first; it documents the contracts and which `.c` implements each.

### Directory map
- `port/` — hardware abstraction (pico-sdk based): `disp.c` (Core-1 display pump), `kbd_uart.c` (STM32 southbridge over uart1, A5/CRC8 register protocol), `indev.c` (LVGL keypad), `storage_sd.c` (pico-vfs POSIX FS over FatFs/SD), `psram.c`+`psram.pio` (8 MB ESP-PSRAM64H via PIO software block store — **not** memory-mapped), `audio.c` (PWM DAC + DMA), `clock.c` (dynamic clk_sys/voltage), `net.c`+`http.c`+`html.c` (CYW43 + lwIP poll-mode networking and the browser engine), `fault.c`, `sys.c`. `board.h` holds all pin/clock constants.
- `apps/` — one file per app; `calc*.c` is a multi-file subsystem (eval/parse/solve/symbolic/graph/table). Globbed into the build by CMake — just drop a `.c` in.
- `ui/` — `launcher.c` (icon grid + app framework), `topbar.c` (persistent OS bar), `theme.c` (amber theme), `deskconf.c` (flat key=value config persisted to `KF_CONFIG` on SD), fonts, boot splash, power menu. Also globbed.
- `lib/` — vendored: `lvgl` (9.2, compiled from source glob), `pico-vfs`, `dr_flac`, `font8x8`.
- `tools/` — `stage_sd.py`, `bake_icons.py` (host-side SD staging, Python).

### App framework
Apps register in the `dapps[]` table in `ui/launcher.c` (id + label + `app_*_open()` + default slot). The `id` doubles as the SD icon filename (`/kefyros/icons/<id>.png`). Each app builds its own LVGL screen and input group; the **persistent topbar** lives on `lv_layer_top()` and shows on every screen, so app content must inset below it — call `kf_inset_top(scr)` (content area is `LCD_W` × `KF_CONTENT_H` at `y = KF_TOPBAR_H`). Return to launcher via `kf_back_to_launcher()`; the launcher does central screen+group cleanup (async-delete) to avoid leaks.

### Memory model
RP2350 SRAM heap is tight (~284 KB ceiling with WiFi linked). Large transient buffers (e.g. the 185 KB graph canvas) use `malloc`/`free` and rely on a contiguous block being free — `LV_CACHE_DEF_SIZE` is kept small (16 KB) so icons re-decode rather than pin memory. **PSRAM is a separate software block store** (`kf_psram_read/write/alloc`), used for wallpaper pixels (streamed via a custom LVGL image decoder) and browser page bodies — you cannot dereference PSRAM addresses as pointers. Keep big app buffers `static` (off-stack); `PICO_STACK_SIZE` is bumped to 4 KB because LVGL's deep render chain overflowed the 2 KB default.

## Gotchas that cost real time (from STATUS.md)
- `PICO_PLATFORM=rp2350` and `PICO_BOARD=pico2_w` **must** be set before the SDK import in `CMakeLists.txt`, or the uf2 gets an RP2040 family id the bootrom rejects.
- **WiFi vs. overclock tradeoff:** the CYW43 radio's PIO-SPI bus won't reliably associate above ~270 MHz, but the UI runs at 400 MHz for speed. CYW43 is brought up only after dropping to a WiFi-safe clock (`kf_clock_eco()`, 250 MHz). `CYW43_PIO_CLOCK_DIV_DYNAMIC=1` lets `net.c` pick the bus divider for whatever clock is current. This area is still being tuned — defer to STATUS.md.
- The wallpaper LVGL decoder needs `wp_dsc.data` non-NULL (a dummy pointer) or LVGL rejects the image before the decoder runs (→ width 0 → div-by-zero in STRETCH).
- `port/_ref_*.c` and `kefyros_linux_ref.h` are reference/demo files from the original Linux port — not compiled into the firmware (only the files explicitly listed in `CMakeLists.txt` plus the `apps/`+`ui/` globs build).
