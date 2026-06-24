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

/* --- PSRAM burn-in: combined throughput + integrity torture test. Hammers the WHOLE
   free region (everything above the allocator high-water, so it won't touch the live
   wallpaper/browser/music arenas) for ~2 s with a fresh pseudo-random pattern each pass,
   write-all then read-verify-all, counting any corrupted byte. Reports MB pounded,
   combined W+R throughput, pass count, and errors. Blocks (UI frozen) while it runs. --- */
#define BURN_BUF   8192u
#define BURN_US    2000000ull        /* ~2 second burn */

static void act_burn(lv_event_t *e){ (void)e;
	uint32_t sz = kf_psram_size();
	if(!sz){ lv_label_set_text(lbl_test, "Burn: no PSRAM"); return; }
	uint8_t *buf = malloc(BURN_BUF);
	if(!buf){ lv_label_set_text(lbl_test, "Burn: out of memory"); return; }

	uint32_t base = kf_psram_brk();           /* free region: [brk, size) */
	if(base > sz) base = 0;
	uint32_t span = (sz - base) - ((sz - base) % BURN_BUF);
	if(!span){ free(buf); lv_label_set_text(lbl_test, "Burn: no free PSRAM"); return; }

	uint64_t total = 0; uint32_t errors = 0, first_at = 0, passes = 0; uint8_t fe = 0, fg = 0;
	uint64_t t0 = time_us_64();
	while(time_us_64() - t0 < BURN_US){
		uint32_t pseed = 0x9E3779B9u ^ (passes * 2654435761u);     /* new pattern each pass */
		for(uint32_t off = 0; off < span; off += BURN_BUF){        /* write the whole region */
			uint32_t s = pseed ^ off;
			for(uint32_t i = 0; i < BURN_BUF; i++){ s = s*1664525u + 1013904223u; buf[i] = (uint8_t)(s >> 24); }
			kf_psram_write(base + off, buf, BURN_BUF);
		}
		total += span;
		for(uint32_t off = 0; off < span; off += BURN_BUF){        /* read it ALL back, verify */
			uint32_t s = pseed ^ off;
			kf_psram_read(base + off, buf, BURN_BUF);
			for(uint32_t i = 0; i < BURN_BUF; i++){
				s = s*1664525u + 1013904223u;
				if(buf[i] != (uint8_t)(s >> 24)){
					if(!errors){ first_at = base + off + i; fe = (uint8_t)(s >> 24); fg = buf[i]; }
					errors++;
				}
			}
		}
		total += span;
		passes++;
	}
	uint64_t dt = time_us_64() - t0;
	free(buf);

	uint32_t mbps10 = dt ? (uint32_t)(total * 10u / dt) : 0;       /* bytes/us *10 = MB/s.1 */
	uint32_t mb = (uint32_t)(total / (1024u*1024u));
	if(errors){
		lv_label_set_text_fmt(lbl_test, "BURN: %u ERR @%06X e%02X g%02X (%uMB)",
			(unsigned)errors, (unsigned)first_at, fe, fg, (unsigned)mb);
		lv_obj_set_style_text_color(lbl_test, lv_color_hex(0xe03c32), 0);
	} else {
		lv_label_set_text_fmt(lbl_test, "BURN OK: %uMB %u.%u MB/s %up 0err",
			(unsigned)mb, mbps10/10, mbps10%10, (unsigned)passes);
		lv_obj_set_style_text_color(lbl_test, KF_ACTIVE, 0);
	}
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
	if(ps) lv_label_set_text_fmt(lbl_ram, "PSRAM: %u MB  OK  @ %u MHz",
	           ps/(1024u*1024u), kf_psram_bus_hz()/1000000u);
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
	additem(list,g, "PSRAM Burn Test",  act_burn, NULL);
	additem(list,g, "Power...",         act_power, NULL);

	refresh_bat();
	refresh_clk();
	stimer = lv_timer_create(tick_timer, 1000, NULL);
	lv_screen_load(scr);
}
