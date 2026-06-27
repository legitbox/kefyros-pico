// apps/morse.c — Kefyros Morse code studio. A HOME menu of tools (mirrors apps/calc.c /
// electronics.c multi-screen + grabbed-key pattern):
//   Send (TX)      — type a message, key it out as tone + on-screen lamp + backlight pulse
//                    (each output independently toggleable), WPM-adjustable.
//   Decode (typed) — type dot/dash notation ('.' '-', space = letter gap, '/' = word gap)
//                    -> plain text, live and exact (no timing involved).
//   Key practice   — straight-key live decode (Phase 3, stub for now).
//   Trainer        — Koch / random groups (Phase 4, stub for now).
//   Settings       — WPM, sidetone pitch, and the three TX output toggles (persisted).
//
// The TX keyer is a non-blocking wall-clock state machine driven from morse_poll(): it
// pre-expands the message into mark/gap segments (ITU timing, unit T = 1200/WPM ms) and
// walks them by absolute time so it self-corrects against poll jitter. The sidetone is a
// 650 Hz sine with a ~5 ms raised-cosine envelope (kills key-clicks), generated a few ms
// ahead of playback into the PWM-DAC ring. ASCII only (font is 0x20-0x7F).
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include "morse_codec.h"
#include "pico/time.h"             // time_us_64
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { S_HOME=0, S_SEND, S_DECODE, S_KEY, S_TRAIN, S_SET, S_COUNT };

static int       active = 0;
static int       screen = S_HOME;
static lv_obj_t *scr = NULL;

/* ---- persisted settings ---- */
static int set_wpm  = 13;        /* words per minute (PARIS standard) */
static int set_tone = 650;       /* sidetone pitch, Hz */
static int set_aud  = 1;         /* output: audio sidetone */
static int set_lamp = 1;         /* output: on-screen lamp */
static int set_bkl  = 0;         /* output: LCD backlight pulse */

static void settings_load(void){
	set_wpm  = deskconf_get_int("morse.wpm", 13);  if(set_wpm<5)  set_wpm=5;  if(set_wpm>40)   set_wpm=40;
	set_tone = deskconf_get_int("morse.tone",650); if(set_tone<300)set_tone=300;if(set_tone>1200)set_tone=1200;
	set_aud  = deskconf_get_int("morse.aud", 1) ? 1 : 0;
	set_lamp = deskconf_get_int("morse.lamp",1) ? 1 : 0;
	set_bkl  = deskconf_get_int("morse.bkl", 0) ? 1 : 0;
}

/* ===================== TX keyer (segment timeline) ===================== */
typedef struct { uint16_t ms; uint8_t tone; char ch; } seg_t;
#define MSG_MAX  120
static seg_t   *s_seg = NULL;       /* malloc'd timeline; freed on stop */
static int      s_nseg = 0, s_si = 0;
static int      s_play = 0;
static uint64_t s_seg_end = 0;      /* absolute us when the current segment ends */
static char     s_last_ch = 0;      /* last letter shown in lbl_now */
static int      s_bkl_norm = 5;     /* backlight level to return to between marks */

/* sidetone generator state */
static int      s_aud_on = 0;       /* did WE start the audio device? */
static float    s_phase = 0.f, s_env = 0.f;
static int      s_tone_now = 0;     /* current keyer mark flag (drives env target) */

/* send-screen widgets */
static lv_obj_t *ta_msg, *lbl_now, *lamp, *lbl_sstat;

static int push_seg(int tone, int ms, char ch){
	if(s_nseg >= MSG_MAX*16) return 0;
	s_seg[s_nseg].tone = (uint8_t)tone;
	s_seg[s_nseg].ms   = (uint16_t)(ms > 0 ? ms : 1);
	s_seg[s_nseg].ch   = ch;
	s_nseg++;
	return 1;
}

/* expand `msg` into the mark/gap timeline. pending-gap model: inter-letter = 3T, a space
   bumps the pending gap to 7T (word gap); intra-element gap = 1T. */
