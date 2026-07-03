// ui/launcher.c — Kefyros desktop: wallpaper (clip-parented, runtime fit) + a grid of
// pixel-art app shortcuts. The status bar lives in the persistent OS top bar now
// (ui/topbar.c), so the grid simply starts below KF_TOPBAR_H.
// D-pad moves a cursor over the grid (incl. empty cells); ENTER launches; long-press
// ENTER picks an icon up, arrows move the cursor, ENTER drops it (layout persisted).
#include "../kefyros.h"
#include "theme.h"
#include "deskconf.h"
#include "lvgl/src/draw/lv_image_decoder_private.h"   /* lv_image_decoder_dsc_t fields */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static lv_obj_t  *scr_home, *wp_img, *wp_dim, *lbl_hint, *cursor;
static lv_group_t *grp_home;
static lv_group_t *app_group = NULL;   /* the CURRENT app's input group; freed on reuse / return home */

/* --- desktop apps --- */
#define APP_NO_SLEEP 1     /* "always active": idle dims the backlight but never downclocks the
                              CPU to the 150 MHz sleep tier — for apps (music) whose realtime work
                              (audio playback) would break at sleep clocks. */
typedef struct {
	const char *id;            /* also the SD icon filename: /kefyros/icons/<id>.png */
	const char *label;
	void (*open)(void);
	int   def_slot;
	int   flags;               /* APP_NO_SLEEP, ... (0 = ordinary) */
} desk_app_t;

static const desk_app_t dapps[] = {
	{ "calc",       "Calc",        app_calc_open,        1 },
	{ "files",      "Files",       app_files_open,       2 },
	{ "wifi",       "WiFi",        app_wifi_open,        3 },
	{ "settings",   "Settings",    app_settings_open,    4 },
	{ "appearance", "Wallpaper",   app_wallpaper_open,   5 },
	{ "notes",      "Notes",       app_editor_open,      6 },
	{ "music",      "Music",       app_music_open,       7, APP_NO_SLEEP },  /* playback dies at sleep clocks */
	{ "electronics","Electronics", app_electronics_open, 8 },
	{ "spineko",    "Spineko",     app_spineko_open,     9 },
	{ "deepseek",   "DeepSeek",    app_deepseek_open,    10 },
	{ "help",       "Help",        app_help_open,        11 },
	{ "term",       "Terminal",    app_term_open,        12 },   /* SSH-2 terminal client */
	/* "KAPI Demo" (app_demo_open, port/kapi.c) is a developer-only class-1 .kx loader
	   test: it needs an /apps/demo/demo.kx rebuilt against each exact firmware ELF, which
	   is too fragile for a distributable card. Dropped from the release launcher; re-add
	   `{ "demo","KAPI Demo",app_demo_open,13 }` for dev builds that ship a matching .kx. */
};
/* index into dapps of the app the user launched (or -1 = on the desktop). The idle timer
   consults dapps[s_cur_app].flags so a no-sleep app keeps its clock while idle. */
static int s_cur_app = -1;
static int kf_app_allows_sleep(void){
	return s_cur_app < 0 || !(dapps[s_cur_app].flags & APP_NO_SLEEP);
}
#define NAPPS  (int)(sizeof(dapps)/sizeof(dapps[0]))
#define GCOLS  4
#define GROWS  3
#define NCELLS (GCOLS*GROWS)         /* cells per page (4x3) */
#define NPAGES 3                     /* horizontal pages -> NCELLS*NPAGES app slots */
#define NSLOTS (NCELLS*NPAGES)
#define BAR_H  KF_TOPBAR_H          /* grid starts below the persistent OS top bar */
#define CELLW  (LCD_W/GCOLS)
#define CELLH  ((LCD_H-BAR_H-2)/GROWS)

static lv_obj_t *cells[NCELLS];      /* the NCELLS widgets show the CURRENT page */
static lv_obj_t *page_ind;           /* row of page dots at the bottom */
static lv_obj_t *dots[NPAGES];
static int slot_app[NSLOTS];      /* app index in each GLOBAL slot, or -1 */
static int page = 0;              /* current page (0..NPAGES-1) */
static int cur = 0;               /* cursor cell within the page (0..NCELLS-1) */
static int carrying = -1;         /* GLOBAL slot being moved, or -1 */

