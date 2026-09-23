// apps/gameboy.c - Game Boy emulator for Kefyros (DMG, RGB111).
//
// Display is exact 2x scaled (160x144 -> 320x288), centered vertically in the
// 320x320 panel. The panel is switched to RGB111 mode (COLMOD 0x22: 3-bit colour,
// 2 pixels packed per wire byte). Each 320-pixel physical row is exactly 160
// bytes on the wire. A scanline is streamed directly to SPI in ~37 us with
// hardware 2x2 physical sub-pixel Bayer dithering for the 4 DMG shades.
// No 46 KB shadow framebuffer or sparse dirty run diffing needed.

#define ENABLE_SOUND                 1
#define ENABLE_LCD                   1
#define PEANUT_GB_12_COLOUR          0
#define PEANUT_FULL_GBC_SUPPORT      0

#include "../kefyros.h"
#include "../port/bt_audio.h"
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

#define GB_W           160
#define GB_H           144
#define GB_Y           ((LCD_H - GB_H * 2) / 2)  /* 16 px top/bottom border */
#define ROM_ROOT       "/kefyros/roms"
#define ROM_SUBDIR     "/kefyros/roms/gb"
#define SAVE_DIR       "/kefyros/saves/gb"
#define MAX_ROMS       128
#define ROM_NAME       96
#define BANK_SIZE      0x4000u
#define BANK_SLOTS     8          /* switchable-bank cache; slots past the first are best-effort */

enum { GB_OFF, GB_PICK, GB_PLAY };

typedef struct {
	char name[ROM_NAME];
	uint8_t subdir;
} rom_ent_t;

static int s_state;
static int s_active;
static lv_obj_t *s_scr, *s_list, *s_status, *s_rows[MAX_ROMS];
static rom_ent_t *s_roms; /* idle KAPI arena while this built-in owns the screen */
static int s_roms_heap;
static int s_nrom, s_sel;

static struct gb_s *s_gb;
static struct minigb_apu_ctx s_apu;
static uint8_t s_row0[GB_W];
static uint8_t s_row1[GB_W];
static int16_t s_audio[1200]; /* AUDIO_SAMPLES_TOTAL is 1096 at 32768 Hz. */
static uint8_t s_buttons;
static volatile int s_gberr;
static uint32_t s_frames;

/* ROM backing and bank cache: Bank 0 is always cached. Switchable banks live in up to
   BANK_SLOTS SRAM slots (LRU), so games that bounce between a few banks every frame
   (GB Studio trampolines, music drivers) stop re-copying 16 KB from PSRAM per switch. */
static uint32_t s_rom_off = 0xFFFFFFFFu;
static uint32_t s_rom_size;
static uint8_t *s_bank0, *s_bankn;           /* s_bankn == s_slot[s_cur] */
static uint32_t s_bank_page = 0xFFFFFFFFu;   /* page held by s_bankn */
static uint8_t *s_slot[BANK_SLOTS];
static uint32_t s_slot_page[BANK_SLOTS];
static uint32_t s_slot_used[BANK_SLOTS];
static int s_nslot, s_cur;
static uint32_t s_tick;

static uint8_t *s_cart;
static size_t s_cart_size;
static int s_cart_heap;
static char s_save_path[256];
static char s_load_detail[120];

/* RGB111 2x2 monochrome dither lookup:
   Each Game Boy pixel expands to a 2x2 physical pixel block.
   In RGB111 (COLMOD 0x22), 2 horizontal pixels are packed in 1 byte:
     bit 5..3 = pixel 0 (RGB), bit 2..0 = pixel 1 (RGB)
     RGB111_WHITE = 0b111 (7), RGB111_BLACK = 0b000 (0)
   Shade 0 (White, 100% W): 4 white pixels -> (W,W), (W,W) = 0x3F, 0x3F
   Shade 1 (Light Grey, 50% W): 2 white pixels -> checkerboard 0x38/0x07
   Shade 2 (Dark Grey, 25% W): 1 white pixel -> diagonal lattice 0x07/0x00
   Shade 3 (Black, 0% W): 4 black pixels -> (B,B), (B,B) = 0x00, 0x00 */
static const uint8_t s_row0_lut[4][2] = {
	{ 0x3F, 0x3F },
	{ 0x38, 0x07 },
	{ 0x07, 0x00 },
	{ 0x00, 0x00 },
};

