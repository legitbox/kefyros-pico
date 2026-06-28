// apps/cakespark.c — CakeSpark for Kefyros: a two-mode scripting app.
//
// Opening the app lands on a MENU with two choices:
//   * Programs — a file manager + editor over /kefyros/scripts/*.cake. You can
//                create / pick / edit a program and RUN it, either from the editor
//                (F2) or straight from the program list (F2). Ships examples.
//   * REPL     — the live read-eval-print loop (type a line, see output now).
//
// CakeSpark is the user's own sandboxed scripting VM (Nim, compiled to C, embedded
// via lib/cakespark). It's tick-based with hard resource limits, so a script can't
// block, run away, or OOM the device. The output builtin is `log`.
//
// SCREENS / INPUT MODEL
//   MENU  : LVGL group navigation (NOT grabbed). ENTER selects, ESC -> launcher.
//   LIST  : GRABBED (so it can read F-keys). Manual up/down highlight.
//           F1/ENTER edit, F2 run, F3 rename, F4 delete; ESC -> launcher; a
//           "[ Menu ]" row hops back to the menu.
//   EDIT  : GRABBED. nano-style. F1 Save F2 Run F3 SaveAs F4 New F5 Quit(->list).
//           ESC -> launcher. Backspace edits text.
//   REPL  : GRABBED. ENTER runs the line, F1 reset, ESC -> launcher.
// Grabbed screens are pumped by cakespark_poll() from the superloop. Each screen
// owns its heap buffers and frees them in its own LV_EVENT_DELETE handler; the VM
// is freed explicitly at every screen transition (so there's no async-delete race).
//
// REPL model: cake_compile() builds a FRESH VM each call (wiping variables), so we
// keep the accepted program in `committed[]` and re-compile+run it every submit. To
// feel like a REPL we (a) track block depth — a line that opens if/while/loop/each/
// block accumulates in `pending[]` and only runs once the matching `end` closes it —
// and (b) print only the NEW slice of the output buffer since the last good run. A
// line that fails is rolled back, so `committed[]` only ever holds working code.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "cakespark.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

extern void NimMain(void);          /* Nim runtime init (module-level state) — call once */

/* ===== shared state ===== */
typedef enum { MODE_NONE, MODE_MENU, MODE_LIST, MODE_EDIT, MODE_REPL } cs_mode_t;
static cs_mode_t mode = MODE_NONE;
static lv_obj_t  *scr;              /* the active app screen */
static lv_group_t *grp;

/* ---- the embedded VM (created lazily, freed at each screen transition) ---- */
static CakeVM *vm = NULL;
static int     nim_inited = 0;
static void ensure_nim(void){ if(!nim_inited){ NimMain(); nim_inited = 1; } }
static void vm_make(void){
	if(vm) return;
	CakeLimits L;
	L.maxCallDepth  = 64;
	L.maxIterations = 100000;
	L.maxTicks      = 200000;
	L.maxVariables  = 256;
	L.maxMemory     = 64*1024;
	vm = cake_new(L);
}
static void vm_free(void){ if(vm){ cake_free(vm); vm = NULL; } }

/* ---- forward decls ---- */
static void build_menu(void);
static void build_proglist(void);
static void open_editor(const char *p);
static void start_repl(void);

/* swap the active screen, deleting the old tree (async: some transitions fire from
   inside an LVGL click callback, so a sync delete would free a dispatching screen). */
static void swap_screen(lv_obj_t *next){
	lv_obj_t *prev = lv_screen_active();
	lv_screen_load(next);
	if(prev && prev != next) lv_obj_delete_async(prev);
}

/* ===== transcript rendering (REPL log + run-output overlay) ===== */
static void add_block(lv_obj_t *parent, const char *text, lv_color_t color){
	if(!text || !text[0]) return;
	lv_obj_t *b = lv_obj_create(parent);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LV_PCT(100));
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(b, 2, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, LV_PCT(100));
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_text(l, text);
	lv_obj_update_layout(parent);
	lv_obj_scroll_to_view(b, LV_ANIM_OFF);
}

/* ===== run-output overlay (shared by editor F2 and list F2) ===== */
static lv_obj_t *ov_box, *ov_log;
static int       ov_up = 0;

