# Kefyros on PicoCalc / Pico 2 W (RP2350) — STATUS & PLAN

Bare-metal RP2350 LVGL PDA in a PicoCalc. Amber CRT theme. Code-only firmware; all art/sounds/notes
live on the SD card at `/kefyros/...`.

Canonical repo: `C:\Users\Matas\Documents\GitHub\kefyros-pico`. WSL accesses the same NTFS working
tree through `/home/legitbox/kefyros-pico`, a compatibility symlink to
`/mnt/c/Users/Matas/Documents/GitHub/kefyros-pico`. Generated Linux build directories and the SSH-test
virtualenv were discarded during the 2026-08-04 migration and must be regenerated when needed.

## BUILD / FLASH / STAGE
- Pimoroni build from Windows:
  `wsl.exe -d Ubuntu -- bash -lc 'bash /home/legitbox/kefyros-pico/build.sh pimoroni'`
  -> `build-pimoroni/kefyros.uf2`. Use `pico2w` for the stock board target. The first post-migration build
  recreates its target directory inside the Windows checkout.
  pico-sdk at `/home/legitbox/pico-sdk`. Check heap: `arm-none-eabi-size` + `nm | grep __HeapLimit/__end__`
  (capacity = __HeapLimit(0x20080000) - __end__).
- Flash: locate the removable volume labelled `RP2350` rather than assuming a drive letter, then copy the UF2.
  Re-enter BOOTSEL with POWER->BOOTSEL or a power-cycle.
- SD drive letters are likewise dynamic. `tools/stage_sd.py` currently stages through WSL `/mnt/e`; confirm the
  Windows SD letter before use. The card physically moves PC<->PicoCalc and mounts at boot.
- `wsl.exe` boundary strips `$VARS` (use literal args / script files). Plain Bash tool = Git-Bash, not WSL.
- Crash debugging: panic screen shows PC + stack; `arm-none-eabi-addr2line -e build/<snapshot>.elf 0x<PC>`.
  Snapshot elfs kept per flash (e.g. `build/kefyros-psram-wallpaper.elf`).

## HARDWARE (from clockwork mainboard v2.0 schematic)
- LCD ILI9488 spi1 (SCK10/MOSI11/MISO12/CS13/DC14/RST15). SD spi0 (SCK18/MOSI19/MISO16/CS17/DET22).
- Keyboard STM32 southbridge uart1 (TX8/RX9, 115200, A5/CRC8 protocol). Debug uart0 GP0/1.
- Audio PWM: PWM_R=GP26, PWM_L=GP27 (slice5), TPS6116x HP amp. (Stereo 12-bit player shipped.)
- **PSRAM 8 MB ESP-PSRAM64H** on its OWN GPIO bus: SIO0=GP2,SIO1=GP3,SIO2=GP4,SIO3=GP5,CS=GP20,SCK=GP21.
  NOT on XIP/QMI -> software block store (PIO), not memory-mapped heap.
- Clock tiers are 250 MHz cold/WiFi join, 400 MHz normal at 1.30 V, and 420 MHz boost at 1.35 V.
  `PICO_PLATFORM=rp2350` plus the selected board must precede SDK import in CMake.

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

## SPINEKO WAVE 3 — "ALIEXPRESS NETSURF" (2026-08-03, flashed)
- Arrow keys now drive a real outlined mouse pointer. ENTER clicks the object beneath it; TAB still jumps
  between controls. Pressing farther against a document edge scrolls in that direction, including sideways.
- Image decode/cache upgraded from opaque RGB565 to streaming RGB565+A8. PNG, SVG and GIF first-frame alpha
  survives; cache version moved to `rgb-v2` so old white-backed files cannot be reused. Images are decoded and
  written once per URL, then streamed from SD scanline-by-scanline instead of occupying SRAM.
- `@font-face` supports two page faces. TTF/OTF files (<=512 KB) download once to `font-v1`, are validated and
  atomically renamed, then LVGL TinyTTF reads the font from SD with bounded 8-glyph caches at 14/20 px. No SD
  swap or repeated page writes. Verpitek's IBM Plex Mono (134 KB) and Model 3x (29 KB) fit comfortably.
- Normal `*.wikipedia.org/wiki/<article>` navigation is internally fetched through MediaWiki's lean REST HTML
  endpoint while retaining the normal address/history/base URL. This avoids rendering the huge desktop shell.
