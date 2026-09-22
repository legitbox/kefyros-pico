// ui/topbar.c — the persistent OS top bar. Lives on lv_layer_top() so it draws on
// top of EVERY screen (launcher + every app) without being re-created per screen.
// Shows memory pressure (live), the RTC clock and battery. Apps render in the
// content area below it (see kf_inset_top / KF_CONTENT_*).
#include "../kefyros.h"
#include "../port/clock.h"
#include "../port/bt_audio.h"
#include "theme.h"
#include "deskconf.h"
#include <malloc.h>
#include <time.h>

/* heap ceiling from the linker: capacity = __HeapLimit - __end__ (~147 KB with the
   48 KiB KAPI arena; was ~197 KB before it existed). */
extern char __HeapLimit[], __end__[];

static lv_obj_t *bar, *lbl_mem, *lbl_clock, *lbl_batt, *lbl_wifi, *lbl_bt, *lbl_mhz;
static lv_obj_t *mem_bar;                  /* stacked RAM bar (OS/app/net/leak segments) */

/* ---- RAM bar segments ----
   The heap is one shared malloc pool, so "who owns what" is attributed by marks:
     OS  (amber)  = heap used at boot, after launcher+topbar exist (no app, no net yet).
     NET (green)  = extra heap captured when the radio stack first comes up
                    (kf_net_init; subtracted from the app-open mark if an app was open).
     APP (purple) = heap growth since the current app opened (0 on the desktop).
     LEAK (gray)  = everything else above OS+NET while no app is open — i.e. the
                    unbucketed bucket: memory apps allocated and never freed. */
#define BAR_W 64
#define BAR_H 8
#define KF_PURPLE lv_color_hex(0x9b59b6)
#define KF_LEAK   lv_color_hex(0x5a5245)
static uint32_t s_os_used = 0;             /* bytes used at boot, captured at the FIRST
                                              tb_update tick (after the first launcher
                                              render, so the boot icon cache + header cache
                                              bucket as OS, not as phantom "leak") */
static uint32_t s_net_used = 0;            /* bytes attributed to the radio stack */
static int32_t  s_app_base = -1;           /* heap used at the current app's open, -1 = none */
static int      s_os_captured = 0;

static uint32_t heap_used(void){
	struct mallinfo mi = mallinfo();
	return (uint32_t)mi.uordblks;
}

/* ---- uart0 census: heap + LVGL caches at every app open/close (leak attribution) ---- */
#include "../lib/lvgl/src/core/lv_global.h"
#include "../lib/lvgl/src/misc/cache/lv_image_cache.h"
#include <stdio.h>
static void heap_census(const char *tag, int32_t session_delta){
	uint32_t cap = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	struct mallinfo mi = mallinfo();
	lv_cache_t *ic = LV_GLOBAL_DEFAULT()->img_cache;
	lv_cache_t *hc = LV_GLOBAL_DEFAULT()->img_header_cache;
	printf("[mem] %-7s used=%u/%u  imgcache=%zu hdrcache=%zu app_base=%d session=%+dK\n",
	       tag, (uint32_t)mi.uordblks, cap,
	       ic ? lv_cache_get_size(ic, NULL) : 0, hc ? lv_cache_get_size(hc, NULL) : 0,
	       s_app_base, (int)(session_delta / 1024));
}
static int32_t s_open_used = -1;
void tb_app_open(void){
	s_app_base = (int32_t)heap_used();
	s_open_used = s_app_base;
	heap_census("open", 0);
}
/* per-session leak log (visible in the Memory monitor app): ring of open/close pairs */
#define NSESS 24
static uint32_t s_sess_open[NSESS], s_sess_close[NSESS];
static int      s_nsess, s_sess_head;
int tb_session_count(void){ return s_nsess; }
int tb_session_get(int i, uint32_t *open_used, uint32_t *close_used){
	if(i < 0 || i >= s_nsess) return 0;
	int idx = (s_sess_head + i) % NSESS;   /* oldest first */
	*open_used = s_sess_open[idx]; *close_used = s_sess_close[idx];
	return 1;
}
void tb_app_close(void){
	int32_t delta = (s_open_used >= 0) ? (int32_t)heap_used() - s_open_used : 0;
	heap_census("close", delta);
	if(s_open_used >= 0){
		s_sess_open[s_sess_head]  = (uint32_t)s_open_used;
		s_sess_close[s_sess_head] = (uint32_t)heap_used();
		s_sess_head = (s_sess_head + 1) % NSESS;
		if(s_nsess < NSESS) s_nsess++;
	}
	s_app_base = -1; s_open_used = -1;
}

