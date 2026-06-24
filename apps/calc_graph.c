// apps/calc_graph.c — 2D function grapher on an LVGL canvas (direct RGB565 writes).
// Cartesian y=f(x), parametric x(t)/y(t), and polar r(θ). Axes/grid/tick-labels via
// an 8x8 bitmap font blitted straight into the buffer. Pan/zoom/trace from the keyboard.
// Redraws only on input (the single core can't do per-frame full redraws).
#include "../kefyros.h"
#include "calc.h"
#include "disp.h"                 /* disp_pause_core1 / disp_resume_core1 */
#include "lcdspi/lcdspi.h"        /* draw_buffer_spi (direct panel blit) */
#include "pico/time.h"            /* time_us_64 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
/* 8x8 bitmap font: the array is *defined* in its header, which apps/terminal.c
   already includes — so reference that single definition (avoid a duplicate). */
extern char font8x8_basic[128][8];

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define GW 320
#define GH KF_CONTENT_H          /* fit under the persistent OS top bar */
#define MAXF 4
#define RGB(r,g,b) (uint16_t)((((r)&0xf8)<<8)|(((g)&0xfc)<<3)|((b)>>3))

/* A full-screen RGB565 canvas is ~185 KB — too big a single contiguous malloc for
   this device's heap under real load (it OOM'd). Instead we render the scene into a
   small STRIP_H-row buffer and blit each horizontal strip straight to the panel with
   Core 1 paused (the LCD's own flush pump). Peak RAM is one strip (~25 KB), which
   always allocates. The scene is just re-rasterised per strip; px() clips to the
   current strip window. */
#define STRIP_H 40
static lv_obj_t *gscr;                       /* blank black screen (background only) */
static uint16_t *strip;                       /* GW*STRIP_H RGB565, malloc'd on open  */
static int       cur_y0, cur_h;               /* current strip window in plot space   */
static cnode    *funcs[MAXF];
static int       nfuncs, gkind;
static double    xmin, xmax, ymin, ymax;     /* world window */
static double    pmin, pmax;                  /* parameter range (param/polar) */
static int       trace_on, trace_col;

/* velocity-driven pan/zoom (smooth, accelerated) — active only when not tracing */
static double    vpx, vpy, vzoomr;            /* pan vel (windows/s), zoom rate (1/s) */
enum { H_L=1, H_R=2, H_U=4, H_D=8, H_ZI=16, H_ZO=32 };
static uint8_t   held;
static uint64_t  last_us;

static const uint16_t COL[MAXF] = {
	RGB(0xf0,0xa5,0x00), RGB(0xb6,0xf0,0x00), RGB(0x4a,0xc8,0xe0), RGB(0xe0,0x6c,0x4a) };
#define C_BG   RGB(0x0e,0x0c,0x0a)
#define C_GRID RGB(0x2a,0x23,0x18)
#define C_AXIS RGB(0x6a,0x5c,0x40)
#define C_LBL  RGB(0x9a,0x8d,0x7a)
#define C_TR   RGB(0xff,0xc9,0x4d)

/* ---- pixel/draw primitives (clipped to the current strip) ---- */
static inline void px(int x, int y, uint16_t c){
	int yy = y - cur_y0;
	if((unsigned)x<GW && (unsigned)yy<(unsigned)cur_h) strip[yy*GW+x]=c;
}
static void line(int x0,int y0,int x1,int y1,uint16_t c){
	int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, e=dx+dy;
	for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*e;
		if(e2>=dy){ e+=dy; x0+=sx; } if(e2<=dx){ e+=dx; y0+=sy; } }
}
static void blit_ch(int x,int y,char ch,uint16_t c){
	if((unsigned char)ch>=128) return; const char *g=font8x8_basic[(int)ch];
	for(int j=0;j<8;j++){ uint8_t bits=(uint8_t)g[j]; for(int i=0;i<8;i++) if((bits>>i)&1) px(x+i,y+j,c); }
}
static void blit_str(int x,int y,const char *s,uint16_t c){ for(;*s;s++,x+=6) blit_ch(x,y,*s,c); }

