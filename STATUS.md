# Kefyros on PicoCalc / Pico 2 W (RP2350) — STATUS & PLAN

Bare-metal RP2350 LVGL PDA in a PicoCalc. Amber CRT theme. Code-only firmware; all art/sounds/notes
live on the SD card at `/kefyros/...`. Repo (WSL): `/home/legitbox/kefyros-pico`.

## BUILD / FLASH / STAGE
- Build: `wsl.exe -d Ubuntu -- bash -lc 'bash /home/legitbox/kefyros-pico/build.sh'` -> `build/kefyros.uf2`.
  pico-sdk at `/home/legitbox/pico-sdk`. Check heap: `arm-none-eabi-size` + `nm | grep __HeapLimit/__end__`
  (capacity = __HeapLimit(0x20080000) - __end__).
- Flash: Pico in BOOTSEL = Windows **F:** -> copy uf2. Re-enter BOOTSEL: POWER->BOOTSEL or power-cycle.
- SD = Windows **E:** (FAT32). Stage with `tools/stage_sd.py` (writes to WSL `/mnt/e` PHANTOM dir) then
  copy `\\wsl.localhost\Ubuntu\mnt\e\kefyros` to real E:. Card physically moves PC<->PicoCalc; mounts at boot.
- `wsl.exe` boundary strips `$VARS` (use literal args / script files). Plain Bash tool = Git-Bash, not WSL.
- Crash debugging: panic screen shows PC + stack; `arm-none-eabi-addr2line -e build/<snapshot>.elf 0x<PC>`.
  Snapshot elfs kept per flash (e.g. `build/kefyros-psram-wallpaper.elf`).

## HARDWARE (from clockwork mainboard v2.0 schematic)
- LCD ILI9488 spi1 (SCK10/MOSI11/MISO12/CS13/DC14/RST15). SD spi0 (SCK18/MOSI19/MISO16/CS17/DET22).
- Keyboard STM32 southbridge uart1 (TX8/RX9, 115200, A5/CRC8 protocol). Debug uart0 GP0/1.
- Audio PWM: PWM_R=GP26, PWM_L=GP27 (slice5), TPS6116x HP amp. (Stereo 12-bit player shipped.)
- **PSRAM 8 MB ESP-PSRAM64H** on its OWN GPIO bus: SIO0=GP2,SIO1=GP3,SIO2=GP4,SIO3=GP5,CS=GP20,SCK=GP21.
  NOT on XIP/QMI -> software block store (PIO), not memory-mapped heap.
- OC 360 MHz @ VREG 1.20 V. `PICO_PLATFORM=rp2350`+`PICO_BOARD=pico2` MUST precede SDK import in CMake.

## WORKING (all on real HW)
Launcher (grid + persistent OS top bar w/ MEM%/clock/battery on lv_layer_top), calc (+electronics/editor/
files/settings/wallpaper/music), wifi stub, fault/panic screen (`:3`), MP3 player (minimp3, PWM+DMA stereo
12-bit). Apps inset below the top bar via `kf_inset_top` (KF_CONTENT_H=296). Central screen+group cleanup
in `launcher_show` (async-delete) killed the leak crashes.

## PHASE 0 — PSRAM + CRASH FIX = DONE (heap ~145 KB -> ~338 KB)
Lingering crashes were OOM (heap ceiling too low, dominated by a 200 KB static `kf_scratch`). Fix:
- `port/psram.c` + `port/psram.pio` + board.h pins: ESP-PSRAM64H driver, PIO **1-bit SPI** @ ~18 MHz
  (reset 0x66/0x99, read 0x03, write 0x02, chunk at 1 KB pages). API `kf_psram_init/read/write/alloc`.
  CMake: `pico_generate_pio_header(... port/psram.pio)` + `hardware_pio`. VALIDATED (Settings: "PSRAM 8MB OK").
- Wallpaper -> PSRAM via a custom **streaming LVGL decoder** in `ui/launcher.c` (`wp_dec_info/open/get_area/
  close`, registered by `kf_wallpaper_init()` from main). Needs `lvgl/src/draw/lv_image_decoder_private.h`.
  GOTCHA: `wp_dsc.data` must be NON-NULL (`wp_data_dummy`) or LVGL rejects it pre-decoder -> img->w=0 ->
  STRETCH div-by-0. Pixels stream from PSRAM via get_area (1 row at a time).
- `kf_scratch` DELETED. 2D/3D graphers (`apps/calc_graph*.c`) back to malloc/free (185 KB transient).
- `LV_CACHE_DEF_SIZE` 64->16 KB so the 185 KB graph malloc finds a contiguous block (icons re-decode on demand).
- **CONFIRMED:** boots w/ SD, wallpaper renders, all old crashers survive. MEM% ~9% launcher, ~66% transient
  while graphing. WATCH: wallpaper streams at 1-bit SPI; if cursor anim lags, upgrade psram.c to quad SPI.

## WiFi vs OVERCLOCK — the hard tradeoff (research 2026-06-19)
The CYW43 radio talks over a PIO-SPI bus whose reliability degrades with clk_sys. Findings:
- **Community ceiling ~260-270 MHz.** MicroPython #16799: "270 MHz is the upper limit the wifi can handle."
  arduino-pico #2590: highest confirmed working = 260 MHz. Everyone only tuned the *divider* (bit rate).
