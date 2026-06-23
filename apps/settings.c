// apps/settings.c — backlight (persisted), keyboard backlight, clock (REG_RTC),
// power, battery. Brightness persists via deskconf and is applied at boot by the
// launcher. UART reg API for the backlights / battery / RTC.
//
// Bare-metal RP2350 (Pico 2 W) port: no Linux system()/popen. Power actions use
// the kefyros.h system funcs (kf_poweroff/kf_reboot/kf_bootsel). The old CPU
// "pmode" governor rows and the WiFi row were Linux-only and have been dropped.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include "pico/time.h"
#include <stdlib.h>
#include <string.h>

static lv_obj_t *scr, *lbl_bat, *lbl_clk, *lbl_test;
static lv_timer_t *stimer;     /* tied to scr's lifetime (deleted with the screen) */
static int bkl, bk2;

/* RTC bytes from the STM32 are BCD (STM32 RTC convention). */
static int bcd(uint8_t v){ return (v>>4)*10 + (v&0x0f); }

static void refresh_bat(void){
	uint8_t b;
	if(reg_read(REG_BAT,&b,1) >= 1)
		lv_label_set_text_fmt(lbl_bat, "Battery: %d%%%s", b&0x7f, (b&0x80)?" (charging)":"");
	else lv_label_set_text(lbl_bat, "Battery: n/a");
}

static void refresh_clk(void){
	uint8_t d[4], t[4];
	int nd = reg_read(REG_RTC_DATE, d, 4);
	int nt = reg_read(REG_RTC_TIME, t, 4);
	if(nd >= 3 && nt >= 3)
		lv_label_set_text_fmt(lbl_clk, "%02d-%02d-%02d  %02d:%02d:%02d",
			bcd(d[0]), bcd(d[1]), bcd(d[2]),
			bcd(t[0]), bcd(t[1]), bcd(t[2]));
	else
		lv_label_set_text(lbl_clk, "Clock: n/a");
}

static void tick_timer(lv_timer_t *tm){ (void)tm;
	if(lv_screen_active()==scr){ refresh_bat(); refresh_clk(); } }
static void on_settings_del(lv_event_t *e){ (void)e;
	if(stimer){ lv_timer_delete(stimer); stimer = NULL; } }

static void act_bkl(lv_event_t *e){ int d=(int)(intptr_t)lv_event_get_user_data(e);
	bkl+=d; if(bkl<0)bkl=0; if(bkl>9)bkl=9; uint8_t v=bkl; reg_write(REG_BKL,&v,1); deskconf_set_int("bkl",bkl); }
static void act_bk2(lv_event_t *e){ int d=(int)(intptr_t)lv_event_get_user_data(e);
	bk2+=d; if(bk2<0)bk2=0; if(bk2>3)bk2=3; uint8_t v=bk2; reg_write(REG_BK2,&v,1); deskconf_set_int("bk2",bk2); }

/* Power: reuse the shared power menu (ui/power.c) instead of a duplicate local
 * screen — that local pwr_scr was never freed (a leak), and this is identical. */
static void act_power(lv_event_t *e){ (void)e; kf_power_menu(); }

/* --- PSRAM diagnostics (top-of-chip scratch window, clear of the bump-allocated
   wallpaper/browser/music arenas which grow up from address 0). Both block for a
   few tens of ms; fine for a manual button. --- */
#define PS_SCRATCH   0x80000u        /* 512 KB test window at the top of PSRAM */

/* Throughput: time a 256 KB write then a 256 KB read; report MB/s (integer math,
   no %f). Quad QPI should land far above the old 1-bit ~2 MB/s. */
static void act_memspeed(lv_event_t *e){ (void)e;
	uint32_t sz = kf_psram_size();
	if(!sz){ lv_label_set_text(lbl_test, "Speed: no PSRAM"); return; }
	enum { BUF = 8192, ITERS = 32 };          /* 32 * 8 KB = 256 KB per phase */
	uint8_t *buf = malloc(BUF);
	if(!buf){ lv_label_set_text(lbl_test, "Speed: out of memory"); return; }
	for(int i = 0; i < BUF; i++) buf[i] = (uint8_t)(i*7 + 3);
	uint32_t base = sz - PS_SCRATCH;

	uint64_t t0 = time_us_64();
	for(int k = 0; k < ITERS; k++) kf_psram_write(base + (uint32_t)k*BUF, buf, BUF);
	uint64_t t1 = time_us_64();
	for(int k = 0; k < ITERS; k++) kf_psram_read (base + (uint32_t)k*BUF, buf, BUF);
	uint64_t t2 = time_us_64();
	free(buf);

	uint32_t total = (uint32_t)ITERS * BUF;   /* bytes/us == MB/s (decimal) */
	uint32_t w10 = (t1>t0) ? (uint32_t)((uint64_t)total*10u/(t1-t0)) : 0;
	uint32_t r10 = (t2>t1) ? (uint32_t)((uint64_t)total*10u/(t2-t1)) : 0;
	lv_label_set_text_fmt(lbl_test, "Speed: W %u.%u  R %u.%u MB/s",
		w10/10, w10%10, r10/10, r10%10);
	lv_obj_set_style_text_color(lbl_test, KF_ACTIVE, 0);
}