/* ---- world<->screen ---- */
static int SX(double wx){ return (int)lround((wx-xmin)/(xmax-xmin)*(GW-1)); }
static int SY(double wy){ return (int)lround((GH-1) - (wy-ymin)/(ymax-ymin)*(GH-1)); }

static double nicestep(double span){
	double raw = span/8.0, mag = pow(10, floor(log10(raw))), n = raw/mag;
	double s = n<1.5?1: n<3?2: n<7?5:10; return s*mag;
}

/* ---- evaluate one curve sample: parameter p -> (wx,wy); returns 1 if finite ---- */
static int sample(int fi, double p, double *wx, double *wy){
	int ok=1; double v;
	if(gkind==GK_CARTESIAN){
		calc_set_var("x", p); v = calc_eval(funcs[fi], &ok);
		*wx=p; *wy=v;
	}else if(gkind==GK_PARAM){
		calc_set_var("t", p); calc_set_var("theta", p);
		double X = calc_eval(funcs[fi*2], &ok); int ok2=1; double Y = calc_eval(funcs[fi*2+1], &ok2);
		ok = ok&&ok2; *wx=X; *wy=Y;
	}else{ /* polar */
		calc_set_var("t", p); calc_set_var("theta", p);
		double r = calc_eval(funcs[fi], &ok); *wx=r*cos(p); *wy=r*sin(p);
	}
	return ok && isfinite(*wx) && isfinite(*wy);
}
static int ncurves(void){ return gkind==GK_PARAM ? nfuncs/2 : nfuncs; }

/* ---- full redraw ---- */
/* rasterise the whole scene; px() keeps only what lands in the current strip */
static void draw_scene(void){
	/* grid + axes + labels */
	double xs=nicestep(xmax-xmin), ys=nicestep(ymax-ymin);
	for(double gx=ceil(xmin/xs)*xs; gx<=xmax; gx+=xs){ int sx=SX(gx); for(int y=0;y<GH;y++) px(sx,y,C_GRID); }
	for(double gy=ceil(ymin/ys)*ys; gy<=ymax; gy+=ys){ int sy=SY(gy); for(int x=0;x<GW;x++) px(x,sy,C_GRID); }
	int ax0=SX(0), ay0=SY(0);
	if(ay0>=0&&ay0<GH) for(int x=0;x<GW;x++) px(x,ay0,C_AXIS);
	if(ax0>=0&&ax0<GW) for(int y=0;y<GH;y++) px(ax0,y,C_AXIS);
	int lbx = (ay0>=0&&ay0<GH-10)? ay0+2 : GH-9;
	for(double gx=ceil(xmin/xs)*xs; gx<=xmax; gx+=xs){ if(fabs(gx)<xs/4) continue; char b[16]; calc_fmt(gx,b,sizeof b); blit_str(SX(gx)+1, lbx, b, C_LBL); }
	int lby = (ax0>=2&&ax0<GW)? ax0+2 : 2;
	for(double gy=ceil(ymin/ys)*ys; gy<=ymax; gy+=ys){ if(fabs(gy)<ys/4) continue; char b[16]; calc_fmt(gy,b,sizeof b); blit_str(lby, SY(gy)-3, b, C_LBL); }

	/* curves */
	int nc = ncurves(), NS = (gkind==GK_CARTESIAN)?GW:480;
	for(int fi=0; fi<nc; fi++){
		uint16_t c = COL[fi%MAXF];
		double p0 = (gkind==GK_CARTESIAN)?xmin:pmin, p1=(gkind==GK_CARTESIAN)?xmax:pmax;
		int pv=0, psx=0, psy=0;
		for(int s=0;s<NS;s++){
			double p = p0 + (p1-p0)*s/(double)(NS-1), wx,wy;
			if(sample(fi,p,&wx,&wy)){
				int sx=SX(wx), sy=SY(wy);
				if(pv && abs(sy-psy)<=GH && abs(sx-psx)<=GW) line(psx,psy,sx,sy,c);
				else px(sx,sy,c);
				pv=1; psx=sx; psy=sy;
			} else pv=0;
		}
	}
	/* trace (cartesian only) */
	if(trace_on && gkind==GK_CARTESIAN && nc>0){
		double wx = xmin + (xmax-xmin)*trace_col/(double)(GW-1), dwx, wy;
		if(sample(0,wx,&dwx,&wy)){
			int sx=SX(wx), sy=SY(wy);
			for(int y=0;y<GH;y++) px(sx,y,C_TR);
			for(int d=-2;d<=2;d++){ px(sx+d,sy,C_TR); px(sx,sy+d,C_TR); }
			char b[48], xb[20], yb[20]; calc_fmt(wx,xb,sizeof xb); calc_fmt(wy,yb,sizeof yb);
			snprintf(b,sizeof b,"x=%s y=%s", xb, yb); blit_str(2,2,b,C_TR);
		}
	} else {
		char b[40]; snprintf(b,sizeof b,"[%g,%g] arrows pan +/- zoom", xmin,xmax); blit_str(2,2,b,C_LBL);
	}
}

