// apps/calc.c — Kefyros scientific calculator, TI-style multi-screen front-end.
// A HOME menu picks a dedicated screen so you don't type commands every time:
//   Calculator (scientific scratchpad / REPL) · Solve · Graph 2D · Graph 3D · Table.
// Each screen is a form (labelled text fields); ENTER runs it, arrows move between
// fields, ESC returns to the menu. The graph/table/3D viewers (calc_graph*.c,
// calc_table.c) are full-screen canvases launched from the forms; ESC in a viewer
// returns to the form that launched it. All UI strings are ASCII (the bundled font
// is 0x20-0x7F only — non-ASCII rendered as missing-glyph boxes).
#include "../kefyros.h"
#include "../ui/theme.h"
#include "calc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

/* ===== screen state ===== */
enum { SCR_HOME=0, SCR_SCRATCH, SCR_SOLVE, SCR_GRAPH, SCR_GRAPH3D, SCR_TABLE };
static int active = 0;
static int screen = SCR_HOME;
static int g_return_screen = SCR_SCRATCH;   /* where a viewer (graph/table/3D) returns to */
static lv_obj_t *form_scr = NULL;           /* the live non-viewer screen object */

static int mode = CMODE_REPL;               /* viewer sub-mode (CMODE_*) for calc_poll routing */
int  calc_get_mode(void){ return mode; }
void calc_set_mode(int m){ mode = m; }

/* ===== forward decls ===== */
static void show_screen(int s);
static lv_obj_t *build_home(void), *build_scratch(void), *build_solve(void),
                *build_graph2d(void), *build_graph3d(void), *build_table(void);
static void home_key(uint8_t,int), scratch_key(uint8_t,int), solve_key(uint8_t,int),
            g2dform_key(uint8_t,int), g3dform_key(uint8_t,int), tableform_key(uint8_t,int);

/* viewer launch wrappers: remember which screen to return to, then open the viewer */
static void launch_graph2d(cnode **f,int n,int k){ g_return_screen=screen; calc_graph_2d(f,n,k); }
static void launch_graph3d(cnode *f){ g_return_screen=screen; calc_graph3d_open(f); }
static void launch_table(cnode *f,double s,double st){ g_return_screen=screen; calc_table_open(f,s,st); }

/* ===================================================================== */
/* Scratchpad (scientific REPL) — typed expressions stack into a history */
/* ===================================================================== */
static lv_obj_t *status, *hist, *ta, *legend;

#define HMAX 64
static char  hbuf[HMAX][128];     /* recallable input history */
static int   hn = 0, hpos = -1;
#define HIST_LINES_MAX 120

static void set_status(void){
	const char *am = calc_angle()==CALC_DEG?"DEG":calc_angle()==CALC_GRAD?"GRA":"RAD";
	lv_label_set_text_fmt(status, "CALCULATOR  %s", am);
}
static lv_obj_t *add_line(const char *txt, lv_color_t col, const lv_font_t *font){
	while(lv_obj_get_child_count(hist) > HIST_LINES_MAX)
		lv_obj_delete(lv_obj_get_child(hist, 0));
	lv_obj_t *l = lv_label_create(hist);
	lv_obj_set_width(l, LCD_W-14);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col, 0);
	lv_label_set_text(l, txt);
	return l;
}
static void scroll_bottom(lv_obj_t *last){ if(last) lv_obj_scroll_to_view(last, LV_ANIM_OFF); }
static void echo_in(const char *in){ char b[160]; snprintf(b,sizeof b,"> %s", in); add_line(b, KF_TEXT_DIM, KF_FONT); }
static void echo_res(const char *r){ char b[160]; snprintf(b,sizeof b,"= %s", r); scroll_bottom(add_line(b, KF_AMBER_BR, KF_FONT_BIG)); }
static void echo_err(const char *e){ char b[160]; snprintf(b,sizeof b,"! %s", e); scroll_bottom(add_line(b, lv_color_hex(0xe06c4a), KF_FONT)); }
static void echo_note(const char *n){ scroll_bottom(add_line(n, KF_AMBER, KF_FONT)); }

/* command dispatch (scratchpad only): plot/param/polar/plot3d/table/solve/diff/
   simplify/expand/factor/nderiv/integral. Returns 1 if consumed. */
