// apps/wallpaper.c — wallpaper chooser. Decodes real images from /kefyros/wallpapers
// (KF_WALLS; LVGL TJPGD/LODEPNG/BMP via the 'A' POSIX fs) and previews them
// full-screen live with the chosen fit mode (Fill/Fit/Center/Stretch). Selection
// is saved to deskconf and applied by the launcher on return.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#define WPDIR KF_WALLS    /* "/kefyros/wallpapers" */
#define MAXWP 32

static lv_obj_t *scr, *prev_img, *prev_dim, *list, *lbl_fit, *lbl_dim;
static char files[MAXWP][128];
static int  nfiles = 0;
static char pend_src[256];
static char pend_fit[12];
static int  pend_dim = 0;

static const char *FITS[] = { "fill", "fit", "center", "stretch" };
#define NFITS 4
static const int DIMS[] = { 0, 25, 50, 75 };
#define NDIMS 4

static int has_ext(const char *n){
	const char *d = strrchr(n,'.'); if(!d) return 0;
	return !strcasecmp(d,".jpg") || !strcasecmp(d,".jpeg") ||
	       !strcasecmp(d,".png") || !strcasecmp(d,".bmp") ||
	       !strcasecmp(d,".bin");   /* pre-converted RGB565 wallpapers */
}
static void scan(void){
	nfiles = 0;
	mkdir(WPDIR, 0755);                 /* defensive: ensure the dir exists (ignore EEXIST) */
	DIR *d = opendir(WPDIR);
	if(!d) return;
	struct dirent *e;
	while((e = readdir(d)) && nfiles < MAXWP){
		if(e->d_name[0]=='.') continue;
		if(!has_ext(e->d_name)) continue;
		char path[256];
		snprintf(path, sizeof path, "%s/%s", WPDIR, e->d_name);
		struct stat st;
		if(stat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
		snprintf(files[nfiles], sizeof files[nfiles], "%s", e->d_name); nfiles++;
	}
	closedir(d);
}
static void apply_preview(void){
	kf_wallpaper_apply(prev_img, pend_src, pend_fit);
	if(prev_dim) lv_obj_set_style_bg_opa(prev_dim, (lv_opa_t)(pend_dim*255/100), 0);
}
static void set_fit_label(void){
	char b[24]; snprintf(b,sizeof b,"fit: %s", pend_fit);
	lv_label_set_text(lbl_fit, b);
}
static void set_dim_label(void){
	char b[24]; snprintf(b,sizeof b,"dim: %d%%", pend_dim);
	lv_label_set_text(lbl_dim, b);
}

static void item_cb(lv_event_t *e){
	int idx = (int)(intptr_t)lv_event_get_user_data(e);
	if(idx == -1){                                   /* cycle fit mode */
		int cur = 0;
		for(int i=0;i<NFITS;i++) if(!strcmp(pend_fit,FITS[i])){ cur=i; break; }
		snprintf(pend_fit,sizeof pend_fit,"%s",FITS[(cur+1)%NFITS]);
		set_fit_label(); apply_preview(); return;
	}
	if(idx == -4){                                   /* cycle dim level */
		int cur = 0;
		for(int i=0;i<NDIMS;i++) if(DIMS[i]==pend_dim){ cur=i; break; }
		pend_dim = DIMS[(cur+1)%NDIMS];
		set_dim_label(); apply_preview(); return;
	}
	if(idx == -2){ pend_src[0]=0; apply_preview(); return; }   /* default (baked) */
	if(idx == -3){                                   /* apply + save */
		deskconf_set("wallpaper", pend_src);
		deskconf_set("fit", pend_fit);
		deskconf_set_int("dim", pend_dim);
		kf_back_to_launcher(); return;
	}
	snprintf(pend_src,sizeof pend_src,"A:%s/%s",WPDIR,files[idx]);  /* a file */
	apply_preview();
}

static void additem(lv_group_t *g, const char *txt, int idx){
	lv_obj_t *b = lv_list_add_button(list, NULL, txt);
	lv_obj_add_event_cb(b, item_cb, LV_EVENT_CLICKED, (void*)(intptr_t)idx);
	lv_group_add_obj(g, b);
	if(idx == -1) lbl_fit = lv_obj_get_child(b, lv_obj_get_child_cnt(b)-1);
	if(idx == -4) lbl_dim = lv_obj_get_child(b, lv_obj_get_child_cnt(b)-1);
}

void app_wallpaper_open(void){
	/* NOTE: the previous chooser screen is freed centrally by launcher_show() when
	   we returned to the desktop, so do NOT delete `scr` here (it would dangle). */
	scr = NULL;
	scan();
	snprintf(pend_src,sizeof pend_src,"%s",deskconf_get("wallpaper",""));
	snprintf(pend_fit,sizeof pend_fit,"%s",deskconf_get("fit","fill"));
	pend_dim = deskconf_get_int("dim", 0);

	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

	/* full-screen live preview behind the panel (clip parent crops cover overflow) */
	lv_obj_t *clip = lv_obj_create(scr);
	lv_obj_remove_style_all(clip);
	lv_obj_set_size(clip, LCD_W, LCD_H);
	lv_obj_set_pos(clip, 0, 0);
	lv_obj_set_style_bg_color(clip, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(clip, LV_OPA_COVER, 0);
	lv_obj_clear_flag(clip, LV_OBJ_FLAG_SCROLLABLE);
	prev_img = lv_image_create(clip);
	lv_obj_set_pos(prev_img, 0, 0);
	prev_dim = lv_obj_create(clip);
	lv_obj_remove_style_all(prev_dim);
	lv_obj_set_size(prev_dim, LCD_W, LCD_H);
	lv_obj_set_pos(prev_dim, 0, 0);
	lv_obj_set_style_bg_color(prev_dim, lv_color_black(), 0);
	lv_obj_clear_flag(prev_dim, LV_OBJ_FLAG_SCROLLABLE);
	apply_preview();

	/* translucent panel with the chooser list on the right (inset under the OS bar;
	   the full-screen preview behind stays full-bleed). */
	lv_obj_t *panel = lv_obj_create(scr);
	lv_obj_set_size(panel, 152, KF_CONTENT_H);
	lv_obj_align(panel, LV_ALIGN_TOP_RIGHT, 0, KF_CONTENT_Y);
	lv_obj_set_style_bg_color(panel, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(panel, LV_OPA_80, 0);
	lv_obj_set_style_border_width(panel, 0, 0);
	lv_obj_set_style_radius(panel, 0, 0);
	lv_obj_set_style_pad_all(panel, 0, 0);
	lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);

	list = lv_list_create(panel);
	lv_obj_set_size(list, 152, KF_CONTENT_H);
	lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(list, 0, 0);

	lv_group_t *g = kf_use_group();
	additem(g, "fit: fill", -1);   set_fit_label();
	additem(g, "dim: 0%", -4);     set_dim_label();
	additem(g, "[ default ]", -2);
	for(int i=0;i<nfiles;i++) additem(g, files[i], i);
	additem(g, "[ apply ]", -3);
	lv_group_focus_obj(lv_obj_get_child(list, 0));

	lv_screen_load(scr);
}
