// ui/theme.h — LunaTech amber CRT palette + theme for the Kefyros launcher.
// "Amber for everything, green-yellow for active." Colours lifted from the
// LunaTechBible site CSS (index.html :root tokens).
#ifndef KF_THEME_H
#define KF_THEME_H
#include "lvgl/lvgl.h"
#include "fonts.h"

/* --- backgrounds (near-black with the faintest amber warmth) --- */
#define KF_BG_DEEP   lv_color_hex(0x0a0806)
#define KF_BG        lv_color_hex(0x111010)
#define KF_CARD      lv_color_hex(0x1a1612)
#define KF_CARD_HI   lv_color_hex(0x211c15)
#define KF_BORDER    lv_color_hex(0x2a2318)
#define KF_BORDER_HI lv_color_hex(0x3d3222)

/* --- amber: the whole interface --- */
#define KF_AMBER_DIM lv_color_hex(0xb8860b)   /* inactive / secondary */
#define KF_AMBER     lv_color_hex(0xd4940a)   /* primary text/elements */
#define KF_AMBER_BR  lv_color_hex(0xf0a500)   /* headings */
#define KF_AMBER_GLOW lv_color_hex(0xffb71b)  /* highlight */
#define KF_AMBER_HOT lv_color_hex(0xffc94d)   /* hottest */

/* --- SoulTec green-yellow: ONLY for "active / alive" (focus, charging, live) --- */
#define KF_ACTIVE    lv_color_hex(0xb6f000)
#define KF_GREEN     lv_color_hex(0x5a8a3a)   /* Composite-34, rare chrome accent */

#define KF_TEXT      lv_color_hex(0xe8dcc8)
#define KF_TEXT_DIM  lv_color_hex(0x9a8d7a)
#define KF_TEXT_MUTED lv_color_hex(0x5a5245)

#define KF_FONT      (&lv_font_plex_mono_13)
#define KF_FONT_BIG  (&lv_font_plex_mono_20)
/* (No baked JP font — the music app loads one from the SD at runtime via
   lv_binfont_create, because a baked ~1 MB font bricked boot. KF_FONT_JP is kept
   as a harmless alias in case any other code wants a "JP" font handle.) */
#define KF_FONT_JP   KF_FONT

/* Install the custom theme on the display (call once, after disp_init). */
void kf_theme_init(lv_display_t *disp);

#endif
