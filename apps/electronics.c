// apps/electronics.c — Kefyros electronics toolkit. A TI-style HOME menu of bench
// tools, each its own screen (mirrors apps/calc.c's multi-screen pattern):
//   Resistor colour (combination-locker colour scrollwheels) · SMD code decoder ·
//   LED series resistor · Ohm's law / power · Voltage divider · Capacitor / RC ·
//   Inductor / LC resonance · 555 timer · PCB trace (IPC-2221) · Battery life.
// Form screens are labelled text fields: ENTER computes, arrows move fields, F2 (where
// shown) flips a sub-mode, ESC backs to the menu. Numeric fields accept SI suffixes
// (p n u m k M G), e.g. "4.7k", "100n", "10m". ASCII only (font is 0x20-0x7F):
// units print as "ohm/kohm/...", "+/-N%".
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

enum { S_HOME=0, S_RES, S_SMD, S_LED, S_OHM, S_VDIV, S_CAP, S_LC, S_555, S_PCB, S_BATT, S_COUNT };

static int       active = 0;
static int       screen = S_HOME;
static lv_obj_t *scr = NULL;                 /* the live screen object */
static void    (*cur_compute)(void) = NULL;  /* form ENTER action */
static void    (*cur_f2)(void) = NULL;       /* form F2 action (optional) */

static void show_screen(int s);

/* ===================== numeric helpers ===================== */
/* parse a number with an optional SI suffix (p n u m k M G). M=mega, m=milli. */
static double eparse(const char *s, int *ok){
	while(*s==' ') s++;
	char *end; double v = strtod(s, &end);
	if(end==s){ *ok=0; return 0; }
	while(*end==' ') end++;
	double mul = 1;
	switch(*end){
	case 'p': mul=1e-12; break;
	case 'n': mul=1e-9;  break;
	case 'u': case 'U': mul=1e-6; break;
	case 'm': mul=1e-3;  break;
	case 'k': case 'K': mul=1e3;  break;
	case 'M': mul=1e6;   break;
	case 'G': mul=1e9;   break;
	default: break;
	}
	*ok=1; return v*mul;
}
/* engineering format: pick an SI prefix so the mantissa reads cleanly */
static void eng_fmt(double v, const char *unit, char *out, int n){
	if(!(v>0) && !(v<0)){ snprintf(out,n,"0 %s",unit); return; }
	int neg = v<0; double a = neg?-v:v;
	static const char *pre[] = {"p","n","u","m","","k","M","G","T"};
	static const double sc[]  = {1e-12,1e-9,1e-6,1e-3,1,1e3,1e6,1e9,1e12};
	int idx=4;
	for(int i=0;i<9;i++) if(a>=sc[i]) idx=i;
	snprintf(out, n, "%s%.4g %s%s", neg?"-":"", a/sc[idx], pre[idx], unit);
}
/* nearest E12 preferred value */
static double nearest_e12(double r){
	static const double e12[12] = {10,12,15,18,22,27,33,39,47,56,68,82};
	if(r<=0) return 0;
	double dec = pow(10, floor(log10(r)));
	double best=0, berr=1e300;
	for(int d=-1; d<=1; d++){
		double base = dec*pow(10,d);
		for(int i=0;i<12;i++){
			double cand = e12[i]*base/10.0;
			double err = fabs(cand-r);
			if(err<berr){ berr=err; best=cand; }
		}
	}
	return best;
}

/* ===================== generic form framework ===================== */
#define MAXFIELDS 6
static lv_obj_t  *fields[MAXFIELDS], *flabels[MAXFIELDS];
static int        nfields;
static lv_group_t*fgrp;
static lv_obj_t  *fmsg;

static const char *fstr(int i){ return lv_textarea_get_text(fields[i]); }
static int    fempty(int i){ return fstr(i)[0]==0; }
static double fnum(int i){ int ok; double v=eparse(fstr(i),&ok); return ok?v:0; }
static void   form_msg(const char *s){ if(fmsg) lv_label_set_text(fmsg, s); }

