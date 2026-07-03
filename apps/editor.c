// apps/editor.c — text editor for Kefyros. Opens any file (the Files app hands a path
// to app_editor_open_path); the Editor tile opens a quick picker over /kefyros/notes.
// Like the terminal it GRABS the keyboard and reads raw device keys, so function keys
// drive the editor (nano-style):
//   F1 Save   F2 Save As   F3 Open (Files)   F4 New   F5 Quit
// A brand-new note stays "untitled" until first save, then is named from its first line.
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define DOCDIR  KF_NOTES
#define MAXLOAD (64*1024)
/* two rows so it fits 320 px in plex_mono_13 (~7 px/char) instead of clipping off-screen */
#define LEGEND  "F1 Save  F2 SaveAs  F3 Open\nF4 New  F5 Quit"
#define LEGEND_H 28

static lv_obj_t  *scr, *ta, *lbl_title, *lbl_keys;
static lv_obj_t  *prompt_box, *prompt_ta;
static lv_group_t *grp;
static int  editing   = 0;
static int  dirty     = 0;
static int  in_prompt = 0;     /* the Save-As name modal is up */
static int  quit_armed = 0;    /* pressed quit once with unsaved changes */
static int  load_ok_to_save = 1; /* 0 => load was partial/failed; never overwrite the file */
static char path[512];         /* "" => untitled new note */

#define MAXN 128
static char *names[MAXN];
static int   nnames = 0;
static void free_names(void){ for(int i=0;i<nnames;i++) free(names[i]); nnames = 0; }

static void build_picker(void);
static void open_editor(const char *p);

/* ---------- helpers ---------- */
static void show_legend(void){ lv_label_set_text(lbl_keys, LEGEND); quit_armed = 0; }

static void set_title(void){
	const char *base = "untitled";
	if(path[0]){ const char *s = strrchr(path,'/'); base = s ? s+1 : path; }
	lv_label_set_text_fmt(lbl_title, "%s%s", base, dirty ? " *" : "");
}

static void load_file(void){
	load_ok_to_save = 1;                       /* clean load until proven otherwise */
	if(path[0] == 0){ lv_textarea_set_text(ta, ""); return; }   /* new untitled note */
	FILE *f = fopen(path, "r");
	if(!f){ lv_textarea_set_text(ta, ""); return; }
	/* refuse to edit a file we cannot fully load — a later save must not truncate it */
	struct stat st;
	if(stat(path,&st)==0 && st.st_size > MAXLOAD){
		fclose(f); lv_textarea_set_text(ta, ""); load_ok_to_save = 0; return;
	}
	char *buf = malloc(MAXLOAD + 1);
	if(!buf){ fclose(f); lv_textarea_set_text(ta, ""); load_ok_to_save = 0; return; }
	size_t n = fread(buf, 1, MAXLOAD, f); buf[n] = 0;
	if(fgetc(f) != EOF) load_ok_to_save = 0;   /* still data past MAXLOAD -> partial load */
	fclose(f);
	lv_textarea_set_text(ta, buf);
	free(buf);
	lv_textarea_set_cursor_pos(ta, 0);
}

/* derive a filename for an untitled note from its first non-empty line */
static void build_path_from_text(void){
	const char *t = lv_textarea_get_text(ta);
	const char *p = t;
	while(*p==' '||*p=='\t'||*p=='\n'||*p=='\r') p++;
	char base[40]; int n = 0;
	for(; *p && *p!='\n' && n<32; p++){
		char c = *p;
		if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='_'||c=='-') base[n++] = c;
		else if(c==' ') base[n++] = '_';
	}
	while(n>0 && base[n-1]=='_') n--;
	base[n] = 0;
	if(n == 0) snprintf(base, sizeof base, "note");
	snprintf(path, sizeof path, "%s/%s.txt", DOCDIR, base);
	struct stat st; int k = 2;
	while(stat(path,&st)==0) snprintf(path, sizeof path, "%s/%s-%d.txt", DOCDIR, base, k++);
}

