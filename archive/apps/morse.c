// apps/morse.c — Kefyros Morse code studio. A HOME menu of tools (mirrors apps/calc.c /
// electronics.c multi-screen + grabbed-key pattern):
//   Send (TX)      — type a message (or load /kefyros/notes/morse.txt), key it out as tone
//                    + on-screen lamp + backlight pulse (each output independently toggleable),
//                    WPM + Farnsworth + pitch + volume adjustable. Prosigns via <AR> <SK> ...
//   Decode (typed) — type dot/dash notation ('.' '-', space = letter gap, '/' = word gap)
//                    -> plain text, live and exact (no timing involved).
//   Key practice   — straight-key live decode: tap SPACE in rhythm, an adaptive threshold
//                    classifies dit/dah/gaps and prints text. Save copy to /kefyros/notes.
//   Trainer        — Koch method: pick a lesson, it sends a random group, you type your copy
//                    and it scores you.
//   Settings       — WPM, Farnsworth WPM, pitch, volume, and the 3 TX output toggles.
//
// The TX keyer is a non-blocking wall-clock state machine driven from morse_poll(): it
// pre-expands the message into mark/gap segments (ITU timing, dit T = 1200/WPM ms) and walks
// them by absolute time so it self-corrects against poll jitter. The sidetone is a sine with
// a ~5 ms raised-cosine envelope (kills key-clicks); crucially it is generated from the
// segment timeline BY PLAYBACK TIME, so a fat anti-underrun ring buffer never smears the
// keying. ASCII only (font is 0x20-0x7F). Config persists to SD via deskconf (morse.*).
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
#include <sys/stat.h>              // mkdir

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS   16000                 /* sidetone sample rate (smoother than 8k for high pitch) */

enum { S_HOME=0, S_SEND, S_DECODE, S_KEY, S_TRAIN, S_SET, S_COUNT };

static int       active = 0;
static int       screen = S_HOME;
static lv_obj_t *scr = NULL;
static void      show_screen(int s);   /* fwd: called from rx_event before its definition */

/* ---- persisted settings ---- */
static int set_wpm  = 13;        /* character speed, words per minute (PARIS) */
static int set_fwpm = 13;        /* Farnsworth overall speed (<= wpm; ==wpm means off) */
static int set_tone = 650;       /* sidetone pitch, Hz */
static int set_vol  = 5;         /* volume 1..10 */
static int set_aud  = 1;         /* output: audio sidetone */
static int set_lamp = 1;         /* output: on-screen lamp */
static int set_bkl  = 0;         /* output: LCD backlight pulse */

static void settings_load(void){
	set_wpm  = deskconf_get_int("morse.wpm", 13);  if(set_wpm<5)  set_wpm=5;   if(set_wpm>40)   set_wpm=40;
	set_fwpm = deskconf_get_int("morse.fwpm",13);  if(set_fwpm<5) set_fwpm=5;  if(set_fwpm>set_wpm) set_fwpm=set_wpm;
	set_tone = deskconf_get_int("morse.tone",650); if(set_tone<300)set_tone=300;if(set_tone>1200)set_tone=1200;
	set_vol  = deskconf_get_int("morse.vol", 5);   if(set_vol<1)  set_vol=1;   if(set_vol>10)   set_vol=10;
	set_aud  = deskconf_get_int("morse.aud", 1) ? 1 : 0;
	set_lamp = deskconf_get_int("morse.lamp",1) ? 1 : 0;
	set_bkl  = deskconf_get_int("morse.bkl", 0) ? 1 : 0;
}

/* ===================== prosigns (run-together letters) ===================== */
static const struct { const char *name, *code; } PROSIGN[] = {
	{ "AR", ".-.-." }, { "SK", "...-.-" }, { "KN", "-.--." }, { "BT", "-...-" },
	{ "AS", ".-..." }, { "SOS", "...---..." }, { "VE", "...-." }, { "CT", "-.-.-" },
};
#define NPROSIGN ((int)(sizeof(PROSIGN)/sizeof(PROSIGN[0])))
static const char *prosign_lookup(const char *name){    /* name must be upper-cased by caller */
	for(int i=0;i<NPROSIGN;i++) if(!strcmp(name, PROSIGN[i].name)) return PROSIGN[i].code;
	return NULL;
}

