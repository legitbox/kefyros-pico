// apps/wifi.c — WiFi manager. Status header (SSID / state / IP), a scan list of
// nearby APs, password entry for secured networks, and Forget. Talks to the CYW43
// radio via the port/net.c API. Credentials are remembered (deskconf) so the radio
// auto-connects on the next boot.
//
// Input model: this is a menu app (no raw-key grab), so the global ESC -> back to
// launcher still works. List rows live in the keypad focus group (UP/DOWN move
// focus, ENTER activates). The password field is a one-line textarea: type the key,
// ENTER connects, ESC leaves the app.
#include "../kefyros.h"
#include "../ui/theme.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define AP_MAX 32

static lv_obj_t *scr, *lbl_status, *lbl_dbg, *lbl_pw, *list, *pw_box;
static lv_group_t *g;
static lv_timer_t *wtimer;

static char ap_ssid[AP_MAX][33];
static int  ap_secured[AP_MAX];
static int  ap_n;
static char pend_ssid[33];        /* AP awaiting a password */
static char last_pass[65];        /* DEBUG: last password actually sent to connect */
static int  s_scan_rc = -99;      /* DEBUG: last kf_net_scan_start() return code */

static void rebuild_list(void);
static void start_scan(void);

/* ---- status header (live) ---- */
static void refresh_status(void){
	const char *ssid = kf_net_ssid();
	/* DEBUG line (separate label so it never pushes the status text off-screen). */
	int dl = kf_net_dbg_link();
	int lk = (int)(signed char)(dl & 0xff);   /* link status (can be negative) */
	int ai = (dl >> 8) & 0xff;                 /* current auth-ladder index */
	lv_label_set_text_fmt(lbl_dbg, "[itf%d n%d rc%d lk%d a%d] %luMHz",
	         kf_net_dbg_itf(), ap_n, s_scan_rc, lk, ai,
	         (unsigned long)(kf_clock_khz()/1000u));
	/* benchmark readout on the pw line: shows progress + final KB/s */
	switch(kf_net_bench_state()){
	case 1: lv_label_set_text(lbl_pw, "bench: connecting..."); break;
	case 2: lv_label_set_text_fmt(lbl_pw, "bench: %lu KB...", (unsigned long)(kf_net_bench_bytes()/1024u)); break;
	case 3: lv_label_set_text_fmt(lbl_pw, "bench: %lu KB/s (%lu KB)",
	            (unsigned long)kf_net_bench_kbps(), (unsigned long)(kf_net_bench_bytes()/1024u)); break;
	case 4: lv_label_set_text(lbl_pw, "bench: failed"); break;
	default: lv_label_set_text_fmt(lbl_pw, "pw='%s'", last_pass); break;
	}

	if(!kf_net_present()){
		lv_label_set_text(lbl_status, "radio not available");
		lv_obj_set_style_text_color(lbl_status, KF_AMBER_DIM, 0);
		return;
	}
	kf_net_state_t st = kf_net_state();
	if(st == KF_NET_ONLINE)
		lv_label_set_text_fmt(lbl_status, "%s online %s", ssid, kf_net_ip());
	else if(ssid[0])
		lv_label_set_text_fmt(lbl_status, "%s %s", ssid, kf_net_state_str());
	else
		lv_label_set_text(lbl_status, "not connected");
	lv_obj_set_style_text_color(lbl_status, st==KF_NET_ONLINE ? KF_ACTIVE : KF_AMBER, 0);
}

static void wtick(lv_timer_t *t){ (void)t; if(lv_screen_active()==scr) refresh_status(); }

/* ---- password entry overlay ---- */
static void close_pw(void){
	if(pw_box){ lv_obj_delete(pw_box); pw_box = NULL; }
	/* refocus the list so navigation continues */
	lv_obj_t *first = lv_obj_get_child(list, 0);
	if(first) lv_group_focus_obj(first);
}

static void pw_ready(lv_event_t *e){
	lv_obj_t *ta = lv_event_get_target(e);
	const char *pass = lv_textarea_get_text(ta);
	snprintf(last_pass, sizeof last_pass, "%s", pass);   /* safe copy BEFORE close_pw frees ta */
	char ssid[33]; snprintf(ssid, sizeof ssid, "%s", pend_ssid);
	close_pw();                     /* deletes the textarea -> `pass` is now dangling! */
	kf_net_connect(ssid, last_pass);/* use the COPY, not the freed textarea buffer */
	refresh_status();
}

