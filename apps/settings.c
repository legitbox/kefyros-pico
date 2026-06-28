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
#include "../port/disp.h"            /* disp_pause_core1 / disp_resume_core1 */
#include "../port/clock.h"           /* kf_clock_set_bare — overclock ladder for the speed test */
#include "lcdspi/lcdspi.h"          /* direct panel blit + LCD_SPI_SPEED */
#include "hardware/spi.h"           /* spi_set_baudrate / spi_get_baudrate */
#include "hardware/vreg.h"          /* vreg_get_voltage + VREG_VOLTAGE_* for the OC ladder */
#include "pico/time.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern char font8x8_basic[128][8];   /* ui/font8x8.c */

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

/* --- Screen test: full-screen direct-SPI blit loop. Shows live FPS + the actual panel
   SPI clock, lets you crank the clock with UP/DOWN to find the corruption ceiling, ESC
   quits. Scrolling colour bars make tearing/corruption obvious. Takes over the panel
   (Core1 parked, grab-mode keys) and hands it back to LVGL on exit. --- */
static void st_putc(int x, int y, unsigned char ch, int scale, int fg, int bg){
	unsigned char t[8];
	const unsigned char *g = (const unsigned char*)font8x8_basic[ch & 0x7f];
	for(int i = 0; i < 8; i++){            /* font8x8 is LSB-first; draw_bitmap_spi wants MSB-first */
		unsigned char v = g[i], r = 0;
		for(int b = 0; b < 8; b++) if((v >> b) & 1) r |= (unsigned char)(1 << (7 - b));
		t[i] = r;
	}
	draw_bitmap_spi(x, y, 8, 8, scale, fg, bg, t);
}
static void st_puts(int x, int y, const char *s, int scale, int fg, int bg){
	for(; *s; s++, x += 8*scale) st_putc(x, y, (unsigned char)*s, scale, fg, bg);
}