/* ===================== TX keyer (segment timeline) ===================== */
typedef struct { uint16_t ms; uint8_t tone; char ch; } seg_t;
#define MSG_MAX  120
#define SEG_MAX  (MSG_MAX*16)
static seg_t   *s_seg = NULL;       /* malloc'd timeline; freed on stop */
static int      s_nseg = 0, s_si = 0;
static int      s_play = 0;
static uint64_t s_seg_end = 0;      /* absolute us when the current VISUAL segment ends */
static char     s_last_ch = 0;      /* last letter shown in lbl_now */
static int      s_bkl_norm = 5;     /* backlight level to return to between marks */

/* sidetone generator state (timeline-driven; see audio_fill) */
static int      s_aud_on = 0;       /* did WE start the audio device? */
static float    s_phase = 0.f, s_env = 0.f;
static uint64_t s_aud_base = 0;     /* absolute playback time of audio sample 0 */
static uint64_t s_aud_n = 0;        /* count of samples generated */
static int      s_aud_si = 0;       /* audio-cursor segment index */
static uint64_t s_aud_segend = 0;   /* audio-cursor segment end (playback time) */

/* widgets shared by the keyer (set per-screen; NULL when absent) */
static lv_obj_t *lamp = NULL, *lbl_now = NULL;
/* send-screen widgets */
static lv_obj_t *ta_msg, *lbl_sstat;

static int push_seg(int tone, int ms, char ch){
	if(s_nseg >= SEG_MAX) return 0;
	s_seg[s_nseg].tone = (uint8_t)tone;
	s_seg[s_nseg].ms   = (uint16_t)(ms > 0 ? ms : 1);
	s_seg[s_nseg].ch   = ch;
	s_nseg++;
	return 1;
}
/* push one symbol's marks (with intra-element gaps), preceded by the pending letter/word gap */
static void emit_code(const char *code, int tc, int *emitted, int *pend_ms, char label){
	for(int e = 0; code[e]; e++){
		if(e == 0){ if(*emitted && *pend_ms > 0) push_seg(0, *pend_ms, label); }
		else        push_seg(0, tc, label);                       /* intra-element gap = 1 dit */
		push_seg(1, (code[e]=='-' ? 3*tc : tc), label);           /* the mark */
		*emitted = 1;
	}
}
/* expand `msg` into the mark/gap timeline. char-speed dit = tc; Farnsworth stretches the
   inter-letter (3) and inter-word (7) gaps to the slower tf. Prosigns: <AR>, <SK>, ... */
static int build_timeline(const char *msg){
	int tc = 1200 / set_wpm;                          /* dit ms at char speed */
	int tf = (set_fwpm < set_wpm) ? (1200 / set_fwpm) : tc;  /* dit ms for spacing */
	s_nseg = 0;
	int emitted = 0, pend = 0;
	for(int i = 0; msg[i] && i < MSG_MAX; ){
		char c = msg[i];
		if(c == '<'){                                 /* prosign token <XX> */
			char name[8]; int n = 0; int j = i + 1;
			while(msg[j] && msg[j] != '>' && n < 7){ name[n++] = (char)toupper((unsigned char)msg[j]); j++; }
			name[n] = 0;
			if(msg[j] == '>') j++;
			const char *pc = prosign_lookup(name);
			if(pc){ emit_code(pc, tc, &emitted, &pend, '*'); pend = 3*tf; }
			i = j;
			continue;
		}
		if(c == ' '){ if(pend < 7*tf) pend = 7*tf; i++; continue; }
		const char *code = morse_encode(c);
		if(code){ emit_code(code, tc, &emitted, &pend, (char)toupper((unsigned char)c)); pend = 3*tf; }
		i++;
	}
	if(emitted) push_seg(0, tc, ' ');                 /* trailing gap so the last mark decays cleanly */
	return s_nseg;
}

static void lamp_set(int on){
	if(lamp) lv_obj_set_style_bg_color(lamp, on ? KF_ACTIVE : KF_BORDER, 0);
}
static void bkl_set(int level){ uint8_t v=(uint8_t)level; reg_write(REG_BKL, &v, 1); }