static int build_timeline(const char *msg){
	int unit = 1200 / set_wpm;                 /* ms per dot */
	s_nseg = 0;
	int emitted = 0, pend = 0;
	for(int i = 0; msg[i] && i < MSG_MAX; i++){
		char c = (char)toupper((unsigned char)msg[i]);
		if(c == ' '){ if(pend < 7) pend = 7; continue; }
		const char *code = morse_encode(c);
		if(!code) continue;
		for(int e = 0; code[e]; e++){
			if(e == 0){ if(emitted && pend > 0) push_seg(0, pend*unit, c); }
			else        push_seg(0, unit, c);                   /* intra-element gap */
			push_seg(1, (code[e]=='-' ? 3 : 1)*unit, c);        /* the mark */
			emitted = 1;
		}
		pend = 3;                                               /* inter-letter gap */
	}
	return s_nseg;
}

static void lamp_set(int on){
	if(lamp) lv_obj_set_style_bg_color(lamp, on ? KF_ACTIVE : KF_BORDER, 0);
}
static void bkl_set(int level){ uint8_t v=(uint8_t)level; reg_write(REG_BKL, &v, 1); }

static void apply_segment(int k){
	s_tone_now = s_seg[k].tone;
	if(set_lamp) lamp_set(s_tone_now);
	if(set_bkl)  bkl_set(s_tone_now ? 9 : s_bkl_norm);
	if(s_tone_now && lbl_now && s_seg[k].ch != s_last_ch){
		s_last_ch = s_seg[k].ch;
		const char *code = morse_encode(s_last_ch);
		char buf[40]; snprintf(buf, sizeof buf, "%c   %s", s_last_ch, code ? code : "");
		lv_label_set_text(lbl_now, buf);
	}
}

static void stop_play(void){
	if(!s_play) return;
	s_play = 0;
	s_tone_now = 0;
	if(s_aud_on){ kf_audio_stop(); s_aud_on = 0; }
	lamp_set(0);
	if(set_bkl) bkl_set(s_bkl_norm);
	free(s_seg); s_seg = NULL; s_nseg = 0;
	if(lbl_sstat) lv_label_set_text(lbl_sstat, "ENTER send   ESC back");
}

static void start_play(void){
	if(s_play) return;
	const char *msg = lv_textarea_get_text(ta_msg);
	if(!msg || !msg[0]) return;
	s_seg = malloc(sizeof(seg_t) * (MSG_MAX*16));
	if(!s_seg) return;
	if(build_timeline(msg) <= 0){ free(s_seg); s_seg = NULL; return; }

	s_bkl_norm = deskconf_get_int("bkl", 5);
	s_last_ch  = 0;
	s_si = 0;
	s_seg_end = time_us_64() + (uint64_t)s_seg[0].ms * 1000ULL;

	if(set_aud && !kf_audio_running()){
		kf_audio_start(8000);
		s_aud_on = kf_audio_running();          /* 0 if the ring malloc failed */
		s_phase = 0.f; s_env = 0.f;
	}
	s_play = 1;
	if(lbl_sstat) lv_label_set_text(lbl_sstat, "sending...   ESC stop");
	apply_segment(0);
}

/* feed the sidetone ring a few ms ahead of playback so mark/space changes stay snappy. */
static void audio_fill(void){
	if(!s_aud_on) return;
	const int   TARGET = 192;                   /* ~24 ms lead at 8 kHz */
	const float dphase = 2.f * (float)M_PI * (float)set_tone / 8000.f;
	const float estep  = 1.f / 40.f;            /* 5 ms env ramp */
	int want = TARGET - kf_audio_buffered();
	int space = kf_audio_space();
	if(want > space) want = space;
	while(want > 0){
		int16_t buf[128*2];
		int n = want > 128 ? 128 : want;
		for(int i = 0; i < n; i++){
			float tgt = s_tone_now ? 1.f : 0.f;
			if(s_env < tgt){ s_env += estep; if(s_env > tgt) s_env = tgt; }
			else if(s_env > tgt){ s_env -= estep; if(s_env < tgt) s_env = tgt; }
			s_phase += dphase; if(s_phase >= 2.f*(float)M_PI) s_phase -= 2.f*(float)M_PI;
			int16_t v = (int16_t)(s_env * sinf(s_phase) * 9000.f);
			buf[2*i] = v; buf[2*i+1] = v;
		}
		kf_audio_write(buf, n);
		want -= n;
	}
}

static void keyer_tick(void){
	uint64_t now = time_us_64();
	while(s_play && now >= s_seg_end){
		s_si++;
		if(s_si >= s_nseg){ stop_play(); return; }
		s_seg_end += (uint64_t)s_seg[s_si].ms * 1000ULL;
		apply_segment(s_si);
	}
}