- `https://verpitek.com/` added to the built-in bookmarks/test sites.
- Pimoroni and generic Pico 2 W builds pass. Pimoroni artifact: 1,467,504 text, 252,668 BSS; UF2 SHA-256
  `338edaad70c1b84fb851faf44ddc1cabd900ad12f26889601a45608520b42d9d`.

## SPINEKO WAVE 3.1 — RECOVERY + LOAD TELEMETRY (2026-08-04)
- Cursor is now a solid white RGB565+A8 classic arrow with a black outline. Its upper-left tip is the
  click hot spot; TAB warps that tip to the target centre.
- F9 hard-stops the current page/CSS/font/image request without rebooting. F10 aborts first, releases open
  image/font users, then clears only Spineko's CSS/font/render/raw caches; F2 retries the retained URL.
- Status bar identifies Page/Style/Font/Image phases with KB, live MHz, fallback state, and stop hint.
  Network-fetched images get a separate visible decode phase; completion reports successful/skipped images.
- Known unsupported image containers (WebP/AVIF/BMP/TIFF/ICO/SVGZ) are skipped before network fetch. This
  fixes Verpitek Crow Bleep: its three WebPs are 351/887/396 KB and previously consumed minutes even though
  no WebP decoder existed. They now become immediate `[img unsupported format]` placeholders.
- HTTP rejects known uncompressed bodies larger than the active PSRAM arena at header time, and rejects
  gzip/chunked overflow at the first excess byte instead of silently truncating/cache-poisoning the body.

## GAME BOY / COLOR — EXACT-2X SPARSE PANEL RENDERER (2026-08-04)
- Current MIT pico-peanutGB core, compiled with full GBC/CGB double-speed support and minigb_apu audio.
  ROM picker scans `/kefyros/roms/gb` plus `/kefyros/roms`; saves live under `/kefyros/saves/gb`.
- ROMs load once into PSRAM as a block store. Both Pimoroni QMI PSRAM and generic PicoCalc PIO PSRAM use a
  fixed + switchable 16 KB SRAM bank cache; the emulator never dereferences PSRAM. Long-lived desktop/browser/
  music/chat/wallpaper PSRAM offsets are invalidated around the emulator's exclusive allocator epoch.
- Display is always exact 2x: 160x144 -> 320x288, vertically centred. The panel is cleared white once and its
  GRAM retains the image. A 46 KB RGB565 source shadow detects changes; changed runs become doubled 2-row
  rectangles, while busy lines collapse to one 320x2 write. No full-size framebuffer or SD swap exists.
- Controls: arrows D-pad, F5 A, F4 B, ENTER Start, BACKSPACE Select, ESC/BREAK save+quit. Audio-ring pacing
  holds the native ~59.7 Hz rate. RTC carts are seeded from Kefyros local time on launch.
- Host smoke test ran 120 frames each of Tetris, Pokemon Red, Link's Awakening and Wario Land without a core
  error. Both Pimoroni Pico Plus 2 W and generic Pico 2 W firmware targets build and link.

### Hardware-test hotfix 2: block-only PSRAM + silent audio
- The first cache-maintenance experiment regressed hardware: Zelda reached the white panel clear and then
  froze, and warm reboot again lost PSRAM. It has been removed. The speculative 0x66/0x99 reset and fixed-size
  fallback are also gone; init again follows Pimoroni MicroPython's proven exit-QPI -> RDID -> enter-QPI flow.
- Before QMI takes GP47, firmware explicitly drives PSRAM CS high for 10 us. This gives CPU-only warm resets a
  clean transaction boundary even when the separately-powered PSRAM was left selected by a crash.
- All Kefyros PSRAM access is now block-only through the RP2350 uncached alias. `kf_psram_map()` deliberately
  returns NULL on the Pimoroni too. GB uses fixed + switchable 16 KB SRAM ROM banks and heap cart RAM, removing
  cached PSRAM pointers and dirty lines from the reset/crash equation entirely.
- The banked frontend ran 300 frames each of Zelda, Pokemon Red, Wario Land and Tetris. Zelda's worst observed
  traffic was 1,649 bank loads / 300 frames (~5.4 MB/s), below the measured ~13.5 MB/s QMI bandwidth.
