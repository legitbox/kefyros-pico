// apps/wallpaper.c — wallpaper chooser. Decodes real images from /kefyros/wallpapers
// (KF_WALLS; LVGL TJPGD/LODEPNG/BMP via the 'A' POSIX fs) and previews them
// full-screen live with the chosen fit mode (Fill/Fit/Center/Stretch). Selection
// is saved to deskconf and applied by the launcher on return.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include "wallconv.h"
#include <stdio.h>
#include <stdlib.h>
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

/* ===================== on-device JPEG crop / convert editor =====================
   Pick a .jpg in the chooser -> this opens a full-screen live editor: the source is
   shown shrunk-to-fit with a movable/zoomable 320x320 crop box (arrows pan, '='/'-'
   zoom). ENTER bakes the crop to RGB565 in PSRAM (via wallconv), previews it, and a
   second ENTER writes it out as a streamable <name>.bin and sets it as the wallpaper.
   The whole thing runs without ever holding a full frame in the SRAM heap. */
static lv_obj_t *ed_scr, *ed_imgw, *ed_box, *ed_hint, *ed_keyc;
static int   ed_active, ed_confirm;
static char  ed_path[256], ed_name[128];
static int   ed_W, ed_H;                 /* native source dims                     */
static int   ed_st, ed_tw, ed_th;        /* thumbnail decode scale (1<<st) and dims */
static uint32_t ed_mark, ed_thumb, ed_out;  /* PSRAM mark + thumbnail + baked 320^2 */
static int   ed_cx, ed_cy, ed_side;      /* crop window (square) in NATIVE px       */
static float ed_disp;                    /* displayed-px per thumbnail-px (contain) */
static int   ed_offx, ed_offy;           /* thumbnail top-left on screen            */

#define ED_OUT 320

static int is_jpg_name(const char *n){
	const char *d = strrchr(n, '.');
	return d && (!strcasecmp(d, ".jpg") || !strcasecmp(d, ".jpeg"));
}
static void ed_key_cb(lv_event_t *e);
static void ed_click_cb(lv_event_t *e);
static void ed_bake(void);
static void ed_save(void);
static void ed_cancel(void);
static void ed_recrop(void);

static void ed_clamp(void){
	if(ed_side < 16) ed_side = 16;
	if(ed_side > ed_W) ed_side = ed_W;
	if(ed_side > ed_H) ed_side = ed_H;
	if(ed_cx < 0) ed_cx = 0;
	if(ed_cy < 0) ed_cy = 0;
	if(ed_cx > ed_W - ed_side) ed_cx = ed_W - ed_side;
	if(ed_cy > ed_H - ed_side) ed_cy = ed_H - ed_side;
}
static void ed_update_box(void){
	float k = ed_disp / (float)(1 << ed_st);          /* native px -> screen px */
	int bx = ed_offx + (int)(ed_cx * k + 0.5f);
	int by = ed_offy + (int)(ed_cy * k + 0.5f);
	int bw = (int)(ed_side * k + 0.5f);
	lv_obj_set_pos(ed_box, bx, by);
	lv_obj_set_size(ed_box, bw, bw);
}
static void ed_teardown(void){
	if(ed_imgw) lv_image_set_src(ed_imgw, NULL);  /* stop streaming before freeing the PSRAM */
	if(ed_active){ kf_psram_free_to(ed_mark); ed_active = 0; }
}

/* open the editor for `posix` (source path) named `name`. Returns 1 if it took over the
   screen, 0 on failure (caller should fall back to a plain preview). */
