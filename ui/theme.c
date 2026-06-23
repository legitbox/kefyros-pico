// ui/theme.c — LunaTech amber CRT theme. Layered over the default theme (as a
// parent) so we inherit sane metrics/paddings and only override colour + corners.
// Rule: amber for everything; green-yellow (KF_ACTIVE) only for focus / active.
#include "theme.h"
#include "lvgl/src/themes/lv_theme_private.h"   /* lv_theme_t is opaque in the public API */

static lv_style_t s_scr;      /* screens: near-black bg, amber text */
static lv_style_t s_cont;     /* generic containers: recolour + square corners */
static lv_style_t s_btn;      /* buttons (incl. list items): amber on card */
static lv_style_t s_btn_act;  /* focused/pressed button -> green-yellow */
static lv_style_t s_list;     /* list body: deep bg, no border */
static lv_style_t s_bm_main;  /* button matrix body (calc) */
static lv_style_t s_bm_items; /* matrix cells: amber */
static lv_style_t s_bm_act;   /* selected/pressed cell -> green-yellow */
static lv_style_t s_ta;       /* text fields: flat (NO rounded border / focus ring) */
static lv_style_t s_ta_foc;   /* focused text field: green-yellow square border */

/* Instant (0 ms) transition — the default theme fades state changes over ~hundreds
 * of ms, which made the highlight "wave in" slowly (NOT an SPI-speed issue). Force
 * every animated property to snap. */
static lv_style_transition_dsc_t s_trans0;
static const lv_style_prop_t s_trans_props[] = {
	LV_STYLE_BG_OPA, LV_STYLE_BG_COLOR, LV_STYLE_TEXT_COLOR,
	LV_STYLE_BORDER_OPA, LV_STYLE_BORDER_WIDTH, LV_STYLE_BORDER_COLOR,
	LV_STYLE_OUTLINE_OPA, LV_STYLE_OUTLINE_WIDTH, LV_STYLE_OUTLINE_PAD,
	LV_STYLE_TRANSFORM_WIDTH, LV_STYLE_TRANSFORM_HEIGHT, 0
};

/* kill the default theme's focus OUTLINE (the rounded highlight border) on a style */
static void no_outline(lv_style_t *s){
	lv_style_set_outline_width(s, 0);
	lv_style_set_outline_opa(s, LV_OPA_TRANSP);
}

static void styles_init(void){
	lv_style_transition_dsc_init(&s_trans0, s_trans_props, lv_anim_path_linear, 0, 0, NULL);

	lv_style_init(&s_scr);
	lv_style_set_bg_color(&s_scr, KF_BG_DEEP);
	lv_style_set_bg_opa(&s_scr, LV_OPA_COVER);
	lv_style_set_text_color(&s_scr, KF_AMBER);
	lv_style_set_text_font(&s_scr, KF_FONT);
	lv_style_set_border_width(&s_scr, 0);
	lv_style_set_radius(&s_scr, 0);
	lv_style_set_pad_all(&s_scr, 0);

	lv_style_init(&s_cont);
	lv_style_set_bg_color(&s_cont, KF_BG);
	lv_style_set_border_color(&s_cont, KF_BORDER);
	lv_style_set_text_color(&s_cont, KF_AMBER);
	lv_style_set_radius(&s_cont, 0);

	lv_style_init(&s_btn);
	lv_style_set_bg_color(&s_btn, KF_CARD);
	lv_style_set_text_color(&s_btn, KF_AMBER);
	lv_style_set_border_color(&s_btn, KF_AMBER_DIM);
	lv_style_set_border_width(&s_btn, 1);
	lv_style_set_radius(&s_btn, 0);
	no_outline(&s_btn);
	lv_style_set_transition(&s_btn, &s_trans0);

	/* focused/active = INVERTED: green-yellow fill, near-black text. NO border,
	 * NO outline (the rounded highlight border the user wants gone), instant. */
	lv_style_init(&s_btn_act);
	lv_style_set_bg_color(&s_btn_act, KF_ACTIVE);
	lv_style_set_bg_opa(&s_btn_act, LV_OPA_COVER);
	lv_style_set_text_color(&s_btn_act, KF_BG_DEEP);
	lv_style_set_border_width(&s_btn_act, 0);
	no_outline(&s_btn_act);

	lv_style_init(&s_list);
	lv_style_set_bg_color(&s_list, KF_BG_DEEP);
	lv_style_set_border_width(&s_list, 0);
	lv_style_set_radius(&s_list, 0);
	lv_style_set_pad_all(&s_list, 2);

	lv_style_init(&s_bm_main);
	lv_style_set_bg_color(&s_bm_main, KF_BG_DEEP);
	lv_style_set_border_width(&s_bm_main, 0);
	lv_style_set_radius(&s_bm_main, 0);
	lv_style_set_pad_all(&s_bm_main, 4);
	lv_style_set_pad_gap(&s_bm_main, 4);
	no_outline(&s_bm_main);   /* kill the default theme's outline around the whole keypad */

	lv_style_init(&s_bm_items);
	lv_style_set_bg_color(&s_bm_items, KF_CARD);
	lv_style_set_text_color(&s_bm_items, KF_AMBER);
	lv_style_set_border_color(&s_bm_items, KF_AMBER_DIM);
	lv_style_set_border_width(&s_bm_items, 1);
	lv_style_set_radius(&s_bm_items, 0);
	no_outline(&s_bm_items);
	lv_style_set_transition(&s_bm_items, &s_trans0);

	lv_style_init(&s_bm_act);
	lv_style_set_bg_color(&s_bm_act, KF_ACTIVE);
	lv_style_set_bg_opa(&s_bm_act, LV_OPA_COVER);
	lv_style_set_text_color(&s_bm_act, KF_BG_DEEP);
	lv_style_set_border_width(&s_bm_act, 0);
	no_outline(&s_bm_act);

	/* text fields: the default theme gives textareas a rounded border + a rounded
	 * focus OUTLINE ring (drawn on FOCUS_KEY for keyboard nav). Override to a flat
	 * square card with NO radius and NO outline, in every state. */
	lv_style_init(&s_ta);
	lv_style_set_bg_color(&s_ta, KF_CARD);
	lv_style_set_bg_opa(&s_ta, LV_OPA_COVER);
	lv_style_set_text_color(&s_ta, KF_TEXT);
	lv_style_set_border_color(&s_ta, KF_BORDER_HI);
	lv_style_set_border_width(&s_ta, 1);
	lv_style_set_radius(&s_ta, 0);
	lv_style_set_pad_all(&s_ta, 4);
	no_outline(&s_ta);
	lv_style_set_transition(&s_ta, &s_trans0);

	/* focused field = a clear square green-yellow border (replaces the killed ring) */
	lv_style_init(&s_ta_foc);
	lv_style_set_border_color(&s_ta_foc, KF_ACTIVE);
	lv_style_set_border_width(&s_ta_foc, 2);
	lv_style_set_radius(&s_ta_foc, 0);
	no_outline(&s_ta_foc);
}