/* global slot backing the current page's cell i */
static int gslot(int i){ return page*NCELLS + i; }

/* fresh group, made active on the keypad indev — apps call this when they open.
   CRITICAL: free the previous app group first, or every screen/form that calls
   this leaks a whole lv_group (calc/electronics call it per form). grp_home is
   created directly (not via this) so it is never freed here. */
lv_group_t *kf_use_group(void){
	if(app_group){ lv_group_delete(app_group); app_group = NULL; }
	app_group = lv_group_create();
	lv_indev_set_group(indev_get(), app_group);
	return app_group;
}

/* ---- wallpaper: stored in PSRAM (off-heap, off the 520 KB SRAM) and streamed row
   by row to the panel via a custom LVGL image decoder. This frees the old 200 KB
   static SRAM scratch (the heap-ceiling hog that was causing the OOM crashes). The
   .bin is a LVGL RGB565 image: 12-byte header + raw pixel data. ---- */
#define KF_WP_DEFAULT "A:/kefyros/wallpapers/wall.bin"
#define KF_WP_PSRAM_MAX (320u*320u*2u)        /* reserved PSRAM region for the wallpaper */

static lv_image_dsc_t wp_dsc;                 /* header only; data is in PSRAM (streamed) */
static uint8_t   wp_data_dummy[2];            /* non-NULL sentinel: LVGL rejects variable images
                                                with data==NULL before our decoder runs. Never read
                                                (get_area streams the real pixels from PSRAM). */
static uint32_t  wp_region = 0xFFFFFFFFu;     /* PSRAM offset of the wallpaper pixels    */
static int       wp_valid  = 0;
static char      wp_path[160] = "";           /* which file is currently loaded          */
static lv_draw_buf_t *wp_strip = NULL;        /* reused 1-row SRAM buffer for streaming   */
static lv_image_dsc_t raw_dsc;                /* a SECOND streamed image: a raw RGB565 region
                                                already resident in PSRAM. Used by the on-device
                                                wallpaper converter for its live preview and the
                                                baked result (see kf_wallpaper_show_raw). The
                                                desktop wallpaper (wp_dsc) is left untouched. */
static uint32_t  raw_region = 0xFFFFFFFFu;     /* PSRAM offset backing raw_dsc            */

/* Map an image source to its (header, PSRAM region). Both wp_dsc (desktop wallpaper) and
   raw_dsc (converter scratch) are streamed by the one decoder below. */
static lv_image_dsc_t *wp_pick(const void *src, uint32_t *region){
	if(src == &wp_dsc){  *region = wp_region;  return &wp_dsc;  }
	if(src == &raw_dsc){ *region = raw_region; return &raw_dsc; }
	return NULL;
}

