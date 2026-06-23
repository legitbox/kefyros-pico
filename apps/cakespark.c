// apps/cakespark.c — CakeSpark REPL for Kefyros.
//
// CakeSpark is the user's own sandboxed scripting VM (Nim, compiled to C, embedded
// via lib/cakespark). It's tick-based with hard resource limits, so a script can't
// block, run away, or OOM the device — ideal for a handheld REPL.
//
// Like the editor, this GRABS the keyboard and reads raw device keys (cakespark_poll
// runs from the superloop). ENTER submits a line; ESC quits to the launcher; F1 resets
// the session.
//
// REPL model: cake_compile() builds a FRESH VM each call (wiping variables), so we keep
// the whole accepted program in `committed[]` and re-compile+run it every submit. To make
// that feel like a REPL we (a) track block depth — a line that opens if/while/loop/each/
// block accumulates in `pending[]` and only runs once the matching `end` closes it — and
// (b) print only the NEW slice of the output buffer since the last successful run. A line
// that fails to compile or run is rolled back, so `committed[]` only ever holds code that
// works and the session stays usable.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "cakespark.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

extern void NimMain(void);          /* Nim runtime init (module-level state) — call once */

#define SRC_MAX 4000

static lv_obj_t  *scr, *log_w, *input_ta, *lbl_legend;
static lv_group_t *grp;
static int   active = 0;            /* cakespark_poll runs only while the REPL is up */

#define CAND_MAX (2*SRC_MAX)

static CakeVM *vm = NULL;
/* The session buffers (~16 KB total) are only used while the REPL is open, so they're
   heap-allocated on open and freed on close rather than parked in .bss — keeps the idle
   heap floor low so memory-hungry apps (e.g. the calc plotter) have room. One block
   carved into the three buffers; `s_buf` owns it. */
static char  *s_buf;                /* owns committed|pending|cand                         */
static char  *committed;            /* accepted program (only code that compiled+ran) [SRC_MAX] */
static char  *pending;              /* lines of an in-progress block (depth > 0)      [SRC_MAX] */
static char  *cand;                 /* compile candidate scratch                      [CAND_MAX] */
static int    depth = 0;            /* open block nesting */
static size_t last_out;             /* length of the output buffer after the last good run */

/* ---- transcript rendering ---- */
static void add_block(const char *text, lv_color_t color){
	if(!text || !text[0]) return;
	lv_obj_t *b = lv_obj_create(log_w);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LCD_W-12);
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(b, 2, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, LCD_W-12-4);
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_text(l, text);
	lv_obj_update_layout(log_w);
	lv_obj_scroll_to_view(b, LV_ANIM_OFF);
}

/* first whitespace-delimited word of a line */
static void first_word(const char *line, char *out, int n){
	while(*line==' '||*line=='\t') line++;
	int i = 0;
	while(line[i] && line[i]!=' ' && line[i]!='\t' && i<n-1){ out[i]=line[i]; i++; }
	out[i] = 0;
}
static int opens_block(const char *w){
	return !strcmp(w,"if")||!strcmp(w,"while")||!strcmp(w,"loop")||
	       !strcmp(w,"each")||!strcmp(w,"block");
}

static void run_session(const char *extra_pending){
	/* candidate = committed + pending(+the new line, already folded into pending) */
	snprintf(cand, CAND_MAX, "%s%s", committed, extra_pending ? extra_pending : pending);

	if(cake_compile(vm, cand) != CAKE_OK){
		add_block(cake_get_error(vm), KF_AMBER_DIM);   /* roll back: keep committed clean */
		pending[0] = 0; depth = 0;
		return;
	}
	int rc = cake_run(vm);
	const char *out = cake_get_output(vm);
	size_t olen = out ? strlen(out) : 0;
	if(olen > last_out) add_block(out + last_out, KF_TEXT);   /* only the new output */
	if(rc != CAKE_OK){
		add_block(cake_get_error(vm), KF_AMBER_DIM);
		pending[0] = 0; depth = 0;                     /* discard the failing line(s) */
		/* committed unchanged; re-derive last_out from a clean recompile next time */
		return;
	}
	/* success: fold pending into committed, remember output watermark */
	size_t cl = strlen(committed);
	snprintf(committed + cl, (size_t)SRC_MAX - cl, "%s", pending);
	pending[0] = 0; depth = 0;
	last_out = olen;
}

static void submit_line(const char *line){
	add_block(line, KF_AMBER_BR);                      /* echo input */

	if((strlen(committed)+strlen(pending)+strlen(line)+2) >= SRC_MAX){
		add_block("session full - press F1 to reset", KF_AMBER_DIM);
		return;
	}
	/* fold the line into pending, update block depth */
	char w[16]; first_word(line, w, sizeof w);
	if(!strcmp(w,"end")){ if(depth>0) depth--; }
	size_t pl = strlen(pending);
	snprintf(pending + pl, (size_t)SRC_MAX - pl, "%s\n", line);
	if(opens_block(w)) depth++;

	if(depth > 0){ add_block(".. (block open)", KF_TEXT_MUTED); return; }
	run_session(NULL);
}