/* full census text for the Memory monitor app: heap + LVGL caches + session log */
void tb_mem_snapshot(char *out, int cap){
	uint32_t c = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	struct mallinfo mi = mallinfo();
	lv_cache_t *ic = LV_GLOBAL_DEFAULT()->img_cache;
	lv_cache_t *hc = LV_GLOBAL_DEFAULT()->img_header_cache;
	int p = 0;
	p += snprintf(out+p, cap-p, "heap %u/%u used  %u free\n"
	              "imgcache %zu  hdrcache %zu\n"
	              "os_base %u  net %u  app_base %d\n\n",
	              (uint32_t)mi.uordblks, c, c - (uint32_t)mi.uordblks,
	              ic ? lv_cache_get_size(ic, NULL) : 0, hc ? lv_cache_get_size(hc, NULL) : 0,
	              s_os_used, s_net_used, s_app_base);
	for(int i = 0; i < s_nsess; i++){
		uint32_t o, cl; tb_session_get(i, &o, &cl);
		p += snprintf(out+p, cap-p, "S%02d %+dK  (open %u -> close %u)\n",
		              i, (int)((int64_t)cl - o) / 1024, o, cl);
		if(p >= cap - 1) break;
	}
	if(p < cap) out[p] = 0;
}
void tb_net_up(void){
	if(s_net_used) return;                 /* first bring-up only */
	uint32_t base = (s_app_base >= 0) ? (uint32_t)s_app_base : s_os_used;
	uint32_t used = heap_used();
	s_net_used = used > base ? used - base : 0;
	if(s_app_base >= 0) s_app_base = (int32_t)((uint32_t)s_app_base + s_net_used);
	/* app segment must not double-count the (permanent) radio heap: fold it into the
	   app's open mark so purple only ever shows the app's own growth. */
}

/* redraw the stacked RAM bar: one flex child per live segment, widths in px */
static void draw_mem_bar(uint32_t cap, uint32_t used){
	if(!mem_bar) return;
	lv_obj_clean(mem_bar);
	uint32_t app_used = (s_app_base >= 0 && (uint32_t)s_app_base <= used) ? used - (uint32_t)s_app_base : 0;
	uint32_t leak = 0;
	if(used > s_os_used + s_net_used + app_used) leak = used - s_os_used - s_net_used - app_used;
	struct { uint32_t bytes; lv_color_t col; } segs[4] = {
		{ s_os_used,          KF_AMBER },
		{ s_net_used,         KF_ACTIVE },
		{ app_used,           KF_PURPLE },
		{ leak,               KF_LEAK },
	};
	if(used > cap) used = cap;
	if(cap == 0) cap = 1;
	for(int i = 0; i < 4; i++){
		uint32_t w = (uint64_t)segs[i].bytes * BAR_W / cap;
		if(segs[i].bytes && !w) w = 1;
		if(w > BAR_W) w = BAR_W;
		lv_obj_t *seg = lv_obj_create(mem_bar);
		lv_obj_remove_style_all(seg);
		lv_obj_set_size(seg, (int32_t)w, BAR_H);
		lv_obj_set_style_bg_color(seg, segs[i].col, 0);
		lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
		lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
	}
	(void)used;
}