/* render the plot strip-by-strip straight to the panel (Core 1 paused so Core 0 owns
   the SPI). One ~25 KB strip buffer instead of a ~185 KB full-screen canvas. */
static void redraw(void){
	if(!strip) return;
	disp_pause_core1();
	for(cur_y0 = 0; cur_y0 < GH; cur_y0 += STRIP_H){
		cur_h = (GH - cur_y0 < STRIP_H) ? (GH - cur_y0) : STRIP_H;
		for(int i = 0; i < GW*cur_h; i++) strip[i] = C_BG;
		draw_scene();
		draw_buffer_spi(0, KF_CONTENT_Y + cur_y0, GW-1, KF_CONTENT_Y + cur_y0 + cur_h - 1,
		                (unsigned char*)strip);
	}
	disp_resume_core1();
}

/* ---- public ---- */
void calc_graph_2d(cnode **f, int nf, int kind){
	if(nf<1) return;
	if(kind==GK_PARAM && nf<2){ calc_note("param needs x(t),y(t)"); return; }
	if(!strip) strip = malloc((size_t)GW*STRIP_H*2);   /* one strip; freed on exit */
	if(!strip){ calc_note("out of memory"); return; }
	for(int i=0;i<MAXF;i++){ cn_free(funcs[i]); funcs[i]=NULL; }
	nfuncs = nf>MAXF?MAXF:nf;
	for(int i=0;i<nfuncs;i++) funcs[i]=cn_clone(f[i]);
	gkind = kind; trace_on=0; trace_col=GW/2;
	xmin=-10; xmax=10; ymin=-10; ymax=10;
	pmin = (kind==GK_POLAR)?0:-10; pmax = (kind==GK_POLAR)?2*M_PI:10;

	/* blank black screen: stops the worksheet's widgets (blinking cursors etc.) from
	   invalidating the content area and re-flushing over our direct strip blits. */
	gscr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(gscr,0,0); lv_obj_set_style_bg_color(gscr,lv_color_black(),0);
	lv_obj_remove_flag(gscr, LV_OBJ_FLAG_SCROLLABLE);

	vpx=vpy=vzoomr=0; held=0;
	calc_set_mode(CMODE_GRAPH);
	lv_screen_load(gscr);
	lv_refr_now(lv_display_get_default());   /* flush the black bg before we paint over it */
	last_us = time_us_64();
	redraw();
}

