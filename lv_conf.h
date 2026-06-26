// lv_conf.h — Kefyros launcher (LVGL v9.2). Only overrides; lv_conf_internal.h
// supplies defaults for everything not set here.
#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* Color: RGB565 in the draw buffer; flush expands to RGB666 for the ILI9488. */
#define LV_COLOR_DEPTH 16

/* Use the C library (musl) for malloc/string/sprintf — it's Linux. */
#define LV_USE_STDLIB_MALLOC   1   /* LV_STDLIB_CLIB */
#define LV_USE_STDLIB_STRING   1
#define LV_USE_STDLIB_SPRINTF  1

/* No RTOS: single-threaded main loop. */
#define LV_USE_OS 0               /* LV_OS_NONE */

/* Route LVGL assertions (incl. malloc-returned-NULL) to the amber screen of
   death instead of a silent while(1) hang. Catches OOM + NULL derefs early. */
#define LV_USE_ASSERT_NULL          1
#define LV_USE_ASSERT_MALLOC        1
#define LV_USE_ASSERT_STYLE         0
#define LV_USE_ASSERT_MEM_INTEGRITY 0   /* only works with LVGL's own allocator, not CLIB */
#define LV_USE_ASSERT_OBJ           0
#define LV_ASSERT_HANDLER_INCLUDE   "panic.h"
#define LV_ASSERT_HANDLER           kf_panic("LVGL ASSERT", __FILE__, 0);

/* Tick comes from lv_tick_set_cb (port/tick.c). */
#define LV_DPI_DEF 130

/* Logging on (warnings) via printf — handy during bring-up. */
/* LVGL logging OFF: it routed warnings (e.g. low-memory) through pico stdio printf,
   whose output path branched to NULL and hard-faulted (PC=0) mid-render. We don't
   read these logs, so disable the whole subsystem. */
#define LV_USE_LOG 0
#define LV_LOG_LEVEL 2            /* LV_LOG_LEVEL_WARN (unused while LV_USE_LOG=0) */
#define LV_LOG_PRINTF 0

/* Fonts: Monocraft (generated, ui/lv_font_monocraft_*.c) is the whole UI.
   Montserrat 14 kept only as a safety fallback. Terminal uses font8x8 directly. */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_CUSTOM_DECLARE  LV_FONT_DECLARE(lv_font_plex_mono_13) LV_FONT_DECLARE(lv_font_plex_mono_20)
#define LV_FONT_DEFAULT &lv_font_plex_mono_13

/* Keep the default theme; no demos/examples. */
#define LV_USE_DEMO_WIDGETS 0
#define LV_BUILD_EXAMPLES 0

/* Image decoders + POSIX filesystem so the wallpaper chooser can decode real
   files from /root/wallpapers on-device. Paths are drive-letter prefixed: the
   POSIX driver is mounted at 'A', so use e.g. "A:/root/wallpapers/x.jpg". */
#define LV_USE_TJPGD        1     /* baseline JPEG (tiny decoder) */
#define LV_USE_LODEPNG      1     /* PNG (self-contained, no libpng/zlib) */
#define LV_USE_BMP          1
#define LV_USE_FS_POSIX     1
#define LV_FS_POSIX_LETTER  'A'
#define LV_FS_POSIX_PATH    ""

/* Image cache budget. CRITICAL: this lives in the heap, and our heap is only
   ~210 KB (the rest of SRAM is the 200 KB wallpaper scratch + LVGL draw buffers).
   A 2 MB budget meant LVGL NEVER evicted decoded images (it thought it had 2 MB
   of room) so decoded icons/images piled up until malloc failed -> "Out of
   memory" panic. Bound it so the launcher icon set fits but transient decodes
   (chooser previews, off-screen icons) get evicted. The wallpaper itself is a
   raw RGB565 descriptor rendered in place (not cached), so it doesn't count. */
/* Small image cache: with the heap now ~338 KB (wallpaper moved to PSRAM) a big
   cache only fragments it and starves the 185 KB graph-canvas malloc. Icons
   re-decode from SD on demand (safe + fast now that the heap is roomy). */
#define LV_CACHE_DEF_SIZE             (16 * 1024)
#define LV_IMAGE_HEADER_CACHE_DEF_CNT 8

#endif /* LV_CONF_H */
