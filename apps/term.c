// apps/term.c — "Term": SSH-2 terminal client for the PicoCalc.
//
// Flow: a small LVGL quick-connect screen (user@host[:port] + password) hands
// off to a modal full-screen session loop (core 1 parked, LVGL frozen) that
// pumps Wi-Fi/lwIP, drives the SSH core (port/ssh.c via port/ssh_tcp.c), feeds
// decrypted shell output into the VT100 emulator (port/vt100.c) and paints dirty
// rows through the direct-blit renderer. The Break key exits.
//
// M4 scope: password auth + quick-connect + accept-all host keys (fingerprint is
// shown). Saved hosts, TOFU known_hosts, SD key auth, scrollback and keygen are
// M5/M6. The whole app runs at eco 250 MHz (radio ceiling).
#include "kefyros.h"
#include "theme.h"
#include "port/disp.h"
#include "vt100.h"
#include "ssh.h"
#include "ssh_tcp.h"
#include "lcdspi/lcdspi.h"
#include "pico/stdlib.h"
#include "pico/rand.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern const uint8_t kf_font6x12[0x91][12];

#define CELL_W 6
#define CELL_H 12
#define GRID_X 1
#define GRID_Y 4
#define STRIP_W (VT_COLS * CELL_W)

static uint16_t s_rowbuf[STRIP_W * CELL_H];
static vt_t   *g_vt;
static ssh_t  *g_ssh;
static int     s_cursor_on = 1;
static int     s_need_render;

static char s_user[64], s_host[128], s_pass[128];
static int  s_port;