static void tb_update(lv_timer_t *t){
	(void)t;
	/* memory PRESSURE = live heap bytes / true capacity; the stacked bar breaks it
	   down by OS/app/net/leak (colors in draw_mem_bar). */
	uint32_t cap = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	uint32_t used = heap_used();
	/* OS baseline after the first render (launcher icons decoded -> cache resident):
	   before any app opens or the radio initializes. */
	if(!s_os_captured && s_app_base < 0 && !s_net_used){
		s_os_used = used; s_os_captured = 1;
		heap_census("boot", 0);
	}
	int pct = cap ? (int)(((uint64_t)used * 100) / cap) : 0;
	if(pct < 0) pct = 0; if(pct > 100) pct = 100;
	lv_label_set_text_fmt(lbl_mem, "%d%%", pct);
	draw_mem_bar(cap, used);

	/* live CPU clock in MHz (updates when dynamic clock-switching kicks in). */
	lv_label_set_text_fmt(lbl_mhz, "%luMHz", (unsigned long)clock_sys_mhz());

	/* clock: prefer the SNTP-synced software time; fall back to the STM32 RTC. 12h/24h per
	   deskconf "clock24" (Settings -> Time format). */
	struct tm lt;
	uint8_t tm[3];
	int h24 = -1, mn = 0;
	if(kf_time_local(&lt))                       { h24 = lt.tm_hour; mn = lt.tm_min; }
	else if(reg_read(REG_RTC_TIME, tm, 3) >= 2)  { h24 = tm[0];      mn = tm[1];     }
	if(h24 < 0) lv_label_set_text(lbl_clock, "--:--");
	else if(deskconf_get_int("clock24", 1))
		lv_label_set_text_fmt(lbl_clock, "%02d:%02d", h24, mn);
	else {
		int h12 = h24 % 12; if(h12 == 0) h12 = 12;
		lv_label_set_text_fmt(lbl_clock, "%d:%02d%c", h12, mn, h24 < 12 ? 'a' : 'p');
	}

	/* WiFi: amber word, green when online, dim while connecting, red on fail.
	   Hidden (empty) when no network is targeted, to keep the bar uncluttered. */
	if(kf_net_present()){
		switch(kf_net_state()){
		case KF_NET_ONLINE:     lv_label_set_text(lbl_wifi, "wifi");
		                        lv_obj_set_style_text_color(lbl_wifi, KF_ACTIVE, 0); break;
		case KF_NET_CONNECTING: lv_label_set_text(lbl_wifi, "wifi");
		                        lv_obj_set_style_text_color(lbl_wifi, KF_AMBER_DIM, 0); break;
		case KF_NET_FAILED:     lv_label_set_text(lbl_wifi, "wifi");
		                        lv_obj_set_style_text_color(lbl_wifi, lv_color_hex(0xe03c32), 0); break;
		default:                lv_label_set_text(lbl_wifi, ""); break;
		}
	} else lv_label_set_text(lbl_wifi, "");
	/* Bluetooth output state is global, including when the manager is closed. */
	switch(kf_bt_state()){
	case KF_BT_CONNECTED:
		lv_label_set_text(lbl_bt,"BT");
		lv_obj_set_style_text_color(lbl_bt,KF_ACTIVE,0); break;
	case KF_BT_STARTING: case KF_BT_CONNECTING: case KF_BT_SCANNING:
		lv_label_set_text(lbl_bt,"BT?");
		lv_obj_set_style_text_color(lbl_bt,KF_AMBER_DIM,0); break;
	case KF_BT_FAILED:
		lv_label_set_text(lbl_bt,"BT!");
		lv_obj_set_style_text_color(lbl_bt,lv_color_hex(0xe03c32),0); break;
	default: lv_label_set_text(lbl_bt,""); break;
	}

	/* battery %, bit7 = charging. red when low. */
	uint8_t bt;
	if(reg_read(REG_BAT, &bt, 1) >= 1){
		int charging = bt & 0x80, p = bt & 0x7f;
		lv_label_set_text_fmt(lbl_batt, "%s%d%%", charging?"+":"", p);
		lv_obj_set_style_text_color(lbl_batt,
			(!charging && p <= 10) ? lv_color_hex(0xe03c32) : (charging ? KF_ACTIVE : KF_AMBER), 0);
	} else {
		lv_label_set_text(lbl_batt, "--");
		lv_obj_set_style_text_color(lbl_batt, KF_AMBER_DIM, 0);
	}
}