static void open_pw(const char *ssid){
	snprintf(pend_ssid, sizeof pend_ssid, "%s", ssid);
	if(pw_box) lv_obj_delete(pw_box);

	pw_box = lv_obj_create(scr);
	lv_obj_remove_style_all(pw_box);
	lv_obj_set_size(pw_box, LCD_W-16, 70);
	lv_obj_align(pw_box, LV_ALIGN_TOP_MID, 0, 26);
	lv_obj_set_style_bg_color(pw_box, KF_CARD, 0);
	lv_obj_set_style_bg_opa(pw_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(pw_box, KF_BORDER_HI, 0);
	lv_obj_set_style_border_width(pw_box, 1, 0);
	lv_obj_set_style_pad_all(pw_box, 6, 0);
	lv_obj_remove_flag(pw_box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *lbl = lv_label_create(pw_box);
	lv_label_set_text_fmt(lbl, "Password for %s  (ENTER ok, ESC cancel)", ssid);
	lv_obj_set_style_text_color(lbl, KF_AMBER, 0);
	lv_obj_align(lbl, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *ta = lv_textarea_create(pw_box);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_password_mode(ta, false);   /* DEBUG: show what the keyboard captures */
	lv_textarea_set_placeholder_text(ta, "password");
	lv_obj_set_width(ta, LCD_W-40);
	lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_add_event_cb(ta, pw_ready, LV_EVENT_READY, NULL);
	lv_group_add_obj(g, ta);
	lv_group_focus_obj(ta);
}

/* ---- list actions ---- */
static void act_rescan(lv_event_t *e){ (void)e;
	close_pw();
	rebuild_list();      /* clear stale AP rows, keep the control rows */
	start_scan();        /* re-populate as results arrive */
}

static void act_forget(lv_event_t *e){ (void)e;
	close_pw();
	kf_net_forget();
	rebuild_list();
	refresh_status();
}

static void act_ap(lv_event_t *e){
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if(idx < 0 || idx >= ap_n) return;
	if(ap_secured[idx]) open_pw(ap_ssid[idx]);
	else { close_pw(); kf_net_connect(ap_ssid[idx], ""); refresh_status(); }
}

/* one AP discovered by the scan -> append a row (runs on core0 from kf_net_poll). */
static void on_ap(const char *ssid, int rssi, int secured){
	if(ap_n >= AP_MAX || !scr) return;
	int idx = ap_n;
	snprintf(ap_ssid[idx], sizeof ap_ssid[idx], "%s", ssid);
	ap_secured[idx] = secured;
	ap_n++;
	char row[64];
	snprintf(row, sizeof row, "%s   %ddBm %s", ssid, rssi, secured ? "[*]" : "[o]");
	lv_obj_t *b = lv_list_add_button(list, NULL, row);
	lv_obj_add_event_cb(b, act_ap, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
	lv_group_add_obj(g, b);
}

static void start_scan(void){ ap_n = 0; s_scan_rc = kf_net_scan_start(on_ap); }

static void act_bench(lv_event_t *e){ (void)e; kf_net_bench_start(); }

static void rebuild_list(void){
	lv_obj_clean(list);            /* drops old rows (auto-removed from the group) */
	lv_obj_t *b;
	b = lv_list_add_button(list, NULL, "Rescan");
	lv_obj_add_event_cb(b, act_rescan, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(g, b);
	b = lv_list_add_button(list, NULL, "Speed test");
	lv_obj_add_event_cb(b, act_bench, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(g, b);
	if(kf_net_ssid()[0]){
		b = lv_list_add_button(list, NULL, "Forget network");
		lv_obj_add_event_cb(b, act_forget, LV_EVENT_CLICKED, NULL);
		lv_group_add_obj(g, b);
	}
	lv_group_focus_obj(lv_obj_get_child(list, 0));
}

static void on_del(lv_event_t *e){ (void)e;
	if(wtimer){ lv_timer_delete(wtimer); wtimer = NULL; }
	scr = NULL; pw_box = NULL;
	/* Back to the 360 MHz smooth-UI clock on exit. NOTE: 360 > ~270 MHz, so the radio
	   can't hold a link here — leaving WiFi drops the connection (re-entering WiFi
	   auto-reconnects, since it drops to 250 on open and creds are remembered). */
	kf_clock_ui();
}

void app_wifi_open(void){
	/* WiFi only associates at <=~270 MHz, so drop to eco for the whole session here.
	   Watch the topbar MHz flip to 250M. */
	/* WiFi needs <=~270 MHz: the cyw43 bring-up HANGS at 400 MHz. Drop to eco (250 MHz)
	   before bringing the radio up. (We confirmed 400 hangs init — overclock + password
	   were two separate bugs.) */
	kf_clock_eco();
	kf_net_init();         /* bring the radio up NOW, at the safe clock (idempotent) */

	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr, on_del, LV_EVENT_DELETE, NULL);

	lbl_status = lv_label_create(scr);
	lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_status, LCD_W-16);
	lv_obj_set_style_text_font(lbl_status, KF_FONT, 0);
	lv_obj_align(lbl_status, LV_ALIGN_TOP_LEFT, 8, 4);

	lbl_dbg = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_dbg, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_dbg, KF_TEXT_DIM, 0);
	lv_obj_align(lbl_dbg, LV_ALIGN_TOP_LEFT, 8, 20);

	lbl_pw = lv_label_create(scr);
	lv_label_set_long_mode(lbl_pw, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_pw, LCD_W-16);
	lv_obj_set_style_text_font(lbl_pw, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_pw, KF_AMBER_BR, 0);
	lv_obj_align(lbl_pw, LV_ALIGN_TOP_LEFT, 8, 36);

	list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-58);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 54);

	g = kf_use_group();
	pw_box = NULL;
	rebuild_list();
	refresh_status();
	start_scan();

	wtimer = lv_timer_create(wtick, 500, NULL);
	lv_screen_load(scr);
}