/* ----- the custom decoder: claims ONLY our &wp_dsc / &raw_dsc images, streams rows from PSRAM ----- */
static lv_result_t wp_dec_info(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc, lv_image_header_t *hdr){
	(void)d;
	uint32_t region; lv_image_dsc_t *img = wp_pick(dsc->src, &region);
	if(dsc->src_type != LV_IMAGE_SRC_VARIABLE || !img) return LV_RESULT_INVALID;
	*hdr = img->header;
	return LV_RESULT_OK;
}
static lv_result_t wp_dec_open(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc){
	(void)d;
	uint32_t region; lv_image_dsc_t *img = wp_pick(dsc->src, &region);
	if(!img) return LV_RESULT_INVALID;
	dsc->header  = img->header;
	dsc->decoded = NULL;                       /* streamed via get_area */
	return LV_RESULT_OK;
}
static lv_result_t wp_dec_get_area(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc,
                                   const lv_area_t *full, lv_area_t *area){
	(void)d;
	uint32_t region; lv_image_dsc_t *img = wp_pick(dsc->src, &region);
	if(!img) return LV_RESULT_INVALID;
	int32_t w_px = lv_area_get_width(full);
	if(area->y1 == LV_COORD_MIN){             /* first call: (re)size the 1-row strip */
		lv_draw_buf_t *nb = lv_draw_buf_reshape(wp_strip, LV_COLOR_FORMAT_RGB565, w_px, 1, LV_STRIDE_AUTO);
		if(!nb){
			if(wp_strip) lv_draw_buf_destroy(wp_strip);
			wp_strip = lv_draw_buf_create(w_px, 1, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
			if(!wp_strip) return LV_RESULT_INVALID;
			nb = wp_strip;
		}
		wp_strip = nb;
		*area = *full;
		area->y2 = area->y1;
	} else {
		area->y1++; area->y2++;
	}
	if(area->y1 > full->y2) return LV_RESULT_INVALID;
	uint32_t off = region + (uint32_t)area->y1 * img->header.stride + (uint32_t)area->x1 * 2u;
	kf_psram_read(off, wp_strip->data, (uint32_t)w_px * 2u);
	dsc->decoded = wp_strip;
	return LV_RESULT_OK;
}
static void wp_dec_close(lv_image_decoder_t *d, lv_image_decoder_dsc_t *dsc){ (void)d; (void)dsc; }

void kf_wallpaper_init(void){                  /* register the decoder once (from main) */
	lv_image_decoder_t *dec = lv_image_decoder_create();
	lv_image_decoder_set_info_cb(dec, wp_dec_info);
	lv_image_decoder_set_open_cb(dec, wp_dec_open);
	lv_image_decoder_set_get_area_cb(dec, wp_dec_get_area);
	lv_image_decoder_set_close_cb(dec, wp_dec_close);
}

/* stream a .bin wallpaper from SD into the reserved PSRAM region; fills wp_dsc header */
static int wp_load_to_psram(const char *path){
	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	uint8_t h[12];
	if(fread(h, 1, 12, f) != 12 || h[0] != 0x19 /*LVGL bin magic*/){ fclose(f); return 0; }
	int w = h[4] | (h[5]<<8), ih = h[6] | (h[7]<<8), stride = h[8] | (h[9]<<8);
	if(w <= 0 || ih <= 0){ fclose(f); return 0; }
	if(stride <= 0) stride = w * 2;
	uint32_t bytes = (uint32_t)stride * (uint32_t)ih;
	if(bytes > KF_WP_PSRAM_MAX){ fclose(f); return 0; }
	if(wp_region == 0xFFFFFFFFu) wp_region = kf_psram_alloc(KF_WP_PSRAM_MAX);
	if(wp_region == 0xFFFFFFFFu){ fclose(f); return 0; }
	uint8_t tmp[1024]; uint32_t pos = 0;
	while(pos < bytes){
		uint32_t c = bytes - pos; if(c > sizeof tmp) c = sizeof tmp;
		size_t rd = fread(tmp, 1, c, f);
		if(!rd) break;
		kf_psram_write(wp_region + pos, tmp, (uint32_t)rd);
		pos += rd;
	}
	fclose(f);
	if(pos != bytes) return 0;
	lv_memzero(&wp_dsc, sizeof wp_dsc);
	wp_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
	wp_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
	wp_dsc.header.w      = w;
	wp_dsc.header.h      = ih;
	wp_dsc.header.stride = stride;
	wp_dsc.data          = wp_data_dummy;      /* non-NULL sentinel (pixels stream from PSRAM) */
	wp_dsc.data_size     = bytes;
	return 1;
}

/* Size `img` to the panel and apply a fit mode (fill/cover, fit/contain, center, stretch)
   for a source of iw x ih. Shared by the .bin wallpaper path and the raw-PSRAM path. */
static void wp_apply_fit(lv_obj_t *img, int iw, int ih, const char *fit){
	if(iw <= 0) iw = LCD_W;
	if(ih <= 0) ih = LCD_H;
	lv_obj_set_size(img, LCD_W, LCD_H);
	if(fit && !strcmp(fit, "stretch")){
		lv_image_set_inner_align(img, LV_IMAGE_ALIGN_STRETCH);   /* LVGL scales x/y */
		return;
	}
	lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);
	float sx = (float)LCD_W/iw, sy = (float)LCD_H/ih, s;
	if(fit && !strcmp(fit, "fit"))         s = sx < sy ? sx : sy;   /* contain */
	else if(fit && !strcmp(fit, "center")) s = 1.0f;
	else                                   s = sx > sy ? sx : sy;   /* fill/crop = cover */
	int z = (int)(s*256 + 0.5f);
	if(z < 1) z = 1;
	lv_image_set_scale(img, z);
}

