// apps/gameboy.c - Game Boy / Game Boy Color for Kefyros.
//
// The ILI9488 GRAM is the 2x framebuffer: its persistent white surface is cleared
// once, then each Peanut-GB scanline is compared with a 160x144 RGB565 shadow.
// Changed source-pixel runs are emitted as 2-pixel-wide, 2-row-high rectangles.
// Busy scanlines collapse to one 320x2 transfer. No 320x288 SRAM framebuffer exists.

#define ENABLE_SOUND                 1
#define ENABLE_LCD                   1
#define PEANUT_GB_12_COLOUR          1
#define PEANUT_FULL_GBC_SUPPORT      1

#include "../kefyros.h"
#include "../ui/theme.h"
#include "../port/disp.h"
#include "../lib/peanut-gbc/minigb_apu.h"

#include "hardware/spi.h"
#include "pico/time.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

static uint8_t audio_read(uint16_t addr);
static void audio_write(uint16_t addr, uint8_t val);
/* The current pico-peanutGB header expects this frontend conversion helper
   even though Kefyros never uses its packed RGB444 palette. */
#define RGB555_TO_RGB444(c) (uint16_t)((((((c) >> 10) & 31u) >> 1) << 8) | \
	((((c) >> 5) & 31u) >> 1) << 4 | (((c) & 31u) >> 1))
#include "../lib/peanut-gbc/peanut_gb.h"
#undef LCD_WIDTH
#undef LCD_HEIGHT
#include "../port/lcdspi/lcdspi.h"

#define JOYPAD_A       0x01u
#define JOYPAD_B       0x02u
#define JOYPAD_SELECT  0x04u
#define JOYPAD_START   0x08u
#define JOYPAD_RIGHT   0x10u
#define JOYPAD_LEFT    0x20u
#define JOYPAD_UP      0x40u
#define JOYPAD_DOWN    0x80u

#define GB_W             160
#define GB_H             144
#define GB_Y             ((LCD_H - GB_H * 2) / 2)
#define ROM_ROOT         "/kefyros/roms"
#define ROM_SUBDIR       "/kefyros/roms/gb"
#define SAVE_DIR         "/kefyros/saves/gb"
#define MAX_ROMS         128
#define ROM_NAME         96
#define ROM_CHUNK        4096
#define BANK_SIZE        0x4000u
#define FULL_DIRTY       88
#define MAX_SPARSE_RUNS  10

enum { GB_OFF, GB_PICK, GB_PLAY };

typedef struct {
	char name[ROM_NAME];
	uint8_t subdir;
} rom_ent_t;

static int s_state;
static int s_active;
static lv_obj_t *s_scr, *s_list, *s_status, *s_rows[MAX_ROMS];
static rom_ent_t s_roms[MAX_ROMS];
static int s_nrom, s_sel;

static struct gb_s *s_gb;
static struct minigb_apu_ctx s_apu;
static uint16_t *s_prev;
static uint16_t s_line565[GB_W];
static uint8_t s_dirty[GB_W];
static uint8_t s_rgb[GB_W * 2 * 2];
static int16_t s_audio[1200]; /* AUDIO_SAMPLES_TOTAL is 1096 at 32768 Hz. */
static uint8_t s_buttons;
static volatile int s_gberr;
static uint32_t s_frames, s_rects, s_full_lines;

/* The ROM always occupies the first allocation in an exclusive PSRAM epoch. */
static uint32_t s_rom_off = 0xFFFFFFFFu;
static uint32_t s_rom_size;
static const uint8_t *s_rom_map;
static uint8_t *s_bank0, *s_bankn;
static uint32_t s_bank_page = 0xFFFFFFFFu;

static uint8_t *s_cart;
static uint32_t s_cart_off = 0xFFFFFFFFu;
static size_t s_cart_size;
static int s_cart_heap;
static char s_save_path[256];
static char s_load_detail[120];

/* Neutral DMG shades. Shade zero is literal panel white, which is also the
   retained background and therefore costs no SPI traffic while unchanged. */
static const uint16_t s_dmg[3][4] = {
	{0xffff, 0xad75, 0x52aa, 0x0000},
	{0xffff, 0xad75, 0x52aa, 0x0000},
	{0xffff, 0xad75, 0x52aa, 0x0000},
};

static void set_status(const char *s){
	if(s_status) lv_label_set_text(s_status, s ? s : "");
}

