// apps/term.c — "Term": SSH-2 terminal client for the PicoCalc.
//
// MILESTONE 1: on-device font + grid renderer validation. This builds a real
// vt_t emulator, feeds it a scripted demo (colors, DEC line-drawing, attributes,
// a full glyph ramp), and paints it through the exact render path the live
// terminal will use: per-row compose into an RGB565 strip buffer -> draw_buffer_spi.
// The session runs as a modal full-screen loop (core 1 parked, LVGL frozen) modeled
// on the Settings screen-test, and the Break key exits back to the launcher.
//
// Later milestones bolt the SSH transport (port/ssh.c) + lwIP glue onto this same
// renderer; the VT100 core (port/vt100.c) is already host-verified.
#include "kefyros.h"
#include "port/disp.h"
#include "vt100.h"
#include "lcdspi/lcdspi.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* baked 6x12 font (ui/font_term6x12.c), indexed by internal glyph value */
extern const uint8_t kf_font6x12[0x91][12];

#define CELL_W 6
#define CELL_H 12
#define GRID_X 1
#define GRID_Y 4
#define STRIP_W (VT_COLS * CELL_W)          /* 318 px */

static uint16_t s_rowbuf[STRIP_W * CELL_H]; /* one text-row strip (7.6 KB .bss) */
static vt_t    *s_vt;
static int      s_cursor_on = 1;

/* compose one grid row into s_rowbuf and blit it to the panel */
static void render_row(vt_t *t, int r){
	const vt_cell_t *row = vt_row(t, r);
	int scr_rev = vt_reverse_screen(t);
	int cr, cc, cvis; vt_cursor(t, &cr, &cc, &cvis);
	for(int py = 0; py < CELL_H; py++){
		uint16_t *out = &s_rowbuf[py * STRIP_W];
		for(int cx = 0; cx < VT_COLS; cx++){
			const vt_cell_t *cell = &row[cx];
			uint16_t fg = cell->fg, bg = cell->bg;
			int rev = (cell->attr & VT_A_REVERSE) ? 1 : 0;
			rev ^= scr_rev;
			if(cvis && s_cursor_on && r == cr && cx == cc) rev ^= 1;
			if(rev){ uint16_t tmp = fg; fg = bg; bg = tmp; }
			const uint8_t *bits = kf_font6x12[cell->glyph];
			uint8_t rb = bits[py];
			int underline = (cell->attr & VT_A_UNDER) && py == CELL_H - 2;
			uint16_t *px = &out[cx * CELL_W];
			for(int b = 0; b < CELL_W; b++){
				int on = (rb >> (7 - b)) & 1;
				if(underline) on = 1;
				px[b] = on ? fg : bg;
			}
		}
	}
	draw_buffer_spi(GRID_X, GRID_Y + r * CELL_H,
	                GRID_X + STRIP_W - 1, GRID_Y + r * CELL_H + CELL_H - 1,
	                (unsigned char *)s_rowbuf);
}

static void render_all(vt_t *t){
	(void)vt_dirty_and_clear(t);
	for(int r = 0; r < VT_ROWS; r++) render_row(t, r);
}

static void feed(vt_t *t, const char *s){ vt_feed(t, (const uint8_t *)s, (int)strlen(s)); }

/* scripted demo content exercising colors, charsets, attributes and glyphs */
static void demo_fill(vt_t *t){
	feed(t, "\033[2J\033[H");
	feed(t, "\033[1;36mKefyros Term\033[0m \033[37m- 6x12 render test\033[0m\r\n\r\n");

	feed(t, "16 colors: ");
	for(int i = 0; i < 8; i++){ char b[16]; snprintf(b, sizeof b, "\033[4%dm  ", i); feed(t, b); }
	feed(t, "\033[0m\r\n           ");
	for(int i = 0; i < 8; i++){ char b[16]; snprintf(b, sizeof b, "\033[10%dm  ", i); feed(t, b); }
	feed(t, "\033[0m\r\n\r\n");

	feed(t, "attrs: \033[0mnormal \033[1mbold\033[0m \033[4munderline\033[0m "
	        "\033[7mreverse\033[0m \033[31mred\033[32mgrn\033[34mblu\033[0m\r\n\r\n");

	/* a box drawn with the DEC special-graphics charset */
	feed(t, "\033(0lqqqqqqqqqk\033(B  box-drawing (ESC(0)\r\n");
	feed(t, "\033(0x\033(B  vt100   \033(0x\033(B\r\n");
	feed(t, "\033(0tqqqqqqqqqu\033(B  tees + cross\r\n");
	feed(t, "\033(0x\033(B  \033[33mgrid\033[0m   \033(0x\033(B\r\n");
	feed(t, "\033(0mqqqqqqqqqj\033(B\r\n\r\n");

	/* 256-color ramp sample */
	feed(t, "256: ");
	for(int i = 16; i < 16 + 36; i += 2){ char b[24]; snprintf(b, sizeof b, "\033[48;5;%dm ", i); feed(t, b); }
	feed(t, "\033[0m\r\n\r\n");

	/* printable ASCII ramp */
	feed(t, "ascii: ");
	for(int c = 0x20; c < 0x7F; c++){ char ch = (char)c; vt_feed(t, (const uint8_t *)&ch, 1); }
	feed(t, "\r\n\r\n\033[90mBreak = exit\033[0m");
}

/* ---- app entry: modal full-screen render demo ---- */
void app_term_open(void){
	/* fresh screen so launcher_show() has an app screen to delete on the way out */
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
	lv_screen_load(scr);

	s_vt = vt_create(malloc, NULL, NULL, NULL);
	if(!s_vt){ kf_back_to_launcher(); return; }
	demo_fill(s_vt);

	kf_grab_input(1);
	disp_pause_core1();               /* take the panel from the LVGL flush pump */

	/* clear the panel to black once */
	draw_rect_spi(0, 0, LCD_W - 1, LCD_H - 1, 0x000000);
	render_all(s_vt);

	int running = 1;
	uint64_t t_blink = time_us_64();
	while(running){
		uart_poll();
		uint8_t st, key;
		while(uart_pop_key(&st, &key)){
			if(st == KS_RELEASE) continue;
			if(key == DK_BREAK){ running = 0; break; }
		}
		if(!running) break;

		uint32_t d = vt_dirty_and_clear(s_vt);
		for(int r = 0; r < VT_ROWS; r++) if(d & (1u << r)) render_row(s_vt, r);

		if(time_us_64() - t_blink >= 500000ull){
			t_blink = time_us_64();
			s_cursor_on ^= 1;
			int cr, cc, cv; vt_cursor(s_vt, &cr, &cc, &cv);
			render_row(s_vt, cr);
		}
		kf_net_poll();                /* keep WiFi/time alive while modal */
		sleep_ms(8);
	}

	disp_resume_core1();              /* hand the panel back to LVGL */
	vt_destroy(s_vt, free); s_vt = NULL;
	kf_grab_input(0);
	kf_back_to_launcher();
}

/* pumped from the superloop; unused while the app is modal (kept for the wiring
   the live SSH session loop will use). */
void term_poll(void){}