static const uint8_t s_row1_lut[4][2] = {
	{ 0x3F, 0x3F },
	{ 0x07, 0x38 },
	{ 0x00, 0x38 },
	{ 0x00, 0x00 },
};

static void set_status(const char *s){
	if(s_status) lv_label_set_text(s_status, s ? s : "");
}

static void lcd_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line){
	(void)gb;
	if(line >= GB_H) return;
	int lp = (int)(line & 1);
	for(int x = 0; x < GB_W; x++){
		uint8_t c = pixels[x] & 3u;
		int phase = (x ^ lp) & 1;
		s_row0[x] = s_row0_lut[c][phase];
		s_row1[x] = s_row1_lut[c][phase];
	}
	int y = GB_Y + (int)line * 2;
	define_region_spi(0, y, LCD_W - 1, y + 1, 1);
	spi_write_fast(Pico_LCD_SPI_MOD, s_row0, GB_W);
	spi_write_fast(Pico_LCD_SPI_MOD, s_row1, GB_W);
	spi_finish(Pico_LCD_SPI_MOD);
	lcd_spi_raise_cs();
	if((line & 15u) == 0) kf_bt_service_audio();
}

static void read_psram_partial(uint32_t off, uint8_t *dst, uint32_t n){
	if(off >= s_rom_size){ memset(dst, 0xff, n); return; }
	uint32_t take = s_rom_size - off;
	if(take > n) take = n;
	kf_psram_read(s_rom_off + off, dst, take);
	if(take < n) memset(dst + take, 0xff, n - take);
}

static void __not_in_flash_func(select_bank)(uint32_t page){
	int victim = 0;
	for(int i = 0; i < s_nslot; i++){
		if(s_slot_page[i] == page){ victim = i; goto hit; }
		if(s_slot_used[i] < s_slot_used[victim]) victim = i;
	}
	read_psram_partial(page * BANK_SIZE, s_slot[victim], BANK_SIZE);
	s_slot_page[victim] = page;
hit:
	s_slot_used[victim] = ++s_tick;
	s_cur = victim;
	s_bankn = s_slot[victim];
	s_bank_page = page;
}

static uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr){
	(void)gb;
	uint32_t a = (uint32_t)addr;
	if(a >= s_rom_size) return 0xff;
	if(a < BANK_SIZE) return s_bank0[a];
	if(s_rom_size <= 2u * BANK_SIZE) return s_bankn[a - BANK_SIZE];
	uint32_t page = a / BANK_SIZE;
	if(page != s_bank_page) select_bank(page);
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
	free(s_bank0); s_bank0 = NULL;
	for(int i = 0; i < BANK_SLOTS; i++){ free(s_slot[i]); s_slot[i] = NULL; }
	s_bankn = NULL; s_nslot = 0; s_cur = 0; s_tick = 0;
	if(s_cart_heap) free(s_cart);
	s_cart = NULL; s_cart_heap = 0; s_cart_size = 0;
	s_rom_off = 0xFFFFFFFFu;
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
	if(s_roms_heap) free(s_roms);
	s_roms = NULL; s_roms_heap = 0;
	reset_exclusive_psram();

	/* Restore display controller to standard 16-bit RGB565 */
	spi_write_command(0x3A);
	spi_write_data(0x55);

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
	case DK_F1 + 4: case 'z': case 'Z': case 'a': case 'A': return JOYPAD_A;       /* F5, Z, A */
	case DK_F1 + 3: case 'x': case 'X': case 'b': case 'B': case 's': case 'S': return JOYPAD_B; /* F4, X, B */
	case DK_ENTER: return JOYPAD_START;
	case DK_BACKSPACE: case DK_TAB: case ' ': return JOYPAD_SELECT;
	default: return 0;
	}
}

