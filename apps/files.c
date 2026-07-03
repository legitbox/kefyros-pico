// apps/files.c — directory browser + basic file management.
//   ENTER (short) : open a folder, or open a file in Notes
//   ENTER (hold)  : action menu for the item -> Rename / Delete
//   [ + New folder ] row : make a folder in the current directory
// Files is a menu app (no raw-key grab), so it drives the LVGL keypad group. Modals (action
// menu / confirm / name entry) add their widgets to that group; text entry rides the same
// keyboard path the WiFi password box uses. Delete handles files and EMPTY folders (a non-empty
// folder is refused, to avoid an accidental recursive wipe).
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static lv_obj_t  *scr, *list, *title, *hint, *modal, *modal_ta;
static lv_group_t *grp = NULL;
static char cwd[512];
static char target[256];          /* item the action menu / rename / delete operates on */

#define MAXN 256
static char *names[MAXN];
static int   nnames = 0;
static void free_names(void){ for(int i=0;i<nnames;i++) free(names[i]); nnames=0; }

static void join(char *out,int sz,const char *dir,const char *name){
	if(!strcmp(dir,"/")) snprintf(out,sz,"/%s",name);
	else snprintf(out,sz,"%s/%s",dir,name);
}
static void open_path(const char *path);

/* ---------- modals ---------- */
static void close_modal(void){
	if(modal){ lv_obj_delete(modal); modal = NULL; modal_ta = NULL; }
	lv_obj_t *first = lv_obj_get_child(list, 0);
	if(first) lv_group_focus_obj(first);
}
/* a centered amber card; replaces any open modal */
static lv_obj_t *make_modal(int h){
	if(modal){ lv_obj_delete(modal); modal_ta = NULL; }
	modal = lv_obj_create(scr);
	lv_obj_set_size(modal, LCD_W-32, h);
	lv_obj_center(modal);
	lv_obj_set_style_bg_color(modal, KF_CARD, 0);
	lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(modal, KF_AMBER, 0);
	lv_obj_set_style_border_width(modal, 2, 0);
	lv_obj_set_style_radius(modal, 0, 0);
	lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(modal, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_all(modal, 8, 0);
	lv_obj_set_style_pad_row(modal, 6, 0);
	lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
	return modal;
}
static void modal_label(lv_obj_t *m, const char *txt){
	lv_obj_t *l = lv_label_create(m);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, LCD_W-56);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_label_set_text(l, txt);
}
static lv_obj_t *modal_btn(lv_obj_t *m, const char *txt, lv_event_cb_t cb){
	lv_obj_t *b = lv_button_create(m);
	lv_obj_set_width(b, LCD_W-72);
	lv_obj_t *l = lv_label_create(b); lv_label_set_text(l, txt);
	lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0); lv_obj_center(l);
	lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
	lv_group_add_obj(grp, b);
	return b;
}

/* ---------- file operations (run from modal buttons; refresh the listing after) ---------- */
static void op_delete(lv_event_t *e){ (void)e;
	char full[600]; join(full,sizeof full,cwd,target);
	struct stat st;
	if(stat(full,&st)==0){
		if(S_ISDIR(st.st_mode)) rmdir(full);   /* fails (kept) if non-empty — safe */
		else remove(full);
	}
	close_modal();
	open_path(cwd);                            /* rebuild listing (kf_use_group fresh group) */
}
static void op_rename(lv_event_t *e){ (void)e;
	char nm[256]; snprintf(nm, sizeof nm, "%s", modal_ta ? lv_textarea_get_text(modal_ta) : "");
	if(nm[0] && strcmp(nm, target) && !strchr(nm,'/')){
		char from[600], to[600];
		join(from,sizeof from,cwd,target);
		join(to,  sizeof to,  cwd,nm);
		rename(from, to);
	}
	close_modal();
	open_path(cwd);
}
static void op_mkdir(lv_event_t *e){ (void)e;
	char nm[256]; snprintf(nm, sizeof nm, "%s", modal_ta ? lv_textarea_get_text(modal_ta) : "");
	if(nm[0] && !strchr(nm,'/')){
		char full[600]; join(full,sizeof full,cwd,nm);
		mkdir(full, 0755);
	}
	close_modal();
	open_path(cwd);
}
static void op_cancel(lv_event_t *e){ (void)e; close_modal(); }