void kf_wallpaper_apply(lv_obj_t *img, const char *src, const char *fit){
	const char *lvp  = (src && src[0] && strcmp(src, "default")) ? src : KF_WP_DEFAULT;
	const char *path = (lvp[0]=='A' && lvp[1]==':') ? lvp + 2 : lvp;   /* "A:/x" -> "/x" */
	lv_image_set_src(img, NULL);
	if(!kfs_ready() || !kf_psram_size()) return;            /* no card / no PSRAM -> blank */
	if(!wp_valid || strcmp(wp_path, path) != 0){            /* (re)load into PSRAM on change */
		lv_image_cache_drop(&wp_dsc);
		wp_valid = wp_load_to_psram(path);
		if(wp_valid) snprintf(wp_path, sizeof wp_path, "%s", path);
		else { wp_path[0] = 0; return; }
	}
	int iw = wp_dsc.header.w, ih = wp_dsc.header.h;
	lv_image_set_src(img, &wp_dsc);
	wp_apply_fit(img, iw, ih, fit);
}

/* Display a raw RGB565 image already resident in PSRAM (off, w, h) through the streaming
   decoder, with the same fit modes as a .bin wallpaper. Used by the on-device converter
   for both its live thumbnail and the baked 320x320 result — no SD round-trip. The PSRAM
   at `off` must stay valid until the image is no longer shown (set src to NULL first). */
void kf_wallpaper_show_raw(lv_obj_t *img, uint32_t off, int w, int h, const char *fit){
	lv_image_set_src(img, NULL);
	lv_image_cache_drop(&raw_dsc);             /* contents/dims may have changed since last show */
	lv_memzero(&raw_dsc, sizeof raw_dsc);
	raw_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
	raw_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
	raw_dsc.header.w      = w;
	raw_dsc.header.h      = h;
	raw_dsc.header.stride = (uint32_t)w * 2u;
	raw_dsc.data          = wp_data_dummy;     /* non-NULL sentinel (pixels stream from PSRAM) */
	raw_dsc.data_size     = (uint32_t)w * (uint32_t)h * 2u;
	raw_region            = off;
	lv_image_set_src(img, &raw_dsc);
	wp_apply_fit(img, w, h, fit);
}

/* idle screen-off: after `screen_timeout` s of no keys, kill BOTH backlights and drop to the
   low-power sleep clock (150 MHz / 1.10 V); the next key restores them. Runs everywhere (not
   just the desktop). The clock is restored (kf_clock_wake) to whatever tier was active before
   sleeping, so idling inside a WiFi session wakes back at eco rather than normal. */