static void apply_segment(int k){
	int tone = s_seg[k].tone;
	if(set_lamp) lamp_set(tone);
	if(set_bkl)  bkl_set(tone ? 9 : s_bkl_norm);
	if(tone && lbl_now && s_seg[k].ch != s_last_ch){
		s_last_ch = s_seg[k].ch;
		const char *code = morse_encode(s_last_ch);
		char buf[40]; snprintf(buf, sizeof buf, "%c   %s", s_last_ch, code ? code : "");
		lv_label_set_text(lbl_now, buf);
	}
}

static void stop_play(void){
	if(!s_play) return;
	s_play = 0;
	if(s_aud_on){ kf_audio_stop(); s_aud_on = 0; }
	lamp_set(0);
	if(set_bkl) bkl_set(s_bkl_norm);
	free(s_seg); s_seg = NULL; s_nseg = 0;
	if(lbl_sstat) lv_label_set_text(lbl_sstat, "ENTER send  F2 load  ESC back");
}

static void start_play_text(const char *msg){
	if(s_play) return;
	if(!msg || !msg[0]) return;
	s_seg = malloc(sizeof(seg_t) * SEG_MAX);
	if(!s_seg) return;
	if(build_timeline(msg) <= 0){ free(s_seg); s_seg = NULL; return; }

	s_bkl_norm = deskconf_get_int("bkl", 5);
	s_last_ch  = 0;
	uint64_t t0 = time_us_64();
	s_si = 0;
	s_seg_end = t0 + (uint64_t)s_seg[0].ms * 1000ULL;

	if(set_aud && !kf_audio_running()){
		kf_audio_start(FS);
		s_aud_on = kf_audio_running();          /* 0 if the ring malloc failed -> silent */
		s_phase = 0.f; s_env = 0.f;
		s_aud_base = t0; s_aud_n = 0;
		s_aud_si = 0; s_aud_segend = t0 + (uint64_t)s_seg[0].ms * 1000ULL;
	} else s_aud_on = 0;

	s_play = 1;
	if(lbl_sstat) lv_label_set_text(lbl_sstat, "sending...   ESC stop");
	apply_segment(0);
}

/* Generate sidetone from the timeline BY PLAYBACK TIME: each sample knows the true mark/space
   state at the moment it will actually play, so a deep (~60 ms) anti-underrun buffer adds
   latency but never smears the keying. */
static void audio_fill(void){
	if(!s_aud_on) return;
	uint64_t now = time_us_64();
	const uint64_t LEAD = 60000;                       /* keep ~60 ms queued ahead of playback */
	const float dphase = 2.f*(float)M_PI*(float)set_tone/(float)FS;
	const float estep  = 1.f / (0.005f*(float)FS);     /* 5 ms env ramp */
	const float amp    = (float)set_vol * 1000.0f;     /* 1000..10000, well below clip */
	int16_t buf[128*2]; int n = 0;
	while(kf_audio_space() > n + 1){
		uint64_t pt = s_aud_base + s_aud_n * 1000000ULL / (uint64_t)FS;  /* this sample's playback time */
		if(pt >= now + LEAD) break;
		while(s_aud_si < s_nseg && pt >= s_aud_segend){
			s_aud_si++;
			if(s_aud_si < s_nseg) s_aud_segend += (uint64_t)s_seg[s_aud_si].ms * 1000ULL;
		}
		int tone = (s_aud_si < s_nseg) ? s_seg[s_aud_si].tone : 0;
		float tgt = tone ? 1.f : 0.f;
		if(s_env < tgt){ s_env += estep; if(s_env > tgt) s_env = tgt; }
		else if(s_env > tgt){ s_env -= estep; if(s_env < tgt) s_env = tgt; }
		s_phase += dphase; if(s_phase >= 2.f*(float)M_PI) s_phase -= 2.f*(float)M_PI;
		int16_t v = (int16_t)(s_env * sinf(s_phase) * amp);
		buf[2*n] = v; buf[2*n+1] = v; n++;
		s_aud_n++;
		if(n == 128){ kf_audio_write(buf, 128); n = 0; }
	}
	if(n > 0) kf_audio_write(buf, n);
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
static lv_obj_t *new_screen(const char *title){
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
	return s;
}
static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, lv_color_t col){
	lv_obj_t *l = lv_label_create(par);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col, 0);
	return l;
}
static lv_obj_t *mk_lamp(lv_obj_t *par){
	lv_obj_t *o = lv_obj_create(par);
	lv_obj_set_size(o, 56, 56);
	lv_obj_set_style_radius(o, 28, 0);
	lv_obj_set_style_border_width(o, 2, 0);
	lv_obj_set_style_border_color(o, KF_BORDER_HI, 0);
	lv_obj_set_style_bg_color(o, KF_BORDER, 0);
	return o;
}