static uint16_t rgb555_to_565(uint16_t c){
	uint16_t r = (c >> 10) & 31u;
	uint16_t g = (c >> 5) & 31u;
	uint16_t b = c & 31u;
	return (uint16_t)((r << 11) | ((g << 1) << 5) | b);
}

static inline void rgb565_put2(uint8_t **dst, uint16_t c){
	uint16_t s = __builtin_bswap16(c);
	uint8_t *o = *dst;
	o[0] = o[2] = (uint8_t)(s & 0xFF);
	o[1] = o[3] = (uint8_t)(s >> 8);
	*dst = o + 4;
}

static void emit_run(int line, int x0, int x1){
	uint8_t *o = s_rgb;
	for(int x = x0; x <= x1; x++) rgb565_put2(&o, s_line565[x]);
	size_t n = (size_t)(x1 - x0 + 1) * 4u;
	int y = GB_Y + line * 2;
	define_region_spi(x0 * 2, y, x1 * 2 + 1, y + 1, 1);
	spi_write_fast(Pico_LCD_SPI_MOD, s_rgb, n);
	spi_write_fast(Pico_LCD_SPI_MOD, s_rgb, n);
	spi_finish(Pico_LCD_SPI_MOD);
	lcd_spi_raise_cs();
	s_rects++;
}

static void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line){
	if(line >= GB_H || !s_prev) return;
	uint16_t *old = s_prev + (size_t)line * GB_W;
	int dirty = 0, runs = 0, in_run = 0;
	for(int x = 0; x < GB_W; x++){
		uint16_t c;
		if(gb->cgb.cgbMode) c = rgb555_to_565(gb->cgb.fixPalette[pixels[x] & 0x3fu]);
		else c = s_dmg[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3u];
		s_line565[x] = c;
		s_dirty[x] = (uint8_t)(c != old[x]);
		if(s_dirty[x]){ dirty++; if(!in_run){ runs++; in_run = 1; } }
		else in_run = 0;
	}
	if(!dirty) return;
	if(dirty >= FULL_DIRTY || runs > MAX_SPARSE_RUNS){
		emit_run((int)line, 0, GB_W - 1);
		s_full_lines++;
	} else {
		int x = 0;
		while(x < GB_W){
			while(x < GB_W && !s_dirty[x]) x++;
			if(x >= GB_W) break;
			int x0 = x++;
			while(x < GB_W && s_dirty[x]) x++;
			emit_run((int)line, x0, x - 1);
		}
	}
	memcpy(old, s_line565, sizeof s_line565);
}

static void read_psram_partial(uint32_t off, uint8_t *dst, uint32_t n){
	if(off >= s_rom_size){ memset(dst, 0xff, n); return; }
	uint32_t take = s_rom_size - off;
	if(take > n) take = n;
	kf_psram_read(s_rom_off + off, dst, take);
	if(take < n) memset(dst + take, 0xff, n - take);
}

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr){
	(void)gb;
	uint32_t a = (uint32_t)addr;
	if(a >= s_rom_size) return 0xff;
	if(s_rom_map) return s_rom_map[a];
	if(a < BANK_SIZE) return s_bank0[a];
	uint32_t page = a / BANK_SIZE;
	if(page != s_bank_page){
		read_psram_partial(page * BANK_SIZE, s_bankn, BANK_SIZE);
		s_bank_page = page;
	}
	return s_bankn[a & (BANK_SIZE - 1u)];
}

static uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr){
	(void)gb;
	return s_cart && addr < s_cart_size ? s_cart[addr] : 0xff;
}

static void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val){
	(void)gb;
	if(s_cart && addr < s_cart_size) s_cart[addr] = val;
}

static void gb_err(struct gb_s *gb, const enum gb_error_e e, const uint16_t val){
	(void)gb; (void)val;
	s_gberr = (int)e + 1;
}

static uint8_t audio_read(uint16_t addr){ return minigb_apu_audio_read(&s_apu, addr); }
static void audio_write(uint16_t addr, uint8_t val){ minigb_apu_audio_write(&s_apu, addr, val); }

static void save_cart(void){
	if(!s_cart || !s_cart_size) return;
	FILE *f = fopen(s_save_path, "wb");
	if(!f) return;
	fwrite(s_cart, 1, s_cart_size, f);
	fclose(f);
}