static void apply_cb(lv_theme_t *th, lv_obj_t *obj){
	(void)th;
	/* a screen = a plain object with no parent */
	if(lv_obj_get_parent(obj)==NULL && lv_obj_check_type(obj,&lv_obj_class)){
		lv_obj_add_style(obj,&s_scr,0); return;
	}
	if(lv_obj_check_type(obj,&lv_buttonmatrix_class)){
		lv_obj_add_style(obj,&s_bm_main,0);
		/* re-assert no-outline on the focused MAIN part so the default theme's
		   focus ring around the whole keypad never draws */
		lv_obj_add_style(obj,&s_bm_main, LV_PART_MAIN|LV_STATE_FOCUSED);
		lv_obj_add_style(obj,&s_bm_main, LV_PART_MAIN|LV_STATE_FOCUS_KEY);
		lv_obj_add_style(obj,&s_bm_items, LV_PART_ITEMS);
		lv_obj_add_style(obj,&s_bm_act,   LV_PART_ITEMS|LV_STATE_FOCUS_KEY);
		lv_obj_add_style(obj,&s_bm_act,   LV_PART_ITEMS|LV_STATE_FOCUSED);
		lv_obj_add_style(obj,&s_bm_act,   LV_PART_ITEMS|LV_STATE_PRESSED);
		return;
	}
	if(lv_obj_check_type(obj,&lv_button_class)){
		lv_obj_add_style(obj,&s_btn,0);
		lv_obj_add_style(obj,&s_btn_act, LV_STATE_FOCUS_KEY);  /* override default outline */
		lv_obj_add_style(obj,&s_btn_act, LV_STATE_FOCUSED);
		lv_obj_add_style(obj,&s_btn_act, LV_STATE_PRESSED);
		return;
	}
	if(lv_obj_check_type(obj,&lv_textarea_class)){
		lv_obj_add_style(obj,&s_ta,0);
		lv_obj_add_style(obj,&s_ta_foc, LV_PART_MAIN|LV_STATE_FOCUSED);
		lv_obj_add_style(obj,&s_ta_foc, LV_PART_MAIN|LV_STATE_FOCUS_KEY);
		return;
	}
	if(lv_obj_check_type(obj,&lv_list_class)){
		lv_obj_add_style(obj,&s_list,0); return;
	}
	if(lv_obj_check_type(obj,&lv_label_class)){
		return;   /* labels inherit amber text from the screen */
	}
	if(lv_obj_check_type(obj,&lv_obj_class)){
		lv_obj_add_style(obj,&s_cont,0); return;
	}
}

static lv_theme_t kf_theme;   /* our theme; parent = default */

void kf_theme_init(lv_display_t *disp){
	styles_init();
	lv_theme_t *base = lv_theme_default_init(disp, KF_AMBER, KF_ACTIVE,
	                                         true /*dark*/, KF_FONT);
	lv_memzero(&kf_theme, sizeof kf_theme);
	kf_theme.disp        = disp;
	kf_theme.color_primary   = KF_AMBER;
	kf_theme.color_secondary = KF_ACTIVE;
	kf_theme.font_small  = KF_FONT;
	kf_theme.font_normal = KF_FONT;
	kf_theme.font_large  = KF_FONT_BIG;
	lv_theme_set_parent(&kf_theme, base);
	lv_theme_set_apply_cb(&kf_theme, apply_cb);
	lv_display_set_theme(disp, &kf_theme);
}