/* ---- renderer (shared with the M1 path) ---- */
static void render_row(int r){
	const vt_cell_t *row = vt_row(g_vt, r);
	int scr_rev = vt_reverse_screen(g_vt);
	int cr, cc, cvis; vt_cursor(g_vt, &cr, &cc, &cvis);
	for(int py = 0; py < CELL_H; py++){
		uint16_t *out = &s_rowbuf[py * STRIP_W];
		for(int cx = 0; cx < VT_COLS; cx++){
			const vt_cell_t *cell = &row[cx];
			uint16_t fg = cell->fg, bg = cell->bg;
			int rev = (cell->attr & VT_A_REVERSE) ? 1 : 0;
			rev ^= scr_rev;
			if(cvis && s_cursor_on && r == cr && cx == cc) rev ^= 1;
			if(rev){ uint16_t t = fg; fg = bg; bg = t; }
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
static void render_dirty(void){
	uint32_t d = vt_dirty_and_clear(g_vt);
	for(int r = 0; r < VT_ROWS; r++) if(d & (1u << r)) render_row(r);
}
static void render_all(void){
	(void)vt_dirty_and_clear(g_vt);
	for(int r = 0; r < VT_ROWS; r++) render_row(r);
}
static void vstatus(const char *msg){
	char line[96]; snprintf(line, sizeof line, "\r\n* %s\r\n", msg);
	vt_feed(g_vt, (const uint8_t *)line, (int)strlen(line));
	s_need_render = 1;
}

/* ---- SSH core callbacks ---- */
static void ev_rng(uint8_t *b, int n, void *ud){ (void)ud;
	while(n > 0){ uint32_t r = get_rand_32(); int k = n < 4 ? n : 4; memcpy(b, &r, k); b += k; n -= k; }
}
static uint32_t ev_now(void *ud){ (void)ud; return (uint32_t)(time_us_64() / 1000); }
static int ev_hostkey(const uint8_t pub[32], const char *fp, void *ud){ (void)pub;(void)ud;
	/* M4: accept-all (TOFU persistence lands in M5). Show the fingerprint. */
	vstatus(fp);
	return 1;
}
static void ev_data(const uint8_t *b, int n, void *ud){ (void)ud;
	vt_feed(g_vt, b, n);
	ssh_consumed(g_ssh, n);
	s_need_render = 1;
}
static void ev_state(ssh_state_t st, const char *d, void *ud){ (void)ud;
	if(st == SSH_ST_RUNNING) return;   /* let the shell own the screen */
	if(d) vstatus(d);
}

/* ---- keyboard -> byte stream ---- */
static int emit(uint8_t *out, const char *s){ int n = (int)strlen(s); memcpy(out, s, n); return n; }
static int keymap(uint8_t key, int mods, int appcursor, uint8_t *out){
	switch(key){
		case DK_UP:    return emit(out, appcursor ? "\x1bOA" : "\x1b[A");
		case DK_DOWN:  return emit(out, appcursor ? "\x1bOB" : "\x1b[B");
		case DK_RIGHT: return emit(out, appcursor ? "\x1bOC" : "\x1b[C");
		case DK_LEFT:  return emit(out, appcursor ? "\x1bOD" : "\x1b[D");
		case DK_HOME:  return emit(out, "\x1b[H");
		case DK_END:   return emit(out, "\x1b[F");
		case DK_INSERT:return emit(out, "\x1b[2~");
		case DK_DEL:   return emit(out, "\x1b[3~");
		case DK_PGUP:  return emit(out, "\x1b[5~");
		case DK_PGDN:  return emit(out, "\x1b[6~");
		case DK_ESC:   out[0] = 0x1b; return 1;
		case DK_ENTER: out[0] = '\r'; return 1;
		case DK_BACKSPACE: out[0] = 0x7f; return 1;
		case DK_TAB:   out[0] = '\t'; return 1;
	}
	if(key >= DK_F1 && key <= DK_F1 + 9){
		static const char *F[10] = { "\x1bOP","\x1bOQ","\x1bOR","\x1bOS","\x1b[15~",
		                             "\x1b[17~","\x1b[18~","\x1b[19~","\x1b[20~","\x1b[21~" };
		return emit(out, F[key - DK_F1]);
	}
	if(key >= 0x20 && key < 0x7f){
		if(mods & MOD_CTRL){
			uint8_t c = key;
			if(c >= 'a' && c <= 'z') c = c - 'a' + 1;
			else if(c >= 'A' && c <= 'Z') c = c - 'A' + 1;
			else if(c == '[') c = 27; else if(c == '\\') c = 28; else if(c == ']') c = 29;
			else if(c == '^') c = 30; else if(c == '_') c = 31;
			else if(c == ' ' || c == '@') c = 0;
			else { out[0] = key; return 1; }
			out[0] = c; return 1;
		}
		if(mods & MOD_ALT){ out[0] = 0x1b; out[1] = key; return 2; }
		out[0] = key; return 1;
	}
	return 0;
}

/* ---- modal session ---- */
static void run_session(void){
	kf_grab_input(1);
	kf_clock_eco();                 /* radio-safe clock for the whole session */
	kf_net_init();
	if(kf_net_state() != KF_NET_ONLINE && !kf_net_ssid()[0]) kf_net_autoconnect();

	ssh_cb_t cb = { ssh_tcp_tx, ev_rng, ev_now, ev_hostkey, ev_data, ev_state, NULL };
	g_vt  = vt_create(malloc, NULL, NULL, NULL);
	g_ssh = ssh_create(&cb, malloc, free);
	if(!g_vt || !g_ssh){ if(g_vt) vt_destroy(g_vt, free); if(g_ssh) ssh_destroy(g_ssh); return; }
	ssh_tcp_init(g_ssh);

	disp_pause_core1();
	draw_rect_spi(0, 0, LCD_W - 1, LCD_H - 1, 0x000000);
	render_all();
	vstatus("connecting to Wi-Fi...");

	int running = 1, tcp_started = 0, ssh_started = 0, done = 0;
	uint32_t net_deadline = ev_now(NULL) + 20000;
	uint64_t t_blink = time_us_64();

	while(running){
		uart_poll();
		uint8_t st, key;
		while(uart_pop_key(&st, &key)){
			if(st == KS_RELEASE) continue;
			if(key == DK_BREAK){ running = 0; break; }
			if(done){ running = 0; break; }   /* any key dismisses an error screen */
			if(g_ssh && ssh_state(g_ssh) == SSH_ST_RUNNING){
				uint8_t ob[8]; int ol = keymap(key, uart_mods(), vt_mode_appcursor(g_vt), ob);
				if(ol > 0) ssh_send_channel(g_ssh, ob, ol);
			}
		}
		if(!running) break;

		kf_net_poll();

		if(!done){
			if(!tcp_started){
				if(kf_net_state() == KF_NET_ONLINE){
					char m[160]; snprintf(m, sizeof m, "connecting to %s:%d", s_host, s_port);
					vstatus(m);
					ssh_tcp_connect(s_host, s_port);
					tcp_started = 1;
				} else if(ev_now(NULL) > net_deadline){
					vstatus("Wi-Fi not connected — Break to exit"); done = 1;
				}
			} else {
				ssh_tcp_poll();
				if(ssh_tcp_is_up() && !ssh_started){ ssh_start(g_ssh, s_user); ssh_started = 1; }
				if(ssh_started){
					ssh_tick(g_ssh);
					if(ssh_wants_password(g_ssh)) ssh_auth_password(g_ssh, s_pass);
				}
				ssh_state_t sst = ssh_state(g_ssh);
				if(sst == SSH_ST_ERROR){ char m[160]; snprintf(m, sizeof m, "%s — Break to exit", ssh_error(g_ssh)); vstatus(m); done = 1; }
				else if(sst == SSH_ST_CLOSED){ vstatus("connection closed — Break to exit"); done = 1; }
				else if(ssh_tcp_is_dead() && sst != SSH_ST_RUNNING){ vstatus("disconnected — Break to exit"); done = 1; }
			}
		}

		if(s_need_render){ render_dirty(); s_need_render = 0; }

		if(time_us_64() - t_blink >= 500000ull){
			t_blink = time_us_64();
			s_cursor_on ^= 1;
			int cr, cc, cv; vt_cursor(g_vt, &cr, &cc, &cv);
			render_row(cr);
		}
		sleep_ms(4);
	}

	if(g_ssh && ssh_state(g_ssh) == SSH_ST_RUNNING) ssh_disconnect(g_ssh, "bye");
	disp_resume_core1();
	ssh_tcp_close();
	ssh_destroy(g_ssh); g_ssh = NULL;
	vt_destroy(g_vt, free); g_vt = NULL;
	memset(s_pass, 0, sizeof s_pass);
	kf_grab_input(0);
	kf_clock_normal();
}

/* ---- connect screen (LVGL) ---- */
static lv_obj_t *scr, *ta_host, *ta_pass, *lbl_err;

static int parse_target(const char *in){
	/* user@host[:port] */
	const char *at = strchr(in, '@');
	if(!at || at == in) return -1;
	int ul = (int)(at - in); if(ul >= (int)sizeof s_user) ul = sizeof s_user - 1;
	memcpy(s_user, in, ul); s_user[ul] = 0;
	const char *hp = at + 1;
	const char *colon = strchr(hp, ':');
	s_port = 22;
	if(colon){
		int hl = (int)(colon - hp); if(hl >= (int)sizeof s_host) hl = sizeof s_host - 1;
		memcpy(s_host, hp, hl); s_host[hl] = 0;
		s_port = atoi(colon + 1); if(s_port <= 0 || s_port > 65535) s_port = 22;
	} else {
		strncpy(s_host, hp, sizeof s_host - 1); s_host[sizeof s_host - 1] = 0;
	}
	return s_host[0] ? 0 : -1;
}

static void do_connect(lv_event_t *e){ (void)e;
	const char *host_in = lv_textarea_get_text(ta_host);
	const char *pass_in = lv_textarea_get_text(ta_pass);
	if(parse_target(host_in) != 0){
		lv_label_set_text(lbl_err, "enter user@host[:port]");
		return;
	}
	strncpy(s_pass, pass_in, sizeof s_pass - 1); s_pass[sizeof s_pass - 1] = 0;

	run_session();       /* modal; returns when the session ends */
	kf_back_to_launcher();
}

void app_term_open(void){
	scr = lv_obj_create(NULL);
	kf_inset_top(scr);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(scr, 8, 0);
	lv_obj_set_style_pad_all(scr, 12, 0);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Terminal — SSH");
	lv_obj_set_style_text_font(title, KF_FONT_BIG, 0);

	lv_group_t *g = kf_use_group();

	ta_host = lv_textarea_create(scr);
	lv_textarea_set_one_line(ta_host, true);
	lv_textarea_set_placeholder_text(ta_host, "user@host[:port]");
	lv_obj_set_width(ta_host, lv_pct(100));
	lv_group_add_obj(g, ta_host);

	ta_pass = lv_textarea_create(scr);
	lv_textarea_set_one_line(ta_pass, true);
	lv_textarea_set_password_mode(ta_pass, true);
	lv_textarea_set_placeholder_text(ta_pass, "password");
	lv_obj_set_width(ta_pass, lv_pct(100));
	lv_group_add_obj(g, ta_pass);

	lv_obj_t *btn = lv_button_create(scr);
	lv_obj_t *bl = lv_label_create(btn);
	lv_label_set_text(bl, "Connect");
	lv_obj_add_event_cb(btn, do_connect, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(g, btn);

	lbl_err = lv_label_create(scr);
	lv_label_set_text(lbl_err, "Tab to move • Enter connects • Break exits session");
	lv_obj_set_style_text_font(lbl_err, KF_FONT, 0);

	lv_group_focus_obj(ta_host);
	lv_screen_load(scr);
}

/* modal loop owns the session; nothing to pump from the superloop. */
void term_poll(void){}