/* ===================== HOME menu ===================== */
static const struct { const char *label; int scr; } MENU[] = {
	{ "Send  (text -> Morse)",   S_SEND   },
	{ "Decode  (Morse -> text)", S_DECODE },
	{ "Key practice  (RX)",      S_KEY    },
	{ "Trainer  (Koch)",         S_TRAIN  },
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
	lv_obj_t *s = new_screen("Morse");
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
static void send_load_file(void){
	FILE *f = fopen(KF_NOTES "/morse.txt", "rb");
	if(!f){ if(lbl_sstat) lv_label_set_text(lbl_sstat, "no notes/morse.txt"); return; }
	char b[MSG_MAX+1]; size_t r = fread(b, 1, MSG_MAX, f); b[r] = 0; fclose(f);
	for(size_t i = 0; i < r; i++) if(b[i]=='\n' || b[i]=='\r') b[i] = ' ';
	lv_textarea_set_text(ta_msg, b);
	if(lbl_sstat) lv_label_set_text(lbl_sstat, "loaded notes/morse.txt");
}
static lv_obj_t *build_send(void){
	lv_obj_t *s = new_screen("Send");
	ta_msg = lv_textarea_create(s);
	lv_textarea_set_one_line(ta_msg, false);
	lv_obj_set_size(ta_msg, LCD_W-12, 64);
	lv_obj_align(ta_msg, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_style_text_font(ta_msg, KF_FONT, 0);
	lv_textarea_set_placeholder_text(ta_msg, "type a message...  <AR> <SK> for prosigns");

	lbl_now = mk_label(s, KF_FONT_BIG, KF_AMBER_HOT);
	lv_obj_align(lbl_now, LV_ALIGN_TOP_LEFT, 0, 104);
	lv_label_set_text(lbl_now, "");

	lamp = mk_lamp(s);
	lv_obj_align(lamp, LV_ALIGN_TOP_RIGHT, 0, 140);

	lbl_sstat = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_align(lbl_sstat, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(lbl_sstat, "ENTER send  F2 load  ESC back");
	return s;
}
static void send_key(uint8_t key, int mods){
	if(s_play){ if(key==DK_ESC||key==DK_BREAK||key==DK_ENTER) stop_play(); return; }
	if(key==DK_ESC || key==DK_BREAK) return;     /* caller -> HOME */
	if(key==DK_ENTER || key==DK_F1){ start_play_text(lv_textarea_get_text(ta_msg)); return; }
	if(key==DK_F1+1){ send_load_file(); return; } /* F2 */
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
		if(tl){ tok[tl] = 0; if(o < (int)sizeof(out)-1) out[o++] = morse_decode_token(tok); tl = 0; }
		if(c == '/'){ if(o < (int)sizeof(out)-1) out[o++] = ' '; }
		if(c == 0) break;
	}
	out[o] = 0;
	lv_label_set_text(lbl_decoded, o ? out : "");
}
static lv_obj_t *build_decode(void){
	lv_obj_t *s = new_screen("Decode");
	lv_obj_t *hint = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 28);
	lv_label_set_text(hint, "'.' '-'  space=letter  ENTER or '/'=word");

	ta_code = lv_textarea_create(s);
	lv_textarea_set_one_line(ta_code, false);
	lv_obj_set_size(ta_code, LCD_W-12, 86);
	lv_obj_align(ta_code, LV_ALIGN_TOP_LEFT, 0, 46);
	lv_obj_set_style_text_font(ta_code, KF_FONT, 0);
	lv_textarea_set_placeholder_text(ta_code, "... --- ...");

	lbl_decoded = mk_label(s, KF_FONT_BIG, KF_AMBER_HOT);
	lv_obj_set_width(lbl_decoded, LCD_W-12);
	lv_label_set_long_mode(lbl_decoded, LV_LABEL_LONG_WRAP);
	lv_obj_align(lbl_decoded, LV_ALIGN_TOP_LEFT, 0, 144);
	lv_label_set_text(lbl_decoded, "");

	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "ESC back");
	return s;
}
static void decode_key(uint8_t key, int mods){
	if(key==DK_ESC || key==DK_BREAK) return;
	if(key==DK_BACKSPACE){ lv_textarea_delete_char(ta_code); decode_refresh(); return; }
	if(key==DK_LEFT){  lv_textarea_cursor_left(ta_code);  return; }
	if(key==DK_RIGHT){ lv_textarea_cursor_right(ta_code); return; }
	if(key==DK_ENTER){ lv_textarea_add_char(ta_code, '/'); decode_refresh(); return; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)){ lv_textarea_add_char(ta_code, key); decode_refresh(); }
}