- The emulator audio ring is 4096 stereo frames (16 KB) instead of the system default 8192 (32 KB). Failure
  is reported on-screen instead of silently launching without sound. Host APU test produced nonzero Wario
  Land audio (peak 7154, 32,880 active samples across 300 frames).
- ROM launch errors now distinguish PSRAM offline, ROM larger than detected PSRAM, and allocator exhaustion,
  including ROM size and allocator position. This makes any remaining hardware failure directly actionable.
- Hotfix-2 Pimoroni UF2 SHA-256: `fe7923ce8173cbacb841a8dd5afb54712e2a5fea6ae871857e3f7170cea0b2bf`.

## CALCULATOR SLEEP GUARD + KAPI SD APP FOUNDATION (2026-08-04, baseline superseded below)
- Calculator is now `KEEP_CLOCK`: screen timeout still turns off both backlights, but Calculator no longer
  downclocks to the hardware-problematic 150 MHz tier and therefore does not attempt a 150->400/420 MHz wake.
  Hardware verify: idle on home, 2D graph and 3D graph screens, then wake with a key; confirm clean input/render.
- The picoTracker synth test tile, superloop pump, C++ engine sources and build wiring were removed. Game Boy
  takes slot 13. Music and Game Boy share none of the deleted Tracker code.
- Launcher now discovers flat ABI-1 manifests at `/apps/<dir>/manifest.json`, validates paths/ABI/executable,
  loads an optional app-local icon, applies manifest `perf`/`idle`, and launches the `.kx` directly from SD.
  A failed load is reported on the desktop instead of silently leaving it.
- KAPI ABI 1 now reserves a stable 64 KiB executable arena at `0x20070000..0x20080000`; the firmware heap ends
  at `0x20070000`. App builds no longer inspect a particular `kefyros.elf`, so compatible `.kx` bundles can be
  rebuilt/replaced on SD without reflashing. The loader also checks image CRC before executing.
- Concrete runtime parity fixes: exclusive apps actively drain keyboard UART; `sys->pump()` services UART+net;
  manifest/runtime `keep_clock` and `keep_awake` work; audio start reports OOM; brightness and UTC are real.
- **Parity gate:** KAPI is not yet allowed to replace a built-in solely because it launches. Managed UI,
  sockets/TLS, image decode, HTTP/doc, view/glyph and remaining sysinfo services must be real, then the full
  Calculator (all screens/2D/3D/performance) must match the built-in on hardware. The small calc `.kx` REPL is
  only a toolchain test and remains explicitly non-replacement.
- Pimoroni build passes. Link map: `g_kapi_arena=0x20070000`, `g_kapi_arena_end=0x20080000`,
  `__end__=0x2004d068`, `__HeapLimit=0x20070000` (143,256 bytes raw heap span). Artifact 2,928,128 bytes;
  SHA-256 `8020e55547a05dd01a56390b257fbb0056f7b914453d63c23282c449d7b6d95b`.
- Standalone SDK verification passes without a kernel build: `hello.kx` base `0x20070000`, 448 bytes total;
  calc compute/REPL `.kx` footprint 22,509 bytes including BSS (20,452-byte bundle), within the 64 KiB arena.

## KAPI ABI 1 MINOR 2 — FULL APP SERVICE SURFACE (2026-08-04)
- Replaced the remaining advertised Layer-0 no-ops: canvas clipping, windowed direct-panel views,
  exclusive panel lease arbitration, glyph alpha extraction, cooperative sleeps and asynchronous tone playback.
- KAPI PSRAM is app-scoped and handle-based. Arbitrary handle release works, teardown rewinds the app's PSRAM
  allocation mark, and `lock/unlock` now supplies a writable SRAM mirror with PSRAM write-back instead of always
  returning `NULL`.
- Added working nonblocking DNS/TCP/UDP sockets, BearSSL wrapping, lazy WiFi-safe radio bring-up, real RSSI/IP,
  a single-request asynchronous HTTP(S) service with custom POST headers/status/error/final URL, and bounded
  PSRAM-backed image decode/composite/BMP encode.
- Layer 1 is no longer decorative: LVGL-backed labels/buttons/lists/textareas/checkboxes/sliders now support
  lifecycle, geometry, align/flex/grow, visibility, enabled state, values, focus, change callbacks, colors and fonts.
  The document service is non-NULL and renders bounded plain text/Markdown/HTML into a canvas.