static int s_idle_dimmed = 0;   /* backlights off */
static int s_idle_slept  = 0;   /* dropped to the 150 MHz sleep clock (+ parked audio) */
static void idle_wake(void){
	if(!s_idle_dimmed && !s_idle_slept) return;
	if(s_idle_slept){                         /* only restore the clock if we actually slept */
		kf_clock_wake();
		kf_audio_idle_unpark();
		s_idle_slept = 0;
	}
	if(s_idle_dimmed){
		uint8_t v=(uint8_t)deskconf_get_int("bkl",5); reg_write(REG_BKL,&v,1);
		uint8_t k=(uint8_t)deskconf_get_int("bk2",2); reg_write(REG_BK2,&k,1);
		s_idle_dimmed = 0;
	}
}
static void idle_timer(lv_timer_t *t){
	(void)t;
	int to = deskconf_get_int("screen_timeout", 60);   /* seconds; 0 = never */
	if(to <= 0){ idle_wake(); return; }
	uint32_t idle = lv_tick_get() - uart_last_activity();
	if(idle > (uint32_t)to*1000){
		if(!s_idle_dimmed){
			uint8_t z=0;
			reg_write(REG_BKL,&z,1);          /* LCD backlight off */
			reg_write(REG_BK2,&z,1);          /* keyboard backlight off */
			s_idle_dimmed = 1;
		}
		/* no-sleep apps (music) dim the screen but hold their clock — sleeping would
		   break realtime playback. Park the speaker before the downclock so the idle
		   PWM carrier doesn't whine at the lower sleep clock. */
		if(!s_idle_slept && kf_app_allows_sleep()){
			kf_clock_sleep();                 /* 150 MHz / 1.10 V */
			kf_audio_idle_park();
			s_idle_slept = 1;
		}
	} else idle_wake();
}

/* (the status bar — mem/clock/battery — now lives in ui/topbar.c, always on top) */

/* --- cells / move mode --- */
#define ICON_PX 48
#define ICON_DIR "A:/kefyros/icons"
/* Icons load from the SD card (/kefyros/icons/<appid>.png), scaled to ICON_PX.
   No card / missing icon -> hide the image so just the label shows. */
static void set_icon(lv_obj_t *ic, int a){
	char lvp[160];
	snprintf(lvp, sizeof lvp, ICON_DIR "/%s.png", dapps[a].id);
	lv_image_header_t hdr;
	if(kfs_ready() && lv_image_decoder_get_info(lvp, &hdr) == LV_RESULT_OK && hdr.w > 0){
		lv_image_set_src(ic, lvp);
		int m = hdr.w > hdr.h ? hdr.w : hdr.h;
		if(m != ICON_PX) lv_image_set_scale(ic, ICON_PX*256/m);
	} else {
		lv_obj_add_flag(ic, LV_OBJ_FLAG_HIDDEN);
	}
}

static void render_cell(int i){
	lv_obj_t *cell = cells[i];
	lv_obj_clean(cell);
	int carried = (carrying == gslot(i));    /* show a border on the carried app's cell */
	lv_obj_set_style_border_width(cell, carried?2:0, 0);
	if(carried) lv_obj_set_style_border_color(cell, KF_AMBER_HOT, 0);
	int a = slot_app[gslot(i)];
	if(a < 0) return;
	lv_obj_t *ic = lv_image_create(cell);
	set_icon(ic, a);
	lv_obj_t *nm = lv_label_create(cell);
	lv_label_set_text(nm, dapps[a].label);
	lv_obj_set_style_text_color(nm, KF_TEXT, 0);
	lv_obj_set_style_bg_color(nm, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(nm, LV_OPA_50, 0);
	lv_obj_set_style_pad_hor(nm, 2, 0);
}
static void render_page(void){ for(int i=0;i<NCELLS;i++) render_cell(i); }
static void save_layout(void){
	char key[48];
	for(int a=0;a<NAPPS;a++)
		for(int s=0;s<NSLOTS;s++)
			if(slot_app[s]==a){ snprintf(key,sizeof key,"slot.%s",dapps[a].id); deskconf_set_int(key,s); break; }
}
static void start_carry(int i){
	if(carrying>=0 || slot_app[gslot(i)]<0) return;
	carrying = gslot(i); render_cell(i);
	lv_label_set_text_fmt(lbl_hint, "moving %s  -  arrows / pages, ENTER to drop", dapps[slot_app[carrying]].label);
	lv_obj_remove_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
}
static void drop_at(int j){
	int o = carrying; carrying = -1;
	int g = gslot(j);                        /* j may be on a different page than `o` */
	int tmp = slot_app[o]; slot_app[o] = slot_app[g]; slot_app[g] = tmp;
	render_page();                           /* o might be off-page; redraw the lot */
	save_layout();
	lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);
	cur = j; lv_group_focus_obj(cells[j]);
}

