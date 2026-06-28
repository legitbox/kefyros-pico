// apps/gameboy.c — Game Boy (DMG) emulator for Kefyros, built on Peanut-GB + minigb_apu.
//
// Flow: a grab-mode ROM picker lists /kefyros/roms/*.gb. Selecting one copies it into the
// reserved flash region (port/gbflash) so gb_rom_read() can index it from XIP at full speed,
// then we enter a self-contained play loop that monopolises the main loop — the LVGL desktop
// is frozen (Core 1 parked, we own the panel/SPI) while the loop still pumps kf_net_poll()
// every frame so WiFi + SNTP time stay alive. ESC saves cart RAM and returns to the launcher.
//
// Timing: the loop is paced to the real Game Boy frame period; audio (PWM via kf_audio) is
// produced every frame so it stays real-time, and the video blit is *skipped* on frames where
// we've fallen behind the deadline (the 25 MHz SPI can't always push a frame in 16.7 ms). So
// sound is always smooth; video drops frames under load (more so at 2x). TAB toggles 1x/2x.
#define ENABLE_SOUND        1
#define ENABLE_LCD          1
#define PEANUT_GB_12_COLOUR 0      // DMG: 2-bit shade only (no GBC palette layer bits)

#include "../kefyros.h"
#include "../ui/theme.h"
#include "gbflash.h"
#include "disp.h"                  // disp_pause_core1 / disp_resume_core1
#include "lcdspi/lcdspi.h"         // define_region_spi, spi_write_fast, draw_rect_spi, Pico_LCD_SPI_MOD
#include "minigb_apu.h"
#include "hardware/spi.h"          // spi_set_baudrate
#include "pico/time.h"             // time_us_64, busy_wait_until
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

/* Peanut-GB calls these globals when ENABLE_SOUND is set; bridge them to minigb_apu. */
static uint8_t audio_read(uint16_t addr);
static void    audio_write(uint16_t addr, uint8_t val);
#include "peanut_gb.h"

#define ROMDIR   "/kefyros/roms"
#define MAXROMS  256
#define GB_W 160
#define GB_H 144
#define GX1  ((LCD_W - GB_W) / 2)        // 1x: centred, x=80
#define GY1  ((LCD_H - GB_H) / 2)        // 1x: centred, y=88
#define GY2  ((LCD_H - GB_H * 2) / 2)    // 2x: 320x288 centred, y=16

/* Classic DMG green palette, RGB888, shade 0 (light) .. 3 (dark). */
static const uint8_t s_pal[4][3] = {
	{0xe0,0xf8,0xd0}, {0x88,0xc0,0x70}, {0x34,0x68,0x56}, {0x08,0x18,0x20}
};

enum { ST_OFF, ST_PICK, ST_PLAY };
static int s_state  = ST_OFF;
static int s_active = 0;

/* ---- picker ---- */
static lv_obj_t *s_scr, *s_listc, *s_status, *s_rows[MAXROMS];
static char s_names[MAXROMS][72];
static int  s_nrom, s_sel;

/* ---- emulator ---- */
static struct gb_s          *s_gb;
static struct minigb_apu_ctx s_apu;
static uint8_t  *s_fb;                 // GB_W*GB_H shade bytes (filled by lcd_line)
static uint8_t  *s_cartram;            // cart RAM (battery save), or NULL
static size_t    s_cartram_sz;
static char      s_savpath[300];
static int       s_scale = 1;          // 1 or 2
static volatile int s_gberr;
static uint8_t   s_btn;                // active-high JOYPAD_* mask of held buttons
static int16_t   s_audio[AUDIO_SAMPLES_TOTAL];
static uint8_t   s_line[GB_W * 2 * 3]; // one output row, RGB888 (room for 2x = 320px)

/* ===== Peanut-GB callbacks ===== */
/* ROM is in PSRAM behind a page cache. Bank 0 (addr < 0x4000) is pinned, so that path is
   a direct index. For the switchable bank we keep the active page's SRAM pointer and only
   re-consult the cache when the bank actually changes (an MBC switch) — so the hot path
   stays a compare + index even though the ROM isn't in SRAM. */