/* ===================== generic screen helpers ===================== */
static lv_obj_t *new_screen(const char *title, lv_obj_t **out_title){
	lv_obj_t *s = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s, 6, 0);
	kf_inset_top(s);
	lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *t = lv_label_create(s);
	lv_obj_set_style_text_font(t, KF_FONT_BIG, 0);
	lv_obj_set_style_text_color(t, KF_AMBER_BR, 0);
	lv_label_set_text(t, title);
	lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
	if(out_title) *out_title = t;
	return s;
}
static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, lv_color_t col){
	lv_obj_t *l = lv_label_create(par);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col, 0);
	return l;
}

/* ===================== HOME menu ===================== */
static const struct { const char *label; int scr; } MENU[] = {
	{ "Send  (text -> Morse)",   S_SEND   },
	{ "Decode  (Morse -> text)", S_DECODE },
	{ "Key practice  (RX)",      S_KEY    },
	{ "Trainer",                 S_TRAIN  },
	{ "Settings",                S_SET    },
};
#define NMENU ((int)(sizeof(MENU)/sizeof(MENU[0])))
static int       home_sel = 0;
static lv_obj_t *home_rows[NMENU];

static void home_hl(void){
	for(int i = 0; i < NMENU; i++){
		int on = (i == home_sel);
		lv_obj_set_style_bg_opa(home_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(home_rows[i], KF_AMBER_DIM, 0);
		lv_obj_set_style_text_color(home_rows[i], on ? KF_BG_DEEP : KF_TEXT, 0);
	}
}
static lv_obj_t *build_home(void){
	lv_obj_t *s = new_screen("Morse", NULL);
	lv_obj_t *col = lv_obj_create(s);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LCD_W-12, KF_CONTENT_H-40);
	lv_obj_align(col, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 4, 0);
	for(int i = 0; i < NMENU; i++){
		lv_obj_t *r = mk_label(col, KF_FONT, KF_TEXT);
		lv_obj_set_width(r, LCD_W-16);
		lv_obj_set_style_pad_all(r, 3, 0);
		lv_obj_set_style_radius(r, 0, 0);
		lv_label_set_text(r, MENU[i].label);
		home_rows[i] = r;
	}
	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "Up/Dn select  ENTER open  ESC exit");
	home_hl();
	return s;
}