- Added the minor-2 `k_device` table so Settings/WiFi/Wallpaper-class apps can also move to SD: persisted desktop
  config, LCD/keyboard backlight, WiFi scan/join/forget, system sounds and power actions stay mediated by the kernel.
- Added a high-level nonblocking `k_ssh` session service over the existing SSH-2/Monocypher core, including
  host-key approval, password/ed25519 auth, channel data, terminal resize, state/error and deterministic teardown.
  This removes the roughly 70 KiB duplicate SSH+crypto burden from an SD-hosted Term binary.
- Fixed KAPI directory `is_dir` checks to use the directory actually opened; added socket length bounds, request/
  socket/UI/canvas/audio/panel teardown, CRC validation and explicit allocation failure paths.
- The stable app arena is now 96 KiB at `0x20068000..0x20080000`. This was required by measured built-in object
  footprints (full Calculator roughly 83 KiB, Music roughly 81 KiB). The linker leaves `__HeapLimit=0x20068000`;
  current `__end__=0x2004f514`, giving 101,100 bytes of raw kernel heap while an SD app is resident.
- Pimoroni firmware build passes: UF2 2,960,896 bytes, SHA-256
  `7a765ce3633ca71e38d933021a2380a4e83d5a0c17280b9297cd0353c2ab3308`.
  Rebuilt SDK artifacts all link at `0x20068000`: hello 473-byte bundle, demo 2,244 bytes, smoke 3,493 bytes,
  and calc REPL 20,452 bytes (22,509-byte text/data/BSS footprint).
- This is not a claim of hardware parity yet. Required next verification is on-device service smoke testing,
  then porting the complete Calculator and comparing every screen, 2D/3D rendering, sleep/wake and performance
  against the built-in before removing its flashed copy. Flash-scratch remains capability-disabled; TLS keeps the
  existing kernel trust model.

## KAPI ARENA 96 -> 64 KiB � HEAP-STARVATION FIX (2026-08-05)
- On-device symptoms (Pimoroni Pico Plus 2 W): launcher icons missing at boot until the cursor
  redraws their cell; Spineko's about:start rendered only the low-memory `...` placeholder;
  Music showed garbled text and "no .flac under /kefyros/music" with the card inserted.
- Root cause: the 2026-08-04 arena bump to 96 KiB at `0x20068000` cut the kernel heap to
  101,100 bytes. At boot the renderer peaks at ~65% (LVGL base + SD app manifests + icon PNG
  decodes); icon decode malloc fails at the peak and succeeds on later redraws (the
  "appears when the cursor passes over it" behavior). Spineko's 48 KB `HEAP_FLOOR` is then
  unmeetable -> `low_mem` -> the `...` label; images additionally demand `FLOOR+48 KB`
  free and never decode. Music's jp font (35 KB) + cover scratch (28.8 KB) + UI hit the
  ceiling so `opendir()`'s malloc fails -> empty library, font fallback garbles JP text.
  PSRAM itself was exonerated (psram_check passed; wallpaper streamed fine).
- Fix: revert to ABI-1's original 64 KiB arena at `0x20070000..0x20080000`
  (`CMakeLists.txt` memmap 448 KB RAM, `port/kapi.c KAPI_ARENA_BYTES`, `sdk/app.ld`,
  `tools/build_kapi_{app,demo}.sh`, `tools/build_calc_kx.sh`, `sdk/examples/hello/build.sh`).
  `__HeapLimit` is back at `0x20070000`; with `__end__=0x2004f514` that is ~131 KB of
  kernel heap (~30 KB headroom over Music's ~109 KB worst-case peak, ~80 KB free against the
  browser's 48 KB render floor). Every shipped .kx (hello 473 B, demo 2.2 KB, smoke 3.5 KB,
  calc REPL 20 KB) still fits; .kx bundles built against 0x20068000 are cleanly refused by the
  loader ("kapi: load_base ... != arena ... (rebuild app)") and must be rebuilt with the
  updated tools.

## KAPI ARENA 64 -> 48 KiB + LODEPNG CACHE-ADD LEAK (2026-08-05, follow-up)
- On-device follow-up (Pimoroni, after the 64 KiB arena fix): calc 3D graph still OOM'd on a
  clean boot (its ~80 KB transient needs ~101 KB free; the 131 KB heap left only ~85-90 KB at
  boot with the 28% base). Music worked until the browser was used, then the library went
  empty again -> the browser starves the kernel heap per session. about:start rendered fine.