static const uint8_t *s_pg0;                       /* pinned bank-0 page */
static uint32_t       s_bpg = 0xFFFFFFFFu;         /* cached active bank page #          */
static const uint8_t *s_bptr;                      /* ...and its SRAM pointer            */
static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr){
	(void)gb;
	if(addr < 0x4000u) return s_pg0[addr];
	uint32_t pg = (uint32_t)addr >> 14;
	if(pg != s_bpg){
		draw_rect_spi(60, 100, 90, 130, 0xFFFF00);   /* DIAG: about to stream a page */
		s_bptr = gbflash_page(pg); s_bpg = pg;
		draw_rect_spi(60, 100, 90, 130, 0x00FFFF);   /* DIAG: stream returned OK */
	}
	return s_bptr[addr & 0x3FFFu];
}
static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr){
	(void)gb; return s_cartram ? s_cartram[addr] : 0xFF;
}
static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val){
	(void)gb; if(s_cartram) s_cartram[addr] = val;
}
static void gb_err_cb(struct gb_s *gb, const enum gb_error_e e, const uint16_t val){
	(void)gb; (void)val; s_gberr = (int)e + 1;     // flag; the play loop exits
}
static void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line){
	(void)gb; if(line < GB_H) memcpy(s_fb + (size_t)line * GB_W, pixels, GB_W);
}
static uint8_t audio_read(uint16_t addr){ return minigb_apu_audio_read(&s_apu, addr); }
static void    audio_write(uint16_t addr, uint8_t val){ minigb_apu_audio_write(&s_apu, addr, val); }

/* ===== display ===== */
static void blit_frame(void){
	if(s_scale == 2){
		define_region_spi(0, GY2, LCD_W - 1, GY2 + GB_H*2 - 1, 1);
		for(int y = 0; y < GB_H; y++){
			const uint8_t *row = s_fb + (size_t)y * GB_W;
			uint8_t *o = s_line;
			for(int x = 0; x < GB_W; x++){
				const uint8_t *c = s_pal[row[x] & 3];
				o[0]=o[3]=c[0]; o[1]=o[4]=c[1]; o[2]=o[5]=c[2]; o += 6;   // 2x horizontal
			}
			spi_write_fast(Pico_LCD_SPI_MOD, s_line, GB_W*2*3);          // 2x vertical:
			spi_write_fast(Pico_LCD_SPI_MOD, s_line, GB_W*2*3);          // same row twice
		}
	} else {
		define_region_spi(GX1, GY1, GX1 + GB_W - 1, GY1 + GB_H - 1, 1);
		for(int y = 0; y < GB_H; y++){
			const uint8_t *row = s_fb + (size_t)y * GB_W;
			uint8_t *o = s_line;
			for(int x = 0; x < GB_W; x++){
				const uint8_t *c = s_pal[row[x] & 3];
				*o++ = c[0]; *o++ = c[1]; *o++ = c[2];
			}
			spi_write_fast(Pico_LCD_SPI_MOD, s_line, GB_W*3);
		}
	}
	spi_finish(Pico_LCD_SPI_MOD);
	lcd_spi_raise_cs();
}

/* ===== cart RAM (.sav) ===== */
static void load_sav(void){
	if(!s_cartram || !s_cartram_sz) return;
	FILE *f = fopen(s_savpath, "rb");
	if(!f) return;
	fread(s_cartram, 1, s_cartram_sz, f);
	fclose(f);
}
static void save_sav(void){
	if(!s_cartram || !s_cartram_sz) return;
	FILE *f = fopen(s_savpath, "wb");
	if(!f) return;
	fwrite(s_cartram, 1, s_cartram_sz, f);
	fclose(f);
}

/* ===== teardown of a running game (back to the picker is not used; we exit to launcher) ===== */
static void stop_game_to_launcher(void){
	save_sav();
	kf_audio_stop();
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);   // restore the OS panel clock
	disp_resume_core1();                                 // hand the panel back to LVGL
	gbflash_free();                                      // release the ROM buffer
	free(s_gb);      s_gb = NULL;
	free(s_fb);      s_fb = NULL;
	free(s_cartram); s_cartram = NULL; s_cartram_sz = 0;
	s_active = 0; s_state = ST_OFF;
	kf_grab_input(0);
	kf_clock_normal();
	kf_back_to_launcher();
}

/* ===== input -> joypad ===== */
static uint8_t key_to_joypad(uint8_t k){
	switch(k){
	case DK_UP:    return JOYPAD_UP;
	case DK_DOWN:  return JOYPAD_DOWN;
	case DK_LEFT:  return JOYPAD_LEFT;
	case DK_RIGHT: return JOYPAD_RIGHT;
	case DK_F1+4:  return JOYPAD_A;        /* F5 = A (right of F4, thumb-friendly) */
	case DK_F1+3:  return JOYPAD_B;        /* F4 = B */
	case DK_ENTER:      return JOYPAD_START;
	case DK_BACKSPACE:  return JOYPAD_SELECT;
	default: return 0;
	}
}

/* ===== the play loop (runs from gameboy_poll, i.e. the superloop — safe to block) =====
 * Pacing is driven by the AUDIO ring, not a wall-clock timer: we emulate frames (each
 * producing one frame of audio) only while the ring has space, so emulation tracks the
 * DMA's real-time 32768 Hz drain. The blit happens once per outer pass and is allowed to
 * be slow — the ring is ~250 ms deep, so it doesn't underrun while a (44 ms) 2x frame
 * transfers. Video naturally drops frames under load; audio stays smooth. */