- Our HW: at 360 MHz scan + bring-up work, hotspot even reached AUTH (PM off), but association is unreliable;
  at 400 MHz association fails (JOIN->FAIL on every AP/auth-mode). Consistent with the ceiling.
- **We run 400 MHz** (board.h KF_SYS_KHZ) for UI speed -> WiFi can't associate there. Two real options:
  (A) run <=250-270 MHz globally (WiFi works, still 1.6x OC) — SIMPLE/RELIABLE; or
  (B) dynamic clock switch: 400 MHz UI, drop to <=250 for WiFi connect + browser transfers, clock back up —
      what the user originally wanted; COMPLEX (must pause core1 display DMA, re-derive every clk_peri/PIO/PWM
      divider + QMI flash timing on each switch, use CYW43_PIO_CLOCK_DIV_DYNAMIC). Needs HW iteration.
- **UNTRIED LEVER (now in the build, EXPERIMENT):** `CYW43_SPI_PROGRAM_NAME=spi_gap010_sample1` shifts the
  MISO *sample point* later (2 extra cycles) — the SDK ships 4 sample-timing variants; the community only ever
  changed the divider, never this. It targets our exact symptom (rare sample-edge bit errors: scan ok, 4-way
  handshake fails). Long shot at 400 MHz (no proven WiFi >270 anywhere) but the one thing left to try cheaply.
  Current build = 400 MHz + INT=8 + gap010_sample1. Flash + test WiFi; only cyw43 is affected, rest unchanged.
  If it doesn't associate -> 400 MHz WiFi is a dead end; pick option A or B above.
- NOTE: the earlier "250 MHz breaks SD/wallpaper/wifi" test was CONFOUNDED by the files.c double-free bug
  (now fixed) — a clean 250 MHz test hasn't been done. QMI flash retune (qmi_set_flash_div, rxdelay hardcoded
  =1 for 360) likely also needs per-clock tuning before any sub-360 clock is trusted.

## PHASE A — WiFi = CODE-COMPLETE, BUILDS (pending HW verify)  [snapshot: build/kefyros-wifi.elf]
Board now `pico2_w`; links `pico_cyw43_arch_lwip_poll` + `pico_lwip_mbedtls` + `pico_mbedtls`. Heap ceiling
~338 KB -> **~284 KB** (lwIP pools + cyw43 + mbedtls BSS ~54 KB); still well above the 185 KB graph canvas,
no OOM regression. Flash ~977 KB / 4 MB.
- `port/lwipopts.h` (NO_SYS poll mode, DHCP/DNS/TCP/UDP + ALTCP-TLS) + `port/mbedtls_config.h` (TLS 1.2
  client, no cert verify but X.509 parse kept; `MBEDTLS_HAVE_TIME`+`MBEDTLS_PLATFORM_MS_TIME_ALT` REQUIRED —
  the SDK altcp glue touches `mbedtls_ssl_session.start`/`ssl_context.out_left` raw).
  GOTCHA (cost an hour): do NOT set `PICO_MBEDTLS_CONFIG_FILE=mbedtls_config.h` — on mbedtls v3 that name
  collides with the bundled `mbedtls/mbedtls_config.h` (sibling of build_info.h, quote-include wins) and your
  defines vanish. Leave the SDK default (`pico_mbedtls_config.h`), which `#include "mbedtls_config.h"` ->
  resolves to ours on the `port/` include path.
- `port/net.c` (API in kefyros.h): `kf_net_init` (cyw43 init WORLDWIDE + STA), `kf_net_poll` (cyw43_arch_poll
  — also drives lwIP timeouts in poll mode — + reconnect watchdog w/ 8 s backoff, 30 s on bad-auth),
  `kf_net_connect(ssid,pass)` async + persists creds (deskconf wifi_ssid/wifi_pass), `kf_net_forget`,
  `kf_net_autoconnect` (boot), `kf_net_state`/`_str`/`_ip`/`_ssid`, `kf_net_scan_start(cb)`/`_scan_active`.
  State from `cyw43_tcpip_link_status` (ONLINE only once DHCP gives an IP).
- `apps/wifi.c` (replaced wifi_stub.c): live status header, Rescan/Forget rows + scan list (RSSI + [*]/[o]
  lock), secured AP -> one-line password textarea (ENTER=connect via LV_EVENT_READY, ESC exits app).
- `ui/topbar.c`: "wifi" indicator (green online / dim connecting / red fail / hidden off), RIGHT_MID -52.
- `src/main.c`: `kf_net_init()` after psram, `kf_net_autoconnect()` after launcher/topbar, `kf_net_poll()` in
  superloop. cyw43 PIO coexists w/ PSRAM (claims a free SM dynamically). cyw43 pins GP23/24/25/29 (no clash).
- **VERIFY ON HW (next):** flash kefyros.uf2; open WiFi; scan list populates; connect to AP (password);
  topbar green + IP shows; reboot -> auto-reconnect. Then lock Phase A.

## NEXT — PHASE B: Spineko browser (HTML-only; HTTP+HTTPS no-verify; links + GET forms; page -> PSRAM)
`port/http.c` (raw lwIP TCP + altcp_tls no-verify, redirects, chunked, body -> PSRAM via kf_psram_*),
`port/html.c` + `apps/spineko.c` (lenient tokenizer, drop script/style, links as focusable buttons in a
group + Back history, GET forms, [img:alt] placeholders, amber style), slot 9, icon spineko.png (Downloads).
