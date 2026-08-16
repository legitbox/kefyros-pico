// apps/mem.c — Memory monitor (leak-hunting aid).
// Shows the live heap census + the per-app-session leak log that the topbar
// tracker collects (see tb_mem_snapshot / tb_session_* in ui/topbar.c).
//   * The "tick" line shows heap growth SINCE THIS APP OPENED, live — watch it
//     climb while the app sits idle to catch a per-refresh leak.
//   * F1 toggles BARE mode: static screen, no timer/group/snapshot — isolates
//     the shared open/close path (sfx, screen load, launcher redraw) from
//     app-specific churn. If bare cycles STILL leak ~8 KB, it's the shared path.
// ENTER re-reads; ESC exits.
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdio.h>
#include <malloc.h>

extern char __HeapLimit[], __end__[];

static lv_obj_t *scr, *lbl;
static lv_timer_t *timer;
static uint32_t s_open_used;
static int bare_mode;

static uint32_t heap_used_now(void){
	struct mallinfo mi = mallinfo();
	return (uint32_t)mi.uordblks;
}

static void mem_refresh(lv_timer_t *t){
	(void)t;
	if(bare_mode) return;
	char b[1024];
	tb_mem_snapshot(b, sizeof b);
	uint32_t now = heap_used_now();
	char b2[1100];
	snprintf(b2, sizeof b2, "TICK open %u -> now %u  (%+dK while open)\n%s",
	         s_open_used, now, (int)((int64_t)now - s_open_used) / 1024, b);
	lv_label_set_text(lbl, b2);
}

static void mem_key(lv_event_t *e){
	uint32_t k = lv_event_get_key(e);
	if(k == LV_KEY_ESC) kf_back_to_launcher();
	else if(k == 'b' || k == 'B'){            /* b: toggle bare mode */
		bare_mode = !bare_mode;
		if(bare_mode){
			if(timer){ lv_timer_delete(timer); timer = NULL; }
			lv_label_set_text(lbl, "BARE MODE\nstatic screen - no timer/group/snapshot\n\n"
			                      "open me, wait, exit, re-enter, read app_base:\n"
			                      "if it still grows ~+8K/cycle the leak is in the\n"
			                      "shared open/close path (sfx / screen load).\n\n"
			                      "b back to live mode, ESC exits.");
		} else {
			timer = lv_timer_create(mem_refresh, 1000, NULL);
			mem_refresh(NULL);
		}
	}
	else if(k == LV_KEY_UP || k == LV_KEY_DOWN || k == LV_KEY_LEFT || k == LV_KEY_RIGHT) mem_refresh(NULL);
	else if(k == LV_KEY_ENTER) mem_refresh(NULL);
}

static void on_del(lv_event_t *e){
	(void)e;
	if(timer){ lv_timer_delete(timer); timer = NULL; }
}

void app_mem_open(void){
	s_open_used = heap_used_now();
	bare_mode = 0;

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(scr, 4, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	kf_inset_top(scr);
	lv_obj_add_event_cb(scr, mem_key, LV_EVENT_KEY, NULL);
	lv_obj_add_event_cb(scr, on_del, LV_EVENT_DELETE, NULL);

	lbl = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl, KF_AMBER, 0);
	lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(lbl, LCD_W - 8);

	/* use the shared app group so the keypad indev feeds us keys */
	lv_group_t *g = kf_use_group();
	lv_group_add_obj(g, lbl);
	lv_group_focus_obj(lbl);

	mem_refresh(NULL);
	timer = lv_timer_create(mem_refresh, 1000, NULL);

	lv_screen_load(scr);
}