/* ===================== KEY PRACTICE (straight-key RX) ===================== */
/* Tap SPACE in rhythm. Marks shorter than ~2 dits are dots, longer are dashes; the dit
   estimate adapts to your keying. Gaps split letters/words (checked by timeout in rx_tick). */
static lv_obj_t *lbl_rx, *lbl_rxstat;
static int      rx_down = 0;
static uint64_t rx_tdown = 0, rx_lastup = 0;
static float    rx_dit = 92.f;            /* adaptive dit length, ms */
static char     rx_elem[10]; static int rx_nelem = 0;
static char     rx_text[256]; static int rx_tlen = 0;
static int      rx_word_pending = 0;

static void rx_show(void){
	char buf[300];
	int wpm = (int)(1200.f / (rx_dit > 1.f ? rx_dit : 1.f));
	rx_elem[rx_nelem] = 0;
	if(rx_nelem) snprintf(buf, sizeof buf, "%s [%s]   %d wpm", rx_text, rx_elem, wpm);
	else         snprintf(buf, sizeof buf, "%s   %d wpm", rx_text, wpm);
	if(lbl_rx) lv_label_set_text(lbl_rx, buf);
}
static void rx_flush_letter(void){
	if(!rx_nelem) return;
	rx_elem[rx_nelem] = 0;
	char c = morse_decode_token(rx_elem);
	if(rx_tlen < (int)sizeof(rx_text)-1) rx_text[rx_tlen++] = c;
	rx_text[rx_tlen] = 0;
	rx_nelem = 0;
	rx_word_pending = 1;
	rx_show();
}
static void rx_add_mark(uint64_t dur_us){
	float dur = dur_us / 1000.f;
	int dash = dur > 2.f*rx_dit;
	if(rx_nelem < (int)sizeof(rx_elem)-1) rx_elem[rx_nelem++] = dash ? '-' : '.';
	float est = dash ? dur/3.f : dur;             /* implied dit length from this mark */
	rx_dit = 0.7f*rx_dit + 0.3f*est;
	if(rx_dit < 30.f) rx_dit = 30.f; if(rx_dit > 400.f) rx_dit = 400.f;
	rx_word_pending = 0;
	rx_show();
}
static void rx_save(void){
	mkdir(KF_NOTES, 0777);
	FILE *f = fopen(KF_NOTES "/morse-rx.txt", "a");
	if(!f){ if(lbl_rxstat) lv_label_set_text(lbl_rxstat, "save failed (SD?)"); return; }
	fputs(rx_text, f); fputc('\n', f); fclose(f);
	if(lbl_rxstat) lv_label_set_text(lbl_rxstat, "saved -> notes/morse-rx.txt");
}
static lv_obj_t *build_key(void){
	lv_obj_t *s = new_screen("Key practice");
	lv_obj_t *hint = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(hint, LV_ALIGN_TOP_LEFT, 0, 28);
	lv_label_set_text(hint, "tap SPACE as a straight key");

	lamp = mk_lamp(s);
	lv_obj_align(lamp, LV_ALIGN_TOP_RIGHT, 0, 46);

	lbl_rx = mk_label(s, KF_FONT_BIG, KF_AMBER_HOT);
	lv_obj_set_width(lbl_rx, LCD_W-80);
	lv_label_set_long_mode(lbl_rx, LV_LABEL_LONG_WRAP);
	lv_obj_align(lbl_rx, LV_ALIGN_TOP_LEFT, 0, 50);
	lv_label_set_text(lbl_rx, "");

	lbl_rxstat = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_align(lbl_rxstat, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(lbl_rxstat, "SPACE key  F1 save  BKSP del  ESC back");

	rx_down = 0; rx_nelem = 0; rx_tlen = 0; rx_text[0] = 0; rx_word_pending = 0;
	rx_dit = 1200.f / set_wpm; rx_lastup = time_us_64();
	return s;
}
static void rx_event(uint8_t st, uint8_t key){
	if(st == KS_PRESS){
		if(key==DK_ESC || key==DK_BREAK){ show_screen(S_HOME); return; }
		if(key==DK_F1){ rx_save(); return; }
		if(key==DK_BACKSPACE){ if(rx_tlen){ rx_text[--rx_tlen]=0; rx_show(); } return; }
		if(key==0x20){ if(!rx_down){ rx_down=1; rx_tdown=time_us_64(); lamp_set(1); } return; }
	} else if(st == KS_RELEASE){
		if(key==0x20 && rx_down){ rx_down=0; lamp_set(0); rx_add_mark(time_us_64()-rx_tdown); rx_lastup=time_us_64(); }
	}
}
static void rx_tick(void){
	if(rx_down) return;
	uint64_t gap = time_us_64() - rx_lastup;
	if(rx_nelem && gap > (uint64_t)(2.f*rx_dit*1000.f)) rx_flush_letter();
	if(rx_word_pending && gap > (uint64_t)(5.f*rx_dit*1000.f)){
		if(rx_tlen < (int)sizeof(rx_text)-1){ rx_text[rx_tlen++]=' '; rx_text[rx_tlen]=0; }
		rx_word_pending = 0; rx_show();
	}
}

/* ===================== TRAINER (Koch) ===================== */
static const char KOCH[] = "KMRSUAPTLOWI.NJEF0Y,VG5/Q9ZH38B?427C1D6X";
#define KOCH_N ((int)(sizeof(KOCH)-1))
static lv_obj_t *lbl_koch, *ta_train, *lbl_trstat;
static int  tr_lesson = 1;                 /* lesson n -> first (n+1) Koch chars */
static char tr_answer[8]; static int tr_have = 0;

static void koch_refresh(void){
	char set[48]; int nc = tr_lesson + 1; if(nc > KOCH_N) nc = KOCH_N;
	memcpy(set, KOCH, nc); set[nc] = 0;
	char buf[80]; snprintf(buf, sizeof buf, "Lesson %d:  %s", tr_lesson, set);
	if(lbl_koch) lv_label_set_text(lbl_koch, buf);
}
static void koch_new_group(void){
	int nc = tr_lesson + 1; if(nc > KOCH_N) nc = KOCH_N;
	for(int i = 0; i < 5; i++) tr_answer[i] = KOCH[rand() % nc];
	tr_answer[5] = 0; tr_have = 1;
	if(ta_train) lv_textarea_set_text(ta_train, "");
	if(lbl_trstat) lv_label_set_text(lbl_trstat, "sending... then type your copy");
	start_play_text(tr_answer);
}
static void koch_check(void){
	if(!tr_have){ if(lbl_trstat) lv_label_set_text(lbl_trstat, "F1 to send a group first"); return; }
	const char *in = lv_textarea_get_text(ta_train);
	int correct = 0;
	for(int i = 0; i < 5; i++){
		char a = tr_answer[i];
		char b = in[i] ? (char)toupper((unsigned char)in[i]) : 0;
		if(a == b) correct++;
	}
	char buf[64]; snprintf(buf, sizeof buf, "%d/5 correct   answer: %s", correct, tr_answer);
	if(lbl_trstat) lv_label_set_text(lbl_trstat, buf);
}
static lv_obj_t *build_train(void){
	lv_obj_t *s = new_screen("Trainer");
	lbl_koch = mk_label(s, KF_FONT, KF_AMBER);
	lv_obj_set_width(lbl_koch, LCD_W-12);
	lv_label_set_long_mode(lbl_koch, LV_LABEL_LONG_WRAP);
	lv_obj_align(lbl_koch, LV_ALIGN_TOP_LEFT, 0, 30);

	lamp = mk_lamp(s);
	lv_obj_align(lamp, LV_ALIGN_TOP_RIGHT, 0, 64);

	ta_train = lv_textarea_create(s);
	lv_textarea_set_one_line(ta_train, true);
	lv_obj_set_size(ta_train, LCD_W-80, 36);
	lv_obj_align(ta_train, LV_ALIGN_TOP_LEFT, 0, 78);
	lv_obj_set_style_text_font(ta_train, KF_FONT_BIG, 0);
	lv_textarea_set_placeholder_text(ta_train, "copy");

	lbl_trstat = mk_label(s, KF_FONT, KF_TEXT_DIM);
	lv_obj_set_width(lbl_trstat, LCD_W-12);
	lv_label_set_long_mode(lbl_trstat, LV_LABEL_LONG_WRAP);
	lv_obj_align(lbl_trstat, LV_ALIGN_TOP_LEFT, 0, 130);

	lv_obj_t *h = mk_label(s, KF_FONT, KF_TEXT_MUTED);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_label_set_text(h, "Up/Dn lesson  F1 send  ENTER check  ESC back");

	tr_have = 0;
	koch_refresh();
	lv_label_set_text(lbl_trstat, "Up/Dn pick lesson, F1 to send a group");
	return s;
}
static void train_key(uint8_t key, int mods){
	if(s_play){ if(key==DK_ESC||key==DK_BREAK) stop_play(); return; }
	if(key==DK_ESC || key==DK_BREAK) return;
	if(key==DK_UP){   if(tr_lesson < KOCH_N-1) tr_lesson++; koch_refresh(); return; }
	if(key==DK_DOWN){ if(tr_lesson > 1) tr_lesson--; koch_refresh(); return; }
	if(key==DK_F1){ koch_new_group(); return; }
	if(key==DK_ENTER){ koch_check(); return; }
	if(key==DK_BACKSPACE){ lv_textarea_delete_char(ta_train); return; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)) lv_textarea_add_char(ta_train, key);
}

