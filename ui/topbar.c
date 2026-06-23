// ui/topbar.c — the persistent OS top bar. Lives on lv_layer_top() so it draws on
// top of EVERY screen (launcher + every app) without being re-created per screen.
// Shows memory pressure (live), the RTC clock and battery. Apps render in the
// content area below it (see kf_inset_top / KF_CONTENT_*).
#include "../kefyros.h"
#include "../port/clock.h"
#include "theme.h"
#include <malloc.h>
#include <time.h>

/* heap ceiling from the linker: capacity = __HeapLimit - __end__ (~197 KB). */
extern char __HeapLimit[], __end__[];

static lv_obj_t *bar, *lbl_mem, *lbl_clock, *lbl_batt, *lbl_wifi, *lbl_mhz;

static void tb_update(lv_timer_t *t){
	(void)t;
	/* memory PRESSURE = live heap bytes / true capacity. Amber bar + "nn% [==== ]". */
	struct mallinfo mi = mallinfo();
	uint32_t cap = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	int pct = cap ? (int)(((uint64_t)mi.uordblks * 100) / cap) : 0;
	if(pct < 0) pct = 0; if(pct > 100) pct = 100;
	int fill = (pct * 5 + 50) / 100;                 /* 5-segment bar (compact) */
	char b[8]; for(int i=0;i<5;i++) b[i] = i<fill ? '=' : ' '; b[5]=0;
	lv_label_set_text_fmt(lbl_mem, "%d%% [%s]", pct, b);

	/* live CPU clock in MHz (updates when dynamic clock-switching kicks in). */
	lv_label_set_text_fmt(lbl_mhz, "%luMHz", (unsigned long)clock_sys_mhz());

	/* clock: prefer the SNTP-synced software time (EET/EEST); fall back to the STM32 RTC. */
	struct tm lt;
	uint8_t tm[3];
	if(kf_time_local(&lt))
		lv_label_set_text_fmt(lbl_clock, "%02d:%02d", lt.tm_hour, lt.tm_min);
	else if(reg_read(REG_RTC_TIME, tm, 3) >= 2)
		lv_label_set_text_fmt(lbl_clock, "%02d:%02d", tm[0], tm[1]);
	else lv_label_set_text(lbl_clock, "--:--");

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
	   [RAM nn% [=====]]   [nnnMHz]   [HH:MM]            [wifi]  [batt]
	   x=4               x=96       x=164            (right) -56   -6   */
	lbl_mem = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_mem, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_mem, KF_AMBER, 0);     /* amber, always */
	lv_obj_align(lbl_mem, LV_ALIGN_LEFT_MID, 4, 0);

	lbl_mhz = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_mhz, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_mhz, KF_AMBER, 0);
	lv_obj_align(lbl_mhz, LV_ALIGN_LEFT_MID, 110, 0);

	lbl_clock = lv_label_create(bar);
	lv_obj_set_style_text_font(lbl_clock, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_clock, KF_AMBER, 0);
	lv_obj_align(lbl_clock, LV_ALIGN_LEFT_MID, 176, 0);

	lbl_wifi = lv_label_create(bar);
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