static void overlay_close(void){
	if(ov_box){ lv_obj_delete(ov_box); ov_box = NULL; ov_log = NULL; }
	ov_up = 0;
}
static void run_and_show(const char *src){
	ensure_nim(); vm_make();
	if(!vm){ return; }

	ov_box = lv_obj_create(scr);
	lv_obj_set_size(ov_box, LCD_W-16, KF_CONTENT_H-24);
	lv_obj_center(ov_box);
	lv_obj_set_style_bg_color(ov_box, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(ov_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(ov_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(ov_box, 2, 0);
	lv_obj_set_style_radius(ov_box, 0, 0);
	lv_obj_set_style_pad_all(ov_box, 3, 0);
	lv_obj_clear_flag(ov_box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *t = lv_label_create(ov_box);
	lv_label_set_text(t, "output");
	lv_obj_set_style_text_font(t, KF_FONT, 0);
	lv_obj_set_style_text_color(t, KF_AMBER_BR, 0);
	lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);

	ov_log = lv_obj_create(ov_box);
	lv_obj_remove_style_all(ov_log);
	lv_obj_set_size(ov_log, LV_PCT(100), KF_CONTENT_H-24-18-16);
	lv_obj_align(ov_log, LV_ALIGN_TOP_MID, 0, 16);
	lv_obj_set_flex_flow(ov_log, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(ov_log, 2, 0);
	lv_obj_set_scroll_dir(ov_log, LV_DIR_VER);

	lv_obj_t *h = lv_label_create(ov_box);
	lv_label_set_text(h, "ESC/ENTER close   up/dn scroll");
	lv_obj_set_style_text_font(h, KF_FONT, 0);
	lv_obj_set_style_text_color(h, KF_TEXT_DIM, 0);
	lv_obj_align(h, LV_ALIGN_BOTTOM_LEFT, 0, 0);

	int crc = cake_compile(vm, src);
	if(crc != CAKE_OK){
		add_block(ov_log, cake_get_error(vm), KF_AMBER_DIM);
	} else {
		int rc = cake_run(vm);
		const char *out = cake_get_output(vm);
		if(out && out[0]) add_block(ov_log, out, KF_TEXT);
		else if(rc == CAKE_OK) add_block(ov_log, "(no output)", KF_TEXT_MUTED);
		if(rc != CAKE_OK) add_block(ov_log, cake_get_error(vm), KF_AMBER_DIM);
	}
	ov_up = 1;
}
static void overlay_key(uint8_t key){
	switch(key){
	case DK_ESC: case DK_ENTER: case DK_BREAK: overlay_close(); break;
	case DK_UP:   lv_obj_scroll_by(ov_log, 0,  60, LV_ANIM_ON);  break;
	case DK_DOWN: lv_obj_scroll_by(ov_log, 0, -60, LV_ANIM_ON);  break;
	case DK_PGUP: lv_obj_scroll_by(ov_log, 0,  200, LV_ANIM_ON); break;
	case DK_PGDN: lv_obj_scroll_by(ov_log, 0, -200, LV_ANIM_ON); break;
	default: break;
	}
}

/* ===================================================================== */
/* ===========================  REPL  ================================== */
/* ===================================================================== */
#define SRC_MAX  4000
#define CAND_MAX (2*SRC_MAX)

static lv_obj_t *repl_log, *repl_input, *repl_legend;
static char  *repl_buf;             /* owns committed|pending|cand */
static char  *committed;            /* accepted program (compiled+ran) [SRC_MAX] */
static char  *pending;              /* in-progress block (depth>0)     [SRC_MAX] */
static char  *cand;                 /* compile scratch                 [CAND_MAX] */
static int    depth = 0;
static size_t last_out;

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
static void run_session(void){
	snprintf(cand, CAND_MAX, "%s%s", committed, pending);
	if(cake_compile(vm, cand) != CAKE_OK){
		add_block(repl_log, cake_get_error(vm), KF_AMBER_DIM);
		pending[0] = 0; depth = 0;
		return;
	}
	int rc = cake_run(vm);
	const char *out = cake_get_output(vm);
	size_t olen = out ? strlen(out) : 0;
	if(olen > last_out) add_block(repl_log, out + last_out, KF_TEXT);
	if(rc != CAKE_OK){
		add_block(repl_log, cake_get_error(vm), KF_AMBER_DIM);
		pending[0] = 0; depth = 0;
		return;
	}
	size_t cl = strlen(committed);
	snprintf(committed + cl, (size_t)SRC_MAX - cl, "%s", pending);
	pending[0] = 0; depth = 0;
	last_out = olen;
}
static void submit_line(const char *line){
	add_block(repl_log, line, KF_AMBER_BR);
	if((strlen(committed)+strlen(pending)+strlen(line)+2) >= SRC_MAX){
		add_block(repl_log, "session full - press F1 to reset", KF_AMBER_DIM);
		return;
	}
	char w[16]; first_word(line, w, sizeof w);
	if(!strcmp(w,"end")){ if(depth>0) depth--; }
	size_t pl = strlen(pending);
	snprintf(pending + pl, (size_t)SRC_MAX - pl, "%s\n", line);
	if(opens_block(w)) depth++;
	if(depth > 0){ add_block(repl_log, ".. (block open)", KF_TEXT_MUTED); return; }
	run_session();
}
static void reset_session(void){
	committed[0] = 0; pending[0] = 0; depth = 0; last_out = 0;
	cake_compile(vm, "");
	lv_obj_clean(repl_log);
	add_block(repl_log, "session reset", KF_TEXT_MUTED);
}
static void repl_on_del(lv_event_t *e){ (void)e;
	free(repl_buf); repl_buf = committed = pending = cand = NULL;
}
static void repl_key(uint8_t key){
	switch(key){
	case DK_ESC:
	case DK_BREAK:     vm_free(); kf_grab_input(0); kf_back_to_launcher(); return;
	case DK_F1:        reset_session(); break;
	case DK_ENTER: {
		const char *t = lv_textarea_get_text(repl_input);
		if(t && t[0]){ char line[256]; snprintf(line,sizeof line,"%s",t);
			lv_textarea_set_text(repl_input,""); submit_line(line); }
		break;
	}
	case DK_BACKSPACE: lv_textarea_delete_char(repl_input); break;
	case DK_LEFT:      lv_textarea_cursor_left(repl_input);  break;
	case DK_RIGHT:     lv_textarea_cursor_right(repl_input); break;
	case DK_UP:        lv_obj_scroll_by(repl_log, 0,  60, LV_ANIM_ON); break;
	case DK_DOWN:      lv_obj_scroll_by(repl_log, 0, -60, LV_ANIM_ON); break;
	case DK_PGUP:      lv_obj_scroll_by(repl_log, 0,  200, LV_ANIM_ON); break;
	case DK_PGDN:      lv_obj_scroll_by(repl_log, 0, -200, LV_ANIM_ON); break;
	default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(repl_input, key); break;
	}
}
static void start_repl(void){
	ensure_nim(); vm_make();

	repl_buf = malloc(SRC_MAX + SRC_MAX + CAND_MAX);
	if(!repl_buf){ kf_grab_input(0); kf_back_to_launcher(); return; }
	committed = repl_buf; pending = repl_buf + SRC_MAX; cand = repl_buf + 2*SRC_MAX;
	committed[0] = 0; pending[0] = 0; depth = 0; last_out = 0;

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr, repl_on_del, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "CakeSpark REPL");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 1);

	int log_h = KF_CONTENT_H - 18 - 24 - 14;
	repl_log = lv_obj_create(scr);
	lv_obj_remove_style_all(repl_log);
	lv_obj_set_size(repl_log, LCD_W, log_h);
	lv_obj_align(repl_log, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_flex_flow(repl_log, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(repl_log, 3, 0);
	lv_obj_set_style_pad_all(repl_log, 4, 0);
	lv_obj_set_style_bg_color(repl_log, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(repl_log, LV_OPA_COVER, 0);
	lv_obj_set_scroll_dir(repl_log, LV_DIR_VER);

	repl_legend = lv_label_create(scr);
	lv_obj_set_style_text_font(repl_legend, KF_FONT, 0);
	lv_obj_set_style_text_color(repl_legend, KF_TEXT_DIM, 0);
	lv_obj_align(repl_legend, LV_ALIGN_BOTTOM_LEFT, 4, -24);
	lv_label_set_text(repl_legend, "ENTER run   F1 reset   ESC quit");

	repl_input = lv_textarea_create(scr);
	lv_textarea_set_one_line(repl_input, true);
	lv_textarea_set_placeholder_text(repl_input, "cake>  e.g.  set 2 >> x");
	lv_obj_set_size(repl_input, LCD_W, 24);
	lv_obj_align(repl_input, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(repl_input, KF_FONT, 0);
	lv_obj_set_style_bg_color(repl_input, KF_BG, 0);
	lv_obj_set_style_text_color(repl_input, KF_TEXT, 0);
	lv_obj_set_style_radius(repl_input, 0, 0);
	lv_obj_set_style_border_width(repl_input, 0, 0);
	lv_obj_set_style_outline_width(repl_input, 0, 0);
	lv_obj_set_style_outline_width(repl_input, 0, LV_STATE_FOCUSED);

	grp = kf_use_group();
	lv_group_add_obj(grp, repl_input);
	lv_group_focus_obj(repl_input);

	add_block(repl_log, "CakeSpark 1.0  -  one rule: function args >> dest", KF_TEXT_MUTED);

	mode = MODE_REPL;
	kf_grab_input(1);
	swap_screen(scr);
}

/* ===================================================================== */
/* ===========================  EDITOR  =============================== */
/* ===================================================================== */
#define DOCDIR   KF_SCRIPTS
#define MAXLOAD  (32*1024)
#define ED_LEGEND  "F1 Save  F2 Run  F3 SaveAs\nF4 New  F5 Quit  ESC desktop"
#define ED_LEGEND_H 28

static lv_obj_t *ed_ta, *ed_title, *ed_keys;
static lv_obj_t *ed_box, *ed_prompt_ta;     /* Save-As modal */
static int  ed_dirty=0, ed_in_prompt=0, ed_quit_armed=0;
static char ed_path[512];

static void ed_show_legend(void){ lv_label_set_text(ed_keys, ED_LEGEND); ed_quit_armed = 0; }
static void ed_set_title(void){
	const char *base = "untitled";
	if(ed_path[0]){ const char *s = strrchr(ed_path,'/'); base = s ? s+1 : ed_path; }
	lv_label_set_text_fmt(ed_title, "%s%s", base, ed_dirty ? " *" : "");
}
static void ed_load(void){
	if(ed_path[0] == 0){ lv_textarea_set_text(ed_ta, ""); return; }
	FILE *f = fopen(ed_path, "r");
	if(!f){ lv_textarea_set_text(ed_ta, ""); return; }
	char *buf = malloc(MAXLOAD + 1);
	if(!buf){ fclose(f); lv_textarea_set_text(ed_ta, ""); return; }
	size_t n = fread(buf, 1, MAXLOAD, f); buf[n] = 0; fclose(f);
	lv_textarea_set_text(ed_ta, buf);
	free(buf);
	lv_textarea_set_cursor_pos(ed_ta, 0);
}
/* derive a .cake filename for an untitled program from its first non-empty line */
static void ed_path_from_text(void){
	const char *t = lv_textarea_get_text(ed_ta);
	const char *p = t;
	while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r'||*p=='/') p++;   /* skip leading // too */
	char base[40]; int n = 0;
	for(; *p && *p!='\n' && n<32; p++){
		char c = *p;
		if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||c=='-') base[n++] = c;
		else if(c==' ') base[n++] = '_';
	}
	while(n>0 && base[n-1]=='_') n--;
	base[n] = 0;
	if(n == 0) snprintf(base, sizeof base, "program");
	snprintf(ed_path, sizeof ed_path, "%s/%s.cake", DOCDIR, base);
	struct stat st; int k = 2;
	while(stat(ed_path,&st)==0) snprintf(ed_path, sizeof ed_path, "%s/%s-%d.cake", DOCDIR, base, k++);
}
static int ed_write(void){
	FILE *f = fopen(ed_path, "w");
	if(!f){ lv_label_set_text(ed_keys, "SAVE FAILED"); return -1; }
	const char *t = lv_textarea_get_text(ed_ta);
	fwrite(t, 1, strlen(t), f); fclose(f);
	ed_dirty = 0; ed_set_title();
	lv_label_set_text(ed_keys, "saved");
	return 0;
}
static void ed_save(void){
	if(ed_path[0] == 0){ mkdir(DOCDIR, 0755); ed_path_from_text(); }
	ed_write();
}
static void ed_cursor_home(void){
	const char *t = lv_textarea_get_text(ed_ta);
	uint32_t i = lv_textarea_get_cursor_pos(ed_ta);
	while(i>0 && t[i-1]!='\n') i--;
	lv_textarea_set_cursor_pos(ed_ta, (int32_t)i);
}
static void ed_cursor_end(void){
	const char *t = lv_textarea_get_text(ed_ta);
	uint32_t len = (uint32_t)strlen(t), i = lv_textarea_get_cursor_pos(ed_ta);
	while(i<len && t[i]!='\n') i++;
	lv_textarea_set_cursor_pos(ed_ta, (int32_t)i);
}
/* quit: to_launcher -> desktop, else -> program list. Guards unsaved changes. */
static void ed_quit(int to_launcher){
	if(ed_dirty && !ed_quit_armed){
		ed_quit_armed = 1;
		lv_label_set_text(ed_keys, "Unsaved!  F1 save, again to discard");
		return;
	}
	vm_free();
	if(to_launcher){ kf_grab_input(0); kf_back_to_launcher(); }
	else build_proglist();
}
/* ---- Save-As name modal ---- */
static void ed_close_prompt(void){
	if(ed_box){ lv_obj_delete(ed_box); ed_box = NULL; }
	ed_in_prompt = 0;
}
static void ed_save_as_confirm(void){
	const char *name = lv_textarea_get_text(ed_prompt_ta);
	if(name && name[0]){
		if(strchr(name, '/')) snprintf(ed_path, sizeof ed_path, "%s", name);
		else snprintf(ed_path, sizeof ed_path, "%s/%s", DOCDIR, name);
		/* default extension */
		if(!strrchr(ed_path,'.')){ size_t l=strlen(ed_path); snprintf(ed_path+l,sizeof ed_path-l,".cake"); }
		ed_close_prompt();
		ed_write();
	} else { ed_close_prompt(); ed_show_legend(); }
}
static void ed_open_save_as(void){
	ed_in_prompt = 1;
	ed_box = lv_obj_create(scr);
	lv_obj_set_size(ed_box, 290, 64);
	lv_obj_center(ed_box);
	lv_obj_set_style_bg_color(ed_box, KF_CARD, 0);
	lv_obj_set_style_border_color(ed_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(ed_box, 2, 0);
	lv_obj_set_style_radius(ed_box, 0, 0);
	lv_obj_clear_flag(ed_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(ed_box);
	lv_label_set_text(l, "Save as (Enter ok, Esc cancel):");
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
	ed_prompt_ta = lv_textarea_create(ed_box);
	lv_textarea_set_one_line(ed_prompt_ta, true);
	lv_obj_set_width(ed_prompt_ta, 270);
	lv_obj_align(ed_prompt_ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_set_style_text_font(ed_prompt_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(ed_prompt_ta, KF_BG, 0);
	lv_obj_set_style_text_color(ed_prompt_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(ed_prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(ed_prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(ed_prompt_ta, 0, LV_STATE_FOCUSED);
	const char *base = "untitled.cake";
	if(ed_path[0]){ const char *s = strrchr(ed_path,'/'); base = s ? s+1 : ed_path; }
	lv_textarea_set_text(ed_prompt_ta, base);
}
static void ed_prompt_key(uint8_t key){
	switch(key){
	case DK_ENTER:     ed_save_as_confirm(); break;
	case DK_ESC:
	case DK_BREAK:     ed_close_prompt(); ed_show_legend(); break;
	case DK_BACKSPACE: lv_textarea_delete_char(ed_prompt_ta); break;
	case DK_LEFT:      lv_textarea_cursor_left(ed_prompt_ta); break;
	case DK_RIGHT:     lv_textarea_cursor_right(ed_prompt_ta); break;
	default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(ed_prompt_ta, key); break;
	}
}
static void editor_key(uint8_t key){
	if(ed_in_prompt){ ed_prompt_key(key); return; }
	int mods = uart_mods();
	if(key >= DK_F1 && key <= DK_F1+4){
		switch(key - DK_F1){
		case 0: ed_save(); break;                                       /* F1 Save   */
		case 1: run_and_show(lv_textarea_get_text(ed_ta)); break;       /* F2 Run    */
		case 2: ed_open_save_as(); break;                               /* F3 SaveAs */
		case 3: open_editor(NULL); return;                             /* F4 New    */
		case 4: ed_quit(0); return;                                     /* F5 Quit   */
		}
		return;
	}
	if(key==DK_ESC || key==DK_BREAK){ ed_quit(1); return; }
	if((mods&MOD_CTRL) && (key=='s'||key=='S'||key==0x13)){ ed_save(); return; }
	switch(key){
	case DK_ENTER:     lv_textarea_add_char(ed_ta,'\n');          ed_dirty=1; break;
	case DK_BACKSPACE: lv_textarea_delete_char(ed_ta);            ed_dirty=1; break;
	case DK_DEL:       lv_textarea_delete_char_forward(ed_ta);    ed_dirty=1; break;
	case DK_TAB:       lv_textarea_add_text(ed_ta,"  ");          ed_dirty=1; break;
	case DK_LEFT:      lv_textarea_cursor_left(ed_ta);  break;
	case DK_RIGHT:     lv_textarea_cursor_right(ed_ta); break;
	case DK_UP:        lv_textarea_cursor_up(ed_ta);    break;
	case DK_DOWN:      lv_textarea_cursor_down(ed_ta);  break;
	case DK_HOME:      ed_cursor_home(); break;
	case DK_END:       ed_cursor_end();  break;
	case DK_PGUP:      for(int i=0;i<10;i++) lv_textarea_cursor_up(ed_ta);   break;
	case DK_PGDN:      for(int i=0;i<10;i++) lv_textarea_cursor_down(ed_ta); break;
	default:
		if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)){ lv_textarea_add_char(ed_ta,key); ed_dirty=1; }
		break;
	}
	ed_show_legend();
	ed_set_title();
}
static void open_editor(const char *p){
	snprintf(ed_path, sizeof ed_path, "%s", p ? p : "");
	ed_dirty = 0; ed_in_prompt = 0; ed_quit_armed = 0; ed_box = NULL;

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *top = lv_obj_create(scr);
	lv_obj_set_size(top, LCD_W, 20);
	lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(top, KF_CARD, 0);
	lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_radius(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
	ed_title = lv_label_create(top);
	lv_obj_set_style_text_color(ed_title, KF_AMBER_BR, 0);
	lv_obj_align(ed_title, LV_ALIGN_LEFT_MID, 4, 0);

	ed_ta = lv_textarea_create(scr);
	lv_obj_set_size(ed_ta, LCD_W, KF_CONTENT_H - 20 - ED_LEGEND_H);
	lv_obj_align(ed_ta, LV_ALIGN_TOP_MID, 0, 20);
	lv_textarea_set_placeholder_text(ed_ta, "set 2 >> x\nlog \"{x}\"");
	lv_obj_set_style_bg_color(ed_ta, KF_BG, 0);
	lv_obj_set_style_bg_opa(ed_ta, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(ed_ta, KF_TEXT, 0);
	lv_obj_set_style_text_font(ed_ta, KF_FONT, 0);
	lv_obj_set_style_border_width(ed_ta, 0, 0);
	lv_obj_set_style_radius(ed_ta, 0, 0);
	lv_obj_set_style_outline_width(ed_ta, 0, 0);
	lv_obj_set_style_outline_width(ed_ta, 0, LV_STATE_FOCUSED);

	lv_obj_t *bot = lv_obj_create(scr);
	lv_obj_set_size(bot, LCD_W, ED_LEGEND_H);
	lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(bot, KF_CARD, 0);
	lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bot, 0, 0);
	lv_obj_set_style_radius(bot, 0, 0);
	lv_obj_set_style_pad_all(bot, 0, 0);
	lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
	ed_keys = lv_label_create(bot);
	lv_label_set_long_mode(ed_keys, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(ed_keys, LCD_W-6);
	lv_obj_set_style_text_color(ed_keys, KF_AMBER_DIM, 0);
	lv_obj_align(ed_keys, LV_ALIGN_LEFT_MID, 3, 0);
	lv_label_set_text(ed_keys, ED_LEGEND);

	grp = kf_use_group();
	lv_group_add_obj(grp, ed_ta);
	lv_group_focus_obj(ed_ta);

	ed_load();
	ed_set_title();

	mode = MODE_EDIT;
	kf_grab_input(1);
	swap_screen(scr);
}

/* ===================================================================== */
/* =========================  PROGRAM LIST  =========================== */
/* ===================================================================== */
#define LI_MAX 128
#define LI_MENU  "\x02"          /* "[ Menu ]"  row sentinel */
#define LI_NEW   "\x01"          /* "[ + New program ]" row sentinel */

static lv_obj_t *li_list;
static lv_obj_t *li_btn[LI_MAX];
static char     *li_name[LI_MAX];     /* sentinel or filename */
static int       li_n = 0, li_sel = 0;
static lv_obj_t *li_box, *li_prompt_ta;
static int       li_in_prompt = 0;    /* 1=rename text modal, 2=delete confirm */
static char      li_target[600];      /* full path of the rename/delete target */

static void li_free_names(void){ for(int i=0;i<li_n;i++) free(li_name[i]); li_n = 0; }
static void li_on_del(lv_event_t *e){ (void)e; li_free_names(); }

static int  li_is_file(int i){ return li_name[i][0] != 1 && li_name[i][0] != 2; }
static void li_highlight(int idx){
	if(idx < 0 || idx >= li_n) return;
	if(li_sel >= 0 && li_sel < li_n) lv_obj_remove_state(li_btn[li_sel], LV_STATE_FOCUSED);
	li_sel = idx;
	lv_obj_add_state(li_btn[li_sel], LV_STATE_FOCUSED);
	lv_obj_scroll_to_view(li_btn[li_sel], LV_ANIM_OFF);
}
static void li_full(int i, char *out, int n){ snprintf(out, n, "%s/%s", KF_SCRIPTS, li_name[i]); }

/* ---- rename / delete modals ---- */
static void li_close_modal(void){
	if(li_box){ lv_obj_delete(li_box); li_box = NULL; li_prompt_ta = NULL; }
	li_in_prompt = 0;
}
static void li_do_rename(void){
	const char *name = lv_textarea_get_text(li_prompt_ta);
	if(name && name[0]){
		char np[600];
		snprintf(np, sizeof np, "%s/%s", KF_SCRIPTS, name);
		if(!strrchr(name,'.')){ size_t l=strlen(np); snprintf(np+l,sizeof np-l,".cake"); }
		rename(li_target, np);
	}
	li_close_modal();
	build_proglist();
}
static void li_do_delete(void){
	remove(li_target);
	li_close_modal();
	build_proglist();
}
static void li_open_rename(int idx){
	if(!li_is_file(idx)) return;
	li_full(idx, li_target, sizeof li_target);
	li_in_prompt = 1;
	li_box = lv_obj_create(scr);
	lv_obj_set_size(li_box, 290, 64);
	lv_obj_center(li_box);
	lv_obj_set_style_bg_color(li_box, KF_CARD, 0);
	lv_obj_set_style_border_color(li_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(li_box, 2, 0);
	lv_obj_set_style_radius(li_box, 0, 0);
	lv_obj_clear_flag(li_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(li_box);
	lv_label_set_text(l, "Rename to (Enter ok, Esc cancel):");
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);
	li_prompt_ta = lv_textarea_create(li_box);
	lv_textarea_set_one_line(li_prompt_ta, true);
	lv_obj_set_width(li_prompt_ta, 270);
	lv_obj_align(li_prompt_ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_set_style_text_font(li_prompt_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(li_prompt_ta, KF_BG, 0);
	lv_obj_set_style_text_color(li_prompt_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(li_prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(li_prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(li_prompt_ta, 0, LV_STATE_FOCUSED);
	lv_textarea_set_text(li_prompt_ta, li_name[idx]);
}
static void li_open_delete(int idx){
	if(!li_is_file(idx)) return;
	li_full(idx, li_target, sizeof li_target);
	li_in_prompt = 2;
	li_box = lv_obj_create(scr);
	lv_obj_set_size(li_box, 290, 60);
	lv_obj_center(li_box);
	lv_obj_set_style_bg_color(li_box, KF_CARD, 0);
	lv_obj_set_style_border_color(li_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(li_box, 2, 0);
	lv_obj_set_style_radius(li_box, 0, 0);
	lv_obj_clear_flag(li_box, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(li_box);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, 280);
	lv_label_set_text_fmt(l, "Delete %s ?\nENTER = yes    ESC = no", li_name[idx]);
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_obj_center(l);
}
static void li_modal_key(uint8_t key){
	if(li_in_prompt == 2){     /* delete confirm */
		if(key==DK_ENTER) li_do_delete();
		else if(key==DK_ESC || key==DK_BREAK) li_close_modal();
		return;
	}
	switch(key){              /* rename text entry */
	case DK_ENTER:     li_do_rename(); break;
	case DK_ESC:
	case DK_BREAK:     li_close_modal(); break;
	case DK_BACKSPACE: lv_textarea_delete_char(li_prompt_ta); break;
	case DK_LEFT:      lv_textarea_cursor_left(li_prompt_ta); break;
	case DK_RIGHT:     lv_textarea_cursor_right(li_prompt_ta); break;
	default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(li_prompt_ta, key); break;
	}
}
static void li_activate(int idx){      /* ENTER / F1 */
	if(!strcmp(li_name[idx], LI_MENU)){ vm_free(); build_menu(); return; }
	if(!strcmp(li_name[idx], LI_NEW)){ open_editor(NULL); return; }
	char full[600]; li_full(idx, full, sizeof full);
	open_editor(full);
}
static void li_run(int idx){
	if(!li_is_file(idx)) return;
	char full[600]; li_full(idx, full, sizeof full);
	FILE *f = fopen(full, "r");
	if(!f){ run_and_show("log \"could not open file\""); return; }
	char *buf = malloc(MAXLOAD + 1);
	if(!buf){ fclose(f); return; }
	size_t n = fread(buf, 1, MAXLOAD, f); buf[n] = 0; fclose(f);
	run_and_show(buf);
	free(buf);
}
static void list_key(uint8_t key){
	if(li_in_prompt){ li_modal_key(key); return; }
	switch(key){
	case DK_ESC:
	case DK_BREAK:  vm_free(); kf_grab_input(0); kf_back_to_launcher(); return;
	case DK_UP:     li_highlight(li_sel>0 ? li_sel-1 : li_n-1); break;
	case DK_DOWN:   li_highlight(li_sel<li_n-1 ? li_sel+1 : 0); break;
	case DK_PGUP:   li_highlight(li_sel>=5 ? li_sel-5 : 0); break;
	case DK_PGDN:   li_highlight(li_sel+5<li_n ? li_sel+5 : li_n-1); break;
	case DK_ENTER:
	case DK_F1:       li_activate(li_sel); return;       /* may transition */
	case DK_F1+1:     li_run(li_sel); break;             /* F2 */
	case DK_F1+2:     li_open_rename(li_sel); break;     /* F3 */
	case DK_F1+3:     li_open_delete(li_sel); break;     /* F4 */
	default: break;
	}
}
static void li_add_row(const char *label, const char *name){
	if(li_n >= LI_MAX) return;
	lv_obj_t *b = lv_list_add_button(li_list, NULL, label);
	lv_obj_set_style_text_font(b, KF_FONT, 0);
	li_btn[li_n]  = b;
	li_name[li_n] = strdup(name);
	li_n++;
}
static void build_proglist(void){
	mkdir(KF_SCRIPTS, 0755);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr, li_on_del, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "CakeSpark  -  Programs");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 3);

	li_list = lv_list_create(scr);
	lv_obj_set_size(li_list, LCD_W-8, KF_CONTENT_H-24-30);
	lv_obj_align(li_list, LV_ALIGN_TOP_MID, 0, 22);
	lv_obj_set_style_bg_color(li_list, KF_BG_DEEP, 0);

	lv_obj_t *legend = lv_label_create(scr);
	lv_label_set_text(legend, "up/dn move  F1 edit  F2 run\nF3 rename  F4 del  ESC quit");
	lv_obj_set_style_text_font(legend, KF_FONT, 0);
	lv_obj_set_style_text_color(legend, KF_TEXT_DIM, 0);
	lv_obj_align(legend, LV_ALIGN_BOTTOM_LEFT, 4, -1);

	li_free_names();
	li_add_row("[ Menu ]",            LI_MENU);
	li_add_row("[ + New program ]",   LI_NEW);

	DIR *d = opendir(KF_SCRIPTS);
	if(d){ struct dirent *e;
		while((e = readdir(d)) && li_n < LI_MAX){
			if(e->d_name[0] == '.') continue;
			char full[600]; snprintf(full, sizeof full, "%s/%s", KF_SCRIPTS, e->d_name);
			struct stat st; if(stat(full,&st)!=0 || !S_ISREG(st.st_mode)) continue;
			li_add_row(e->d_name, e->d_name);
		}
		closedir(d);
	}

	li_sel = 0; li_in_prompt = 0; li_box = NULL;
	if(li_n > 2) li_sel = 2;                /* land on the first program if any */
	lv_obj_add_state(li_btn[li_sel], LV_STATE_FOCUSED);
	lv_obj_scroll_to_view(li_btn[li_sel], LV_ANIM_OFF);

	grp = kf_use_group();                   /* fresh empty group (we drive nav manually) */

	mode = MODE_LIST;
	kf_grab_input(1);
	swap_screen(scr);
}

/* ===================================================================== */
/* ============================  MENU  =============================== */
/* ===================================================================== */
static void menu_programs_cb(lv_event_t *e){ (void)e; build_proglist(); }
static void menu_repl_cb(lv_event_t *e){ (void)e; start_repl(); }

static void build_menu(void){
	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "CakeSpark");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 6, 8);

	lv_obj_t *sub = lv_label_create(scr);
	lv_label_set_text(sub, "a tiny, safe scripting language");
	lv_obj_set_style_text_font(sub, KF_FONT, 0);
	lv_obj_set_style_text_color(sub, KF_TEXT_MUTED, 0);
	lv_obj_align(sub, LV_ALIGN_TOP_LEFT, 6, 24);

	lv_obj_t *list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-24, 96);
	lv_obj_align(list, LV_ALIGN_CENTER, 0, 0);

	grp = kf_use_group();
	lv_obj_t *b1 = lv_list_add_button(list, NULL, "Programs   (write + run .cake files)");
	lv_obj_set_style_text_font(b1, KF_FONT, 0);
	lv_obj_add_event_cb(b1, menu_programs_cb, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(grp, b1);
	lv_obj_t *b2 = lv_list_add_button(list, NULL, "REPL       (live line-by-line)");
	lv_obj_set_style_text_font(b2, KF_FONT, 0);
	lv_obj_add_event_cb(b2, menu_repl_cb, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(grp, b2);
	lv_group_focus_obj(b1);

	lv_obj_t *hint = lv_label_create(scr);
	lv_label_set_text(hint, "ENTER select   ESC quit");
	lv_obj_set_style_text_font(hint, KF_FONT, 0);
	lv_obj_set_style_text_color(hint, KF_TEXT_DIM, 0);
	lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 6, -4);

	mode = MODE_MENU;
	kf_grab_input(0);                       /* menu uses LVGL group nav */
	swap_screen(scr);
}

/* ===================================================================== */
/* ====================  superloop key pump  ========================= */
/* ===================================================================== */
void cakespark_poll(void){
	if(mode != MODE_LIST && mode != MODE_EDIT && mode != MODE_REPL) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		if(ov_up){ overlay_key(key); continue; }
		cs_mode_t m0 = mode;
		switch(mode){
		case MODE_LIST: list_key(key); break;
		case MODE_EDIT: editor_key(key); break;
		case MODE_REPL: repl_key(key); break;
		default: return;
		}
		if(mode != m0) return;              /* a transition rebuilt the screen */
	}
}

void app_cakespark_open(void){
	mode = MODE_NONE;
	ov_box = NULL; ov_log = NULL; ov_up = 0;
	build_menu();
}