static lv_obj_t *form_begin(const char *title){
	nfields=0; fmsg=NULL; fgrp=kf_use_group();
	lv_obj_t *s = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s, 6, 0);
	kf_inset_top(s);                    /* clear the persistent OS top bar */
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s, 3, 0);
	lv_obj_remove_flag(s, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *t = lv_label_create(s);
	lv_obj_set_style_text_font(t, KF_FONT_BIG, 0);
	lv_obj_set_style_text_color(t, KF_AMBER_BR, 0);
	lv_label_set_text(t, title);
	return s;
}
static lv_obj_t *add_field(lv_obj_t *par, const char *label, const char *init){
	lv_obj_t *l = lv_label_create(par);
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, KF_AMBER, 0);
	lv_label_set_text(l, label);
	lv_obj_t *t = lv_textarea_create(par);
	lv_textarea_set_one_line(t, true);
	lv_obj_set_width(t, LCD_W-14);
	lv_obj_set_style_text_font(t, KF_FONT, 0);
	if(init && *init) lv_textarea_set_text(t, init);
	lv_group_add_obj(fgrp, t);
	if(nfields<MAXFIELDS){ flabels[nfields]=l; fields[nfields]=t; nfields++; }
	return t;
}
static void form_finish(lv_obj_t *s, const char *hint){
	fmsg = lv_label_create(s);
	lv_obj_set_width(fmsg, LCD_W-12);
	lv_obj_set_flex_grow(fmsg, 1);
	lv_label_set_long_mode(fmsg, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(fmsg, KF_FONT_BIG, 0);
	lv_obj_set_style_text_color(fmsg, KF_AMBER_BR, 0);
	lv_label_set_text(fmsg, "");
	lv_obj_t *h = lv_label_create(s);
	lv_obj_set_width(h, LCD_W-12);
	lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(h, KF_FONT, 0);
	lv_obj_set_style_text_color(h, KF_TEXT_MUTED, 0);
	lv_label_set_text(h, hint);
	if(nfields) lv_group_focus_obj(fields[0]);
}
static int form_edit_key(uint8_t key, int mods){
	lv_obj_t *t = lv_group_get_focused(fgrp);
	if(key==DK_UP)        { lv_group_focus_prev(fgrp); return 1; }
	if(key==DK_DOWN)      { lv_group_focus_next(fgrp); return 1; }
	if(key==DK_TAB)       { lv_group_focus_next(fgrp); return 1; }
	if(key==DK_LEFT)      { if(t) lv_textarea_cursor_left(t); return 1; }
	if(key==DK_RIGHT)     { if(t) lv_textarea_cursor_right(t); return 1; }
	if(key==DK_HOME)      { if(t) lv_textarea_set_cursor_pos(t,0); return 1; }
	if(key==DK_END)       { if(t) lv_textarea_set_cursor_pos(t,LV_TEXTAREA_CURSOR_LAST); return 1; }
	if(key==DK_BACKSPACE) { if(t) lv_textarea_delete_char(t); return 1; }
	if(key==DK_DEL)       { if(t) lv_textarea_delete_char_forward(t); return 1; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)){ if(t) lv_textarea_add_char(t,key); return 1; }
	return 0;
}
static void form_key(uint8_t k, int m){
	if(k==DK_ESC || k==DK_BREAK){ show_screen(S_HOME); return; }
	if(k==DK_ENTER || k==DK_F1){ if(cur_compute) cur_compute(); return; }
	if(k==DK_F1+1){ if(cur_f2) cur_f2(); return; }     /* F2 sub-mode */
	form_edit_key(k, m);
}

/* ===================================================================== */
/* Resistor colour code — combination-locker colour scrollwheels          */
/* ===================================================================== */
static const struct { const char *name; uint32_t rgb; } DIG[10] = {
	{"black",0x111111},{"brown",0x6b4423},{"red",0xc0201f},{"orange",0xe07a1f},
	{"yellow",0xe6c419},{"green",0x2a9d3a},{"blue",0x2257c4},{"violet",0x8a3ec8},
	{"grey",0x9a9a9a},{"white",0xf0f0f0}
};
static const struct { const char *name; uint32_t rgb; double mul; } MUL[] = {
	{"black",0x111111,1e0},{"brown",0x6b4423,1e1},{"red",0xc0201f,1e2},{"orange",0xe07a1f,1e3},
	{"yellow",0xe6c419,1e4},{"green",0x2a9d3a,1e5},{"blue",0x2257c4,1e6},{"violet",0x8a3ec8,1e7},
	{"grey",0x9a9a9a,1e8},{"white",0xf0f0f0,1e9},{"gold",0xc8a020,0.1},{"silver",0xc0c0c0,0.01}
};
#define NMUL (int)(sizeof MUL / sizeof MUL[0])
static const struct { const char *name; uint32_t rgb; double tol; } TOL[] = {
	{"brown",0x6b4423,1.0},{"red",0xc0201f,2.0},{"green",0x2a9d3a,0.5},{"blue",0x2257c4,0.25},
	{"violet",0x8a3ec8,0.1},{"grey",0x9a9a9a,0.05},{"gold",0xc8a020,5.0},{"silver",0xc0c0c0,10.0},
	{"none",0x303030,20.0}
};
#define NTOL (int)(sizeof TOL / sizeof TOL[0])
enum { BT_DIG, BT_MUL, BT_TOL };
#define MAXB 5

static int        r_nbands = 4, r_fb = 0;
static int        r_btype[MAXB], r_bcount[MAXB];
static lv_obj_t  *r_roll[MAXB], *r_title, *r_val, *r_tol, *r_rowc;
static char       dig_opts[160], mul_opts[200], tol_opts[140];