/* write the textarea to `path` atomically (tmp then rename); returns 0 on success */
static int write_current(void){
	if(!load_ok_to_save){
		lv_label_set_text(lbl_keys, "load failed - file too large, not saved");
		return -1;
	}
	char tmp[520];
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if(!f){ lv_label_set_text(lbl_keys, "SAVE FAILED"); return -1; }
	const char *t = lv_textarea_get_text(ta);
	size_t len = strlen(t);
	size_t wr  = fwrite(t, 1, len, f);
	if(wr != len || fclose(f) != 0){          /* partial write / flush error: keep original */
		remove(tmp);
		lv_label_set_text(lbl_keys, "SAVE FAILED");
		return -1;
	}
	if(rename(tmp, path) != 0){               /* swap in only on full success */
		remove(tmp);
		lv_label_set_text(lbl_keys, "SAVE FAILED");
		return -1;
	}
	dirty = 0; set_title();
	lv_label_set_text(lbl_keys, "saved");
	return 0;
}
static void do_save(void){
	if(path[0] == 0){ mkdir(DOCDIR, 0755); build_path_from_text(); }
	write_current();
}

/* Home/End move to start/end of the current line (ASCII text) */
static void cursor_line_home(void){
	const char *t = lv_textarea_get_text(ta);
	uint32_t i = lv_textarea_get_cursor_pos(ta);
	while(i>0 && t[i-1]!='\n') i--;
	lv_textarea_set_cursor_pos(ta, (int32_t)i);
}
static void cursor_line_end(void){
	const char *t = lv_textarea_get_text(ta);
	uint32_t len = (uint32_t)strlen(t), i = lv_textarea_get_cursor_pos(ta);
	while(i<len && t[i]!='\n') i++;
	lv_textarea_set_cursor_pos(ta, (int32_t)i);
}

static void quit_to_launcher(void){
	editing = 0; in_prompt = 0; kf_grab_input(0);
	kf_back_to_launcher();
}
static void try_quit(void){
	if(dirty && !quit_armed){
		quit_armed = 1;
		lv_label_set_text(lbl_keys, "Unsaved!  F1 save, F5 again to discard");
		return;
	}
	quit_to_launcher();
}