static void play_loop(void){
	uint64_t next_us = time_us_64();              /* wall-clock pacer (used only if audio is off) */
	while(s_state == ST_PLAY){
		/* input — this loop monopolises the superloop, so we must drain the keyboard
		   UART ourselves (the superloop's uart_poll() doesn't run while we're in here). */
		uint8_t st, key;
		uart_poll();
		while(uart_pop_key(&st, &key)){
			if(key == DK_ESC || key == DK_BREAK){ stop_game_to_launcher(); return; }
			if(st == KS_PRESS && key == DK_TAB){            /* toggle 1x/2x */
				s_scale = (s_scale == 1) ? 2 : 1;
				draw_rect_spi(0, 0, LCD_W-1, LCD_H-1, 0x000000);   /* clear the old size's pixels */
				continue;
			}
			uint8_t b = key_to_joypad(key);
			if(!b) continue;
			if(st == KS_RELEASE) s_btn &= ~b; else s_btn |= b;
		}
		s_gb->direct.joypad = (uint8_t)~s_btn;

		/* Pacing. PREFERRED: the audio ring drains at 32768 Hz, so emulating only while
		   it has space tracks real time AND produces sound. FALLBACK: if the ring failed
		   to allocate (heap starved by the page cache + cart RAM — big MBC carts), audio
		   never starts; pace by wall clock instead so the game still RUNS (silently)
		   rather than freezing on a black screen waiting for ring space that never frees. */
		int did = 0, guard = 0;
		if(kf_audio_running()){
			while(kf_audio_space() >= AUDIO_SAMPLES && guard++ < 24){
				gb_run_frame(s_gb);
				if(s_gberr){ stop_game_to_launcher(); return; }
				minigb_apu_audio_callback(&s_apu, s_audio);
				kf_audio_write(s_audio, AUDIO_SAMPLES);
				did = 1;
			}
		} else {
			uint64_t now = time_us_64();
			while(now >= next_us && guard++ < 4){     /* catch up at most 4 frames */
				gb_run_frame(s_gb);
				if(s_gberr){ stop_game_to_launcher(); return; }
				next_us += 16743u;                    /* DMG frame period (59.7 Hz) */
				did = 1;
			}
			if(next_us + 33486u < now) next_us = now; /* fell badly behind: resync */
		}

		if(did) blit_frame();                   /* show the most recent frame */
		else    tight_loop_contents();          /* ahead of real time: idle */

		kf_net_poll();                          // keep WiFi + SNTP time alive during play
	}
}

/* ===== start a game from the picker ===== */
static void set_status(const char *s){ if(s_status) lv_label_set_text(s_status, s ? s : ""); }

static void start_game(const char *name){
	char rompath[300];
	snprintf(rompath, sizeof rompath, ROMDIR "/%s", name);
	/* derive <name>.sav */
	snprintf(s_savpath, sizeof s_savpath, ROMDIR "/%s", name);
	char *dot = strrchr(s_savpath, '.');
	if(dot) strcpy(dot, ".sav"); else strncat(s_savpath, ".sav", sizeof s_savpath - strlen(s_savpath) - 1);

	set_status("loading ROM...");
	lv_refr_now(lv_display_get_default());      // show the message before the (blocking) read

	long sz = gbflash_load(rompath, NULL);
	if(sz < 0){
		set_status(sz == -2 ? "ROM too big for PSRAM" : "load failed");
		return;
	}
	s_pg0 = gbflash_page0();        /* pinned bank 0 — must be set before gb_init reads the header */
	s_bpg = 0xFFFFFFFFu;            /* invalidate the active-bank pointer cache */

	s_gb = malloc(sizeof *s_gb);
	s_fb = malloc((size_t)GB_W * GB_H);
	if(!s_gb || !s_fb){ gbflash_free(); free(s_gb); s_gb=NULL; free(s_fb); s_fb=NULL; set_status("out of memory"); return; }

	enum gb_init_error_e e = gb_init(s_gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_err_cb, NULL);
	if(e != GB_INIT_NO_ERROR){
		set_status(e == GB_INIT_CARTRIDGE_UNSUPPORTED ? "unsupported cartridge" : "bad ROM");
		gbflash_free(); free(s_gb); s_gb=NULL; free(s_fb); s_fb=NULL; return;
	}

	s_cartram_sz = 0;
	gb_get_save_size_s(s_gb, &s_cartram_sz);
	s_cartram = s_cartram_sz ? malloc(s_cartram_sz) : NULL;
	if(s_cartram_sz && !s_cartram){ set_status("out of memory"); gbflash_free(); free(s_gb); s_gb=NULL; free(s_fb); s_fb=NULL; return; }
	if(s_cartram){ memset(s_cartram, 0, s_cartram_sz); load_sav(); }

	minigb_apu_audio_init(&s_apu);
	gb_init_lcd(s_gb, lcd_line);
	gb_reset(s_gb);
	s_gberr = 0; s_btn = 0; s_scale = 1;

	/* enter play at the OS 360 MHz clock. clk_peri now follows clk_sys, so the panel SPI
	   runs at the full LCD_SPI_SPEED (90 MHz) — double the old hard-capped 45 — which is
	   what makes 2x usable. */
	kf_clock_normal();
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
	disp_pause_core1();
	draw_rect_spi(0, 0, LCD_W - 1, LCD_H - 1, 0x000000);
	kf_audio_start(AUDIO_SAMPLE_RATE);

	s_state = ST_PLAY;
}