static int try_command(cnode *n){
	const char *nm = n->name;
	if(!strcmp(nm,"plot"))  { launch_graph2d(n->args, n->nargs, GK_CARTESIAN); return 1; }
	if(!strcmp(nm,"param")) { launch_graph2d(n->args, n->nargs, GK_PARAM); return 1; }
	if(!strcmp(nm,"polar")) { launch_graph2d(n->args, n->nargs, GK_POLAR); return 1; }
	if(!strcmp(nm,"plot3d")){ if(n->nargs>=1) launch_graph3d(n->args[0]); else calc_note("usage: plot3d(f(x,y))"); return 1; }
	if(!strcmp(nm,"table")) {
		if(n->nargs<1){ calc_note("usage: table(f(x)[,start,step])"); return 1; }
		double start=-5, step=1; int ok=1;
		if(n->nargs>=2) start = calc_eval(n->args[1], &ok);
		if(n->nargs>=3) step  = calc_eval(n->args[2], &ok);
		launch_table(n->args[0], start, step); return 1;
	}
	if(!strcmp(nm,"solve")) {
		if(n->nargs==4 && n->args[2]->type==CN_VAR && n->args[3]->type==CN_VAR){   /* 2x2 system */
			double sx,sy;
			if(calc_solve2(n->args[0], n->args[1], n->args[2]->name, n->args[3]->name, &sx, &sy)){
				char xb[40], yb[40], b[110]; calc_fmt(sx,xb,sizeof xb); calc_fmt(sy,yb,sizeof yb);
				snprintf(b,sizeof b,"%s=%s, %s=%s", n->args[2]->name,xb, n->args[3]->name,yb); echo_res(b);
			} else echo_note("no solution found");
			return 1;
		}
		if(n->nargs<2 || n->args[1]->type!=CN_VAR){ calc_note("usage: solve(equation, var)"); return 1; }
		double roots[12]; int nr = calc_solve(n->args[0], n->args[1]->name, roots, 12);
		if(nr<0){ echo_err(calc_err); return 1; }
		if(nr==0){ echo_note("no roots found"); return 1; }
		for(int i=0;i<nr;i++){ char num[48], b[80]; calc_fmt(roots[i],num,sizeof num);
			snprintf(b,sizeof b,"%s = %s", n->args[1]->name, num); echo_res(b); }
		return 1;
	}
	if(!strcmp(nm,"diff")||!strcmp(nm,"d")) {
		if(n->nargs<1){ calc_note("usage: diff(expr[,var])"); return 1; }
		const char *v = (n->nargs>=2 && n->args[1]->type==CN_VAR)? n->args[1]->name : "x";
		cnode *r = calc_diff(n->args[0], v); char b[220]; calc_sym_str(r,b,sizeof b); echo_res(b); cn_free(r); return 1;
	}
	if(!strcmp(nm,"simplify")||!strcmp(nm,"expand")||!strcmp(nm,"factor")) {
		if(n->nargs<1){ calc_note("usage: name(expr)"); return 1; }
		cnode *r;
		if(nm[0]=='s') r = calc_simplify(n->args[0]);
		else if(nm[0]=='e') r = calc_expand(n->args[0]);
		else { const char *v=(n->nargs>=2 && n->args[1]->type==CN_VAR)? n->args[1]->name : "x"; r = calc_factor(n->args[0], v); }
		char b[220]; calc_sym_str(r,b,sizeof b); echo_res(b); cn_free(r); return 1;
	}
	if(!strcmp(nm,"nderiv")) {
		if(n->nargs<2 || n->args[1]->type!=CN_VAR){ calc_note("usage: nderiv(f, var, at)"); return 1; }
		int ok=1; double at = n->nargs>=3 ? calc_eval(n->args[2],&ok) : 0;
		double r = calc_nderiv(n->args[0], n->args[1]->name, at, &ok);
		if(!ok){ echo_err(calc_err); return 1; } char b[48]; calc_fmt(r,b,sizeof b); echo_res(b); return 1;
	}
	if(!strcmp(nm,"integral")||!strcmp(nm,"integ")) {
		const char *v="x"; int base=1;
		if(n->nargs>=4 && n->args[1]->type==CN_VAR){ v=n->args[1]->name; base=2; }
		if(n->nargs < base+2){ calc_note("usage: integral(f,a,b) or integral(f,var,a,b)"); return 1; }
		int ok=1; double a=calc_eval(n->args[base],&ok), b=calc_eval(n->args[base+1],&ok);
		double r = calc_integral(n->args[0], v, a, b, &ok);
		if(!ok){ echo_err(calc_err); return 1; } char bb[48]; calc_fmt(r,bb,sizeof bb); echo_res(bb); return 1;
	}
	return 0;
}

