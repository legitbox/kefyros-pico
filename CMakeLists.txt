# Kefyros PDA firmware — PicoCalc / Pico 2 W (RP2350)
cmake_minimum_required(VERSION 3.13)

# MUST be set before the SDK import, or PICO_PLATFORM defaults to rp2040 and the
# uf2 gets tagged with the RP2040 family id (which the RP2350 bootrom rejects).
set(PICO_PLATFORM rp2350  CACHE STRING "RP2350 (Pico 2 / 2 W)")
set(PICO_BOARD    pico2_w  CACHE STRING "Pico 2 W board (CYW43 WiFi)")

include(pico_sdk_import.cmake)

project(kefyros C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# --- vendored libs ---
add_subdirectory(lib/pico-vfs)     # POSIX FS over FatFs + SD-SPI block device
add_subdirectory(port/lcdspi)      # ILI9488 driver (INTERFACE lib)

# LVGL 9.2 compiled straight in (same as the Linux build: glob src + LV_CONF_INCLUDE_SIMPLE)
file(GLOB_RECURSE LVGL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/lib/lvgl/src/*.c)
file(GLOB APP_SRC ${CMAKE_CURRENT_LIST_DIR}/apps/*.c)
file(GLOB UI_SRC  ${CMAKE_CURRENT_LIST_DIR}/ui/*.c)

# BearSSL — vendored TLS 1.2 client for Spineko HTTPS (replaces the abandoned mbedtls
# attempt). Pure C99, generated codec/EC tables shipped in src/, no T0 build needed.
# Unreferenced code is dropped by --gc-sections, so the binary only grows by what
# port/tls.c actually pulls in.
file(GLOB_RECURSE BEARSSL_SRC CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/lib/bearssl/src/*.c)

# The Pimoroni Pico Plus 2 W has an onboard APS6404 on RP2350 QMI window 1.
# Use that memory-mapped bus there; stock Pico 2 W builds keep using the
# PicoCalc mainboard's GPIO/PIO PSRAM.
if(PICO_BOARD STREQUAL "pimoroni_pico_plus2_w_rp2350")
    set(KF_PSRAM_SRC port/psram_qmi.c)
else()
    set(KF_PSRAM_SRC port/psram.c)
endif()

add_executable(kefyros
    src/main.c
    port/disp.c
    port/kbd_uart.c
    port/tick.c
    port/indev.c
    port/storage_sd.c
    port/clock.c
    port/sys.c
    port/fault.c
    port/audio.c
    ${KF_PSRAM_SRC}
    port/net.c
    port/http.c
    port/html.c
    port/css.c                     # Spineko CSS engine (rules in PSRAM)
    port/vt100.c                   # Term: VT100/xterm terminal emulator core
    port/sha256.c                  # Term: SHA-256 (SSH exchange hash + KDF)
    port/ssh.c                     # Term: SSH-2 protocol core
    port/ssh_tcp.c                 # Term: lwIP glue for the SSH client
    lib/monocypher/monocypher.c            # Term: x25519 / chacha20 / poly1305
    lib/monocypher/monocypher-ed25519.c    # Term: ed25519 + sha512
    port/tls.c
    port/imgdec.c
    port/kapi.c                    # KAPI class-1 app loader + vtable glue
    lib/miniz/miniz_tinfl.c        # streaming DEFLATE inflate for the PNG decoder
	lib/peanut-gbc/minigb_apu.c     # GB/GBC APU (Peanut frontend, MIT)
    ${APP_SRC}
    ${UI_SRC}
    ${LVGL_SRC}
    ${BEARSSL_SRC}
)

# Reserve the top 48 KiB of SRAM at a stable address for SD-loaded KAPI apps.
# The SDK's default linker map lets the heap grow to 0x20080000; shortening its
# RAM region to 464 KiB moves __HeapLimit to 0x20074000 and leaves
# [0x20074000,0x20080000) exclusively for the loader.  g_kapi_arena is an
# absolute linker symbol, so a .kx remains loadable after unrelated firmware
# changes instead of needing to be relinked against every kefyros.elf.
#
# 48 KiB (NOT 96/64): a 96 KiB arena left only ~101 KB of kernel heap (missing
# launcher icons, Spineko "..." low-memory page, empty Music library); even at
# 64 KiB the built-ins stayed marginal — calc's 3D grapher (~80 KB transient)
# OOM'd on a clean boot and any browser session starved music/calc. 48 KiB
# restores ~147 KB of kernel heap (3D graph fits, ~37 KB music headroom, the
# browser's 48 KB render floor has real margin) while every shipped .kx still
# fits (largest: calc REPL 22.5 KB, 2x headroom). If full calc/music KAPI ports
# (81-83 KiB) ever land, they replace the built-ins and the arena can grow again.
set(KF_KAPI_MEMMAP "${CMAKE_CURRENT_BINARY_DIR}/memmap_kapi.ld")
file(READ "${PICO_SDK_PATH}/src/rp2_common/pico_crt0/rp2350/memmap_default.ld" KF_MEMMAP_TEXT)
string(REPLACE
    "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 512k"
    "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 464k"
    KF_MEMMAP_TEXT "${KF_MEMMAP_TEXT}")
string(REPLACE
    "ENTRY(_entry_point)"
    "g_kapi_arena = 0x20074000;\ng_kapi_arena_end = 0x20080000;\n\nENTRY(_entry_point)"
    KF_MEMMAP_TEXT "${KF_MEMMAP_TEXT}")
string(FIND "${KF_MEMMAP_TEXT}" "RAM(rwx) : ORIGIN =  0x20000000, LENGTH = 464k" KF_KAPI_RAM_FOUND)
string(FIND "${KF_MEMMAP_TEXT}" "g_kapi_arena = 0x20074000" KF_KAPI_SYMBOL_FOUND)
if(KF_KAPI_RAM_FOUND EQUAL -1 OR KF_KAPI_SYMBOL_FOUND EQUAL -1)
    message(FATAL_ERROR "RP2350 SDK memmap changed; refusing to build with an unreserved KAPI arena")
endif()
file(WRITE "${KF_KAPI_MEMMAP}" "${KF_MEMMAP_TEXT}")
pico_set_linker_script(kefyros "${KF_KAPI_MEMMAP}")

target_compile_definitions(kefyros PRIVATE
    LV_CONF_INCLUDE_SIMPLE
    # CYW43 PIO-SPI bus clock divider. DYNAMIC lets port/net.c hold the gSPI bus at the
    # known-good Pico W rate (31.25 MHz) at every clk_sys tier, including by updating
    # the live PIO state machine after the SDK has claimed it.
    CYW43_PIO_CLOCK_DIV_DYNAMIC=1
    # CYW43 read-sample timing: spi_gap010_sample1 (latest sample) BROKE bring-up at
    # 400 MHz (itf never came up) — the default spi_gap01_sample0 reads reliably, so
    # we leave it default and isolate the slow-bus lever (INT=16) by itself.
    # Core0 stack: SDK default is 2 KB, which the file browser + LVGL's deep layout/
    # render call chain overflowed (SP ran past __StackBottom -> memory corruption ->
    # bus fault in free()). Bump to 4 KB (fills SCRATCH_Y). Big app buffers are also
    # kept off-stack (static) to stay well clear of this limit.
    PICO_STACK_SIZE=0x1000
    # malloc returns NULL on failure instead of panicking the whole OS. The image
    # decoder (and LVGL, via LV_USE_ASSERT_MALLOC) check for NULL and degrade
    # gracefully (skip the image / amber assert) rather than hard-faulting mid-decode
    # on a fragmented heap — which is exactly what crashed while scrolling Wikipedia.
    PICO_MALLOC_PANIC=0
	MINIGB_APU_AUDIO_FORMAT_S16SYS=1
	AUDIO_SAMPLE_RATE=32768
    # Keep clk_peri attached to clk_sys when we overclock. The SDK default parks clk_peri
    # on the 48 MHz USB PLL after set_sys_clock_khz(), which HARD-CAPS every SPI at 24 MHz
    # (clk_peri/2) — the LCD was stuck at 24 MHz no matter what we requested. With this on,
    # clk_peri follows clk_sys (360 MHz), so the panel SPI can actually run fast;
    # reclock_peripherals() re-derives the SPI/SD/UART bauds after each clock change.
    PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1
)

# PSRAM PIO program -> build/.../psram.pio.h
pico_generate_pio_header(kefyros ${CMAKE_CURRENT_LIST_DIR}/port/psram.pio)

target_include_directories(kefyros PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}            # kefyros.h, lv_conf.h
    ${CMAKE_CURRENT_LIST_DIR}/lib        # so "lvgl/lvgl.h" resolves to lib/lvgl/lvgl.h
    ${CMAKE_CURRENT_LIST_DIR}/lib/lvgl
    ${CMAKE_CURRENT_LIST_DIR}/port       # "lcdspi/lcdspi.h", "disp.h", "clock.h"
    ${CMAKE_CURRENT_LIST_DIR}/ui
    ${CMAKE_CURRENT_LIST_DIR}/apps
    ${CMAKE_CURRENT_LIST_DIR}/lib/bearssl/inc   # bearssl.h (public API)
    ${CMAKE_CURRENT_LIST_DIR}/lib/bearssl/src   # inner.h, config.h (BearSSL internals)
    ${CMAKE_CURRENT_LIST_DIR}/lib/miniz         # miniz_tinfl.h (PNG inflate)
    ${CMAKE_CURRENT_LIST_DIR}/lib/monocypher     # Term: SSH crypto (Monocypher)
    ${CMAKE_CURRENT_LIST_DIR}/lib/nanosvg/src   # nanosvg.h, nanosvgrast.h (SVG)
    ${CMAKE_CURRENT_LIST_DIR}/sdk               # kapi.h (the class-1 app ABI)
)

# pico-vfs filesystem support (SD-FAT only; no flash fs)
pico_enable_filesystem(kefyros)

target_link_libraries(kefyros PRIVATE
    pico_stdlib
    pico_multicore
    hardware_spi
    hardware_dma
    hardware_uart
    hardware_pwm
    hardware_pio
    hardware_clocks
    hardware_vreg
    hardware_clocks
    hardware_watchdog
    hardware_sync
    lcdspi
    blockdevice_sd
    filesystem_fat
    filesystem_vfs
    # --- WiFi: CYW43 radio + lwIP (poll mode). TLS via vendored BearSSL (lib/bearssl). ---
    pico_cyw43_arch_lwip_poll
    pico_lwip_sntp        # SNTP time sync
    pico_rand             # RP2350 hardware TRNG -> BearSSL entropy seed
)

# debug log on uart0 (GP0/GP1); USB free for BOOTSEL flashing
pico_enable_stdio_uart(kefyros 1)
pico_enable_stdio_usb(kefyros 0)

pico_add_extra_outputs(kefyros)     # -> build/kefyros.uf2