static void play_loop(void){
	uint64_t next_us = time_us_64();
	while(s_state == GB_PLAY){
		kf_bt_service_audio();
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
		if(kf_audio_bt_route() && kf_audio_running()){
			uint64_t now = time_us_64();
			while(now >= next_us && guard++ < 2){
				gb_run_frame(s_gb);
				if(s_gberr){ stop_game(); return; }
				minigb_apu_audio_callback(&s_apu, s_audio);
				/* Radio congestion may drop sound, but must not stop emulation. */
				kf_audio_write(s_audio, AUDIO_SAMPLES);
				next_us += 16743u;
				s_frames++; ran = 1;
				now = time_us_64();
			}
			if(next_us + 33486u < now) next_us = now;
		} else if(kf_audio_running()){
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
	if(z < 0x150){ fclose(f); snprintf(s_load_detail, sizeof s_load_detail, "ROM truncated (%ld B)", z); return -1; }
	rewind(f);
	s_rom_size = (uint32_t)z;

	s_bank0 = malloc(BANK_SIZE);
	s_bankn = s_slot[0] = malloc(BANK_SIZE);
	s_nslot = s_bankn ? 1 : 0;
	if(!s_bank0 || !s_bankn){
		fclose(f);
		snprintf(s_load_detail, sizeof s_load_detail, "Out of SRAM for ROM banks");
		return -3;
	}

	if(s_rom_size <= 2u * BANK_SIZE){
		/* ROM <= 32 KB (e.g. Tetris): holds Bank 0 and Bank 1 directly in SRAM.
		   Bypasses PSRAM completely: 100% stable, instant loading! */
		size_t r0 = fread(s_bank0, 1, BANK_SIZE, f);
		size_t r1 = 0;
		if(s_rom_size > BANK_SIZE){
			r1 = fread(s_bankn, 1, s_rom_size - BANK_SIZE, f);
			if(s_rom_size - BANK_SIZE < BANK_SIZE)
				memset(s_bankn + (s_rom_size - BANK_SIZE), 0xff, BANK_SIZE - (s_rom_size - BANK_SIZE));
		} else {
			memset(s_bankn, 0xff, BANK_SIZE);
		}
		fclose(f);
		if(r0 + r1 != s_rom_size){
			snprintf(s_load_detail, sizeof s_load_detail, "ROM read error (%zu/%lu)", r0 + r1, (unsigned long)s_rom_size);
			return -1;
		}
		s_bank_page = 1;
		return 0;
	}

	/* ROM > 32 KB: allocate in PSRAM, stream via s_bank0/s_bankn buffers (no stack buffer) */
	uint32_t psz = kf_psram_size();
	if(!psz){
		fclose(f);
		snprintf(s_load_detail, sizeof s_load_detail, "PSRAM OFFLINE for >32KB ROM");
		return -2;
	}
	if(s_rom_size > psz){
		fclose(f);
		snprintf(s_load_detail, sizeof s_load_detail, "ROM %luKB > PSRAM %luKB",
		         (unsigned long)(s_rom_size/1024), (unsigned long)(psz/1024));
		return -2;
	}

	s_rom_off = kf_psram_alloc(s_rom_size);
	if(s_rom_off == 0xFFFFFFFFu){
		fclose(f);
		snprintf(s_load_detail, sizeof s_load_detail, "PSRAM alloc failed: ROM %luKB", (unsigned long)(s_rom_size/1024));
		return -2;
	}

	uint32_t at = 0;
	while(at < s_rom_size){
		uint32_t n = s_rom_size - at;
		if(n > BANK_SIZE) n = BANK_SIZE;
		uint8_t *chunk = (at == 0) ? s_bank0 : s_bankn;
		if(fread(chunk, 1, n, f) != n){ fclose(f); return -1; }
		kf_psram_write(s_rom_off + at, chunk, n);
		at += n;
	}
	fclose(f);
	for(int i = 0; i < BANK_SLOTS; i++){ s_slot_page[i] = 0xFFFFFFFFu; s_slot_used[i] = 0; }
	s_bank_page = 0xFFFFFFFFu;
	return 0;
}

/* Extra bank slots for multi-bank ROMs, taken last so gb_s, cart RAM and the audio
   ring are already placed. Stop while ~32 KB of kernel heap would remain. */
static void alloc_extra_slots(void){
	if(s_rom_size <= 2u * BANK_SIZE) return;
	uint32_t banks = (s_rom_size + BANK_SIZE - 1u) / BANK_SIZE - 1u;
	while(s_nslot < BANK_SLOTS && (uint32_t)s_nslot < banks){
		void *guard = malloc(32u * 1024u);
		if(!guard) break;
		s_slot[s_nslot] = malloc(BANK_SIZE);
		free(guard);
		if(!s_slot[s_nslot]) break;
		s_slot_page[s_nslot] = 0xFFFFFFFFu; s_slot_used[s_nslot] = 0;
		s_nslot++;
	}
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
	set_status("Loading ROM...");
	lv_refr_now(lv_display_get_default());

	reset_exclusive_psram();
	int lr = load_rom_file(path);
	if(lr){
		free_runtime(); reset_exclusive_psram();
		set_status(s_load_detail[0] ? s_load_detail : lr == -3 ? "Not enough SRAM for ROM cache" : "ROM read failed");
		return;
	}

	s_gb = calloc(1, sizeof *s_gb);
	if(!s_gb){
		free_runtime(); reset_exclusive_psram();
		set_status("Not enough SRAM for emulator");
		return;
	}

	enum gb_init_error_e err = gb_init(s_gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_err, NULL);
	if(err != GB_INIT_NO_ERROR){
		free_runtime(); reset_exclusive_psram();
		set_status(err == GB_INIT_CARTRIDGE_UNSUPPORTED ? "Unsupported cartridge/MBC" : "Invalid ROM");
		return;
	}

	s_cart_size = (size_t)gb_get_save_size(s_gb);
	if(s_cart_size){
		s_cart = malloc(s_cart_size);
		s_cart_heap = 1;
		if(!s_cart){
			free_runtime(); reset_exclusive_psram();
			set_status("Not enough RAM for save");
			return;
		}
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
	s_frames = 0;

	/* 2048 stereo frames = 62.5 ms at 32768 Hz (8 KB contiguous buffer) */
	if(!kf_audio_start_buffered(AUDIO_SAMPLE_RATE, 2048)){
		free_runtime(); reset_exclusive_psram();
		set_status("Not enough SRAM for audio ring");
		return;
	}
	alloc_extra_slots();

	/* Normal clock, park Core 1 and take direct control of SPI */
	kf_clock_normal();
	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
	disp_pause_core1();

	/* Switch panel to RGB111 mode (COLMOD 0x22: 3-bit, 2 px/byte) */
	spi_write_command(0x3A);
	spi_write_data(0x22);

	/* Clear 320x320 screen in RGB111 to Black */
	static uint8_t clear_row[LCD_W / 2];
	memset(clear_row, 0x00, sizeof clear_row);
	define_region_spi(0, 0, LCD_W - 1, LCD_H - 1, 1);
	for(int y = 0; y < LCD_H; y++){
		spi_write_fast(Pico_LCD_SPI_MOD, clear_row, sizeof clear_row);
	}
	spi_finish(Pico_LCD_SPI_MOD);
	lcd_spi_raise_cs();

	s_state = GB_PLAY;
}

static int has_rom_ext(const char *name){
	const char *dot = strrchr(name, '.');
	return dot && !strcasecmp(dot, ".gb");
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
	if(!s_roms) return;
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
		if(key == DK_ESC || key == DK_BREAK){
			if(s_roms_heap) free(s_roms);
			s_roms = NULL; s_roms_heap = 0;
			s_active = 0; s_state = GB_OFF; kf_grab_input(0); launcher_show(); return;
		}
		if(key == DK_UP && s_nrom){ s_sel = (s_sel + s_nrom - 1) % s_nrom; highlight(); }
		else if(key == DK_DOWN && s_nrom){ s_sel = (s_sel + 1) % s_nrom; highlight(); }
		else if(key == DK_ENTER && s_nrom){ start_game(&s_roms[s_sel]); return; }
	}
}

void app_gameboy_open(void){
	s_roms = kapi_idle_scratch(MAX_ROMS * sizeof *s_roms);
	s_roms_heap = 0;
	if(!s_roms){ s_roms = malloc(MAX_ROMS * sizeof *s_roms); s_roms_heap = !!s_roms; }
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
	lv_label_set_text(title, "Game Boy");
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
	set_status(!s_roms ? "Not enough SRAM for ROM list" :
		   s_nrom ? "ENTER play | arrows D-pad | F5/Z A | F4/X B | ESC quit" :
		   "Put .gb in /kefyros/roms/gb");
	highlight();
	kf_grab_input(1);
	s_active = 1; s_state = GB_PICK;
	lv_screen_load(s_scr);
}
