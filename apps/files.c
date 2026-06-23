// apps/files.c — directory browser; opens selected files in the text editor.
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

static lv_obj_t  *scr, *list, *title;
static lv_group_t *grp = NULL;
static char cwd[512];

#define MAXN 256
static char *names[MAXN];
static int   nnames = 0;
static void free_names(void){ for(int i=0;i<nnames;i++) free(names[i]); nnames=0; }

static void join(char *out,int sz,const char *dir,const char *name){
	if(!strcmp(dir,"/")) snprintf(out,sz,"/%s",name);
	else snprintf(out,sz,"%s/%s",dir,name);
}
static void open_path(const char *path);

static void item_cb(lv_event_t *e){
	const char *name = lv_event_get_user_data(e);
	if(!strcmp(name,"..")){
		char *s=strrchr(cwd,'/');
		if(s && s!=cwd) *s=0; else strcpy(cwd,"/");
		open_path(cwd); return;
	}
	static char full[600];        /* static: keep big buffers off the 2-4 KB core0 stack */
	join(full,sizeof full,cwd,name);
	struct stat st;
	if(stat(full,&st)==0 && S_ISDIR(st.st_mode)){ open_path(full); return; }
	app_editor_open_path(full);   /* open any file in the text editor */
}

static void open_path(const char *path){
	strncpy(cwd,path,sizeof cwd-1); cwd[sizeof cwd-1]=0;
	lv_label_set_text(title, cwd);
	free_names();
	lv_obj_clean(list);
	/* DON'T delete grp here: kf_use_group() already frees the previous app_group.
	   Deleting it ourselves first made kf_use_group free a dangling pointer -> a
	   double-free that corrupted the heap free-list (deterministic bus fault in a
	   later free(), BFAR 0x02027559). Just ask for a fresh group. */
	grp = kf_use_group();

	if(strcmp(cwd,"/")){
		lv_obj_t *b=lv_list_add_button(list, NULL, "../");
		names[nnames]=strdup(".."); lv_obj_add_event_cb(b,item_cb,LV_EVENT_CLICKED,names[nnames]);
		lv_group_add_obj(grp,b); nnames++;
	}
	DIR *d=opendir(cwd);
	if(d){ struct dirent *e;
		while((e=readdir(d)) && nnames<MAXN){
			if(!e->d_name[0]) continue;
			if(!strcmp(e->d_name,".")||!strcmp(e->d_name,"..")) continue;
			char *nm=strdup(e->d_name);
			if(!nm) break;                       /* out of heap — stop, don't store NULL */
			int isdir=(e->d_type==DT_DIR);        /* from readdir — no per-entry stat() */
			static char shown[300]; snprintf(shown,sizeof shown,"%s%s",e->d_name,isdir?"/":"");
			lv_obj_t *b=lv_list_add_button(list, NULL, shown);
			names[nnames]=nm; lv_obj_add_event_cb(b,item_cb,LV_EVENT_CLICKED,nm);
			lv_group_add_obj(grp,b); nnames++;
		}
		closedir(d);
	}
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
	lv_obj_set_size(list, LCD_W-8, KF_CONTENT_H-30);
	lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 26);

	grp = NULL;
	open_path("/");   /* start at the SD root; user navigates from there */
	lv_screen_load(scr);
}