/* ---------- modal builders ---------- */
static lv_obj_t *make_text_modal(const char *prompt, const char *initial, lv_event_cb_t on_ready){
	lv_obj_t *m = make_modal(110);
	modal_label(m, prompt);
	modal_ta = lv_textarea_create(m);
	lv_textarea_set_one_line(modal_ta, true);
	lv_obj_set_width(modal_ta, LCD_W-72);
	lv_obj_set_style_text_font(modal_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(modal_ta, KF_BG, 0);
	lv_obj_set_style_text_color(modal_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(modal_ta, 0, 0);
	lv_obj_set_style_outline_width(modal_ta, 0, 0);
	lv_obj_set_style_outline_width(modal_ta, 0, LV_STATE_FOCUSED);
	if(initial) lv_textarea_set_text(modal_ta, initial);
	lv_obj_add_event_cb(modal_ta, on_ready, LV_EVENT_READY, NULL);   /* ENTER confirms */
	lv_group_add_obj(grp, modal_ta);
	lv_group_focus_obj(modal_ta);
	return m;
}
static void act_rename(lv_event_t *e){ (void)e; make_text_modal("Rename to (ENTER ok, ESC cancel):", target, op_rename); }
static void act_confirm_delete(lv_event_t *e){ (void)e;
	char msg[300]; snprintf(msg,sizeof msg,"Delete \"%s\"?", target);
	lv_obj_t *m = make_modal(130);
	modal_label(m, msg);
	lv_obj_t *del = modal_btn(m, "Delete", op_delete);
	modal_btn(m, "Cancel", op_cancel);
	lv_group_focus_obj(del);
}
/* long-press on an item -> action menu */
static void action_menu(const char *name){
	snprintf(target, sizeof target, "%s", name);
	lv_obj_t *m = make_modal(150);
	modal_label(m, name);
	lv_obj_t *r = modal_btn(m, "Rename", act_rename);
	modal_btn(m, "Delete", act_confirm_delete);
	modal_btn(m, "Cancel", op_cancel);
	lv_group_focus_obj(r);
}

/* ---------- listing ---------- */
static void item_cb(lv_event_t *e){               /* short ENTER: open */
	const char *name = lv_event_get_user_data(e);
	if(!strcmp(name,"\x01")){ make_text_modal("New folder name (ENTER ok, ESC cancel):", "", op_mkdir); return; }
	if(!strcmp(name,"..")){
		char *s=strrchr(cwd,'/');
		if(s && s!=cwd) *s=0; else strcpy(cwd,"/");
		open_path(cwd); return;
	}
	static char full[600];
	join(full,sizeof full,cwd,name);
	struct stat st;
	if(stat(full,&st)==0 && S_ISDIR(st.st_mode)){ open_path(full); return; }
	app_editor_open_path(full);                   /* open any file in Notes */
}
static void item_long_cb(lv_event_t *e){          /* hold ENTER: rename/delete */
	const char *name = lv_event_get_user_data(e);
	if(!strcmp(name,"..") || !strcmp(name,"\x01")) return;  /* not on "../" or "+ New folder" */
	action_menu(name);
}

static void add_row(const char *shown, const char *key){
	lv_obj_t *b = lv_list_add_button(list, NULL, shown);
	names[nnames] = strdup(key);
	if(!names[nnames]){ lv_obj_delete(b); return; }   /* strdup failed: no NULL user_data */
	lv_obj_add_event_cb(b, item_cb,      LV_EVENT_SHORT_CLICKED, names[nnames]);
	lv_obj_add_event_cb(b, item_long_cb, LV_EVENT_LONG_PRESSED,  names[nnames]);
	lv_group_add_obj(grp, b);
	nnames++;
}

static void open_path(const char *path){
	strncpy(cwd,path,sizeof cwd-1); cwd[sizeof cwd-1]=0;
	lv_label_set_text(title, cwd);
	free_names();
	if(modal){ lv_obj_delete(modal); modal=NULL; modal_ta=NULL; }   /* drop any open modal */
	lv_obj_clean(list);
	grp = kf_use_group();                         /* fresh group (frees the previous app_group) */

	add_row("[ + New folder ]", "\x01");
	if(strcmp(cwd,"/")) add_row("../", "..");

	DIR *d=opendir(cwd);
	if(d){ struct dirent *e;
		while((e=readdir(d)) && nnames<MAXN){
			if(!e->d_name[0]) continue;
			if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
			int isdir=(e->d_type==DT_DIR);
			static char shown[300]; snprintf(shown,sizeof shown,"%s%s",e->d_name,isdir?"/":"");
			add_row(shown, e->d_name);
		}
		closedir(d);
	}
	lv_group_focus_obj(lv_obj_get_child(list, 0));
}

void app_files_open(void){
	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	kf_inset_top(scr);                  /* clear the persistent OS top bar */

	title = lv_label_create(scr);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 5);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, LCD_W-8);

	list = lv_list_create(scr);
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-44);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 26);

	hint = lv_label_create(scr);
	lv_obj_set_style_text_color(hint, KF_TEXT_MUTED, 0);
	lv_obj_set_style_text_font(hint, KF_FONT, 0);
	lv_label_set_text(hint, "ENTER open   hold ENTER: rename/delete");
	lv_obj_align(hint, LV_ALIGN_BOTTOM_MID, 0, -2);

	grp = NULL; modal = NULL; modal_ta = NULL;
	open_path("/");   /* start at the SD root; user navigates from there */
	lv_screen_load(scr);
}