void topbar_init(void){
	/* one opaque bar parented to the top layer -> visible above every screen. */
	bar = lv_obj_create(lv_layer_top());
	lv_obj_remove_style_all(bar);
	lv_obj_set_size(bar, LCD_W, KF_TOPBAR_H);
	lv_obj_set_pos(bar, 0, 0);
	lv_obj_set_style_bg_color(bar, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);          /* opaque: hides app content behind */
	lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
	lv_obj_set_style_border_color(bar, KF_BORDER, 0);
	lv_obj_set_style_border_width(bar, 1, 0);              /* thin divider under the bar */
	lv_obj_set_style_radius(bar, 0, 0);
	lv_obj_set_style_pad_all(bar, 0, 0);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

	/* Topbar layout, left -> right with clear gaps:
	   [RAM nn%][#####bar#]   [nnnMHz]   [HH:MM]            [wifi]  [batt]
	   x=4     x=44..108     x=110      x=176          (right) -56   -6
	   The 64px bar is a stacked RAM meter: amber=OS base, green=radio stack,
	   purple=current app, gray=unbucketed/leaked, empty=free. */
	lbl_mem = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_mem, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_mem, KF_AMBER, 0);     /* amber, always */
	lv_obj_align(lbl_mem, LV_ALIGN_LEFT_MID, 4, 0);

	mem_bar = lv_obj_create(bar);
	lv_obj_remove_style_all(mem_bar);
	lv_obj_set_size(mem_bar, BAR_W, BAR_H);
	lv_obj_set_pos(mem_bar, 44, (KF_TOPBAR_H - BAR_H) / 2);
	lv_obj_set_flex_flow(mem_bar, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_all(mem_bar, 0, 0);
	lv_obj_set_style_border_width(mem_bar, 1, 0);
	lv_obj_set_style_border_color(mem_bar, KF_BORDER, 0);
	lv_obj_clear_flag(mem_bar, LV_OBJ_FLAG_SCROLLABLE);

	/* OS baseline is captured on the first tb_update tick (after the first render),
	   so the boot icon/header caches bucket as OS rather than phantom leak. */
	(void)0;

	lbl_mhz = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_mhz, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_mhz, KF_AMBER, 0);
	lv_obj_align(lbl_mhz, LV_ALIGN_LEFT_MID, 110, 0);

	lbl_clock = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_clock, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_clock, KF_AMBER, 0);
	lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, 176, 0);

	lbl_wifi = lv_label_create(bar);
	lbl_bt = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_bt, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_bt, KF_AMBER, 0);
	lv_label_set_text(lbl_bt, "");
	lv_obj_align(lbl_bt, LV_ALIGN_LEFT_MID, 230, 0);
	lv_obj_set_style_text_font(lbl_wifi, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_wifi, KF_AMBER, 0);
	lv_label_set_text(lbl_wifi, "");
	lv_obj_align(lbl_wifi, LV_ALIGN_RIGHT_MID, -56, 0);

	lbl_batt = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_batt, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_batt, KF_AMBER, 0);
	lv_obj_align(lbl_batt, LV_ALIGN_RIGHT_MID, -4, 0);

	tb_update(NULL);
	lv_timer_create(tb_update, 2000, NULL);
}

/* push an app screen's content below the bar. Works for flex, list and absolutely
   aligned children because LVGL positions all of them relative to the parent's
   CONTENT area, which pad_top shifts down. */
void kf_inset_top(lv_obj_t *scr){
	lv_obj_set_style_pad_top(scr, KF_TOPBAR_H, 0);
}