static int ed_open(const char *posix, const char *name){
	if(!kf_psram_size()) return 0;                 /* converter needs PSRAM scratch */
	if(!wc_dims(posix, &ed_W, &ed_H) || ed_W < 1 || ed_H < 1) return 0;
	snprintf(ed_path, sizeof ed_path, "%s", posix);
	snprintf(ed_name, sizeof ed_name, "%s", name);

	/* thumbnail: shrink the longest side to <= ~360 px for a crisp full-image preview */
	ed_st = 0;
	while(ed_st < 3 && ((ed_W >> ed_st) > 360 || (ed_H >> ed_st) > 360)) ed_st++;
	ed_tw = ed_W >> ed_st; ed_th = ed_H >> ed_st;
	if(ed_tw < 1) ed_tw = 1;
	if(ed_th < 1) ed_th = 1;

	ed_mark  = kf_psram_brk();
	ed_thumb = kf_psram_alloc((uint32_t)ed_tw * ed_th * 2);
	ed_out   = kf_psram_alloc((uint32_t)ED_OUT * ED_OUT * 2);
	if(ed_thumb == 0xFFFFFFFFu || ed_out == 0xFFFFFFFFu){ kf_psram_free_to(ed_mark); return 0; }
	if(!wc_decode_region(ed_path, ed_st, 0, 0, ed_tw, ed_th, ed_thumb)){ kf_psram_free_to(ed_mark); return 0; }
	ed_active = 1; ed_confirm = 0;

	/* start with the largest centered square crop */
	ed_side = ed_W < ed_H ? ed_W : ed_H;
	ed_cx = (ed_W - ed_side) / 2;
	ed_cy = (ed_H - ed_side) / 2;

	/* contain transform of the thumbnail in the full panel (matches "fit") */
	float sx = (float)LCD_W / ed_tw, sy = (float)LCD_H / ed_th;
	ed_disp = sx < sy ? sx : sy;
	ed_offx = (int)((LCD_W - ed_tw * ed_disp) / 2);
	ed_offy = (int)((LCD_H - ed_th * ed_disp) / 2);

	/* build the editor screen */
	ed_scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(ed_scr, 0, 0);
	lv_obj_set_style_bg_color(ed_scr, lv_color_black(), 0);
	lv_obj_clear_flag(ed_scr, LV_OBJ_FLAG_SCROLLABLE);

	ed_imgw = lv_image_create(ed_scr);
	lv_obj_set_pos(ed_imgw, 0, 0);
	kf_wallpaper_show_raw(ed_imgw, ed_thumb, ed_tw, ed_th, "fit");

	ed_box = lv_obj_create(ed_scr);
	lv_obj_remove_style_all(ed_box);
	lv_obj_set_style_border_width(ed_box, 2, 0);
	lv_obj_set_style_border_color(ed_box, KF_AMBER_HOT, 0);
	lv_obj_clear_flag(ed_box, LV_OBJ_FLAG_SCROLLABLE);

	ed_hint = lv_label_create(ed_scr);
	lv_obj_set_width(ed_hint, LCD_W);
	lv_obj_set_style_text_color(ed_hint, KF_TEXT, 0);
	lv_obj_set_style_bg_color(ed_hint, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(ed_hint, LV_OPA_70, 0);
	lv_obj_set_style_pad_all(ed_hint, 2, 0);
	lv_label_set_text(ed_hint, "arrows move  =/- zoom  ENTER convert  BKSP back");
	lv_obj_align(ed_hint, LV_ALIGN_BOTTOM_MID, 0, 0);

	/* invisible focusable key-catcher: NOT inside a list, so raw arrows reach LV_EVENT_KEY */
	lv_group_t *g = kf_use_group();
	ed_keyc = lv_obj_create(ed_scr);
	lv_obj_remove_style_all(ed_keyc);
	lv_obj_set_size(ed_keyc, 1, 1);
	lv_obj_set_pos(ed_keyc, 0, 0);
	lv_obj_add_flag(ed_keyc, LV_OBJ_FLAG_CLICKABLE);
	lv_group_add_obj(g, ed_keyc);
	lv_group_focus_obj(ed_keyc);
	lv_obj_add_event_cb(ed_keyc, ed_key_cb, LV_EVENT_KEY, NULL);
	lv_obj_add_event_cb(ed_keyc, ed_click_cb, LV_EVENT_CLICKED, NULL);  /* ENTER arrives as CLICKED */

	ed_update_box();
	lv_obj_t *old = scr; scr = NULL;
	lv_screen_load(ed_scr);
	if(old) lv_obj_delete_async(old);              /* free the chooser (async: we're in its button event) */
	return 1;
}

static void ed_bake(void){
	lv_obj_add_flag(ed_box, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(ed_hint, "converting...");
	lv_refr_now(NULL);                             /* paint the label before the blocking decode */
	if(!wc_bake(ed_path, ed_cx, ed_cy, ed_side, ED_OUT, ed_out)){
		lv_label_set_text(ed_hint, "convert failed (out of memory)  -  BKSP back");
		lv_obj_remove_flag(ed_box, LV_OBJ_FLAG_HIDDEN);
		return;                                    /* stays in crop mode; ENTER retries */
	}
	kf_wallpaper_show_raw(ed_imgw, ed_out, ED_OUT, ED_OUT, "fill");
	lv_label_set_text(ed_hint, "ENTER save  -  BKSP re-crop");
	ed_confirm = 1;
}
static void ed_recrop(void){
	kf_wallpaper_show_raw(ed_imgw, ed_thumb, ed_tw, ed_th, "fit");
	lv_obj_remove_flag(ed_box, LV_OBJ_FLAG_HIDDEN);
	lv_label_set_text(ed_hint, "arrows move  =/- zoom  ENTER convert  BKSP back");
	ed_update_box();
	ed_confirm = 0;
}
static void ed_save(void){
	/* write <basename>.bin (LVGL RGB565: 12-byte header + pixels streamed from PSRAM) */
	char base[128]; snprintf(base, sizeof base, "%s", ed_name);
	char *dot = strrchr(base, '.'); if(dot) *dot = 0;
	char outp[256]; snprintf(outp, sizeof outp, "%s/%s.bin", WPDIR, base);
	FILE *f = fopen(outp, "wb");
	int ok = (f != NULL);
	if(ok){
		uint8_t hdr[12] = { 0x19, 0x12, 0, 0,
		                    (uint8_t)ED_OUT, (uint8_t)(ED_OUT >> 8),
		                    (uint8_t)ED_OUT, (uint8_t)(ED_OUT >> 8),
		                    (uint8_t)(ED_OUT * 2), (uint8_t)((ED_OUT * 2) >> 8), 0, 0 };
		ok = (fwrite(hdr, 1, 12, f) == 12);
		uint16_t *row = ok ? malloc((size_t)ED_OUT * 2) : NULL;
		if(!row) ok = 0;
		for(int y = 0; ok && y < ED_OUT; y++){
			kf_psram_read(ed_out + (size_t)y * ED_OUT * 2, row, (uint32_t)ED_OUT * 2);
			if(fwrite(row, 1, (size_t)ED_OUT * 2, f) != (size_t)ED_OUT * 2) ok = 0;
		}
		free(row);
		fclose(f);
	}
	if(!ok){ lv_label_set_text(ed_hint, "save failed (SD?)  -  BKSP re-crop"); return; }

	char lvsrc[256]; snprintf(lvsrc, sizeof lvsrc, "A:%s", outp);
	deskconf_set("wallpaper", lvsrc);
	deskconf_set("fit", "fill");
	ed_teardown();
	kf_back_to_launcher();                         /* launcher frees ed_scr + shows new wallpaper */
}
static void ed_cancel(void){
	ed_teardown();
	lv_obj_t *old = ed_scr; ed_scr = NULL;
	app_wallpaper_open();                          /* rebuild the chooser (sets scr, loads it) */
	if(old) lv_obj_delete_async(old);              /* async: we're inside the editor's key event */
}
/* ENTER (delivered as a CLICKED event by LVGL's group): convert, or save the result. */
static void ed_click_cb(lv_event_t *e){
	(void)e;
	if(ed_confirm) ed_save();
	else           ed_bake();
}
static void ed_key_cb(lv_event_t *e){
	uint32_t k = lv_event_get_key(e);
	if(k == LV_KEY_BACKSPACE){ if(ed_confirm) ed_recrop(); else ed_cancel(); return; }
	if(ed_confirm) return;                          /* in preview: only ENTER / BKSP act */
	int step = ed_side / 20; if(step < 1) step = 1;
	switch(k){
	case LV_KEY_LEFT:  ed_cx -= step; break;
	case LV_KEY_RIGHT: ed_cx += step; break;
	case LV_KEY_UP:    ed_cy -= step; break;
	case LV_KEY_DOWN:  ed_cy += step; break;
	case '=': case '+': { int d = ed_side / 10; if(d < 8) d = 8; ed_cx += d / 2; ed_cy += d / 2; ed_side -= d; } break;
	case '-': case '_': { int d = ed_side / 10; if(d < 8) d = 8; ed_cx -= d / 2; ed_cy -= d / 2; ed_side += d; } break;
	default: return;
	}
	ed_clamp();
	ed_update_box();
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
	if(is_jpg_name(files[idx])){                  /* a JPEG -> crop & convert on-device */
		char posix[256]; snprintf(posix, sizeof posix, "%s/%s", WPDIR, files[idx]);
		if(ed_open(posix, files[idx])) return;    /* editor took over; else fall through to preview */
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