static void r_build_opts(void){
	dig_opts[0]=mul_opts[0]=tol_opts[0]=0;
	for(int i=0;i<10;i++){   strcat(dig_opts,DIG[i].name); if(i<9)      strcat(dig_opts,"\n"); }
	for(int i=0;i<NMUL;i++){ strcat(mul_opts,MUL[i].name); if(i<NMUL-1) strcat(mul_opts,"\n"); }
	for(int i=0;i<NTOL;i++){ strcat(tol_opts,TOL[i].name); if(i<NTOL-1) strcat(tol_opts,"\n"); }
}
static uint32_t r_rgb(int i){
	int sel=(int)lv_roller_get_selected(r_roll[i]);
	if(r_btype[i]==BT_DIG) return DIG[sel].rgb;
	if(r_btype[i]==BT_MUL) return MUL[sel].rgb;
	return TOL[sel].rgb;
}
static void r_style(int i){
	uint32_t rgb=r_rgb(i); int r=(rgb>>16)&0xff,g=(rgb>>8)&0xff,b=rgb&0xff;
	int lum=(r*30+g*59+b*11)/100;
	lv_color_t tc = lum>140?lv_color_black():lv_color_white(), bc=lv_color_hex(rgb);
	lv_obj_set_style_bg_color(r_roll[i], bc, 0);
	lv_obj_set_style_bg_opa(r_roll[i], LV_OPA_COVER, 0);
	lv_obj_set_style_text_color(r_roll[i], tc, 0);
	lv_obj_set_style_bg_color(r_roll[i], bc, LV_PART_SELECTED);
	lv_obj_set_style_bg_opa(r_roll[i], LV_OPA_COVER, LV_PART_SELECTED);
	lv_obj_set_style_text_color(r_roll[i], tc, LV_PART_SELECTED);
	int foc=(i==r_fb);
	lv_obj_set_style_border_width(r_roll[i], foc?3:1, 0);
	lv_obj_set_style_border_color(r_roll[i], foc?KF_AMBER_HOT:KF_BORDER, 0);
}
static void r_ohms_fmt(double v, char *out, int n){
	const char *u="ohm"; double s=v;
	if(v>=1e9){ u="Gohm"; s=v/1e9; } else if(v>=1e6){ u="Mohm"; s=v/1e6; }
	else if(v>=1e3){ u="kohm"; s=v/1e3; }
	snprintf(out,n,"%g %s",s,u);
}
static void r_recompute(void){
	long digits=0; double mul=1, tol=20;
	for(int i=0;i<r_nbands;i++){
		int sel=(int)lv_roller_get_selected(r_roll[i]);
		if(r_btype[i]==BT_DIG) digits=digits*10+sel;
		else if(r_btype[i]==BT_MUL) mul=MUL[sel].mul;
		else tol=TOL[sel].tol;
	}
	double ohms=(double)digits*mul; char vb[48]; r_ohms_fmt(ohms,vb,sizeof vb);
	lv_label_set_text(r_val, vb);
	if(tol>=20) lv_label_set_text(r_tol, "+/-20%");
	else { char lo[32],hi[32]; r_ohms_fmt(ohms*(1-tol/100),lo,sizeof lo); r_ohms_fmt(ohms*(1+tol/100),hi,sizeof hi);
		lv_label_set_text_fmt(r_tol, "+/-%g%%   [%s .. %s]", tol, lo, hi); }
}
static void r_set_btypes(void){
	if(r_nbands==4){ r_btype[0]=BT_DIG;r_btype[1]=BT_DIG;r_btype[2]=BT_MUL;r_btype[3]=BT_TOL; }
	else { r_btype[0]=BT_DIG;r_btype[1]=BT_DIG;r_btype[2]=BT_DIG;r_btype[3]=BT_MUL;r_btype[4]=BT_TOL; }
}
static void r_rebuild(void){
	lv_obj_clean(r_rowc); r_set_btypes();
	int w=(LCD_W-14)/r_nbands - 2;
	for(int i=0;i<r_nbands;i++){
		lv_obj_t *r=lv_roller_create(r_rowc); const char*o; int c;
		if(r_btype[i]==BT_DIG){o=dig_opts;c=10;} else if(r_btype[i]==BT_MUL){o=mul_opts;c=NMUL;} else {o=tol_opts;c=NTOL;}
		lv_roller_set_options(r,o,LV_ROLLER_MODE_NORMAL);
		lv_roller_set_visible_row_count(r,3);
		lv_obj_set_width(r,w);
		lv_obj_set_style_text_font(r,KF_FONT,0);
		lv_obj_set_style_radius(r,0,0);
		lv_obj_set_style_pad_hor(r,1,0);
		r_roll[i]=r; r_bcount[i]=c;
	}
	if(r_nbands==4){ lv_roller_set_selected(r_roll[0],4,LV_ANIM_OFF); lv_roller_set_selected(r_roll[1],7,LV_ANIM_OFF);
		lv_roller_set_selected(r_roll[2],2,LV_ANIM_OFF); lv_roller_set_selected(r_roll[3],6,LV_ANIM_OFF); }
	else { lv_roller_set_selected(r_roll[0],4,LV_ANIM_OFF); lv_roller_set_selected(r_roll[1],7,LV_ANIM_OFF);
		lv_roller_set_selected(r_roll[2],0,LV_ANIM_OFF); lv_roller_set_selected(r_roll[3],1,LV_ANIM_OFF); lv_roller_set_selected(r_roll[4],0,LV_ANIM_OFF); }
	if(r_fb>=r_nbands) r_fb=r_nbands-1;
	for(int i=0;i<r_nbands;i++) r_style(i);
	r_recompute();
}
static void r_set_title(void){ lv_label_set_text_fmt(r_title, "RESISTOR   %d-band", r_nbands); }
static lv_obj_t *build_res(void){
	cur_compute=NULL; cur_f2=NULL;
	r_build_opts(); r_nbands=4; r_fb=0;
	lv_obj_t *s=lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s,KF_BG_DEEP,0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
	lv_obj_set_style_pad_all(s,6,0); kf_inset_top(s); lv_obj_set_flex_flow(s,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s,4,0); lv_obj_remove_flag(s,LV_OBJ_FLAG_SCROLLABLE);
	r_title=lv_label_create(s); lv_obj_set_style_text_font(r_title,KF_FONT,0); lv_obj_set_style_text_color(r_title,KF_AMBER,0); r_set_title();
	r_val=lv_label_create(s); lv_obj_set_width(r_val,LCD_W-12); lv_obj_set_style_text_font(r_val,KF_FONT_BIG,0); lv_obj_set_style_text_color(r_val,KF_AMBER_BR,0);
	r_tol=lv_label_create(s); lv_obj_set_width(r_tol,LCD_W-12); lv_label_set_long_mode(r_tol,LV_LABEL_LONG_WRAP); lv_obj_set_style_text_font(r_tol,KF_FONT,0); lv_obj_set_style_text_color(r_tol,KF_TEXT_DIM,0);
	r_rowc=lv_obj_create(s); lv_obj_set_width(r_rowc,LCD_W-12); lv_obj_set_flex_grow(r_rowc,1);
	lv_obj_set_flex_flow(r_rowc,LV_FLEX_FLOW_ROW);
	lv_obj_set_flex_align(r_rowc,LV_FLEX_ALIGN_SPACE_BETWEEN,LV_FLEX_ALIGN_CENTER,LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_bg_opa(r_rowc,0,0); lv_obj_set_style_border_width(r_rowc,0,0); lv_obj_set_style_pad_all(r_rowc,0,0);
	lv_obj_remove_flag(r_rowc,LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *h=lv_label_create(s); lv_obj_set_width(h,LCD_W-12); lv_label_set_long_mode(h,LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(h,KF_FONT,0); lv_obj_set_style_text_color(h,KF_TEXT_MUTED,0);
	lv_label_set_text(h,"L/R band  Up/Dn colour  F1 4/5-band  ESC menu");
	r_rebuild();
	return s;
}
static void res_key(uint8_t key, int mods){
	(void)mods;
	if(key==DK_ESC || key==DK_BREAK){ show_screen(S_HOME); return; }
	if(key==DK_F1){ r_nbands=(r_nbands==4)?5:4; r_rebuild(); r_set_title(); return; }
	if(key==DK_LEFT){  if(r_fb>0){ int o=r_fb; r_fb--; r_style(o); r_style(r_fb);} return; }
	if(key==DK_RIGHT){ if(r_fb<r_nbands-1){ int o=r_fb; r_fb++; r_style(o); r_style(r_fb);} return; }
	if(key==DK_UP){ int s=(int)lv_roller_get_selected(r_roll[r_fb]); if(s>0){ lv_roller_set_selected(r_roll[r_fb],s-1,LV_ANIM_ON); r_style(r_fb); r_recompute(); } return; }
	if(key==DK_DOWN){ int s=(int)lv_roller_get_selected(r_roll[r_fb]); if(s<r_bcount[r_fb]-1){ lv_roller_set_selected(r_roll[r_fb],s+1,LV_ANIM_ON); r_style(r_fb); r_recompute(); } return; }
}

/* ===================================================================== */
/* SMD code decoder (3/4-digit, R-notation, EIA-96)                       */
/* ===================================================================== */
static const int EIA96[96] = {
	100,102,105,107,110,113,115,118,121,124, 127,130,133,137,140,143,147,150,154,158,
	162,165,169,174,178,182,187,191,196,200, 205,210,215,221,226,232,237,243,249,255,
	261,267,274,280,287,294,301,309,316,324, 332,340,348,357,365,374,383,392,402,412,
	422,432,442,453,464,475,487,499,511,523, 536,549,562,576,590,604,619,634,649,665,
	681,698,715,732,750,768,787,806,825,845, 866,887,909,931,953,976
};
static double eia96_mult(char c){
	switch(c){
	case 'Z': return 0.001; case 'Y': case 'R': return 0.01; case 'X': case 'S': return 0.1;
	case 'A': return 1; case 'B': case 'H': return 10; case 'C': return 100;
	case 'D': return 1000; case 'E': return 10000; case 'F': return 100000;
	default: return -1;
	}
}
static void c_smd(void){
	const char *code = fstr(0);
	if(!*code){ form_msg("enter an SMD code (e.g. 472, 4R7, 01C)"); return; }
	char b[16]; strncpy(b,code,15); b[15]=0; int len=(int)strlen(b);
	double r=-1; int hasR=0;
	for(int i=0;i<len;i++) if(b[i]=='r'||b[i]=='R') hasR=1;
	if(hasR){
		char t[16]; int j=0; for(int i=0;i<len&&j<15;i++) t[j++]=(b[i]=='r'||b[i]=='R')?'.':b[i]; t[j]=0;
		r=strtod(t,NULL);
	} else if(len==3 && isdigit((unsigned char)b[0]) && isdigit((unsigned char)b[1]) && isalpha((unsigned char)b[2])){
		int code96=(b[0]-'0')*10+(b[1]-'0'); double m=eia96_mult((char)toupper((unsigned char)b[2]));
		if(code96>=1 && code96<=96 && m>0) r=EIA96[code96-1]*m;
	} else {
		int alldig=1; for(int i=0;i<len;i++) if(!isdigit((unsigned char)b[i])) alldig=0;
		if(alldig && len==3){ int d=(b[0]-'0')*10+(b[1]-'0'); r=d*pow(10,b[2]-'0'); }
		else if(alldig && len==4){ int d=(b[0]-'0')*100+(b[1]-'0')*10+(b[2]-'0'); r=d*pow(10,b[3]-'0'); }
	}
	if(r<0){ form_msg("unrecognised code"); return; }
	char rb[48]; eng_fmt(r,"ohm",rb,sizeof rb); char out[200];
	if(len==3 && isdigit((unsigned char)b[0]) && isdigit((unsigned char)b[1]) && isdigit((unsigned char)b[2])){
		double pf=((b[0]-'0')*10+(b[1]-'0'))*pow(10,b[2]-'0'); char cb[48]; eng_fmt(pf*1e-12,"F",cb,sizeof cb);
		snprintf(out,sizeof out,"as R: %s\nas C: %s  (%g pF)", rb, cb, pf);
	} else snprintf(out,sizeof out,"R = %s", rb);
	form_msg(out);
}
static lv_obj_t *build_smd(void){
	lv_obj_t *s=form_begin("SMD Code");
	add_field(s,"Code:","");
	form_finish(s,"3-digit / 4-digit / R-note / EIA-96   ENTER=decode  ESC=menu");
	cur_compute=c_smd; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* LED series resistor                                                    */
/* ===================================================================== */
static void c_led(void){
	if(fempty(0)||fempty(1)||fempty(2)){ form_msg("fill Vsupply, Vf, current"); return; }
	double vs=fnum(0), vf=fnum(1), ma=fnum(2), n=fempty(3)?1:fnum(3);
	if(n<1) n=1;
	double vft=vf*n, i=ma/1000.0;
	if(i<=0){ form_msg("current must be > 0"); return; }
	if(vs<=vft){ form_msg("Vsupply must exceed total Vf"); return; }
	double R=(vs-vft)/i, P=(vs-vft)*i, e=nearest_e12(R);
	char rb[40],eb[40],pb[40]; eng_fmt(R,"ohm",rb,sizeof rb); eng_fmt(e,"ohm",eb,sizeof eb); eng_fmt(P,"W",pb,sizeof pb);
	char out[220]; snprintf(out,sizeof out,"R = %s\nE12: %s\nR dissipates %s\n(Vf total %gV)", rb, eb, pb, vft);
	form_msg(out);
}
static lv_obj_t *build_led(void){
	lv_obj_t *s=form_begin("LED Resistor");
	add_field(s,"Vsupply (V):","5");
	add_field(s,"Vf LED (V):","2");
	add_field(s,"Current (mA):","20");
	add_field(s,"LEDs in series:","1");
	form_finish(s,"ENTER=calc  Up/Dn=field  ESC=menu");
	cur_compute=c_led; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* Ohm's law / power wheel                                                */
/* ===================================================================== */
static void c_ohm(void){
	int hv=!fempty(0),hi=!fempty(1),hr=!fempty(2),hp=!fempty(3);
	double V=fnum(0),I=fnum(1),R=fnum(2),P=fnum(3);
	if(hv+hi+hr+hp < 2){ form_msg("enter any two of V, I, R, P"); return; }
	if(hv&&hi){ R=(I!=0)?V/I:0; P=V*I; }
	else if(hv&&hr){ I=(R!=0)?V/R:0; P=(R!=0)?V*V/R:0; }
	else if(hv&&hp){ I=(V!=0)?P/V:0; R=(P!=0)?V*V/P:0; }
	else if(hi&&hr){ V=I*R; P=I*I*R; }
	else if(hi&&hp){ V=(I!=0)?P/I:0; R=(I!=0)?P/(I*I):0; }
	else { V=sqrt(P*R); I=(R!=0)?sqrt(P/R):0; }   /* R & P */
	char vb[40],ib[40],rb[40],pb[40];
	eng_fmt(V,"V",vb,sizeof vb); eng_fmt(I,"A",ib,sizeof ib); eng_fmt(R,"ohm",rb,sizeof rb); eng_fmt(P,"W",pb,sizeof pb);
	char out[220]; snprintf(out,sizeof out,"V = %s\nI = %s\nR = %s\nP = %s", vb,ib,rb,pb);
	form_msg(out);
}
static lv_obj_t *build_ohm(void){
	lv_obj_t *s=form_begin("Ohm's Law");
	add_field(s,"V (volts):","");
	add_field(s,"I (amps):","");
	add_field(s,"R (ohm):","");
	add_field(s,"P (watts):","");
	form_finish(s,"fill any 2  ENTER=solve  ESC=menu");
	cur_compute=c_ohm; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* Voltage divider (forward + reverse-solve R2)                           */
/* ===================================================================== */
static void c_vdiv(void){
	if(fempty(0)||fempty(1)){ form_msg("enter Vin and R1"); return; }
	double vin=fnum(0), r1=fnum(1);
	if(!fempty(3) && fempty(2)){                 /* reverse: solve R2 for target Vout */
		double vout=fnum(3);
		if(vin<=vout || vout<=0){ form_msg("need 0 < Vout < Vin"); return; }
		double r2=r1*vout/(vin-vout); char rb[40]; eng_fmt(r2,"ohm",rb,sizeof rb);
		char out[160]; snprintf(out,sizeof out,"R2 = %s\n(gives Vout = %gV)", rb, vout); form_msg(out); return;
	}
	if(fempty(2)){ form_msg("enter R2, or a target Vout"); return; }
	double r2=fnum(2);
	if(r1+r2<=0){ form_msg("R1+R2 must be > 0"); return; }
	double vout=vin*r2/(r1+r2), i=vin/(r1+r2);
	char vb[40],ib[40]; eng_fmt(vout,"V",vb,sizeof vb); eng_fmt(i,"A",ib,sizeof ib);
	char out[200]; snprintf(out,sizeof out,"Vout = %s\nratio = %.4g\ndivider I = %s", vb, r2/(r1+r2), ib);
	form_msg(out);
}
static lv_obj_t *build_vdiv(void){
	lv_obj_t *s=form_begin("Voltage Divider");
	add_field(s,"Vin (V):","");
	add_field(s,"R1 (ohm):","");
	add_field(s,"R2 (ohm):","");
	add_field(s,"Vout target (opt):","");
	form_finish(s,"R2 blank + Vout = solve R2   ENTER=calc  ESC=menu");
	cur_compute=c_vdiv; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* Capacitor code + RC time constant                                      */
/* ===================================================================== */
static void c_cap(void){
	const char *code=fstr(0);
	if(!*code){ form_msg("enter a cap code (e.g. 104) or value (100n)"); return; }
	int len=(int)strlen(code), alldig=len>0;
	for(int i=0;i<len;i++) if(!isdigit((unsigned char)code[i])) alldig=0;
	double C=-1;
	if(alldig && len==3){ double pf=((code[0]-'0')*10+(code[1]-'0'))*pow(10,code[2]-'0'); C=pf*1e-12; }
	else if(alldig && len<=2){ C=strtod(code,NULL)*1e-12; }     /* raw pF */
	else { int ok; double v=eparse(code,&ok); if(ok) C=v; }     /* e.g. 100n, 0.1u */
	if(C<0){ form_msg("unrecognised cap code"); return; }
	char cb[48]; eng_fmt(C,"F",cb,sizeof cb); char out[220];
	if(!fempty(1)){
		double R=fnum(1), tau=R*C, fc=(R>0&&C>0)?1.0/(2*M_PI*R*C):0;
		char tb[40],fb[40]; eng_fmt(tau,"s",tb,sizeof tb); eng_fmt(fc,"Hz",fb,sizeof fb);
		snprintf(out,sizeof out,"C = %s\nRC tau = %s\nf -3dB = %s", cb, tb, fb);
	} else snprintf(out,sizeof out,"C = %s", cb);
	form_msg(out);
}
static lv_obj_t *build_cap(void){
	lv_obj_t *s=form_begin("Capacitor / RC");
	add_field(s,"Cap code/value:","");
	add_field(s,"R for RC (opt):","");
	form_finish(s,"104 -> 100nF; add R for tau + cutoff   ENTER  ESC=menu");
	cur_compute=c_cap; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* Inductor / LC resonance                                                */
/* ===================================================================== */
static void c_lc(void){
	int hl=!fempty(0),hc=!fempty(1),hf=!fempty(2);
	double L=fnum(0),C=fnum(1),f=fnum(2); char out[200];
	if(hl&&hc && L>0&&C>0){ double fr=1.0/(2*M_PI*sqrt(L*C)), x=2*M_PI*fr*L;
		char fb[40],xb[40]; eng_fmt(fr,"Hz",fb,sizeof fb); eng_fmt(x,"ohm",xb,sizeof xb);
		snprintf(out,sizeof out,"f0 = %s\nX (at f0) = %s", fb, xb); }
	else if(hf&&hl && f>0&&L>0){ double c=1.0/(pow(2*M_PI*f,2)*L); char cb[40]; eng_fmt(c,"F",cb,sizeof cb);
		snprintf(out,sizeof out,"C = %s\n(for f0 = %gHz)", cb, f); }
	else if(hf&&hc && f>0&&C>0){ double l=1.0/(pow(2*M_PI*f,2)*C); char lb[40]; eng_fmt(l,"H",lb,sizeof lb);
		snprintf(out,sizeof out,"L = %s\n(for f0 = %gHz)", lb, f); }
	else { form_msg("enter L+C, or f+L, or f+C"); return; }
	form_msg(out);
}
static lv_obj_t *build_lc(void){
	lv_obj_t *s=form_begin("Inductor / LC");
	add_field(s,"L (henry):","");
	add_field(s,"C (farad):","");
	add_field(s,"f0 (Hz, opt):","");
	form_finish(s,"L+C -> f0; or f0 + one -> other   ENTER  ESC=menu");
	cur_compute=c_lc; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* 555 timer (astable / monostable, F2 toggles)                           */
/* ===================================================================== */
static int e555_mono = 0;
static void c_555(void){
	if(!e555_mono){
		if(fempty(0)||fempty(1)||fempty(2)){ form_msg("enter R1, R2, C"); return; }
		double r1=fnum(0),r2=fnum(1),c=fnum(2);
		double den=(r1+2*r2)*c; if(den<=0){ form_msg("bad values"); return; }
		double f=1.44/den, th=0.693*(r1+r2)*c, tl=0.693*r2*c, duty=(r1+r2)/(r1+2*r2)*100;
		char fb[40],hb[40],lb[40]; eng_fmt(f,"Hz",fb,sizeof fb); eng_fmt(th,"s",hb,sizeof hb); eng_fmt(tl,"s",lb,sizeof lb);
		char out[220]; snprintf(out,sizeof out,"f = %s\nduty = %.3g%%\nt_high = %s\nt_low = %s", fb, duty, hb, lb);
		form_msg(out);
	} else {
		if(fempty(0)||fempty(1)){ form_msg("enter R, C"); return; }
		double r=fnum(0),c=fnum(1), t=1.1*r*c; char tb[40]; eng_fmt(t,"s",tb,sizeof tb);
		char out[120]; snprintf(out,sizeof out,"pulse t = %s\n(t = 1.1 R C)", tb); form_msg(out);
	}
}
static void f2_555(void){ e555_mono=!e555_mono; show_screen(S_555); }
static lv_obj_t *build_555(void){
	lv_obj_t *s=form_begin(e555_mono ? "555  Monostable" : "555  Astable");
	if(!e555_mono){ add_field(s,"R1 (ohm):",""); add_field(s,"R2 (ohm):",""); add_field(s,"C (farad):",""); }
	else { add_field(s,"R (ohm):",""); add_field(s,"C (farad):",""); }
	form_finish(s,"ENTER=calc  F2=astable/mono  ESC=menu");
	cur_compute=c_555; cur_f2=f2_555; return s;
}

/* ===================================================================== */
/* PCB trace width <-> current (IPC-2221)                                 */
/* ===================================================================== */
static int pcb_internal = 0;
static void c_pcb(void){
	if(fempty(0)){ form_msg("enter current (A)"); return; }
	double I=fnum(0), dT=fempty(1)?10:fnum(1), oz=fempty(2)?1:fnum(2);
	if(I<=0||dT<=0||oz<=0){ form_msg("values must be > 0"); return; }
	double k=pcb_internal?0.024:0.048;
	double area=pow(I/(k*pow(dT,0.44)), 1.0/0.725);     /* mils^2 */
	double th_mils=1.378*oz, w_mils=area/th_mils, w_mm=w_mils*0.0254;
	char out[220]; snprintf(out,sizeof out,"width = %.4g mil\n      = %.4g mm\n(%s, dT=%gC, %goz Cu)",
		w_mils, w_mm, pcb_internal?"internal":"external", dT, oz);
	form_msg(out);
}
static void f2_pcb(void){ pcb_internal=!pcb_internal; if(!fempty(0)) c_pcb(); else form_msg(pcb_internal?"internal layer":"external layer"); }
static lv_obj_t *build_pcb(void){
	lv_obj_t *s=form_begin("PCB Trace (IPC-2221)");
	add_field(s,"Current (A):","");
	add_field(s,"Temp rise (C):","10");
	add_field(s,"Copper (oz):","1");
	form_finish(s,"ENTER=width  F2=ext/internal  ESC=menu");
	cur_compute=c_pcb; cur_f2=f2_pcb; return s;
}

/* ===================================================================== */
/* Battery life estimator                                                 */
/* ===================================================================== */
static void c_batt(void){
	if(fempty(0)||fempty(1)){ form_msg("enter capacity (mAh) and load (mA)"); return; }
	double mah=fnum(0), ma=fnum(1), der=fempty(2)?85:fnum(2);
	if(ma<=0){ form_msg("load must be > 0"); return; }
	double hours=(mah*der/100.0)/ma;
	int d=(int)(hours/24); double rem=hours-d*24; int h=(int)rem, m=(int)((rem-h)*60);
	char out[200]; snprintf(out,sizeof out,"runtime = %.4g h\n        = %dd %dh %dm\n(%g%% usable capacity)", hours, d, h, m, der);
	form_msg(out);
}
static lv_obj_t *build_batt(void){
	lv_obj_t *s=form_begin("Battery Life");
	add_field(s,"Capacity (mAh):","");
	add_field(s,"Load (mA):","");
	add_field(s,"Usable % (opt):","85");
	form_finish(s,"ENTER=estimate  Up/Dn=field  ESC=menu");
	cur_compute=c_batt; cur_f2=NULL; return s;
}

/* ===================================================================== */
/* Home menu                                                              */
/* ===================================================================== */
static const struct { const char *label; int scr; } MENU[] = {
	{ "Resistor colour", S_RES  },
	{ "SMD code",        S_SMD  },
	{ "LED resistor",    S_LED  },
	{ "Ohm's law",       S_OHM  },
	{ "Voltage divider", S_VDIV },
	{ "Capacitor / RC",  S_CAP  },
	{ "Inductor / LC",   S_LC   },
	{ "555 timer",       S_555  },
	{ "PCB trace",       S_PCB  },
	{ "Battery life",    S_BATT },
};
#define NMENU (int)(sizeof MENU / sizeof MENU[0])
static lv_obj_t *mrows[NMENU];
static int home_sel = 0;

static void home_hl(void){
	for(int i=0;i<NMENU;i++){
		int sel=(i==home_sel);
		lv_obj_set_style_bg_color(mrows[i], KF_ACTIVE, 0);
		lv_obj_set_style_bg_opa(mrows[i], sel?LV_OPA_COVER:LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(mrows[i], sel?KF_BG_DEEP:KF_AMBER, 0);
	}
}
static lv_obj_t *build_home(void){
	cur_compute=NULL; cur_f2=NULL; fmsg=NULL;
	lv_obj_t *s=lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s,KF_BG_DEEP,0); lv_obj_set_style_bg_opa(s,LV_OPA_COVER,0);
	lv_obj_set_style_pad_all(s,6,0); kf_inset_top(s); lv_obj_set_flex_flow(s,LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s,1,0); lv_obj_remove_flag(s,LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *t=lv_label_create(s); lv_obj_set_style_text_font(t,KF_FONT_BIG,0); lv_obj_set_style_text_color(t,KF_AMBER_BR,0);
	lv_label_set_text(t,"ELECTRONICS");
	for(int i=0;i<NMENU;i++){
		lv_obj_t *r=lv_label_create(s); lv_obj_set_width(r,LCD_W-12);
		lv_obj_set_style_text_font(r,KF_FONT,0); lv_obj_set_style_pad_ver(r,2,0); lv_obj_set_style_pad_left(r,4,0);
		lv_obj_set_style_radius(r,0,0); lv_label_set_text(r,MENU[i].label); mrows[i]=r;
	}
	lv_obj_t *h=lv_label_create(s); lv_obj_set_width(h,LCD_W-12); lv_label_set_long_mode(h,LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(h,KF_FONT,0); lv_obj_set_style_text_color(h,KF_TEXT_MUTED,0);
	lv_label_set_text(h,"Up/Dn select  ENTER open  ESC exit");
	home_hl();
	return s;
}
static void home_key(uint8_t k, int m){
	(void)m;
	if(k==DK_ESC || k==DK_BREAK){ active=0; kf_grab_input(0); kf_back_to_launcher(); return; }
	if(k==DK_UP){   home_sel=(home_sel+NMENU-1)%NMENU; home_hl(); return; }
	if(k==DK_DOWN){ home_sel=(home_sel+1)%NMENU; home_hl(); return; }
	if(k==DK_ENTER){ show_screen(MENU[home_sel].scr); }
}

/* ===================================================================== */
/* screen switching + input pump                                          */
/* ===================================================================== */
static void show_screen(int s){
	lv_obj_t *old=scr;
	screen=s;
	switch(s){
	case S_HOME: scr=build_home(); break;
	case S_RES:  scr=build_res();  break;
	case S_SMD:  scr=build_smd();  break;
	case S_LED:  scr=build_led();  break;
	case S_OHM:  scr=build_ohm();  break;
	case S_VDIV: scr=build_vdiv(); break;
	case S_CAP:  scr=build_cap();  break;
	case S_LC:   scr=build_lc();   break;
	case S_555:  scr=build_555();  break;
	case S_PCB:  scr=build_pcb();  break;
	case S_BATT: scr=build_batt(); break;
	default:     scr=build_home(); break;
	}
	lv_screen_load(scr);
	if(old && old!=scr) lv_obj_delete(old);
	kf_grab_input(1);
}

void electronics_poll(void){
	if(!active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st==KS_RELEASE) continue;
		int mods=uart_mods();
		if(screen==S_HOME)      home_key(key, mods);
		else if(screen==S_RES)  res_key(key, mods);
		else                    form_key(key, mods);
	}
}

void app_electronics_open(void){
	active=1; home_sel=0; scr=NULL; e555_mono=0; pcb_internal=0;
	show_screen(S_HOME);
}
