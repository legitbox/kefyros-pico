# AGENTS.md — Kefyros PicoCalc firmware (RP2350, LVGL 9.2)

See `STATUS.md` for the running engineering log (more current than this file on
in-progress work). This file covers what an agent would otherwise get wrong.

## Repository location

- Canonical working tree: `C:\Users\Matas\Documents\GitHub\kefyros-pico`.
- WSL sees the same files at `/mnt/c/Users/Matas/Documents/GitHub/kefyros-pico`.
- `/home/legitbox/kefyros-pico` is a compatibility symlink to that Windows tree. Keep it: existing
  build commands and a few helper scripts intentionally use the short path.
- The Windows checkout has repo-local `core.filemode=false`, because NTFS does not preserve Linux
  executable-bit reporting. File contents and Git history remain authoritative.

## Build

```bash
# Inside WSL Ubuntu (NOT Git-Bash — the shell must be WSL). The short path is a symlink:
cd /home/legitbox/kefyros-pico
bash build.sh pimoroni  # Pimoroni Pico Plus 2 W — the actual hardware (default: pico2w)
bash build.sh pico2w    # stock Pico 2 W target

# From Windows / non-WSL:
wsl.exe -d Ubuntu -- bash -lc 'bash /home/legitbox/kefyros-pico/build.sh pimoroni'
```

- pico-sdk at `/home/legitbox/pico-sdk` (hardcoded in `build.sh`). Outputs go to
  `build-pimoroni/kefyros.uf2` / `build-pico2w/kefyros.uf2`; they are regenerated on demand.
- **No unit tests.** Verification is build success only; run-to-hardware.
- `wsl.exe` boundary strips `$VARS`; plain Bash tool on Windows = Git-Bash, not WSL.
- Flashing: find the removable volume labelled `RP2350` (drive letter is dynamic) and copy the UF2.

## CMake gotchas (`CMakeLists.txt`)

- `PICO_PLATFORM=rp2350` and `PICO_BOARD` MUST be set before `include(pico_sdk_import.cmake)`.
  Otherwise the UF2 gets an RP2040 family ID that the RP2350 bootrom rejects.
- `PICO_BOARD` selects the PSRAM backend (`CMakeLists.txt:33`): `port/psram_qmi.c` for Pimoroni
  (QMI window 1), `port/psram.c` (GPIO PIO) otherwise.