/* ===================== SEND (TX) ===================== */
static lv_obj_t *build_send(void){
	lv_obj_t *s = new_screen("Send", NULL);

	ta_msg = lv_textarea_create(s);
	lv_textarea_set_one_line(ta_msg, false);
	lv_obj_set_size(ta_msg, LCD_W-12, 70);
	lv_obj_align(ta_msg, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_style_text_font(ta_msg, KF_FONT, 0);
	lv_textarea_set_placeholder_text(ta_msg, "type a message...");

	lbl_now = mk_label(s, KF_FONT_BIG, KF_AMBER_HOT);
	lv_obj_align(lbl_now, LV_ALIGN_TOP_LEFT, 0, 110);
	lv_label_set_text(lbl_now, "");

	lamp = lv_obj_create(s);
	lv_obj_set_size(lamp, 60, 60);
	lv_obj_align(lamp, LV_ALIGN_TOP_RIGHT, 0, 150);
	lv_obj_set_style_radius(lamp, 30, 0);
	lv_obj_set_style_border_width(lamp, 2, 0);
	lv_obj_set_style_border_color(lamp, KF_BORDER_HI, 0);
	lv_obj_set_style_bg_color(lamp, KF_BORDER, 0);

	lbl_sstat = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_align(lbl_sstat, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(lbl_sstat, "ENTER send   ESC back");
	return s;
}
static void send_key(uint8_t key, int mods){
	if(s_play){                                  /* while sending: any key stops */
		if(key==DK_ESC || key==DK_BREAK || key==DK_ENTER) stop_play();
		return;
	}
	if(key==DK_ESC || key==DK_BREAK){ return; }  /* handled by caller -> show_screen(HOME) */
	if(key==DK_ENTER || key==DK_F1){ start_play(); return; }
	if(key==DK_BACKSPACE){ lv_textarea_delete_char(ta_msg); return; }
	if(key==DK_LEFT){  lv_textarea_cursor_left(ta_msg);  return; }
	if(key==DK_RIGHT){ lv_textarea_cursor_right(ta_msg); return; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)) lv_textarea_add_char(ta_msg, key);
}

/* ===================== DECODE (typed notation) ===================== */
static lv_obj_t *ta_code, *lbl_decoded;

static void decode_refresh(void){
	const char *in = lv_textarea_get_text(ta_code);
	char out[160]; int o = 0;
	char tok[12]; int tl = 0;
	for(int i = 0; ; i++){
		char c = in[i];
		if(c=='.' || c=='-'){ if(tl < (int)sizeof(tok)-1) tok[tl++] = c; continue; }
		/* delimiter (space, '/', end, anything else) flushes the current token */
		if(tl){ tok[tl] = 0; if(o < (int)sizeof(out)-1) out[o++] = morse_decode_token(tok); tl = 0; }
		if(c == '/'){ if(o < (int)sizeof(out)-1) out[o++] = ' '; }
		if(c == 0) break;
	}
	out[o] = 0;
	lv_label_set_text(lbl_decoded, o ? out : "");
}
static lv_obj_t *build_decode(void){
	lv_obj_t *s = new_screen("Decode", NULL);
	lv_obj_t *hint = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 28);
	lv_label_set_text(hint, "'.' '-'  space=letter  '/'=word");

	ta_code = lv_textarea_create(s);
	lv_textarea_set_one_line(ta_code, false);
	lv_obj_set_size(ta_code, LCD_W-12, 90);
	lv_obj_align(ta_code, LV_ALIGN_TOP_LEFT, 0, 48);
	lv_obj_set_style_text_font(ta_code, KF_FONT, 0);
	lv_textarea_set_placeholder_text(ta_code, "... --- ...");

	lbl_decoded = mk_label(s, KF_FONT_BIG, KF_AMBER_HOT);
	lv_obj_set_width(lbl_decoded, LCD_W-12);
	lv_label_set_long_mode(lbl_decoded, LV_LABEL_LONG_WRAP);
	lv_obj_align(lbl_decoded, LV_ALIGN_TOP_LEFT, 0, 150);
	lv_label_set_text(lbl_decoded, "");

	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "ESC back");
	return s;
}
static void decode_key(uint8_t key, int mods){
	if(key==DK_ESC || key==DK_BREAK) return;     /* caller -> HOME */
	if(key==DK_BACKSPACE){ lv_textarea_delete_char(ta_code); decode_refresh(); return; }
	if(key==DK_LEFT){  lv_textarea_cursor_left(ta_code);  return; }
	if(key==DK_RIGHT){ lv_textarea_cursor_right(ta_code); return; }
	if(key==DK_ENTER){ lv_textarea_add_char(ta_code, '/'); decode_refresh(); return; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)){ lv_textarea_add_char(ta_code, key); decode_refresh(); }
}

/* ===================== SETTINGS ===================== */
static int       set_sel = 0;
static lv_obj_t *set_rows[5];
#define NSET 5