static void run_line(const char *text){
	if(!*text) return;
	if(hn<HMAX) strncpy(hbuf[hn++], text, sizeof hbuf[0]-1);
	else { memmove(hbuf[0], hbuf[1], sizeof hbuf[0]*(HMAX-1)); strncpy(hbuf[HMAX-1], text, sizeof hbuf[0]-1); }
	hpos = -1;
	echo_in(text);

	cnode *n = calc_parse(text);
	if(!n){ echo_err(calc_err); return; }

	if(n->type == CN_CALL && try_command(n)){ cn_free(n); return; }

	if(n->type == CN_EQ){
		if(n->a->type == CN_VAR){                 /* assignment: name = expr */
			int ok=1; double v = calc_eval(n->b, &ok);
			if(!ok){ echo_err(calc_err); cn_free(n); return; }
			calc_set_var(n->a->name, v); calc_set_var("ans", v);
			char r[80], num[64]; calc_fmt(v,num,sizeof num);
			snprintf(r,sizeof r,"%s = %s", n->a->name, num); echo_res(r);
			cn_free(n); return;
		}
		if(n->a->type == CN_CALL){                /* function def: f(p..) = body */
			char params[CN_MAXPARAMS][CN_NAMELEN]; int np = n->a->nargs, good = np<=CN_MAXPARAMS;
			for(int i=0;i<np && good;i++){
				if(n->a->args[i]->type != CN_VAR){ good=0; break; }
				strncpy(params[i], n->a->args[i]->name, CN_NAMELEN-1); params[i][CN_NAMELEN-1]=0;
			}
			if(good){
				calc_def_fun(n->a->name, params, np, n->b);
				char r[96]; snprintf(r,sizeof r,"%s defined", n->a->name); echo_note(r);
				cn_free(n); return;
			}
		}
		echo_note("equation - use the Solve screen, or solve(eq, var)");
		cn_free(n); return;
	}

	int ok=1; double v = calc_eval(n, &ok);
	if(!ok){ echo_err(calc_err); cn_free(n); return; }
	calc_set_var("ans", v);
	char num[64]; calc_fmt(v, num, sizeof num); echo_res(num);
	cn_free(n);
}

static lv_obj_t *build_scratch(void){
	lv_obj_t *s = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s, 4, 0);
	kf_inset_top(s);                    /* clear the persistent OS top bar */
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);

	status = lv_label_create(s);
	lv_obj_set_style_text_font(status, KF_FONT, 0);
	lv_obj_set_style_text_color(status, KF_AMBER, 0);
	set_status();

	hist = lv_obj_create(s);
	lv_obj_set_width(hist, LCD_W-8);
	lv_obj_set_flex_grow(hist, 1);
	lv_obj_set_flex_flow(hist, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_bg_opa(hist, 0, 0);
	lv_obj_set_style_border_width(hist, 0, 0);
	lv_obj_set_style_pad_all(hist, 2, 0);
	lv_obj_set_style_pad_row(hist, 1, 0);

	ta = lv_textarea_create(s);
	lv_textarea_set_one_line(ta, true);
	lv_textarea_set_placeholder_text(ta, "type an expression");
	lv_obj_set_width(ta, LCD_W-8);
	lv_obj_set_style_text_font(ta, KF_FONT, 0);     /* flat style comes from the theme */

	legend = lv_label_create(s);
	lv_obj_set_width(legend, LCD_W-8);
	lv_label_set_long_mode(legend, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(legend, KF_FONT, 0);
	lv_obj_set_style_text_color(legend, KF_TEXT_MUTED, 0);
	lv_label_set_text(legend, "ans/vars/f(x)= and plot() solve() d()  F4=ang  ESC=menu");

	lv_group_t *g = kf_use_group();
	lv_group_add_obj(g, ta);
	lv_group_focus_obj(ta);
	return s;
}

static void scratch_key(uint8_t key, int mods){
	if(key==DK_ESC || key==DK_F1+4 || key==DK_BREAK){ show_screen(SCR_HOME); return; }
	if(key==DK_F1+3){ calc_set_angle((calc_angle()+1)%3); set_status(); return; } /* F4 angle */
	if(key==DK_ENTER){
		const char *t = lv_textarea_get_text(ta);
		char line[160]; strncpy(line, t, sizeof line-1); line[sizeof line-1]=0;
		lv_textarea_set_text(ta, "");
		run_line(line);
		return;
	}
	if(key==DK_BACKSPACE){ lv_textarea_delete_char(ta); return; }
	if(key==DK_DEL){ lv_textarea_delete_char_forward(ta); return; }
	if(key==DK_LEFT){ lv_textarea_cursor_left(ta); return; }
	if(key==DK_RIGHT){ lv_textarea_cursor_right(ta); return; }
	if(key==DK_HOME){ lv_textarea_set_cursor_pos(ta, 0); return; }
	if(key==DK_END){ lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); return; }
	if(key==DK_UP){ if(hn){ if(hpos<0) hpos=hn-1; else if(hpos>0) hpos--; lv_textarea_set_text(ta, hbuf[hpos]); lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); } return; }
	if(key==DK_DOWN){ if(hn && hpos>=0){ if(hpos<hn-1){ hpos++; lv_textarea_set_text(ta, hbuf[hpos]); } else { hpos=-1; lv_textarea_set_text(ta,""); } lv_textarea_set_cursor_pos(ta, LV_TEXTAREA_CURSOR_LAST); } return; }
	if(key>=0x20 && key<0x7f && !(mods&MOD_CTRL)) lv_textarea_add_char(ta, key);
}