static void zoom(double f){
	double cx=(xmin+xmax)/2, cy=(ymin+ymax)/2, hx=(xmax-xmin)/2*f, hy=(ymax-ymin)/2*f;
	xmin=cx-hx; xmax=cx+hx; ymin=cy-hy; ymax=cy+hy;
}
static double vel_step(double v, int dir, double acc, double vmax, double fric, double dt){
	if(dir){ v += dir*acc*dt; if(v>vmax)v=vmax; if(v<-vmax)v=-vmax; }
	else { double d=fric*dt; if(v>0){ v-=d; if(v<0)v=0; } else if(v<0){ v+=d; if(v>0)v=0; } }
	return v;
}

void calc_graph_key(uint8_t key, int mods, int pressed){
	(void)mods;
	if(pressed){
		switch(key){
		case DK_ESC: case DK_F1+4: case DK_BREAK:
			for(int i=0;i<MAXF;i++){ cn_free(funcs[i]); funcs[i]=NULL; }
			free(strip); strip=NULL; held=0; vpx=vpy=vzoomr=0;
			lv_obj_delete(gscr); gscr=NULL;
			calc_show_worksheet(); return;
		case 't': case 'T': trace_on=!trace_on; trace_col=GW/2; held=0; vpx=vpy=vzoomr=0; redraw(); return;
		case 'r': case 'R': xmin=-10;xmax=10;ymin=-10;ymax=10; trace_on=0; held=0; vpx=vpy=vzoomr=0; redraw(); return;
		}
	}
	/* trace mode keeps the old discrete stepping (cursor + manual y-pan) */
	if(trace_on){
		if(!pressed) return;
		double ph=(ymax-ymin)*0.12;
		switch(key){
		case DK_LEFT:  if(trace_col>0)    trace_col--; break;
		case DK_RIGHT: if(trace_col<GW-1) trace_col++; break;
		case DK_UP:    ymin+=ph; ymax+=ph; break;
		case DK_DOWN:  ymin-=ph; ymax-=ph; break;
		default: return;
		}
		redraw(); return;
	}
	/* free-roam: arrows pan, +/- zoom — all velocity-driven (see calc_graph_tick) */
	uint8_t bit=0;
	switch(key){
	case DK_LEFT:  bit=H_L;  break;
	case DK_RIGHT: bit=H_R;  break;
	case DK_UP:    bit=H_U;  break;
	case DK_DOWN:  bit=H_D;  break;
	case '+': case '=': bit=H_ZI; break;
	case '-': case '_': bit=H_ZO; break;
	default: return;
	}
	if(pressed) held |= bit; else held &= (uint8_t)~bit;
}

/* per-frame integrate + redraw; returns 1 while animating (called from calc_poll) */
int calc_graph_tick(void){
	if(!gscr || trace_on) return 0;
	uint64_t now = time_us_64();
	double dt = (double)(now - last_us) / 1e6; last_us = now;
	if(dt <= 0) return 0; if(dt > 0.05) dt = 0.05;

	int dx = ((held&H_R)?1:0) - ((held&H_L)?1:0);
	int dy = ((held&H_U)?1:0) - ((held&H_D)?1:0);
	int dz = ((held&H_ZI)?1:0) - ((held&H_ZO)?1:0);
	vpx    = vel_step(vpx,    dx, 4.0, 1.6, 6.0, dt);
	vpy    = vel_step(vpy,    dy, 4.0, 1.6, 6.0, dt);
	vzoomr = vel_step(vzoomr, dz, 4.0, 2.0, 6.0, dt);
	if(vpx==0 && vpy==0 && vzoomr==0) return 0;

	double mx = vpx*(xmax-xmin)*dt, my = vpy*(ymax-ymin)*dt;
	xmin+=mx; xmax+=mx; ymin+=my; ymax+=my;
	if(vzoomr) zoom(exp(-vzoomr*dt));      /* +zoom (ZI) shrinks the window */
	redraw();
	return 1;
}