/* smooth sliding selection cursor */
static void cur_x_cb(void *o, int32_t v){ lv_obj_set_x((lv_obj_t*)o, v); }
static void cur_y_cb(void *o, int32_t v){ lv_obj_set_y((lv_obj_t*)o, v); }
static void move_cursor(int to, int animate){
	if(!cursor) return;
	int x = (to%GCOLS)*CELLW, y = BAR_H + 2 + (to/GCOLS)*CELLH;
	if(!animate){ lv_obj_set_pos(cursor, x, y); return; }
	lv_anim_t a; lv_anim_init(&a); lv_anim_set_var(&a, cursor);
	lv_anim_set_duration(&a, 120); lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
	lv_anim_set_exec_cb(&a, cur_x_cb); lv_anim_set_values(&a, lv_obj_get_x(cursor), x); lv_anim_start(&a);
	lv_anim_set_exec_cb(&a, cur_y_cb); lv_anim_set_values(&a, lv_obj_get_y(cursor), y); lv_anim_start(&a);
}
/* wallpaper dim overlay (0..90%) so icons/logos stay readable over bright art */
static void apply_dim(void){
	if(!wp_dim) return;
	int d = deskconf_get_int("dim", 0);
	if(d < 0) d = 0; if(d > 90) d = 90;
	lv_obj_set_style_bg_opa(wp_dim, (lv_opa_t)(d*255/100), 0);
}

/* page dots: highlight the current page */
static void update_dots(void){
	if(!page_ind) return;
	for(int p=0;p<NPAGES;p++)
		lv_obj_set_style_bg_opa(dots[p], p==page ? LV_OPA_COVER : LV_OPA_30, 0);
}
/* flip to page `p`, landing the cursor on `focus` (cell within the new page) */
static void go_page(int p, int focus){
	if(p < 0 || p >= NPAGES || p == page) return;
	page = p;
	render_page();                 /* repopulate the NCELLS widgets from the new page */
	update_dots();
	cur = focus;
	lv_group_focus_obj(cells[cur]);
	move_cursor(cur, 0);           /* jump (no slide) across a page break */
}

static void cell_key_cb(lv_event_t *e){
	uint32_t k = lv_event_get_key(e);
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	int c = i % GCOLS, r = i / GCOLS, nxt = -1;
	if(k==LV_KEY_LEFT){
		if(c>0) nxt = i-1;
		else { go_page(page-1, r*GCOLS + (GCOLS-1)); return; }   /* off the left edge -> prev page */
	}
	else if(k==LV_KEY_RIGHT){
		if(c<GCOLS-1) nxt = i+1;
		else { go_page(page+1, r*GCOLS + 0); return; }           /* off the right edge -> next page */
	}
	else if(k==LV_KEY_UP   && r>0)        nxt = i-GCOLS;
	else if(k==LV_KEY_DOWN && r<GROWS-1)  nxt = i+GCOLS;
	if(nxt>=0){ cur = nxt; lv_group_focus_obj(cells[nxt]); move_cursor(nxt, 1); }
}
static void cell_click_cb(lv_event_t *e){          /* short ENTER: launch or drop */
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	if(carrying >= 0){ drop_at(i); return; }
	int a = slot_app[gslot(i)];
	if(a >= 0 && dapps[a].open){ s_cur_app = a; kf_sfx_play("open"); dapps[a].open(); }
}
static void cell_long_cb(lv_event_t *e){           /* long ENTER: pick up */
	int i = (int)(intptr_t)lv_event_get_user_data(e);
	start_carry(i);
}

static lv_obj_t *make_cell(int i){
	lv_obj_t *cell = lv_obj_create(scr_home);
	lv_obj_remove_style_all(cell);
	lv_obj_set_size(cell, CELLW, CELLH);
	lv_obj_set_pos(cell, (i%GCOLS)*CELLW, BAR_H + 2 + (i/GCOLS)*CELLH);
	lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(cell, 3, 0);
	lv_obj_set_style_radius(cell, 0, 0);
	lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
	lv_obj_set_style_outline_width(cell, 0, 0);
	/* selection shown by the sliding `cursor` box, not a per-cell focus style */
	lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
	lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(cell, cell_key_cb,   LV_EVENT_KEY,           (void*)(intptr_t)i);
	lv_obj_add_event_cb(cell, cell_click_cb, LV_EVENT_SHORT_CLICKED, (void*)(intptr_t)i);
	lv_obj_add_event_cb(cell, cell_long_cb,  LV_EVENT_LONG_PRESSED,  (void*)(intptr_t)i);
	return cell;
}