/* ---------- Save-As name modal ---------- */
static void close_prompt(void){
	if(prompt_box){ lv_obj_delete(prompt_box); prompt_box = NULL; }
	in_prompt = 0;
}
static void save_as_confirm(void){
	const char *name = lv_textarea_get_text(prompt_ta);
	if(name && name[0]){
		if(strchr(name, '/')){
			snprintf(path, sizeof path, "%s", name);        /* explicit path */
		} else {
			char dir[512];
			if(path[0]){ snprintf(dir, sizeof dir, "%s", path);
				char *s = strrchr(dir,'/'); if(s) *s = 0; else snprintf(dir,sizeof dir,"%s",DOCDIR); }
			else snprintf(dir, sizeof dir, "%s", DOCDIR);
			snprintf(path, sizeof path, "%s/%s", dir, name);
		}
		close_prompt();
		write_current();
	} else {
		close_prompt(); show_legend();
	}
}
static void open_save_as(void){
	in_prompt = 1;
	prompt_box = lv_obj_create(scr);
	lv_obj_set_size(prompt_box, 290, 64);
	lv_obj_center(prompt_box);
	lv_obj_set_style_bg_color(prompt_box, KF_CARD, 0);
	lv_obj_set_style_border_color(prompt_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(prompt_box, 2, 0);
	lv_obj_set_style_radius(prompt_box, 0, 0);
	lv_obj_clear_flag(prompt_box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *l = lv_label_create(prompt_box);
	lv_label_set_text(l, "Save as (Enter ok, Esc cancel):");
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

	prompt_ta = lv_textarea_create(prompt_box);
	lv_textarea_set_one_line(prompt_ta, true);
	lv_obj_set_width(prompt_ta, 270);
	lv_obj_align(prompt_ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_set_style_text_font(prompt_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(prompt_ta, KF_BG, 0);
	lv_obj_set_style_text_color(prompt_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(prompt_ta, 0, 0);
	lv_obj_set_style_outline_width(prompt_ta, 0, LV_STATE_FOCUSED);
	const char *base = "untitled.txt";
	if(path[0]){ const char *s = strrchr(path,'/'); base = s ? s+1 : path; }
	lv_textarea_set_text(prompt_ta, base);
}
static void prompt_key(uint8_t key){
	switch(key){
	case DK_ENTER:     save_as_confirm(); break;
	case DK_ESC:
	case DK_BREAK:     close_prompt(); show_legend(); break;
	case DK_BACKSPACE: lv_textarea_delete_char(prompt_ta); break;
	case DK_LEFT:      lv_textarea_cursor_left(prompt_ta); break;
	case DK_RIGHT:     lv_textarea_cursor_right(prompt_ta); break;
	default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(prompt_ta, key); break;
	}
}

/* ---------- main key pump (from the launcher loop) ---------- */
void editor_poll(void){
	if(!editing) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		if(in_prompt){ prompt_key(key); continue; }
		int mods = uart_mods();

		/* function keys: editor commands */
		if(key >= DK_F1 && key <= DK_F1+4){
			switch(key - DK_F1){
			case 0: do_save(); break;                                            /* F1 Save */
			case 1: open_save_as(); break;                                       /* F2 SaveAs */
			case 2: { editing=0; kf_grab_input(0);
			          lv_obj_t *prev = lv_screen_active();                       /* our editor screen */
			          app_files_open();
			          if(prev) lv_obj_delete_async(prev); }
			        return;                                                       /* F3 Open  */
			case 3: open_editor(NULL); return;                                   /* F4 New   */
			case 4: try_quit(); return;                                          /* F5 Quit  */
			}
			continue;
		}
		if(key==DK_ESC || key==DK_BREAK){ try_quit(); return; }
		if((mods&MOD_CTRL) && (key=='s'||key=='S'||key==0x13)){ do_save(); continue; }

		switch(key){
		case DK_ENTER:     lv_textarea_add_char(ta,'\n');          dirty=1; break;
		case DK_BACKSPACE: lv_textarea_delete_char(ta);            dirty=1; break;
		case DK_DEL:       lv_textarea_delete_char_forward(ta);    dirty=1; break;
		case DK_TAB:       lv_textarea_add_text(ta,"  ");          dirty=1; break;
		case DK_LEFT:      lv_textarea_cursor_left(ta);  break;
		case DK_RIGHT:     lv_textarea_cursor_right(ta); break;
		case DK_UP:        lv_textarea_cursor_up(ta);    break;
		case DK_DOWN:      lv_textarea_cursor_down(ta);  break;
		case DK_HOME:      cursor_line_home(); break;
		case DK_END:       cursor_line_end();  break;
		case DK_PGUP:      for(int i=0;i<10;i++) lv_textarea_cursor_up(ta);   break;
		case DK_PGDN:      for(int i=0;i<10;i++) lv_textarea_cursor_down(ta); break;
		default:
			if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)){ lv_textarea_add_char(ta,key); dirty=1; }
			break;
		}
		show_legend();   /* clear a transient "saved"/quit-armed message on the next key */
		set_title();
	}
}

static void open_editor(const char *p){
	/* the picker / a prior editor / the Files screen is active — reclaim it after we load
	   the new one (async: we may be inside that screen's own button-click dispatch) */
	lv_obj_t *prev = lv_screen_active();
	snprintf(path, sizeof path, "%s", p ? p : "");
	dirty = 0; in_prompt = 0; quit_armed = 0; prompt_box = NULL;

	scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);                  /* clear the persistent OS top bar */
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* top bar: filename */
	lv_obj_t *top = lv_obj_create(scr);
	lv_obj_set_size(top, LCD_W, 20);
	lv_obj_align(top, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_set_style_bg_color(top, KF_CARD, 0);
	lv_obj_set_style_bg_opa(top, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(top, 0, 0);
	lv_obj_set_style_radius(top, 0, 0);
	lv_obj_set_style_pad_all(top, 0, 0);
	lv_obj_clear_flag(top, LV_OBJ_FLAG_SCROLLABLE);
	lbl_title = lv_label_create(top);
	lv_obj_set_style_text_color(lbl_title, KF_AMBER_BR, 0);
	lv_obj_align(lbl_title, LV_ALIGN_LEFT_MID, 4, 0);

	/* text area */
	ta = lv_textarea_create(scr);
	lv_obj_set_size(ta, LCD_W, KF_CONTENT_H - 20 - LEGEND_H);
	lv_obj_align(ta, LV_ALIGN_TOP_MID, 0, 20);
	lv_textarea_set_placeholder_text(ta, "type here...");
	lv_obj_set_style_bg_color(ta, KF_BG, 0);
	lv_obj_set_style_bg_opa(ta, LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(ta, KF_TEXT, 0);
	lv_obj_set_style_text_font(ta, KF_FONT, 0);
	lv_obj_set_style_border_width(ta, 0, 0);
	lv_obj_set_style_radius(ta, 0, 0);
	lv_obj_set_style_outline_width(ta, 0, 0);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);

	/* bottom bar: F-key legend (two rows) / transient status */
	lv_obj_t *bot = lv_obj_create(scr);
	lv_obj_set_size(bot, LCD_W, LEGEND_H);
	lv_obj_align(bot, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_bg_color(bot, KF_CARD, 0);
	lv_obj_set_style_bg_opa(bot, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(bot, 0, 0);
	lv_obj_set_style_radius(bot, 0, 0);
	lv_obj_set_style_pad_all(bot, 0, 0);
	lv_obj_clear_flag(bot, LV_OBJ_FLAG_SCROLLABLE);
	lbl_keys = lv_label_create(bot);
	lv_label_set_long_mode(lbl_keys, LV_LABEL_LONG_CLIP);
	lv_obj_set_width(lbl_keys, LCD_W-6);
	lv_obj_set_style_text_color(lbl_keys, KF_AMBER_DIM, 0);
	lv_obj_align(lbl_keys, LV_ALIGN_LEFT_MID, 3, 0);
	lv_label_set_text(lbl_keys, LEGEND);

	grp = kf_use_group();
	lv_group_add_obj(grp, ta);
	lv_group_focus_obj(ta);

	load_file();
	set_title();

	kf_grab_input(1);
	editing = 1;
	lv_screen_load(scr);
	if(prev && prev != scr) lv_obj_delete_async(prev);
}

/* ---------- /kefyros/notes quick picker (the Editor tile) ---------- */
static void item_cb(lv_event_t *e){
	const char *name = lv_event_get_user_data(e);
	if(!strcmp(name, "\x01")){ open_editor(NULL); return; }   /* [ + New note ] */
	char full[600]; snprintf(full, sizeof full, "%s/%s", DOCDIR, name);
	open_editor(full);
}

static void build_picker(void){
	editing = 0;
	mkdir(DOCDIR, 0755);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);                  /* clear the persistent OS top bar */

	lv_obj_t *title = lv_label_create(scr);
	lv_label_set_text(title, "Notes  -  " DOCDIR);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, LCD_W-8);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 5);

	lv_obj_t *list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-30);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 26);

	free_names();
	grp = kf_use_group();

	lv_obj_t *b = lv_list_add_button(list, NULL, "[ + New note ]");
	names[nnames] = strdup("\x01");
	if(names[nnames]){
		lv_obj_add_event_cb(b, item_cb, LV_EVENT_CLICKED, names[nnames]);
		lv_group_add_obj(grp, b); nnames++;
	} else lv_obj_delete(b);                    /* strdup failed: no NULL user_data */

	DIR *d = opendir(DOCDIR);
	if(d){ struct dirent *e;
		while((e = readdir(d)) && nnames < MAXN){
			if(e->d_name[0] == '.') continue;
			char full[600]; snprintf(full, sizeof full, "%s/%s", DOCDIR, e->d_name);
			struct stat st; if(stat(full,&st)!=0 || !S_ISREG(st.st_mode)) continue;
			lv_obj_t *bb = lv_list_add_button(list, NULL, e->d_name);
			names[nnames] = strdup(e->d_name);
			if(names[nnames]){
				lv_obj_add_event_cb(bb, item_cb, LV_EVENT_CLICKED, names[nnames]);
				lv_group_add_obj(grp, bb); nnames++;
			} else lv_obj_delete(bb);           /* strdup failed: skip this entry */
		}
		closedir(d);
	}
	lv_group_focus_obj(lv_obj_get_child(list, 0));
	lv_screen_load(scr);
}

void app_editor_open(void){ build_picker(); }
void app_editor_open_path(const char *p){ open_editor(p); }   /* Files -> open any file */