/* ===================== SETTINGS ===================== */
static int       set_sel = 0;
static lv_obj_t *set_rows[7];
#define NSET 7
static void settings_refresh(void){
	char b[48];
	snprintf(b,sizeof b,"WPM (char)     %d", set_wpm);                            lv_label_set_text(set_rows[0],b);
	if(set_fwpm<set_wpm) snprintf(b,sizeof b,"Farnsworth     %d", set_fwpm);
	else                 snprintf(b,sizeof b,"Farnsworth     off");
	lv_label_set_text(set_rows[1],b);
	snprintf(b,sizeof b,"Pitch Hz       %d", set_tone);                           lv_label_set_text(set_rows[2],b);
	snprintf(b,sizeof b,"Volume         %d", set_vol);                            lv_label_set_text(set_rows[3],b);
	snprintf(b,sizeof b,"Audio tone     %s", set_aud ?"ON":"off");                lv_label_set_text(set_rows[4],b);
	snprintf(b,sizeof b,"Screen lamp    %s", set_lamp?"ON":"off");                lv_label_set_text(set_rows[5],b);
	snprintf(b,sizeof b,"Backlight key  %s", set_bkl ?"ON":"off");                lv_label_set_text(set_rows[6],b);
	for(int i=0;i<NSET;i++){
		int on=(i==set_sel);
		lv_obj_set_style_bg_opa(set_rows[i], on?LV_OPA_COVER:LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(set_rows[i], KF_AMBER_DIM, 0);
		lv_obj_set_style_text_color(set_rows[i], on?KF_BG_DEEP:KF_TEXT, 0);
	}
}
static lv_obj_t *build_settings(void){
	lv_obj_t *s = new_screen("Settings");
	lv_obj_t *col = lv_obj_create(s);
	lv_obj_remove_style_all(col);
	lv_obj_set_size(col, LCD_W-12, KF_CONTENT_H-40);
	lv_obj_align(col, LV_ALIGN_TOP_LEFT, 0, 30);
	lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(col, 3, 0);
	for(int i=0;i<NSET;i++){
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
	case 0: set_wpm  += dir;     if(set_wpm<5)set_wpm=5; if(set_wpm>40)set_wpm=40;
	        if(set_fwpm>set_wpm){set_fwpm=set_wpm;deskconf_set_int("morse.fwpm",set_fwpm);}
	        deskconf_set_int("morse.wpm",set_wpm); break;
	case 1: set_fwpm += dir;     if(set_fwpm<5)set_fwpm=5; if(set_fwpm>set_wpm)set_fwpm=set_wpm;
	        deskconf_set_int("morse.fwpm",set_fwpm); break;
	case 2: set_tone += dir*25;  if(set_tone<300)set_tone=300; if(set_tone>1200)set_tone=1200;
	        deskconf_set_int("morse.tone",set_tone); break;
	case 3: set_vol  += dir;     if(set_vol<1)set_vol=1; if(set_vol>10)set_vol=10;
	        deskconf_set_int("morse.vol",set_vol); break;
	case 4: set_aud  = !set_aud;  deskconf_set_int("morse.aud", set_aud);  break;
	case 5: set_lamp = !set_lamp; deskconf_set_int("morse.lamp",set_lamp); break;
	case 6: set_bkl  = !set_bkl;  deskconf_set_int("morse.bkl", set_bkl);  break;
	}
	settings_refresh();
}
static void settings_key(uint8_t key, int mods){
	(void)mods;
	if(key==DK_ESC || key==DK_BREAK) return;
	if(key==DK_UP){   set_sel=(set_sel+NSET-1)%NSET; settings_refresh(); return; }
	if(key==DK_DOWN){ set_sel=(set_sel+1)%NSET;      settings_refresh(); return; }
	if(key==DK_LEFT)  settings_adjust(-1);
	if(key==DK_RIGHT) settings_adjust(+1);
}

/* ===================== screen switching + input pump ===================== */
static void show_screen(int s){
	if(s_play) stop_play();
	lv_obj_t *old = scr;
	screen = s;
	lamp = lbl_now = ta_msg = lbl_sstat = NULL;
	ta_code = lbl_decoded = NULL;
	lbl_rx = lbl_rxstat = NULL;
	lbl_koch = ta_train = lbl_trstat = NULL;
	switch(s){
	case S_SEND:   scr = build_send();     break;
	case S_DECODE: scr = build_decode();   break;
	case S_KEY:    scr = build_key();      break;
	case S_TRAIN:  scr = build_train();    break;
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
		if(screen == S_KEY){ rx_event(st, key); continue; }   /* needs press+release edges */
		if(st == KS_RELEASE) continue;
		int mods = uart_mods();
		if(screen == S_HOME){ home_key(key); continue; }
		if((key==DK_ESC || key==DK_BREAK) && !(screen==S_SEND && s_play) && !(screen==S_TRAIN && s_play)){
			show_screen(S_HOME); continue;
		}
		switch(screen){
		case S_SEND:   send_key(key, mods);     break;
		case S_DECODE: decode_key(key, mods);   break;
		case S_TRAIN:  train_key(key, mods);    break;
		case S_SET:    settings_key(key, mods); break;
		default: break;
		}
	}
	if(s_play){ keyer_tick(); audio_fill(); }
	if(screen == S_KEY) rx_tick();
}

void app_morse_open(void){
	settings_load();
	srand((unsigned)time_us_64());
	active = 1; home_sel = 0; set_sel = 0; tr_lesson = 1; scr = NULL;
	show_screen(S_HOME);
}