static void load_layout(void){
	for(int s=0;s<NSLOTS;s++) slot_app[s] = -1;
	for(int a=0;a<NAPPS;a++){
		char key[48]; snprintf(key,sizeof key,"slot.%s",dapps[a].id);
		int slot = deskconf_get_int(key, dapps[a].def_slot);
		if(slot < 0 || slot >= NSLOTS || slot_app[slot] != -1){
			slot = -1;
			for(int s=0;s<NSLOTS;s++) if(slot_app[s]==-1){ slot=s; break; }
		}
		if(slot >= 0) slot_app[slot] = a;
	}
}

/* stop drawing the wallpaper when we leave the desktop. The pixels live in PSRAM
   (not the heap), so nothing to free; launcher_show() re-points the image on return. */
static void on_home_unload(lv_event_t *e){ (void)e; if(wp_img) lv_image_set_src(wp_img, NULL); }

void launcher_init(void){
	deskconf_load();
	scr_home = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr_home, 0, 0);
	lv_obj_clear_flag(scr_home, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr_home, on_home_unload, LV_EVENT_SCREEN_UNLOADED, NULL);

	/* wallpaper: dark clip parent (clips cover-scaled overflow) + image child */
	lv_obj_t *wpclip = lv_obj_create(scr_home);
	lv_obj_remove_style_all(wpclip);
	lv_obj_set_size(wpclip, LCD_W, LCD_H);
	lv_obj_set_pos(wpclip, 0, 0);
	lv_obj_set_style_bg_color(wpclip, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(wpclip, LV_OPA_COVER, 0);
	lv_obj_clear_flag(wpclip, LV_OBJ_FLAG_SCROLLABLE);
	wp_img = lv_image_create(wpclip);
	lv_obj_set_pos(wp_img, 0, 0);
	kf_wallpaper_apply(wp_img, deskconf_get("wallpaper",""), deskconf_get("fit","crop"));

	/* dim overlay over the wallpaper (under bar + icons) for readability */
	wp_dim = lv_obj_create(scr_home);
	lv_obj_remove_style_all(wp_dim);
	lv_obj_set_size(wp_dim, LCD_W, LCD_H);
	lv_obj_set_pos(wp_dim, 0, 0);
	lv_obj_set_style_bg_color(wp_dim, lv_color_black(), 0);
	lv_obj_clear_flag(wp_dim, LV_OBJ_FLAG_SCROLLABLE);
	apply_dim();

	/* (the status bar is the persistent OS top bar now — ui/topbar.c, on layer_top) */

	/* sliding selection cursor (under the icons; darker-amber box, no rounding) */
	cursor = lv_obj_create(scr_home);
	lv_obj_remove_style_all(cursor);
	lv_obj_set_size(cursor, CELLW, CELLH);
	lv_obj_set_style_radius(cursor, 0, 0);
	lv_obj_set_style_bg_color(cursor, KF_AMBER_DIM, 0);
	lv_obj_set_style_bg_opa(cursor, LV_OPA_40, 0);
	lv_obj_set_style_border_width(cursor, 2, 0);
	lv_obj_set_style_border_color(cursor, KF_AMBER, 0);
	lv_obj_clear_flag(cursor, LV_OBJ_FLAG_SCROLLABLE);

	/* desktop cells. grp_home is created DIRECTLY (not kf_use_group) so it is the
	   persistent home group and never gets freed as an "app group". */
	grp_home = lv_group_create();
	lv_indev_set_group(indev_get(), grp_home);
	load_layout();
	for(int i=0;i<NCELLS;i++){
		cells[i] = make_cell(i);
		render_cell(i);
		lv_group_add_obj(grp_home, cells[i]);
	}

	/* page dots (bottom-center): one per page, current one fully opaque */
	page_ind = lv_obj_create(scr_home);
	lv_obj_remove_style_all(page_ind);
	lv_obj_set_size(page_ind, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
	lv_obj_set_flex_flow(page_ind, LV_FLEX_FLOW_ROW);
	lv_obj_set_style_pad_column(page_ind, 5, 0);
	lv_obj_clear_flag(page_ind, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_align(page_ind, LV_ALIGN_BOTTOM_MID, 0, -3);
	for(int p=0;p<NPAGES;p++){
		dots[p] = lv_obj_create(page_ind);
		lv_obj_remove_style_all(dots[p]);
		lv_obj_set_size(dots[p], 6, 6);
		lv_obj_set_style_radius(dots[p], 3, 0);
		lv_obj_set_style_bg_color(dots[p], KF_AMBER, 0);
		lv_obj_set_style_bg_opa(dots[p], LV_OPA_30, 0);
		lv_obj_clear_flag(dots[p], LV_OBJ_FLAG_SCROLLABLE);
	}
	update_dots();

	/* move-mode hint (hidden until carrying) */
	lbl_hint = lv_label_create(scr_home);
	lv_obj_align(lbl_hint, LV_ALIGN_BOTTOM_MID, 0, -2);
	lv_obj_set_style_text_color(lbl_hint, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_color(lbl_hint, KF_AMBER_HOT, 0);
	lv_obj_set_style_bg_opa(lbl_hint, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_hor(lbl_hint, 3, 0);
	lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN);

	/* focus the first occupied cell on page 0 */
	cur = 0;
	for(int i=0;i<NCELLS;i++) if(slot_app[i]>=0){ cur=i; break; }
	lv_group_focus_obj(cells[cur]);
	move_cursor(cur, 0);

	/* apply saved backlight levels (LCD + keyboard) */
	{ uint8_t v=(uint8_t)deskconf_get_int("bkl",5); reg_write(REG_BKL,&v,1);
	  uint8_t k=(uint8_t)deskconf_get_int("bk2",2); reg_write(REG_BK2,&k,1); }

	lv_screen_load(scr_home);
	lv_timer_create(idle_timer, 250, NULL);   /* idle screen-off / wake */
}

/* defer group deletion to the next tick: when launcher_show() is called from
   inside a list button's event, the indev keypad handler still holds the old
   group pointer for the rest of this dispatch. */
static void del_group_cb(void *g){ lv_group_delete((lv_group_t*)g); }

void kf_back_to_launcher(void){ kf_sfx_play("back"); launcher_show(); }

void launcher_show(void){
	/* whatever app screen is active is about to be abandoned — capture it so we
	   can DELETE it (apps don't free their own screen on exit -> the big leak). */
	lv_obj_t *prev = lv_screen_active();

	s_cur_app = -1;                /* back on the desktop — idle may sleep again */
	kf_grab_input(0);
	if(carrying >= 0){ carrying = -1; render_page(); lv_obj_add_flag(lbl_hint, LV_OBJ_FLAG_HIDDEN); }
	/* a sub-app (the chooser) may have changed the wallpaper / dim choice */
	kf_wallpaper_apply(wp_img, deskconf_get("wallpaper",""), deskconf_get("fit","crop"));
	apply_dim();

	/* point the indev back at the home group, THEN free the app's group + screen. */
	lv_indev_set_group(indev_get(), grp_home);
	if(app_group){ lv_async_call(del_group_cb, app_group); app_group = NULL; }
	lv_group_focus_obj(cells[cur]);
	move_cursor(cur, 0);
	lv_screen_load(scr_home);
	/* ASYNC delete: launcher_show() is often called from inside an app button's
	   CLICKED callback (files/settings/chooser/power), so deleting `prev` now would
	   free the screen whose event is still dispatching. Defer to the next tick. */
	if(prev && prev != scr_home) lv_obj_delete_async(prev);   /* reclaim the app's whole screen tree */
}
