// apps/calc_table.c — table of values x -> f(x) in a scrollable list. Reuses the
// engine (binds x, evaluates the cloned AST). Up/Down/PgUp/PgDn scroll; ESC returns.
#include "../kefyros.h"
#include "calc.h"
#include "../ui/theme.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#define TROWS 160
static lv_obj_t *tscr, *tcont;
static cnode    *tfn;

void calc_table_open(const cnode *f, double start, double step){
	if(!f) return;
	cn_free(tfn); tfn = cn_clone(f);
	if(step==0) step = 1;

	tscr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(tscr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(tscr, 4, 0);
	kf_inset_top(tscr);                 /* clear the persistent OS top bar */
	lv_obj_set_flex_flow(tscr, LV_FLEX_FLOW_COLUMN);

	lv_obj_t *hdr = lv_label_create(tscr);
	lv_obj_set_style_text_font(hdr, KF_FONT, 0);
	lv_obj_set_style_text_color(hdr, KF_AMBER_BR, 0);
	lv_label_set_text(hdr, "    x          f(x)");

	tcont = lv_obj_create(tscr);
	lv_obj_set_width(tcont, LCD_W-8);
	lv_obj_set_flex_grow(tcont, 1);
	lv_obj_set_flex_flow(tcont, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_opa(tcont, 0, 0);
	lv_obj_set_style_border_width(tcont, 0, 0);
	lv_obj_set_style_pad_all(tcont, 2, 0);
	lv_obj_set_style_pad_row(tcont, 1, 0);

	for(int i=0;i<TROWS;i++){
		double x = start + i*step; calc_set_var("x", x);
		int ok=1; double y = calc_eval(tfn, &ok);
		char xb[28], yb[28], row[64]; calc_fmt(x,xb,sizeof xb); calc_fmt(y,yb,sizeof yb);
		snprintf(row,sizeof row,"%-12s %s", xb, ok?yb:"undef");
		lv_obj_t *l = lv_label_create(tcont);
		lv_obj_set_style_text_font(l, KF_FONT, 0);
		lv_obj_set_style_text_color(l, (i&1)?KF_TEXT:KF_TEXT_DIM, 0);
		lv_label_set_text(l, row);
	}

	lv_obj_t *leg = lv_label_create(tscr);
	lv_obj_set_style_text_font(leg, KF_FONT, 0);
	lv_obj_set_style_text_color(leg, KF_TEXT_MUTED, 0);
	lv_label_set_text(leg, "Up/Dn scroll   ESC back");

	calc_set_mode(CMODE_TABLE);
	lv_screen_load(tscr);
}

void calc_table_key(uint8_t key, int mods){
	(void)mods;
	switch(key){
	case DK_ESC: case DK_F1+4: case DK_BREAK:
		cn_free(tfn); tfn=NULL; lv_obj_delete(tscr); tscr=NULL; calc_show_worksheet(); return;
	case DK_UP:   lv_obj_scroll_by(tcont, 0,  28, LV_ANIM_OFF); break;
	case DK_DOWN: lv_obj_scroll_by(tcont, 0, -28, LV_ANIM_OFF); break;
	case DK_PGUP: lv_obj_scroll_by(tcont, 0,  140, LV_ANIM_OFF); break;
	case DK_PGDN: lv_obj_scroll_by(tcont, 0, -140, LV_ANIM_OFF); break;
	default: break;
	}
}