static void reset_session(void){
	committed[0] = 0; pending[0] = 0; depth = 0; last_out = 0;
	cake_compile(vm, "");
	lv_obj_clean(log_w);
	add_block("session reset", KF_TEXT_MUTED);
}

/* ---- grabbed key pump (from the superloop) ---- */
void cakespark_poll(void){
	if(!active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		switch(key){
		case DK_ESC:
		case DK_BREAK:     active=0; kf_grab_input(0); kf_back_to_launcher(); return;
		case DK_F1:        reset_session(); break;
		case DK_ENTER: {
			const char *t = lv_textarea_get_text(input_ta);
			if(t && t[0]){ char line[256]; snprintf(line,sizeof line,"%s",t); lv_textarea_set_text(input_ta,""); submit_line(line); }
			break;
		}
		case DK_BACKSPACE: lv_textarea_delete_char(input_ta); break;
		case DK_LEFT:      lv_textarea_cursor_left(input_ta);  break;
		case DK_RIGHT:     lv_textarea_cursor_right(input_ta); break;
		case DK_UP:        lv_obj_scroll_by(log_w, 0,  60, LV_ANIM_ON); break;
		case DK_DOWN:      lv_obj_scroll_by(log_w, 0, -60, LV_ANIM_ON); break;
		case DK_PGUP:      lv_obj_scroll_by(log_w, 0,  200, LV_ANIM_ON); break;
		case DK_PGDN:      lv_obj_scroll_by(log_w, 0, -200, LV_ANIM_ON); break;
		default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(input_ta, key); break;
		}
	}
}

static void on_del(lv_event_t *e){ (void)e;
	active = 0;
	if(vm){ cake_free(vm); vm = NULL; }
	free(s_buf); s_buf = committed = pending = cand = NULL;
	scr = NULL;
}

void app_cakespark_open(void){
	static int nim_inited = 0;
	if(!nim_inited){ NimMain(); nim_inited = 1; }   /* init the Nim runtime once */

	/* one block -> committed[SRC_MAX] | pending[SRC_MAX] | cand[CAND_MAX] (freed in on_del) */
	s_buf = malloc(SRC_MAX + SRC_MAX + CAND_MAX);
	if(!s_buf){ kf_back_to_launcher(); return; }    /* no RAM -> bail to desktop */
	committed = s_buf; pending = s_buf + SRC_MAX; cand = s_buf + 2*SRC_MAX;
	committed[0] = 0; pending[0] = 0; depth = 0; last_out = 0;

	CakeLimits limits;
	limits.maxCallDepth  = 64;
	limits.maxIterations = 100000;
	limits.maxTicks      = 200000;
	limits.maxVariables  = 256;
	limits.maxMemory     = 64*1024;
	vm = cake_new(limits);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr, on_del, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "CakeSpark REPL");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 1);

	int log_h = KF_CONTENT_H - 18 - 24 - 14;
	log_w = lv_obj_create(scr);
	lv_obj_remove_style_all(log_w);
	lv_obj_set_size(log_w, LCD_W, log_h);
	lv_obj_align(log_w, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_flex_flow(log_w, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(log_w, 3, 0);
	lv_obj_set_style_pad_all(log_w, 4, 0);
	lv_obj_set_style_bg_color(log_w, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(log_w, LV_OPA_COVER, 0);
	lv_obj_set_scroll_dir(log_w, LV_DIR_VER);

	lbl_legend = lv_label_create(scr);
	lv_obj_set_style_text_font(lbl_legend, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_legend, KF_TEXT_DIM, 0);
	lv_obj_align(lbl_legend, LV_ALIGN_BOTTOM_LEFT, 4, -24);
	lv_label_set_text(lbl_legend, "ENTER run   F1 reset   ESC quit");

	input_ta = lv_textarea_create(scr);
	lv_textarea_set_one_line(input_ta, true);
	lv_textarea_set_placeholder_text(input_ta, "cake>  e.g.  set 2 >> x");
	lv_obj_set_size(input_ta, LCD_W, 24);
	lv_obj_align(input_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(input_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(input_ta, KF_BG, 0);
	lv_obj_set_style_text_color(input_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(input_ta, 0, 0);
	lv_obj_set_style_border_width(input_ta, 0, 0);
	lv_obj_set_style_outline_width(input_ta, 0, 0);
	lv_obj_set_style_outline_width(input_ta, 0, LV_STATE_FOCUSED);

	grp = kf_use_group();
	lv_group_add_obj(grp, input_ta);
	lv_group_focus_obj(input_ta);

	add_block("CakeSpark 1.0  -  one rule: function args >> dest", KF_TEXT_MUTED);

	kf_grab_input(1);
	active = 1;
	lv_screen_load(scr);
}