- The KAPI arena linker script is generated at build time from the SDK's `memmap_default.ld`
  (`CMakeLists.txt:78-94`): heap is shortened to 464 KB so `[0x20074000, 0x20080000)` stays free
  for SD-loaded apps. The build **refuses (FATAL_ERROR)** if the SDK memmap drifts — that is
  deliberate, not a bug. Keep the arena at 48 KiB: 96/64 KiB starved the built-ins' heap
  (missing launcher icons, Spineko's "..." low-memory page, empty Music library, calc 3D OOM).
- New files: `apps/` and `ui/` are globbed into the build automatically, but every `port/*.c`
  must be listed explicitly in the `add_executable` block.
- `PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1`: clk_peri follows clk_sys, so SPI/UART bauds,
  PSRAM PIO timing, and the cyw43 bus must all be re-derived after any clock change
  (`reclock_peripherals()` in `port/clock.c`).

## Code that is NOT compiled

- `port/_ref_*.c`, `kefyros_linux_ref.h` — reference/demo files from the original Linux port.
- `archive/` — whole directory of dead code; never copy from it as if it were live.
- Vendored libs under `lib/` (lvgl 9.2, bearssl, monocypher, miniz, peanut-gbc, nanosvg, pico-vfs):
  don't edit them.

## Architecture constraints an agent must not break

1. **Two-core display:** Core 0's LVGL flush_cb hands the dirty rect to Core 1 through a mailbox;
   Core 1 (`disp_core1_main`) is launched *after* `disp_init()`. Before changing `clk_sys`,
   call `disp_pause_core1()` / `disp_resume_core1()` so no SPI+DMA blit is in flight. For a flash
   write (GB ROM load), `disp_core1_reset()` / `disp_core1_relaunch()` halt and restart Core 1.

2. **Clock tiers** (`port/clock.c` / `port/board.h`): sleep 150 MHz @1.10 V · eco 250 MHz @1.20 V ·
   **normal 400 MHz @1.30 V (the default UI clock)** · boost 420 MHz @1.35 V. Boot is **cold at
   250 MHz**, then `kf_clock_normal()` warm-ramps AFTER the UI is up (booting cold at 400 was an
   intermittent marginal-XIP/PSRAM lottery). `kf_clock_*()` calls already handle the Core 1 pause
   handshake; `kf_clock_set_bare()` does not — only use it when Core 1 is already parked.

3. **WiFi ceiling ~270 MHz:** the CYW43 radio won't associate above that. Any radio touch
   (init, scan, join, TLS handshake) must happen under `kf_clock_eco()`; once joined, apps return
   to `kf_clock_normal()` — the link rides 400 MHz because `kf_net_reclock()` retunes the live
   cyw43 PIO bus to a fixed 31.25 MHz (`CYW43_PIO_CLOCK_DIV_DYNAMIC=1`). cyw43 is brought up
   lazily by apps (`kf_net_init`), never at boot.

4. **Boot order** in `src/main.c:29-63` is load-bearing: clock_init (250 MHz) → kf_fault_init →
   lv_init → disp_init → kf_theme_init → launch Core 1 → keyboard → audio → PSRAM → indev →
   tick → kf_sd_gate (non-fatal; PDA boots without SD) → wallpaper decoder → launcher → topbar →
   kf_clock_normal → boot sfx.

5. **Superloop** at `src/main.c:64-79`: add a `*_poll()` call for any new app that needs raw keys
   or background work; `kf_net_poll()` and `lv_timer_handler()` come last.

6. **PSRAM is never dereferenceable** on either board: stock PicoCalc uses a PIO 1-bit SPI block
   store, Pimoroni is memory-mapped QMI but the code is deliberately block-only —
   `kf_psram_map()` returns NULL on purpose. Use `kf_psram_read/write/alloc` (or the GB emulator's
   fixed 16 KB SRAM bank cache).

7. **KAPI arena:** heap ends at `__HeapLimit = 0x20074000`; `[0x20074000, 0x20080000)` is the
   fixed 48 KiB arena for SD-loaded `.kx` apps. Build them with `tools/build_kapi_*.sh` against
   `sdk/kapi.h` (see `KAPI.md`) — never against a particular `kefyros.elf`.

8. **Stack is only 4 KB** (`PICO_STACK_SIZE=0x1000`). Large buffers and the KAPI manifest parser
   go `static` (off-stack).

9. **Malloc returns NULL** on failure (`PICO_MALLOC_PANIC=0`) — always check, don't rely on
   SDK default panic.

## App framework conventions

- Built-ins register in `builtin_apps[]` in `ui/launcher.c` (id + label + `app_*_open()` +
  default slot + flags). Mark realtime apps `APP_NO_SLEEP` (calc, music, gb) so idle never
  downclocks them to the 150 MHz sleep tier.
- The id doubles as the SD icon path: `/kefyros/icons/<id>.png`.
- Content area is `LCD_W` × `KF_CONTENT_H` at `y = KF_TOPBAR_H`. Call `kf_inset_top(scr)` to
  push app content below the persistent topbar; return via `kf_back_to_launcher()` (the launcher
  handles central screen+group cleanup).
- SD-hosted apps: launcher discovers flat ABI-1 manifests at `/apps/<dir>/manifest.json`
  (id/label/icon/executable/`perf`/`idle`), then `kapi_run()` loads and launches the `.kx` from
  SD. Perf tier 1=eco, 2=boost.

## LVGL quirks

- `LV_CONF_INCLUDE_SIMPLE` is set; `lv_conf.h` lives at repo root.
- `LV_CACHE_DEF_SIZE` is kept small (16 KB) so large transient mallocs (185 KB graph canvas)
  can find contiguous blocks.
- The wallpaper decoder (`ui/launcher.c`) needs `wp_dsc.data` set to a dummy non-NULL pointer,
  or LVGL rejects the image before the decoder runs (→ width 0 → div-by-zero in STRETCH). It
  also includes `lvgl/src/draw/lv_image_decoder_private.h` directly — an internal header.

## Crash debugging

```bash
arm-none-eabi-addr2line -e build-pimoroni/kefyros.elf 0x<PC>   # or build-pico2w/
```

Match the ELF to the firmware snapshot that crashed (snapshot ELFs are kept per flash).
Most historical crashes were OOM, not logic bugs — check heap headroom with
`arm-none-eabi-size` and `nm | grep __HeapLimit/__end__`
(capacity = `__HeapLimit` 0x20074000 − `__end__`).
