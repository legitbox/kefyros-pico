// apps/calc_graph3d.c — 3D surface z=f(x,y) as a rotatable wireframe on a black LVGL
// screen. The grid is sampled ONCE (heights don't change as you move the camera); each
// frame only re-projects + re-rasterises, driven by velocity+acceleration so rotation /
// pitch / zoom glide smoothly instead of stepping. Space toggles a steady auto-spin.
// Runs from calc_poll() in the superloop: key events set held flags, the per-frame tick
// integrates and blits strip-by-strip with Core 1 paused.
#include "../kefyros.h"
#include "calc.h"
#include "disp.h"                 /* disp_pause_core1 / disp_resume_core1 */
#include "lcdspi/lcdspi.h"        /* draw_buffer_spi (direct panel blit) */
#include "pico/time.h"            /* time_us_64 */
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

extern char font8x8_basic[128][8];
#define GW 320
#define GH KF_CONTENT_H          /* fit under the persistent OS top bar */
#define NG 26
#define RGB(r,g,b) (uint16_t)((((r)&0xf8)<<8)|(((g)&0xfc)<<3)|((b)>>3))
#define STRIP_H 40

static lv_obj_t *scr3;                         /* blank black screen (background only) */
static uint16_t *strip;                         /* GW*STRIP_H RGB565, malloc'd on open  */
static int       cur_y0, cur_h;                 /* current strip window in plot space   */
static cnode    *fn3;
static double    yaw=0.7, pitch=0.45, zoom=1.0;
static double    X0=-5, X1=5, Y0=-5, Y1=5;

/* cached surface (recomputed only when the function changes — not while moving) */
static double s_z[NG][NG];  static int s_ok[NG][NG];
static double s_zmin, s_zmax;
static int    s_sx[NG][NG], s_sy[NG][NG];        /* per-frame projection */

/* velocity-driven view */
static double   vyaw, vpitch, vzoomr;
static int      auto_rot;
enum { H_L=1, H_R=2, H_U=4, H_D=8, H_ZI=16, H_ZO=32 };
static uint8_t  held;
static uint64_t last_us;

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

/* evaluate the surface once — the heights are independent of the camera */
static void mesh_eval(void){
	s_zmin=1e300; s_zmax=-1e300;
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double wx=X0+(X1-X0)*i/(NG-1), wy=Y0+(Y1-Y0)*j/(NG-1);
		calc_set_var("x",wx); calc_set_var("y",wy);
		int o=1; double v=calc_eval(fn3,&o); s_ok[i][j]= o && isfinite(v);
		s_z[i][j]=v;
		if(s_ok[i][j]){ if(v<s_zmin) s_zmin=v; if(v>s_zmax) s_zmax=v; }
	}
	if(s_zmin>s_zmax){ s_zmin=0; s_zmax=1; }     /* no finite samples */
}

/* re-project the cached mesh for the current camera and blit it strip-by-strip */
static void render3(void){
	if(!strip) return;
	double zmid=(s_zmin+s_zmax)/2, zh=(s_zmax-s_zmin)/2; if(!(zh>1e-9)) zh=1;
	double ca=cos(yaw), sa=sin(yaw), cb=cos(pitch), sb=sin(pitch), scale=20*zoom;
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double X=(X0+(X1-X0)*i/(NG-1))*0.8, Y=(Y0+(Y1-Y0)*j/(NG-1))*0.8;
		double Z=s_ok[i][j]? (s_z[i][j]-zmid)/zh*3.5 : 0;
		double xr=X*ca - Y*sa, yr=X*sa + Y*ca;            /* yaw about vertical */
		double zr=yr*sb + Z*cb;                            /* pitch */
		s_sx[i][j]=(int)lround(GW/2 + xr*scale);
		s_sy[i][j]=(int)lround(GH/2 - zr*scale);
	}
	disp_pause_core1();
	for(cur_y0 = 0; cur_y0 < GH; cur_y0 += STRIP_H){
		cur_h = (GH - cur_y0 < STRIP_H) ? (GH - cur_y0) : STRIP_H;
		for(int k = 0; k < GW*cur_h; k++) strip[k] = RGB(0x0e,0x0c,0x0a);
		for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
			if(!s_ok[i][j]) continue;
			double t = (s_z[i][j]-s_zmin)/(2*zh+1e-9); uint16_t c=shade(t);
			if(i<NG-1 && s_ok[i+1][j]) line(s_sx[i][j],s_sy[i][j],s_sx[i+1][j],s_sy[i+1][j],c);
			if(j<NG-1 && s_ok[i][j+1]) line(s_sx[i][j],s_sy[i][j],s_sx[i][j+1],s_sy[i][j+1],c);
		}
		blit_str(2,2,"arrows rotate  +/- zoom  space spin  ESC", RGB(0x9a,0x8d,0x7a));
		draw_buffer_spi(0, KF_CONTENT_Y + cur_y0, GW-1, KF_CONTENT_Y + cur_y0 + cur_h - 1,
		                (unsigned char*)strip);
	}
	disp_resume_core1();
}