static void load_cart(void){
	if(!s_cart || !s_cart_size) return;
	memset(s_cart, 0, s_cart_size);
	FILE *f = fopen(s_save_path, "rb");
	if(!f) return;
	fread(s_cart, 1, s_cart_size, f);
	fclose(f);
}

static void free_runtime(void){
	free(s_gb); s_gb = NULL;
	free(s_prev); s_prev = NULL;
	free(s_bank0); s_bank0 = NULL;
	free(s_bankn); s_bankn = NULL;
	if(s_cart_heap) free(s_cart);
	s_cart = NULL; s_cart_heap = 0; s_cart_size = 0;
	s_rom_map = NULL; s_rom_off = s_cart_off = 0xFFFFFFFFu;
	s_rom_size = 0; s_bank_page = 0xFFFFFFFFu;
}

static void reset_exclusive_psram(void){
	kf_psram_clients_invalidate();
	kf_psram_reset_alloc();
}

static void stop_game(void){
	save_cart();
	kf_audio_stop();
	free_runtime();
	reset_exclusive_psram();
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
	disp_resume_core1();
	s_state = GB_OFF; s_active = 0;
	kf_grab_input(0);
	launcher_show();
	lv_obj_invalidate(lv_screen_active());
}

static uint8_t joy_for_key(uint8_t key){
	switch(key){
	case DK_UP: return JOYPAD_UP;
	case DK_DOWN: return JOYPAD_DOWN;
	case DK_LEFT: return JOYPAD_LEFT;
	case DK_RIGHT: return JOYPAD_RIGHT;
	case DK_F1 + 4: return JOYPAD_A;       /* F5 */
	case DK_F1 + 3: return JOYPAD_B;       /* F4 */
	case DK_ENTER: return JOYPAD_START;
	case DK_BACKSPACE: return JOYPAD_SELECT;
	default: return 0;
	}
}

static void play_loop(void){
	uint64_t next_us = time_us_64();
	while(s_state == GB_PLAY){
		uart_poll();
		uint8_t st, key;
		while(uart_pop_key(&st, &key)){
			if((key == DK_ESC || key == DK_BREAK) && st != KS_RELEASE){ stop_game(); return; }
			uint8_t b = joy_for_key(key);
			if(!b) continue;
			if(st == KS_RELEASE) s_buttons &= (uint8_t)~b;
			else s_buttons |= b;
		}
		s_gb->direct.joypad = (uint8_t)~s_buttons;

		int ran = 0, guard = 0;
		if(kf_audio_running()){
			while(kf_audio_space() >= AUDIO_SAMPLES && guard++ < 8){
				gb_run_frame(s_gb);
				if(s_gberr){ stop_game(); return; }
				minigb_apu_audio_callback(&s_apu, s_audio);
				kf_audio_write(s_audio, AUDIO_SAMPLES);
				s_frames++; ran = 1;
			}
		} else {
			uint64_t now = time_us_64();
			while(now >= next_us && guard++ < 3){
				gb_run_frame(s_gb);
				if(s_gberr){ stop_game(); return; }
				next_us += 16743u;
				s_frames++; ran = 1;
			}
			if(next_us + 33486u < now) next_us = now;
		}
		if(!ran) tight_loop_contents();
	}
}

static int load_rom_file(const char *path){
	s_load_detail[0] = 0;
	FILE *f = fopen(path, "rb");
	if(!f){ snprintf(s_load_detail, sizeof s_load_detail, "Cannot open ROM"); return -1; }
	if(fseek(f, 0, SEEK_END) != 0){ fclose(f); return -1; }
	long z = ftell(f);
	uint32_t psz = kf_psram_size();
	if(!psz){ fclose(f); snprintf(s_load_detail, sizeof s_load_detail, "PSRAM OFFLINE after init/reset"); return -2; }
	if(z < 0x150){ fclose(f); snprintf(s_load_detail, sizeof s_load_detail, "ROM file is truncated (%ld bytes)", z); return -1; }
	if(z > (long)psz){ fclose(f); snprintf(s_load_detail, sizeof s_load_detail, "ROM %ldKB > PSRAM %luKB", z/1024, (unsigned long)(psz/1024)); return -2; }
	rewind(f);
	s_rom_size = (uint32_t)z;
	s_rom_off = kf_psram_alloc(s_rom_size);
	if(s_rom_off == 0xFFFFFFFFu){
		fclose(f);
		snprintf(s_load_detail, sizeof s_load_detail, "PSRAM alloc failed: ROM %luKB, brk %lu/%luKB",
		         (unsigned long)(s_rom_size/1024), (unsigned long)(kf_psram_brk()/1024),
		         (unsigned long)(psz/1024));
		return -2;
	}
	uint8_t buf[ROM_CHUNK];
	uint32_t at = 0;
	while(at < s_rom_size){
		uint32_t n = s_rom_size - at;
		if(n > sizeof buf) n = sizeof buf;
		if(fread(buf, 1, n, f) != n){ fclose(f); return -1; }
		kf_psram_write(s_rom_off + at, buf, n);
		at += n;
	}
	fclose(f);
	s_rom_map = (const uint8_t *)kf_psram_map(s_rom_off, s_rom_size);
	if(!s_rom_map){
		s_bank0 = malloc(BANK_SIZE);
		s_bankn = malloc(BANK_SIZE);
		if(!s_bank0 || !s_bankn) return -3;
		read_psram_partial(0, s_bank0, BANK_SIZE);
	}
	return 0;
}