static void settings_refresh(void){
	char b[48];
	snprintf(b, sizeof b, "WPM            %d", set_wpm);                  lv_label_set_text(set_rows[0], b);
	snprintf(b, sizeof b, "Sidetone Hz    %d", set_tone);                lv_label_set_text(set_rows[1], b);
	snprintf(b, sizeof b, "Audio tone     %s", set_aud ?"ON":"off");     lv_label_set_text(set_rows[2], b);
	snprintf(b, sizeof b, "Screen lamp    %s", set_lamp?"ON":"off");     lv_label_set_text(set_rows[3], b);
	snprintf(b, sizeof b, "Backlight key  %s", set_bkl ?"ON":"off");     lv_label_set_text(set_rows[4], b);
	for(int i = 0; i < NSET; i++){
		int on = (i == set_sel);
		lv_obj_set_style_bg_opa(set_rows[i], on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(set_rows[i], KF_AMBER_DIM, 0);
		lv_obj_set_style_text_color(set_rows[i], on ? KF_BG_DEEP : KF_TEXT, 0);
	}
}
static lv_obj_t *build_settings(void){
	lv_obj_t *s = new_screen("Settings", NULL);
	lv_obj_t *col = lv_obj_create(s);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LCD_W-12, KF_CONTENT_H-40);
	lv_obj_align(col, LV_ALIGN_TOP_LEFT, 0, 32);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 5, 0);
	for(int i = 0; i < NSET; i++){
		lv_obj_t *r = mk_label(col, KF_FONT, KF_TEXT);
		lv_obj_set_width(r, LCD_W-16);
		lv_obj_set_style_pad_all(r, 3, 0);
		set_rows[i] = r;
	}
	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "Up/Dn pick  Left/Right adjust  ESC back");
	settings_refresh();
	return s;
}
static void settings_adjust(int dir){
	switch(set_sel){
	case 0: set_wpm  += dir*1;  if(set_wpm<5)  set_wpm=5;   if(set_wpm>40)   set_wpm=40;   deskconf_set_int("morse.wpm",  set_wpm);  break;
	case 1: set_tone += dir*25; if(set_tone<300)set_tone=300;if(set_tone>1200)set_tone=1200;deskconf_set_int("morse.tone", set_tone); break;
	case 2: set_aud  = !set_aud;  deskconf_set_int("morse.aud",  set_aud);  break;
	case 3: set_lamp = !set_lamp; deskconf_set_int("morse.lamp", set_lamp); break;
	case 4: set_bkl  = !set_bkl;  deskconf_set_int("morse.bkl",  set_bkl);  break;
	}
	settings_refresh();
}
static void settings_key(uint8_t key, int mods){
	(void)mods;
	if(key==DK_ESC || key==DK_BREAK) return;     /* caller -> HOME */
	if(key==DK_UP){   set_sel=(set_sel+NSET-1)%NSET; settings_refresh(); return; }
	if(key==DK_DOWN){ set_sel=(set_sel+1)%NSET;      settings_refresh(); return; }
	if(key==DK_LEFT)  settings_adjust(-1);
	if(key==DK_RIGHT) settings_adjust(+1);
}

/* ===================== stub screens (Phase 3 / 4) ===================== */
static lv_obj_t *build_stub(const char *title, const char *body){
	lv_obj_t *s = new_screen(title, NULL);
	lv_obj_t *l = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_set_width(l, LCD_W-12);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 40);
	lv_label_set_text(l, body);
	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "ESC back");
	return s;
}

/* ===================== screen switching + input pump ===================== */
static void show_screen(int s){
	if(s_play) stop_play();
	lv_obj_t *old = scr;
	screen = s;
	ta_msg = lbl_now = lamp = lbl_sstat = NULL;
	ta_code = lbl_decoded = NULL;
	switch(s){
	case S_SEND:   scr = build_send();     break;
	case S_DECODE: scr = build_decode();   break;
	case S_KEY:    scr = build_stub("Key practice",
		"Straight-key live decode (tap a key in rhythm) lands in Phase 3."); break;
	case S_TRAIN:  scr = build_stub("Trainer",
		"Koch method + random callsign/QSO group drills land in Phase 4."); break;
	case S_SET:    scr = build_settings(); break;
	default:       scr = build_home();     break;
	}
	lv_screen_load(scr);
	if(old && old != scr) lv_obj_delete(old);
	kf_grab_input(1);
}

static void home_key(uint8_t key){
	if(key==DK_ESC || key==DK_BREAK){ active=0; kf_grab_input(0); kf_back_to_launcher(); return; }
	if(key==DK_UP){   home_sel=(home_sel+NMENU-1)%NMENU; home_hl(); return; }
	if(key==DK_DOWN){ home_sel=(home_sel+1)%NMENU;      home_hl(); return; }
	if(key==DK_ENTER) show_screen(MENU[home_sel].scr);
}

void morse_poll(void){
	if(!active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st==KS_RELEASE) continue;
		int mods = uart_mods();
		if(screen==S_HOME){ home_key(key); continue; }
		/* every sub-screen: ESC returns to the menu */
		if((key==DK_ESC || key==DK_BREAK) && !(screen==S_SEND && s_play)){ show_screen(S_HOME); continue; }
		switch(screen){
		case S_SEND:   send_key(key, mods);     break;
		case S_DECODE: decode_key(key, mods);   break;
		case S_SET:    settings_key(key, mods); break;
		default: break;
		}
	}
	if(s_play){ keyer_tick(); audio_fill(); }
}

void app_morse_open(void){
	settings_load();
	active = 1; home_sel = 0; set_sel = 0; scr = NULL;
	show_screen(S_HOME);
}
