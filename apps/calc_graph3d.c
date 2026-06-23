// apps/calc_graph3d.c — 3D surface z=f(x,y) as a rotatable wireframe on an LVGL
// canvas. Samples an NxN grid, rotates (yaw/pitch), orthographic-projects, and draws
// a height-shaded see-through mesh. Redraws only on key input (single core).
#include "../kefyros.h"
#include "calc.h"
#include "disp.h"                 /* disp_pause_core1 / disp_resume_core1 */
#include "lcdspi/lcdspi.h"        /* draw_buffer_spi (direct panel blit) */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern char font8x8_basic[128][8];
#define GW 320
#define GH KF_CONTENT_H          /* fit under the persistent OS top bar */
#define NG 26
#define RGB(r,g,b) (uint16_t)((((r)&0xf8)<<8)|(((g)&0xfc)<<3)|((b)>>3))

/* Strip rendering: blit the wireframe to the panel one STRIP_H-row band at a time from
   a small (~25 KB) buffer, rather than malloc'ing a ~185 KB full-screen canvas (which
   OOM'd). The mesh is projected once; only the line raster repeats per strip. */
#define STRIP_H 40
static lv_obj_t *scr3;                        /* blank black screen (background only) */
static uint16_t *strip;                        /* GW*STRIP_H RGB565, malloc'd on open  */
static int       cur_y0, cur_h;                /* current strip window in plot space   */
static cnode    *fn3;
static double    yaw=0.7, pitch=0.45, zoom=1.0;
static double    X0=-5, X1=5, Y0=-5, Y1=5;

static inline void px(int x,int y,uint16_t c){
	int yy = y - cur_y0;
	if((unsigned)x<GW && (unsigned)yy<(unsigned)cur_h) strip[yy*GW+x]=c;
}
static void line(int x0,int y0,int x1,int y1,uint16_t c){
	int dx=abs(x1-x0), sx=x0<x1?1:-1, dy=-abs(y1-y0), sy=y0<y1?1:-1, e=dx+dy;
	for(;;){ px(x0,y0,c); if(x0==x1&&y0==y1) break; int e2=2*e;
		if(e2>=dy){ e+=dy; x0+=sx; } if(e2<=dx){ e+=dx; y0+=sy; } }
}
static void blit_ch(int x,int y,char ch,uint16_t c){ if((unsigned char)ch>=128) return;
	const char *g=font8x8_basic[(int)ch]; for(int j=0;j<8;j++){ uint8_t b=(uint8_t)g[j]; for(int i=0;i<8;i++) if((b>>i)&1) px(x+i,y+j,c); } }
static void blit_str(int x,int y,const char*s,uint16_t c){ for(;*s;s++,x+=6) blit_ch(x,y,*s,c); }

static uint16_t shade(double t){ /* t in [0,1] -> dim amber .. hot */
	if(t<0)t=0; if(t>1)t=1;
	int r=(int)(0xb8 + t*(0xff-0xb8)), g=(int)(0x86 + t*(0xc9-0x86)), b=(int)(0x0b + t*(0x4d-0x0b));
	return RGB(r,g,b);
}

static void redraw3(void){
	if(!strip) return;
	static double z[NG][NG]; static int sxp[NG][NG], syp[NG][NG], ok[NG][NG];
	double zmin=1e300, zmax=-1e300;
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double wx=X0+(X1-X0)*i/(NG-1), wy=Y0+(Y1-Y0)*j/(NG-1);
		calc_set_var("x",wx); calc_set_var("y",wy);
		int o=1; double v=calc_eval(fn3,&o); ok[i][j]= o && isfinite(v);
		z[i][j]=v; if(ok[i][j]){ if(v<zmin)zmin=v; if(v>zmax)zmax=v; }
	}
	double zmid=(zmin+zmax)/2, zh=(zmax-zmin)/2; if(!(zh>1e-9)) zh=1;
	double ca=cos(yaw), sa=sin(yaw), cb=cos(pitch), sb=sin(pitch), scale=20*zoom;
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double X=(X0+(X1-X0)*i/(NG-1))*0.8, Y=(Y0+(Y1-Y0)*j/(NG-1))*0.8;
		double Z=ok[i][j]? (z[i][j]-zmid)/zh*3.5 : 0;
		double xr=X*ca - Y*sa, yr=X*sa + Y*ca;           /* yaw about vertical */
		double zr=yr*sb + Z*cb, yd=yr*cb - Z*sb; (void)yd; /* pitch */
		sxp[i][j]=(int)lround(GW/2 + xr*scale);
		syp[i][j]=(int)lround(GH/2 - zr*scale);
	}
	/* rasterise the (already-projected) mesh strip-by-strip, blitting each to the panel */
	disp_pause_core1();
	for(cur_y0 = 0; cur_y0 < GH; cur_y0 += STRIP_H){
		cur_h = (GH - cur_y0 < STRIP_H) ? (GH - cur_y0) : STRIP_H;
		for(int k = 0; k < GW*cur_h; k++) strip[k] = RGB(0x0e,0x0c,0x0a);
		for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
			if(!ok[i][j]) continue;
			double t = (z[i][j]-zmin)/(2*zh+1e-9); uint16_t c=shade(t);
			if(i<NG-1 && ok[i+1][j]) line(sxp[i][j],syp[i][j],sxp[i+1][j],syp[i+1][j],c);
			if(j<NG-1 && ok[i][j+1]) line(sxp[i][j],syp[i][j],sxp[i][j+1],syp[i][j+1],c);
		}
		blit_str(2,2,"z=f(x,y)  arrows rotate +/- zoom ESC", RGB(0x9a,0x8d,0x7a));
		draw_buffer_spi(0, KF_CONTENT_Y + cur_y0, GW-1, KF_CONTENT_Y + cur_y0 + cur_h - 1,
		                (unsigned char*)strip);
	}
	disp_resume_core1();
}

void calc_graph3d_open(const cnode *f){
	if(!f) return;
	if(!strip) strip = malloc((size_t)GW*STRIP_H*2);   /* one strip; freed on exit */
	if(!strip){ calc_note("out of memory"); return; }
	cn_free(fn3); fn3=cn_clone(f);
	yaw=0.7; pitch=0.45; zoom=1.0;

	scr3 = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr3,0,0); lv_obj_set_style_bg_color(scr3, lv_color_black(), 0);
	lv_obj_remove_flag(scr3, LV_OBJ_FLAG_SCROLLABLE);

	calc_set_mode(CMODE_3D);
	lv_screen_load(scr3);
	lv_refr_now(lv_display_get_default());   /* flush the black bg before painting over it */
	redraw3();
}

void calc_graph3d_key(uint8_t key, int mods){
	(void)mods;
	switch(key){
	case DK_ESC: case DK_F1+4: case DK_BREAK:
		cn_free(fn3); fn3=NULL; free(strip); strip=NULL;
		lv_obj_delete(scr3); scr3=NULL; calc_show_worksheet(); return;
	case DK_LEFT:  yaw-=0.18; break;
	case DK_RIGHT: yaw+=0.18; break;
	case DK_UP:    pitch+=0.15; break;
	case DK_DOWN:  pitch-=0.15; break;
	case '+': case '=': zoom*=1.2; break;
	case '-': case '_': zoom/=1.2; break;
	case 'r': case 'R': yaw=0.7; pitch=0.45; zoom=1.0; break;
	default: return;
	}
	redraw3();
}