/* ===== picker ===== */
static void pick_highlight(void){
	for(int i = 0; i < s_nrom; i++){
		int on = (i == s_sel);
		lv_obj_set_style_bg_opa(s_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(s_rows[i], KF_AMBER_DIM, 0);
		lv_obj_set_style_text_color(s_rows[i], on ? KF_BG_DEEP : KF_TEXT, 0);
	}
	if(s_nrom) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

static int has_ext(const char *n, const char *ext){
	size_t ln = strlen(n), le = strlen(ext);
	if(ln < le) return 0;
	for(size_t i = 0; i < le; i++){
		char a = n[ln-le+i], b = ext[i];
		if(a>='A'&&a<='Z') a += 32;
		if(a != b) return 0;
	}
	return 1;
}

static void scan_roms(void){
	s_nrom = 0;
	DIR *d = opendir(ROMDIR);
	if(!d) return;
	struct dirent *e;
	while((e = readdir(d)) && s_nrom < MAXROMS){
		if(e->d_name[0] == '.') continue;
		if(has_ext(e->d_name, ".gb") || has_ext(e->d_name, ".gbc"))
			snprintf(s_names[s_nrom++], sizeof s_names[0], "%s", e->d_name);
	}
	closedir(d);
}

void gameboy_poll(void){
	if(!s_active) return;

	if(s_state == ST_PLAY){ play_loop(); return; }

	/* ST_PICK: navigate the list with grabbed keys */
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		switch(key){
		case DK_ESC: case DK_BREAK:
			s_active = 0; s_state = ST_OFF; kf_grab_input(0); kf_back_to_launcher(); return;
		case DK_UP:   if(s_nrom){ s_sel = (s_sel + s_nrom - 1) % s_nrom; pick_highlight(); } break;
		case DK_DOWN: if(s_nrom){ s_sel = (s_sel + 1) % s_nrom; pick_highlight(); } break;
		case DK_ENTER:
			if(s_nrom){ start_game(s_names[s_sel]); return; }
			break;
		default: break;
		}
	}
}

void app_gameboy_open(void){
	mkdir(ROMDIR, 0777);             // ensure the roms dir exists (no-op if present)
	scan_roms();
	s_sel = 0;

	s_scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s_scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(s_scr, 0, 0);
	kf_inset_top(s_scr);
	lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(s_scr);
	lv_label_set_text(title, "Game Boy");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 1);

	int list_h = KF_CONTENT_H - 18 - 16;
	s_listc = lv_obj_create(s_scr);
	lv_obj_remove_style_all(s_listc);
	lv_obj_set_size(s_listc, LCD_W, list_h);
	lv_obj_align(s_listc, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_flex_flow(s_listc, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s_listc, 2, 0);
	lv_obj_set_style_pad_all(s_listc, 4, 0);
	lv_obj_set_scroll_dir(s_listc, LV_DIR_VER);

	for(int i = 0; i < s_nrom; i++){
		lv_obj_t *r = lv_label_create(s_listc);
		lv_label_set_text(r, s_names[i]);
		lv_obj_set_style_text_font(r, KF_FONT, 0);
		lv_obj_set_width(r, LCD_W - 12);
		lv_obj_set_style_pad_hor(r, 3, 0);
		lv_obj_set_style_radius(r, 0, 0);
		s_rows[i] = r;
	}

	s_status = lv_label_create(s_scr);
	lv_obj_set_style_text_font(s_status, KF_FONT, 0);
	lv_obj_set_style_text_color(s_status, KF_TEXT_DIM, 0);
	lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, 4, -2);
	set_status(s_nrom ? "ENTER play   arrows D-pad   F5=A F4=B   ESC quit" : "no ROMs in /kefyros/roms");
	pick_highlight();

	kf_grab_input(1);
	s_active = 1;
	s_state = ST_PICK;
	lv_screen_load(s_scr);
}