/* ===================================================================== */
/* Generic form framework (Solve / Graph / Table screens)                */
/* ===================================================================== */
#define MAXFIELDS 6
static lv_obj_t  *fields[MAXFIELDS], *flabels[MAXFIELDS];
static int        nfields;
static lv_group_t*fgrp;
static lv_obj_t  *fmsg;             /* result / error line (may be NULL on non-form screens) */

static void form_msg(const char *s){ if(fmsg) lv_label_set_text(fmsg, s); }

static lv_obj_t *form_begin(const char *title){
	nfields = 0; fmsg = NULL;
	fgrp = kf_use_group();
	lv_obj_t *s = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s, 6, 0);
	kf_inset_top(s);                    /* clear the persistent OS top bar */
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s, 3, 0);
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
	lv_label_set_long_mode(fmsg, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(fmsg, KF_FONT, 0);
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
/* shared field editing; returns 1 if the key was consumed (cursor/edit/field-nav) */
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

/* residual of an equation node: a=b -> a-b, plain expr -> copy. Returns a NEW node. */
static cnode *to_residual(const cnode *n){
	if(n->type==CN_EQ) return cn_bin('-', cn_clone(n->a), cn_clone(n->b));
	return cn_clone(n);
}

/* ----- Solve screen ----- */
static lv_obj_t *build_solve(void){
	lv_obj_t *s = form_begin("Solve");
	add_field(s, "Equation:",    "");
	add_field(s, "Variable:",    "x");
	add_field(s, "Eq 2 (opt):",  "");
	add_field(s, "Var 2 (opt):", "y");
	form_finish(s, "ENTER=solve  Up/Dn=field  ESC=menu");
	return s;
}
static void do_solve(void){
	const char *e1 = lv_textarea_get_text(fields[0]);
	const char *v1 = lv_textarea_get_text(fields[1]);
	const char *e2 = lv_textarea_get_text(fields[2]);
	const char *v2 = lv_textarea_get_text(fields[3]);
	if(!*e1){ form_msg("enter an equation"); return; }
	if(!*v1) v1 = "x";
	cnode *n1 = calc_parse(e1);
	if(!n1){ form_msg(calc_err); return; }
	cnode *r1 = to_residual(n1); cn_free(n1);

	if(*e2){                                  /* 2x2 system */
		cnode *n2 = calc_parse(e2);
		if(!n2){ cn_free(r1); form_msg(calc_err); return; }
		cnode *r2 = to_residual(n2); cn_free(n2);
		const char *vv2 = *v2 ? v2 : "y";
		double sx, sy;
		if(calc_solve2(r1, r2, v1, vv2, &sx, &sy)){
			char xb[40], yb[40], b[120]; calc_fmt(sx,xb,sizeof xb); calc_fmt(sy,yb,sizeof yb);
			snprintf(b,sizeof b,"%s = %s ,  %s = %s", v1, xb, vv2, yb); form_msg(b);
		} else form_msg("no solution found");
		cn_free(r1); cn_free(r2); return;
	}

	double roots[12]; int nr = calc_solve(r1, v1, roots, 12);
	cn_free(r1);
	if(nr<0){ form_msg(calc_err); return; }
	if(nr==0){ form_msg("no real roots found"); return; }
	char b[220]; int p = snprintf(b,sizeof b,"%s = ", v1);
	for(int i=0;i<nr && p<(int)sizeof b-2;i++){
		char nb[40]; calc_fmt(roots[i],nb,sizeof nb);
		p += snprintf(b+p, sizeof b-p, "%s%s", i?" , ":"", nb);
	}
	form_msg(b);
}
static void solve_key(uint8_t k, int m){
	if(k==DK_ESC || k==DK_BREAK){ show_screen(SCR_HOME); return; }
	if(k==DK_ENTER || k==DK_F1){ do_solve(); return; }
	form_edit_key(k, m);
}

/* ----- Graph 2D screen ----- */
static int       g2d_type = 0;        /* 0 cartesian, 1 parametric, 2 polar */
static lv_obj_t *g2d_tl;              /* type indicator label */
static void g2d_relabel(void){
	static const char *L0[3] = { "Y1 =", "X(t) =",  "r(t) =" };
	static const char *L1[3] = { "Y2 =", "Y(t) =",  "(unused)" };
	static const char *L2[3] = { "Y3 =", "X2(t) =", "(unused)" };
	static const char *L3[3] = { "Y4 =", "Y2(t) =", "(unused)" };
	lv_label_set_text(flabels[0], L0[g2d_type]);
	lv_label_set_text(flabels[1], L1[g2d_type]);
	lv_label_set_text(flabels[2], L2[g2d_type]);
	lv_label_set_text(flabels[3], L3[g2d_type]);
	const char *tn = g2d_type==0?"Cartesian": g2d_type==1?"Parametric":"Polar";
	lv_label_set_text_fmt(g2d_tl, "Type: %s   (F2 to change)", tn);
}
static lv_obj_t *build_graph2d(void){
	lv_obj_t *s = form_begin("Graph 2D");
	g2d_tl = lv_label_create(s);
	lv_obj_set_style_text_font(g2d_tl, KF_FONT, 0);
	lv_obj_set_style_text_color(g2d_tl, KF_ACTIVE, 0);
	add_field(s, "Y1 =", "");
	add_field(s, "Y2 =", "");
	add_field(s, "Y3 =", "");
	add_field(s, "Y4 =", "");
	form_finish(s, "ENTER=graph  F2=type  Up/Dn=field  ESC=menu");
	g2d_relabel();
	return s;
}
static void do_graph2d(void){
	cnode *fs[4] = {0,0,0,0}; int n = 0;
	if(g2d_type==1){                                  /* parametric */
		const char *xt=lv_textarea_get_text(fields[0]), *yt=lv_textarea_get_text(fields[1]);
		if(!*xt || !*yt){ form_msg("need X(t) and Y(t)"); return; }
		fs[0]=calc_parse(xt); if(!fs[0]){ form_msg(calc_err); return; }
		fs[1]=calc_parse(yt); if(!fs[1]){ cn_free(fs[0]); form_msg(calc_err); return; }
		n=2;
		const char *x2=lv_textarea_get_text(fields[2]), *y2=lv_textarea_get_text(fields[3]);
		if(*x2 && *y2){ fs[2]=calc_parse(x2); fs[3]=calc_parse(y2);
			if(fs[2] && fs[3]) n=4; else { cn_free(fs[2]); cn_free(fs[3]); fs[2]=fs[3]=NULL; } }
		launch_graph2d(fs, n, GK_PARAM);
	} else if(g2d_type==2){                           /* polar */
		const char *rt=lv_textarea_get_text(fields[0]);
		if(!*rt){ form_msg("need r(t)"); return; }
		fs[0]=calc_parse(rt); if(!fs[0]){ form_msg(calc_err); return; }
		n=1; launch_graph2d(fs, n, GK_POLAR);
	} else {                                          /* cartesian */
		int ok=1;
		for(int i=0;i<4;i++){ const char *t=lv_textarea_get_text(fields[i]);
			if(*t){ cnode *c=calc_parse(t); if(!c){ form_msg(calc_err); ok=0; break; } fs[n++]=c; } }
		if(!ok){ for(int i=0;i<n;i++) cn_free(fs[i]); return; }
		if(!n){ form_msg("enter at least Y1"); return; }
		launch_graph2d(fs, n, GK_CARTESIAN);
	}
	for(int i=0;i<n;i++) cn_free(fs[i]);               /* the viewer cloned them */
}
static void g2dform_key(uint8_t k, int m){
	if(k==DK_ESC || k==DK_BREAK){ show_screen(SCR_HOME); return; }
	if(k==DK_ENTER || k==DK_F1){ do_graph2d(); return; }
	if(k==DK_F1+1){ g2d_type=(g2d_type+1)%3; g2d_relabel(); return; }  /* F2 */
	form_edit_key(k, m);
}

/* ----- Graph 3D screen ----- */
static lv_obj_t *build_graph3d(void){
	lv_obj_t *s = form_begin("Graph 3D");
	add_field(s, "Z = f(x,y):", "");
	form_finish(s, "ENTER=graph  ESC=menu");
	return s;
}
static void do_graph3d(void){
	const char *t = lv_textarea_get_text(fields[0]);
	if(!*t){ form_msg("enter z = f(x,y)"); return; }
	cnode *n = calc_parse(t);
	if(!n){ form_msg(calc_err); return; }
	launch_graph3d(n); cn_free(n);
}
static void g3dform_key(uint8_t k, int m){
	if(k==DK_ESC || k==DK_BREAK){ show_screen(SCR_HOME); return; }
	if(k==DK_ENTER || k==DK_F1){ do_graph3d(); return; }
	form_edit_key(k, m);
}

/* ----- Table screen ----- */
static lv_obj_t *build_table(void){
	lv_obj_t *s = form_begin("Table");
	add_field(s, "f(x) =",  "");
	add_field(s, "Start:",  "-5");
	add_field(s, "Step:",   "1");
	form_finish(s, "ENTER=build  Up/Dn=field  ESC=menu");
	return s;
}
static void do_table(void){
	const char *ft = lv_textarea_get_text(fields[0]);
	if(!*ft){ form_msg("enter f(x)"); return; }
	cnode *f = calc_parse(ft);
	if(!f){ form_msg(calc_err); return; }
	double start=-5, step=1; int ok=1;
	const char *st=lv_textarea_get_text(fields[1]), *sp=lv_textarea_get_text(fields[2]);
	if(*st){ cnode *a=calc_parse(st); if(a){ start=calc_eval(a,&ok); cn_free(a); } }
	if(*sp){ cnode *a=calc_parse(sp); if(a){ step =calc_eval(a,&ok); cn_free(a); } }
	if(step==0) step=1;
	launch_table(f, start, step); cn_free(f);
}
static void tableform_key(uint8_t k, int m){
	if(k==DK_ESC || k==DK_BREAK){ show_screen(SCR_HOME); return; }
	if(k==DK_ENTER || k==DK_F1){ do_table(); return; }
	form_edit_key(k, m);
}

/* ===================================================================== */
/* Home menu                                                             */
/* ===================================================================== */
static const struct { const char *label; int scr; } HITEMS[] = {
	{ "Calculator", SCR_SCRATCH  },
	{ "Solve",      SCR_SOLVE    },
	{ "Graph 2D",   SCR_GRAPH    },
	{ "Graph 3D",   SCR_GRAPH3D  },
	{ "Table",      SCR_TABLE    },
	{ "Angle",      -1           },   /* in-place: cycle DEG/RAD/GRAD */
};
#define NHITEMS 6
static lv_obj_t *hrows[NHITEMS];
static int home_sel = 0;

static void home_hl(void){
	for(int i=0;i<NHITEMS;i++){
		int sel = (i==home_sel);
		lv_obj_set_style_bg_color(hrows[i], KF_ACTIVE, 0);
		lv_obj_set_style_bg_opa(hrows[i], sel?LV_OPA_COVER:LV_OPA_TRANSP, 0);
		lv_obj_set_style_text_color(hrows[i], sel?KF_BG_DEEP:KF_AMBER, 0);
	}
	const char *am = calc_angle()==CALC_DEG?"DEG": calc_angle()==CALC_RAD?"RAD":"GRAD";
	lv_label_set_text_fmt(hrows[NHITEMS-1], "Angle: %s", am);
}
static lv_obj_t *build_home(void){
	fmsg = NULL;
	lv_obj_t *s = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(s, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(s, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(s, 6, 0);
	kf_inset_top(s);                    /* clear the persistent OS top bar */
	lv_obj_set_flex_flow(s, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(s, 3, 0);

	lv_obj_t *t = lv_label_create(s);
	lv_obj_set_style_text_font(t, KF_FONT_BIG, 0);
	lv_obj_set_style_text_color(t, KF_AMBER_BR, 0);
	lv_label_set_text(t, "CALC");

	for(int i=0;i<NHITEMS;i++){
		lv_obj_t *r = lv_label_create(s);
		lv_obj_set_width(r, LCD_W-12);
		lv_obj_set_style_text_font(r, KF_FONT, 0);
		lv_obj_set_style_pad_ver(r, 4, 0);
		lv_obj_set_style_pad_left(r, 4, 0);
		lv_obj_set_style_radius(r, 0, 0);
		lv_label_set_text(r, HITEMS[i].label);
		hrows[i] = r;
	}
	lv_obj_t *h = lv_label_create(s);
	lv_obj_set_width(h, LCD_W-12);
	lv_label_set_long_mode(h, LV_LABEL_LONG_WRAP);
	lv_obj_set_style_text_font(h, KF_FONT, 0);
	lv_obj_set_style_text_color(h, KF_TEXT_MUTED, 0);
	lv_label_set_text(h, "Up/Dn select  ENTER open  ESC exit");

	home_hl();
	return s;
}
static void home_key(uint8_t k, int m){
	(void)m;
	if(k==DK_ESC || k==DK_BREAK){ active=0; kf_grab_input(0); kf_back_to_launcher(); return; }
	if(k==DK_UP){   home_sel=(home_sel+NHITEMS-1)%NHITEMS; home_hl(); return; }
	if(k==DK_DOWN){ home_sel=(home_sel+1)%NHITEMS; home_hl(); return; }
	if(k==DK_ENTER){
		if(HITEMS[home_sel].scr < 0){ calc_set_angle((calc_angle()+1)%3); home_hl(); }
		else show_screen(HITEMS[home_sel].scr);
	}
}

/* ===================================================================== */
/* Screen switching + input pump                                         */
/* ===================================================================== */
static void show_screen(int s){
	lv_obj_t *old = form_scr;
	screen = s;
	switch(s){
	case SCR_HOME:    form_scr = build_home();    break;
	case SCR_SCRATCH: form_scr = build_scratch(); break;
	case SCR_SOLVE:   form_scr = build_solve();   break;
	case SCR_GRAPH:   form_scr = build_graph2d(); break;
	case SCR_GRAPH3D: form_scr = build_graph3d(); break;
	case SCR_TABLE:   form_scr = build_table();   break;
	default:          form_scr = build_home();    break;
	}
	lv_screen_load(form_scr);
	if(old && old != form_scr) lv_obj_delete(old);
	kf_grab_input(1);
}

/* called by the graph/table/3D viewers on ESC: return to the launching screen
   (the form_scr is still alive — viewers create their own screen object). */
void calc_show_worksheet(void){
	calc_set_mode(CMODE_REPL);
	screen = g_return_screen;
	if(form_scr) lv_screen_load(form_scr);
	kf_grab_input(1);
}

/* append a note: to the scratchpad history if shown, else the active form's msg line */
void calc_note(const char *s){
	if(screen==SCR_SCRATCH && hist){ echo_note(s); return; }
	form_msg(s);
}

void calc_poll(void){
	if(!active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		int mods = uart_mods();
		/* a full-screen viewer owns all keys while active */
		if(mode==CMODE_GRAPH){ calc_graph_key(key, mods);   continue; }
		if(mode==CMODE_TABLE){ calc_table_key(key, mods);   continue; }
		if(mode==CMODE_3D){    calc_graph3d_key(key, mods); continue; }
		switch(screen){
		case SCR_HOME:    home_key(key, mods);      break;
		case SCR_SCRATCH: scratch_key(key, mods);   break;
		case SCR_SOLVE:   solve_key(key, mods);     break;
		case SCR_GRAPH:   g2dform_key(key, mods);   break;
		case SCR_GRAPH3D: g3dform_key(key, mods);   break;
		case SCR_TABLE:   tableform_key(key, mods); break;
		}
	}
}

void app_calc_open(void){
	active = 1; hpos = -1; mode = CMODE_REPL;
	home_sel = 0; form_scr = NULL;
	show_screen(SCR_HOME);
}