/* SPI/QPI bus check: write a per-block pseudo-random pattern across the whole
   scratch window, then read it ALL back and verify byte-exact (separate passes,
   so a stuck address or stale-nibble carry between transactions is caught). */
static void act_memcheck(lv_event_t *e){ (void)e;
	uint32_t sz = kf_psram_size();
	if(!sz){ lv_label_set_text(lbl_test, "Check: no PSRAM"); return; }
	uint8_t *w = malloc(1024), *r = malloc(1024);
	if(!w || !r){ free(w); free(r); lv_label_set_text(lbl_test, "Check: out of memory"); return; }
	uint32_t base = sz - PS_SCRATCH, fail_at = 0; int fail = 0;

	for(uint32_t off = 0; off < PS_SCRATCH; off += 1024){
		uint32_t s = 0x9E3779B9u ^ (base + off);
		for(int i = 0; i < 1024; i++){ s = s*1664525u + 1013904223u; w[i] = (uint8_t)(s >> 24); }
		kf_psram_write(base + off, w, 1024);
	}
	for(uint32_t off = 0; off < PS_SCRATCH && !fail; off += 1024){
		uint32_t s = 0x9E3779B9u ^ (base + off);
		for(int i = 0; i < 1024; i++){ s = s*1664525u + 1013904223u; w[i] = (uint8_t)(s >> 24); }
		kf_psram_read(base + off, r, 1024);
		if(memcmp(w, r, 1024)){
			for(int i = 0; i < 1024; i++) if(w[i] != r[i]){ fail_at = base + off + i; break; }
			fail = 1;
		}
	}
	free(w); free(r);
	if(fail) lv_label_set_text_fmt(lbl_test, "Check: FAIL @ 0x%06X", (unsigned)fail_at);
	else     lv_label_set_text(lbl_test, "Check: OK (512 KB verified)");
	lv_obj_set_style_text_color(lbl_test, fail ? lv_color_hex(0xe03c32) : KF_ACTIVE, 0);
}

static lv_obj_t *additem(lv_obj_t *list, lv_group_t *g, const char *txt,
                         lv_event_cb_t cb, void *ud){
	lv_obj_t *b = lv_list_add_button(list, NULL, txt);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	lv_group_add_obj(g, b);
	return b;
}

void app_settings_open(void){
	bkl = deskconf_get_int("bkl", 5);
	bk2 = deskconf_get_int("bk2", 2);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);                  /* clear the persistent OS top bar */
	/* delete the 1 Hz refresh timer when this screen is freed (launcher_show
	   deletes the screen on exit) — otherwise it leaks and fires on dead labels. */
	lv_obj_add_event_cb(scr, on_settings_del, LV_EVENT_DELETE, NULL);

	lbl_bat = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_bat, KF_AMBER, 0);
	lv_obj_align(lbl_bat, LV_ALIGN_TOP_MID, 0, 4);

	lbl_clk = lv_label_create(scr);
	lv_obj_set_style_text_color(lbl_clk, KF_AMBER, 0);
	lv_obj_align(lbl_clk, LV_ALIGN_TOP_MID, 0, 18);

	/* PSRAM self-test result (static after boot) */
	lv_obj_t *lbl_ram = lv_label_create(scr);
	uint32_t ps = kf_psram_size();
	if(ps) lv_label_set_text_fmt(lbl_ram, "PSRAM: %u MB  OK", ps/(1024u*1024u));
	else   lv_label_set_text(lbl_ram, "PSRAM: not detected");
	lv_obj_set_style_text_color(lbl_ram, ps ? KF_ACTIVE : lv_color_hex(0xe03c32), 0);
	lv_obj_align(lbl_ram, LV_ALIGN_TOP_MID, 0, 32);

	/* live result line for the speed test / bus check below */
	lbl_test = lv_label_create(scr);
	lv_label_set_text(lbl_test, "Run a PSRAM test below");
	lv_obj_set_style_text_color(lbl_test, KF_TEXT_MUTED, 0);
	lv_obj_align(lbl_test, LV_ALIGN_TOP_MID, 0, 46);

	lv_obj_t *list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-68);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 64);

	lv_group_t *g = kf_use_group();
	additem(list,g, "LCD Light +",      act_bkl, (void*)(intptr_t)+1);
	additem(list,g, "LCD Light -",      act_bkl, (void*)(intptr_t)-1);
	additem(list,g, "Keyboard Light +", act_bk2, (void*)(intptr_t)+1);
	additem(list,g, "Keyboard Light -", act_bk2, (void*)(intptr_t)-1);
	additem(list,g, "PSRAM Speed Test", act_memspeed, NULL);
	additem(list,g, "PSRAM Bus Check",  act_memcheck, NULL);
	additem(list,g, "Power...",         act_power, NULL);

	refresh_bat();
	refresh_clk();
	stimer = lv_timer_create(tick_timer, 1000, NULL);
	lv_screen_load(scr);
}