static void build_save_path(const char *rom_name){
	char stem[ROM_NAME];
	snprintf(stem, sizeof stem, "%s", rom_name);
	char *dot = strrchr(stem, '.');
	if(dot) *dot = 0;
	for(char *p = stem; *p; p++) if(*p == '/' || *p == '\\' || *p == ':') *p = '_';
	snprintf(s_save_path, sizeof s_save_path, SAVE_DIR "/%s.sav", stem);
}

static void start_game(const rom_ent_t *ent){
	char path[256];
	snprintf(path, sizeof path, "%s/%s", ent->subdir ? ROM_SUBDIR : ROM_ROOT, ent->name);
	set_status("Loading ROM into PSRAM...");
	lv_refr_now(lv_display_get_default());

	reset_exclusive_psram();
	int lr = load_rom_file(path);
	if(lr){
		free_runtime(); reset_exclusive_psram();
		set_status(s_load_detail[0] ? s_load_detail : lr == -3 ? "Not enough SRAM for ROM cache" : "ROM read failed");
		return;
	}

	s_gb = calloc(1, sizeof *s_gb);
	s_prev = malloc((size_t)GB_W * GB_H * sizeof *s_prev);
	if(!s_gb || !s_prev){ free_runtime(); reset_exclusive_psram(); set_status("Not enough SRAM for emulator"); return; }
	for(size_t i = 0; i < (size_t)GB_W * GB_H; i++) s_prev[i] = 0xffff;

	enum gb_init_error_e err = gb_init(s_gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_err, NULL);
	if(err != GB_INIT_NO_ERROR){
		free_runtime(); reset_exclusive_psram();
		set_status(err == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Unsupported cartridge/MBC" : "Invalid ROM");
		return;
	}

	s_cart_size = (size_t)gb_get_save_size(s_gb);
	if(s_cart_size){
		s_cart_off = kf_psram_alloc((uint32_t)s_cart_size);
		s_cart = s_cart_off == 0xFFFFFFFFu ? NULL : kf_psram_map(s_cart_off, (uint32_t)s_cart_size);
		if(!s_cart){ s_cart = malloc(s_cart_size); s_cart_heap = 1; }
		if(!s_cart){ free_runtime(); reset_exclusive_psram(); set_status("Not enough RAM for cartridge save"); return; }
	}
	build_save_path(ent->name);
	load_cart();

	minigb_apu_audio_init(&s_apu);
	gb_init_lcd(s_gb, lcd_line);
	s_gb->direct.interlace = 0;
	s_gb->direct.frame_skip = 0;
	gb_reset(s_gb);
	struct tm now;
	if(kf_time_local(&now)) gb_set_rtc(s_gb, &now);
	s_gberr = 0; s_buttons = 0;
	s_frames = s_rects = s_full_lines = 0;
	/* 4096 stereo frames = 125 ms at 32768 Hz, ample for a worst-case 2x
	   refresh but only a 16 KB contiguous allocation. Never launch silently. */
	if(!kf_audio_start_buffered(AUDIO_SAMPLE_RATE, 4096)){
		free_runtime(); reset_exclusive_psram();
		set_status("Not enough SRAM for audio ring");
		return;
	}

	/* Clock first (clock changes briefly coordinate with Core 1), then park Core 1
	   for the whole modal session and take direct ownership of the panel. */
	kf_clock_normal();
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
	disp_pause_core1();
	draw_rect_spi(0, 0, LCD_W - 1, LCD_H - 1, WHITE);
	s_state = GB_PLAY;
}

static int has_rom_ext(const char *name){
	const char *dot = strrchr(name, '.');
	return dot && (!strcasecmp(dot, ".gb") || !strcasecmp(dot, ".gbc"));
}

static void scan_dir(const char *path, int subdir){
	DIR *d = opendir(path);
	if(!d) return;
	struct dirent *e;
	while(s_nrom < MAX_ROMS && (e = readdir(d))){
		if(e->d_name[0] == '.' || !has_rom_ext(e->d_name)) continue;
		snprintf(s_roms[s_nrom].name, sizeof s_roms[s_nrom].name, "%s", e->d_name);
		s_roms[s_nrom].subdir = (uint8_t)subdir;
		s_nrom++;
	}
	closedir(d);
}

static int rom_cmp(const void *a, const void *b){
	return strcasecmp(((const rom_ent_t *)a)->name, ((const rom_ent_t *)b)->name);
}

static void scan_roms(void){
	s_nrom = 0;
	scan_dir(ROM_SUBDIR, 1);
	scan_dir(ROM_ROOT, 0);
	qsort(s_roms, (size_t)s_nrom, sizeof s_roms[0], rom_cmp);
}

static void highlight(void){
	for(int i = 0; i < s_nrom; i++){
		int on = i == s_sel;
		lv_obj_set_style_bg_opa(s_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(s_rows[i], KF_AMBER_DIM, 0);
		lv_obj_set_style_text_color(s_rows[i], on ? KF_BG_DEEP : KF_TEXT, 0);
	}
	if(s_nrom) lv_obj_scroll_to_view(s_rows[s_sel], LV_ANIM_OFF);
}

void gameboy_poll(void){
	if(!s_active) return;
	if(s_state == GB_PLAY){ play_loop(); return; }
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		if(key == DK_ESC || key == DK_BREAK){ s_active = 0; s_state = GB_OFF; kf_grab_input(0); launcher_show(); return; }
		if(key == DK_UP && s_nrom){ s_sel = (s_sel + s_nrom - 1) % s_nrom; highlight(); }
		else if(key == DK_DOWN && s_nrom){ s_sel = (s_sel + 1) % s_nrom; highlight(); }
		else if(key == DK_ENTER && s_nrom){ start_game(&s_roms[s_sel]); return; }
	}
}

void app_gameboy_open(void){
	mkdir(ROM_ROOT, 0777);
	mkdir(ROM_SUBDIR, 0777);
	mkdir("/kefyros/saves", 0777);
	mkdir(SAVE_DIR, 0777);
	scan_roms(); s_sel = 0;

	s_scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s_scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(s_scr, 0, 0);
	kf_inset_top(s_scr);
	lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *title = lv_label_create(s_scr);
	lv_label_set_text(title, "Game Boy / Color");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 1);

	s_list = lv_obj_create(s_scr);
	lv_obj_remove_style_all(s_list);
	lv_obj_set_size(s_list, LCD_W, KF_CONTENT_H - 38);
	lv_obj_align(s_list, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s_list, 2, 0);
	lv_obj_set_style_pad_all(s_list, 4, 0);
	lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
	for(int i = 0; i < s_nrom; i++){
		s_rows[i] = lv_label_create(s_list);
		lv_label_set_text(s_rows[i], s_roms[i].name);
		lv_obj_set_style_text_font(s_rows[i], KF_FONT, 0);
		lv_obj_set_width(s_rows[i], LCD_W - 12);
		lv_obj_set_style_pad_hor(s_rows[i], 3, 0);
		lv_obj_set_style_radius(s_rows[i], 0, 0);
	}
	s_status = lv_label_create(s_scr);
	lv_obj_set_style_text_font(s_status, KF_FONT, 0);
	lv_obj_set_style_text_color(s_status, KF_TEXT_DIM, 0);
	lv_obj_align(s_status, LV_ALIGN_BOTTOM_LEFT, 4, -2);
	set_status(s_nrom ? "ENTER play | arrows D-pad | F5 A | F4 B | ESC quit" : "Put .gb/.gbc in /kefyros/roms/gb");
	highlight();
	kf_grab_input(1);
	s_active = 1; s_state = GB_PICK;
	lv_screen_load(s_scr);
}