- Root causes:
  * Calc 3D: intrinsic shortage, fixed by heap budget, not by code: 3D needs ~80 KB transient
    (g3d_arena 54.7 KB + 25.6 KB strip) vs ~85-90 KB free at boot -> marginal fail.
  * Browser contamination: confirmed real leak in LVGL's lodepng decoder -- when a decoded
    PNG exceeds the 16 KB image cache, lv_cache_add returns NULL and decoder_open returned
    LV_RESULT_INVALID WITHOUT freeing the decoded buffer (up to w*h*4 bytes per attempt).
    Any PNG > ~64x64 (wallpaper-chooser previews, SD-app manifest icons, larger icon sets)
    leaked on every decode attempt. The bin decoder was already leak-free (free_decoder_data).
  * Music after browser: opendir() callocs an fs_dir_t (pico-vfs vfs.c:628); with the
    browser-eaten heap it failed -> empty scan ("no .flac"). Not SD, not PSRAM.
- Fix: KAPI arena 64 -> 48 KiB at `0x20074000..0x20080000` (CMakeLists 464k RAM,
  `port/kapi.c KAPI_ARENA_BYTES`, `sdk/app.ld`, `tools/build_kapi_{app,demo}.sh`,
  `tools/build_calc_kx.sh`, `sdk/examples/hello/build.sh`). Heap ~147 KB: calc 3D fits
  (~101 KB free at boot), music gets ~37 KB headroom, the browser's 48 KB render floor has
  real margin. Every shipped .kx still fits (calc REPL 22.5 KB, 2x headroom); the future
  81-83 KB calc/music KAPI ports must bump the arena back when they replace the built-ins.
  Plus the lodepng leak fix (lib/lvgl/src/libs/lodepng/lv_lodepng.c).
- .kx bundles rebuilt at 0x20074000 (hello/demo/smoke/calc); stale-base bundles are refused
  by the loader with "rebuild app". Both pimoroni and pico2w targets build.

## MUSIC EMBEDDED PNG FRONT COVERS (2026-09-23, pending hardware check)
- The D:/SPLOONMUSIC sample contains 583 FLACs: 299 embedded baseline JPEG front covers and
  284 embedded PNG front covers. All are PICTURE type 3; no sidecars match the old JPEG fallback.
  The old flac_meta() accepted only JPEG MIME. All PNG PICTURE records declare zero dimensions,
  so the renderer reads image dimensions from PNG IHDR instead.
- Music records JPEG/PNG PICTURE regions with bounds checks and front-cover priority. PNG decode
  streams IDAT from the bounded FLAC file region through miniz into a 96px RGB565 thumbnail,
  compositing alpha on the Music box background. It does not allocate the whole embedded PNG;
  the largest sample is 2.26 MB. Sidecar PNGs are recognized too.
- Track starts and auto-advance load art before allocating FLAC/audio buffers. Cover changes
  detach and drop the previous LVGL source before reusing its thumbnail buffer.
- Host smoke check decoded all 38 distinct embedded PNGs; every RGB565 thumbnail matched a
  Pillow reference pixel-for-pixel, including the largest image through a nonzero file offset.
  Pimoroni and stock pico2w builds pass. Pimoroni UF2 SHA-256:
  a917907fd897cd775c5354e5dcbfa1b1cf95b5b399263f65861d7d7ebf9fd0f1.
- Pending hardware: flash build-pimoroni/kefyros.uf2; browse PNG and JPEG tracks, play and
  auto-advance, and check rapid navigation and Music reopen for art, audio and memory faults.

## MUSIC HARDWARE RESULT (2026-09-23)
- After the first art fix, covers rendered but playback stayed at 0:00. Music now places the persistent 96px RGB565 cover (18,432 bytes) and 4,096-frame audio ring (16,384 bytes) in the idle 48 KiB KAPI arena; cover decode uses a temporary heap thumbnail and releases it before FLAC playback. PNG inflate borrows the arena only while audio is stopped.
- Both pimoroni and pico2w firmware builds pass. The pimoroni UF2 was copied to the RP2350 bootloader volume, which disconnected after flashing. User confirmed the current firmware works, including cover display and playback. The post-fix RAM percentage was not reported; Bluetooth streaming remains to be budgeted and tested.