/* one velocity step: accelerate toward ±vmax while a key is held, else coast to 0 */
static double vel_step(double v, int dir, double acc, double vmax, double fric, double dt){
	if(dir){ v += dir*acc*dt; if(v>vmax)v=vmax; if(v<-vmax)v=-vmax; }
	else { double d=fric*dt; if(v>0){ v-=d; if(v<0)v=0; } else if(v<0){ v+=d; if(v>0)v=0; } }
	return v;
}

/* per-frame integrate + redraw; returns 1 if it drew (i.e. is animating) */
int calc_graph3d_tick(void){
	if(!scr3) return 0;
	uint64_t now = time_us_64();
	double dt = (double)(now - last_us) / 1e6; last_us = now;
	if(dt <= 0) return 0; if(dt > 0.05) dt = 0.05;

	int dyaw = ((held&H_R)?1:0) - ((held&H_L)?1:0);
	int dpit = ((held&H_U)?1:0) - ((held&H_D)?1:0);
	int dzm  = ((held&H_ZI)?1:0) - ((held&H_ZO)?1:0);
	vyaw   = vel_step(vyaw,   dyaw, 9.0, 3.5, 11.0, dt);
	vpitch = vel_step(vpitch, dpit, 9.0, 3.5, 11.0, dt);
	vzoomr = vel_step(vzoomr, dzm,  4.0, 2.0,  6.0, dt);

	if(vyaw==0 && vpitch==0 && vzoomr==0 && !auto_rot) return 0;

	yaw   += (vyaw + (auto_rot ? 0.9 : 0.0)) * dt;       /* auto-spin left->right */
	pitch += vpitch * dt;
	if(pitch >  1.55){ pitch =  1.55; if(vpitch>0) vpitch=0; }   /* avoid gimbal flip */
	if(pitch < -1.55){ pitch = -1.55; if(vpitch<0) vpitch=0; }
	if(vzoomr){ zoom *= exp(vzoomr*dt); if(zoom<0.1) zoom=0.1; if(zoom>30) zoom=30; }
	render3();
	return 1;
}

void calc_graph3d_open(const cnode *f){
	if(!f) return;
	if(!strip) strip = malloc((size_t)GW*STRIP_H*2);   /* one strip; freed on exit */
	if(!strip){ calc_note("out of memory"); return; }
	cn_free(fn3); fn3=cn_clone(f);
	yaw=0.7; pitch=0.45; zoom=1.0; vyaw=vpitch=vzoomr=0; held=0; auto_rot=0;

	scr3 = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr3,0,0); lv_obj_set_style_bg_color(scr3, lv_color_black(), 0);
	lv_obj_remove_flag(scr3, LV_OBJ_FLAG_SCROLLABLE);

	calc_set_mode(CMODE_3D);
	lv_screen_load(scr3);
	lv_refr_now(lv_display_get_default());   /* flush the black bg before painting over it */
	mesh_eval();
	last_us = time_us_64();
	render3();
}

void calc_graph3d_key(uint8_t key, int mods, int pressed){
	(void)mods;
	if(pressed){
		switch(key){
		case DK_ESC: case DK_F1+4: case DK_BREAK:
			cn_free(fn3); fn3=NULL; free(strip); strip=NULL;
			held=0; vyaw=vpitch=vzoomr=0; auto_rot=0;
			lv_obj_delete(scr3); scr3=NULL; calc_show_worksheet(); return;
		case ' ':
			auto_rot = !auto_rot; last_us = time_us_64(); return;
		case 'r': case 'R':
			yaw=0.7; pitch=0.45; zoom=1.0; vyaw=vpitch=vzoomr=0; held=0; render3(); return;
		}
	}
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