static void act_screentest(lv_event_t *e){ (void)e;
	static const uint8_t pal[6][3] = {
		{255,255,255},{255,0,0},{0,255,0},{0,0,255},{255,255,0},{0,255,255}
	};
	static uint8_t row[LCD_W * 3];        /* one RGB888 scanline (static: off the stack) */
	lv_obj_t *back = lv_screen_active();

	/* Overclock ladder. Panel SPI = clk_sys/4 on each rung: 400->100, 420->105 MHz. Capped at
	   420/105 - the panel SPI corrupts above ~110, so 420 is the practical ceiling. The 420 rung
	   overvolts to 1.35 V (above the 1.30 V longevity cap) - held ONLY while this test is open;
	   the entry clock + voltage are restored on exit (ESC). */
	static const struct { uint32_t khz; enum vreg_voltage v; uint32_t spi; } STEP[] = {
		{400000, VREG_VOLTAGE_1_30, 100000000u},   /* 400/4 = 100  (UI default) */
		{420000, VREG_VOLTAGE_1_35, 105000000u},   /* 420/4 = 105  (fastest)    */
	};
	const int NSTEP = (int)(sizeof STEP / sizeof STEP[0]);
	int step = 0;
	const uint32_t entry_khz = clock_sys_mhz() * 1000u;   /* restore clk_sys on exit  */
	const enum vreg_voltage entry_v = vreg_get_voltage(); /* restore rail on exit     */

	kf_grab_input(1);                     /* raw keys to us, not LVGL */
	disp_pause_core1();                   /* take the panel from the flush pump */
	spi_set_baudrate(Pico_LCD_SPI_MOD, STEP[step].spi);

	const int TEXT_H = 44;            /* top strip reserved for the readout (bars stay below it) */
	int frame = 0, fps = 0, fcount = 0, running = 1;
	int shown_fps = -1; uint32_t shown_mhz = 0;
	uint64_t t_fps = time_us_64();
	st_puts(6, 28, "UP/DN clock  ESC quit", 1, 0xb6f000, 0x000000);   /* static hint, drawn once */
	while(running){
		uint8_t kst, key; uart_poll();
		while(uart_pop_key(&kst, &key)){
			if(key == DK_ESC || key == DK_BREAK){ running = 0; break; }
			if(kst == KS_PRESS && key == DK_UP && step < NSTEP-1){
				/* climb a rung: raise the rail+clock, then set the panel SPI (clk_sys/4) */
				if(kf_clock_set_bare(STEP[step+1].khz, STEP[step+1].v, true)){
					step++;
					spi_set_baudrate(Pico_LCD_SPI_MOD, STEP[step].spi);
				}                          /* PLL rejected the rate -> stay on this rung */
			} else if(kst == KS_PRESS && key == DK_DOWN && step > 0){
				/* drop a rung: clock+rail down first, then SPI */
				if(kf_clock_set_bare(STEP[step-1].khz, STEP[step-1].v, false)){
					step--;
					spi_set_baudrate(Pico_LCD_SPI_MOD, STEP[step].spi);
				}
			}
		}
		if(!running) break;

		/* one scrolling-bar scanline pushed to the whole panel BELOW the text strip */
		for(int x = 0; x < LCD_W; x++){
			const uint8_t *c = pal[((x + frame) / 24) % 6];
			row[x*3] = c[0]; row[x*3+1] = c[1]; row[x*3+2] = c[2];
		}
		define_region_spi(0, TEXT_H, LCD_W - 1, LCD_H - 1, 1);
		for(int y = TEXT_H; y < LCD_H; y++) spi_write_fast(Pico_LCD_SPI_MOD, row, LCD_W * 3);
		spi_finish(Pico_LCD_SPI_MOD);
		lcd_spi_raise_cs();

		/* repaint the readout ONLY when it changes (the strip is never bar-filled, so no flicker) */
		uint32_t mhz = spi_get_baudrate(Pico_LCD_SPI_MOD) / 1000000u;
		if(fps != shown_fps || mhz != shown_mhz){
			char buf[64];
			snprintf(buf, sizeof buf, "SPI %lu  SYS %lu  FPS %d  ",
			         (unsigned long)mhz, (unsigned long)clock_sys_mhz(), fps);
			draw_rect_spi(0, 0, LCD_W - 1, 19, 0x000000);
			st_puts(6, 6, buf, 2, 0xffc94d, 0x000000);
			shown_fps = fps; shown_mhz = mhz;
		}

		frame++; fcount++;
		uint64_t now = time_us_64();
		if(now - t_fps >= 500000ull){
			fps = (int)((uint64_t)fcount * 1000000ull / (now - t_fps));
			fcount = 0; t_fps = now;
		}
		kf_net_poll();                /* keep WiFi/time alive like the GB loop does */
	}

	/* Restore the core clock + voltage we entered with (drop down from whatever rung
	   we left on), then hand the panel back at the normal OS SPI speed. */
	if(step != 0) kf_clock_set_bare(entry_khz, entry_v, entry_khz > STEP[step].khz);
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);   /* restore the OS panel clock */
	disp_resume_core1();
	kf_grab_input(0);
	lv_obj_invalidate(back);          /* force LVGL to repaint the settings screen */
}

static lv_obj_t *additem(lv_obj_t *list, lv_group_t *g, const char *txt,
                         lv_event_cb_t cb, void *ud){
	lv_obj_t *b = lv_list_add_button(list, NULL, txt);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
	lv_group_add_obj(g, b);
	return b;
}

/* OS sound-effects on/off (deskconf "sfx", default on). The button's label child shows
   the live state and flips on each press. */
static lv_obj_t *btn_sfx;
static void sfx_label(void){
	lv_obj_t *l = lv_obj_get_child(btn_sfx, 0);
	if(l) lv_label_set_text_fmt(l, "Sound FX: %s", deskconf_get_int("sfx",1) ? "ON" : "off");
}
static void act_sfx(lv_event_t *e){ (void)e;
	deskconf_set_int("sfx", !deskconf_get_int("sfx",1));
	sfx_label();
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
	additem(list,g, "Screen Test",      act_screentest, NULL);
	btn_sfx = additem(list,g, "Sound FX: ON", act_sfx, NULL); sfx_label();
	additem(list,g, "Power...",         act_power, NULL);

	refresh_bat();
	refresh_clk();
	stimer = lv_timer_create(tick_timer, 1000, NULL);
	lv_screen_load(scr);
}
