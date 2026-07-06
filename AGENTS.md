# AGENTS.md — Kefyros PicoCalc firmware (RP2350, LVGL 9.2)

See `STATUS.md` for the running engineering log (more current than this file on
in-progress work). This file covers what an agent would otherwise get wrong.

## Build

```bash
# Inside WSL Ubuntu (NOT Git-Bash — the shell must be WSL):
bash build.sh          # -> build/kefyros.uf2  (CMake + Ninja, Release)

# From Windows / non-WSL:
wsl.exe -d Ubuntu -- bash -lc 'bash /home/legitbox/kefyros-pico/build.sh'
```

- pico-sdk at `/home/legitbox/pico-sdk` (hardcoded in `build.sh`).
- **No unit tests.** Verification is build success only; run-to-hardware.

## Critical CMake gotcha

`PICO_PLATFORM=rp2350` and `PICO_BOARD=pico2_w` MUST be set before `include(pico_sdk_import.cmake)`
in `CMakeLists.txt`. Otherwise the UF2 gets an RP2040 family ID that the RP2350 bootrom rejects.

## Code that is NOT compiled

- `port/_ref_*.c` and `kefyros_linux_ref.h` — reference/demo files from the original Linux port.
- `apps/` and `ui/` are globbed into the build automatically. `port/` files are listed explicitly.

## Architecture constraints an agent must not break

1. **Two-core display:** Core 0's LVGL flush_cb hands the dirty rect to Core 1 through a mailbox.
   Core 1 (`disp_core1_main`) is launched *after* `disp_init()`. Before changing `clk_sys`,
   call `disp_pause_core1()` / `disp_resume_core1()` so no SPI+DMA blit is in flight.

2. **Boot order** in `src/main.c:29-55` is load-bearing: clock_init → lv_init → disp_init →
   launch Core 1 → mount SD → keyboard → audio → PSRAM → indev → tick → splash →
   wallpaper decoder → launcher → topbar.

3. **Superloop** at `src/main.c:77`: add a `*_poll()` call for any new app that needs raw keys
   or background work.

4. **WiFi cannot associate above ~270 MHz.** The UI runs at 400 MHz, so `kf_clock_eco()` (250 MHz)
   must wrap any CYW43 radio bring-up. `CYW43_PIO_CLOCK_DIV_DYNAMIC=1` lets the bus divider
   adapt at runtime.

5. **PSRAM is NOT memory-mapped.** Use `kf_psram_read/write/alloc`; never dereference a PSRAM
   address as a pointer.

6. **Stack is only 4 KB** (`PICO_STACK_SIZE=0x1000`). Large buffers go `static` (off-stack).

7. **Malloc returns NULL** on failure (`PICO_MALLOC_PANIC=0`) — always check, don't rely on
   SDK default panic.

## App framework conventions

- Register in `dapps[]` table in `ui/launcher.c` (id + label + `app_*_open()` + default slot).
- The id doubles as the SD icon path: `/kefyros/icons/<id>.png`.
- Content area is `LCD_W` × `KF_CONTENT_H` at `y = KF_TOPBAR_H`. Call `kf_inset_top(scr)` to
  push app content below the persistent topbar.
- Return to launcher via `kf_back_to_launcher()` — the launcher handles central screen+group cleanup.

## LVGL quirks

- `LV_CONF_INCLUDE_SIMPLE` is set; `lv_conf.h` lives at repo root.
- `LV_CACHE_DEF_SIZE` is kept small (16 KB) so large transient mallocs (185 KB graph canvas)
  can find contiguous blocks.
- The wallpaper decoder needs `wp_dsc.data` set to a dummy non-NULL pointer, or LVGL rejects
  the image before the decoder runs (→ width 0 → div-by-zero in STRETCH).

## Crash debugging

```bash
arm-none-eabi-addr2line -e build/<snapshot>.elf 0x<PC>
```

Match the ELF to the firmware snapshot that crashed. Most historical crashes were OOM, not
logic bugs — check heap headroom with `arm-none-eabi-size` and `nm | grep __HeapLimit/__end__`
(capacity = `__HeapLimit` − `__end__`).
