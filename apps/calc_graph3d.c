// apps/calc_graph3d.c — 3D surface z=f(x,y) on a black LVGL screen, Desmos-style: a
// fit-to-cube projection with an X/Y/Z gizmo (arrows + tick labels), a faint ground grid
// plane, and either a wireframe or flat-shaded solid surface. The grid is sampled ONCE
// (heights don't change as the camera moves); each frame only re-projects + re-rasterises,
// driven by velocity+acceleration so motion glides. Runs from calc_poll() in the superloop.
//
// Keys (in plot): arrows rotate, +/- zoom, space auto-spin, F1 shade/wireframe,
// F2 ground plane, R reset, ESC back.
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
#define NQ ((NG-1)*(NG-1))
#define RGB(r,g,b) (uint16_t)((((r)&0xf8)<<8)|(((g)&0xfc)<<3)|((b)>>3))
#define STRIP_H 40

#define C_BG   RGB(0x0e,0x0c,0x0a)
#define C_GRID RGB(0x24,0x20,0x1a)
#define C_AXIS RGB(0x6c,0x62,0x52)
#define C_LBL  RGB(0xa4,0x98,0x84)
#define C_TICK RGB(0x74,0x6a,0x58)

static lv_obj_t *scr3;                         /* blank black screen (background only) */
static uint16_t *strip;                         /* GW*STRIP_H RGB565, malloc'd on open  */
static int       cur_y0, cur_h;                 /* current strip window in plot space   */
static cnode    *fn3;
static double    yaw=0.7, pitch=0.45, zoom=1.0;
static double    X0=-5, X1=5, Y0=-5, Y1=5;

/* cached surface (recomputed only when the function changes — not while moving) */
static double s_z[NG][NG];  static int s_ok[NG][NG];
static double s_zmin, s_zmax, s_zlo, s_zhi;     /* data range + nice box range */
static int    s_sx[NG][NG], s_sy[NG][NG];        /* per-frame projected screen coords */
static double s_vd[NG][NG];                      /* per-frame vertex depth (for sorting) */
static int    s_ord[NQ]; static double s_qd[NQ]; static int s_qn;   /* shaded quad order */

/* view params set per frame by setup_view(), consumed by projw() */
static double v_cx,v_cy,v_cz,v_hx,v_hy,v_hz,v_scl,v_ca,v_sa,v_cb,v_sb;

/* velocity-driven view + toggles */
static double   vyaw, vpitch, vzoomr;
static int      auto_rot;
static int      shaded   = 0;        /* F1: 0=wireframe, 1=flat solid */
static int      show_plane = 1;      /* F2: ground grid plane */
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
/* flat-filled triangle, scanlines clamped to the current strip (px() clips x) */
static void tri(int xa,int ya,int xb,int yb,int xc,int yc,uint16_t col){
	int t;
	if(yb<ya){ t=xa;xa=xb;xb=t; t=ya;ya=yb;yb=t; }
	if(yc<ya){ t=xa;xa=xc;xc=t; t=ya;ya=yc;yc=t; }
	if(yc<yb){ t=xb;xb=xc;xc=t; t=yb;yb=yc;yc=t; }
	if(yc==ya) return;
	int ylo=ya<cur_y0?cur_y0:ya, yhi=yc>=cur_y0+cur_h?cur_y0+cur_h-1:yc;
	for(int y=ylo; y<=yhi; y++){
		int xac = xa + (int)((long)(xc-xa)*(y-ya)/(yc-ya));     /* long edge a->c */
		int xo;
		if(y < yb) xo = (yb==ya)? xa : xa + (int)((long)(xb-xa)*(y-ya)/(yb-ya));
		else       xo = (yc==yb)? xb : xb + (int)((long)(xc-xb)*(y-yb)/(yc-yb));
		int xl=xac<xo?xac:xo, xr=xac<xo?xo:xac;
		for(int x=xl; x<=xr; x++) px(x,y,col);
	}
}
static void blit_ch(int x,int y,char ch,uint16_t c){ if((unsigned char)ch>=128) return;
	const char *g=font8x8_basic[(int)ch]; for(int j=0;j<8;j++){ uint8_t b=(uint8_t)g[j]; for(int i=0;i<8;i++) if((b>>i)&1) px(x+i,y+j,c); } }
static void blit_str(int x,int y,const char*s,uint16_t c){ for(;*s;s++,x+=6) blit_ch(x,y,*s,c); }

static uint16_t shade(double t){ /* t in [0,1] -> dim amber .. hot */
	if(t<0)t=0; if(t>1)t=1;
	int r=(int)(0xb8 + t*(0xff-0xb8)), g=(int)(0x86 + t*(0xc9-0x86)), b=(int)(0x0b + t*(0x4d-0x0b));
	return RGB(r,g,b);
}

/* evaluate the surface once + derive a nice box z-range */
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
	double pad=(s_zmax-s_zmin)*0.08; if(!(pad>1e-9)) pad=1;
	s_zlo=floor(s_zmin-pad); s_zhi=ceil(s_zmax+pad);
	if(s_zhi-s_zlo < 1) s_zhi = s_zlo + 1;
}

static void setup_view(void){
	v_cx=(X0+X1)/2; v_hx=(X1-X0)/2;
	v_cy=(Y0+Y1)/2; v_hy=(Y1-Y0)/2;
	v_cz=(s_zlo+s_zhi)/2; v_hz=(s_zhi-s_zlo)/2;
	if(v_hx<1e-9)v_hx=1; if(v_hy<1e-9)v_hy=1; if(v_hz<1e-9)v_hz=1;
	v_ca=cos(yaw); v_sa=sin(yaw); v_cb=cos(pitch); v_sb=sin(pitch);
	v_scl = (GH<GW?GH:GW) * 0.34 * zoom;        /* fit the unit cube on screen */
}
/* world -> screen, fit-to-cube (each axis normalised to [-1,1], like Desmos) */
static void projw(double wx,double wy,double wz,int*ox,int*oy){
	double nx=(wx-v_cx)/v_hx, ny=(wy-v_cy)/v_hy, nz=(wz-v_cz)/v_hz;
	double xr=nx*v_ca - ny*v_sa, yr=nx*v_sa + ny*v_ca;
	double scr=yr*v_sb + nz*v_cb;
	*ox=(int)lround(GW/2 + xr*v_scl);
	*oy=(int)lround(GH/2 - scr*v_scl);
}
static void linw(double ax,double ay,double az,double bx,double by,double bz,uint16_t c){
	int x0,y0,x1,y1; projw(ax,ay,az,&x0,&y0); projw(bx,by,bz,&x1,&y1); line(x0,y0,x1,y1,c);
}
/* axis line a->b with a little arrowhead at b */
static void arrow(double ax,double ay,double az,double bx,double by,double bz,uint16_t c){
	int x0,y0,x1,y1; projw(ax,ay,az,&x0,&y0); projw(bx,by,bz,&x1,&y1);
	line(x0,y0,x1,y1,c);
	double dx=x1-x0, dy=y1-y0, L=sqrt(dx*dx+dy*dy); if(L<1) return; dx/=L; dy/=L;
	double pxx=-dy, pyy=dx;
	line(x1,y1,(int)lround(x1-7*dx+3*pxx),(int)lround(y1-7*dy+3*pyy),c);
	line(x1,y1,(int)lround(x1-7*dx-3*pxx),(int)lround(y1-7*dy-3*pyy),c);
}

/* ground grid plane (F2) + axes gizmo + tick labels */
static void draw_frame(void){
	double zp = (s_zlo<=0.0 && 0.0<=s_zhi) ? 0.0 : s_zlo;   /* grid plane at z=0 if in range */
	if(show_plane){
		for(double gx=ceil(X0); gx<=X1+1e-9; gx+=1) linw(gx,Y0,zp, gx,Y1,zp, C_GRID);
		for(double gy=ceil(Y0); gy<=Y1+1e-9; gy+=1) linw(X0,gy,zp, X1,gy,zp, C_GRID);
	}
	arrow(X0,0,zp, X1,0,zp, C_AXIS);            /* x */
	arrow(0,Y0,zp, 0,Y1,zp, C_AXIS);            /* y */
	arrow(0,0,s_zlo, 0,0,s_zhi, C_AXIS);        /* z */
	int tx,ty;
	projw(X1,0,zp,&tx,&ty);    blit_str(tx+4,ty-4,"x",C_LBL);
	projw(0,Y1,zp,&tx,&ty);    blit_str(tx+4,ty-4,"y",C_LBL);
	projw(0,0,s_zhi,&tx,&ty);  blit_str(tx+4,ty-8,"z",C_LBL);
	for(double gx=ceil(X0); gx<=X1+1e-9; gx+=2){ if(fabs(gx)<0.5) continue;
		projw(gx,0,zp,&tx,&ty); char b[12]; snprintf(b,sizeof b,"%g",gx); blit_str(tx-2,ty+3,b,C_TICK); }
	double zr=s_zhi-s_zlo, zs = zr>40?10: zr>16?5: zr>8?2:1;
	for(double gz=ceil(s_zlo/zs)*zs; gz<=s_zhi+1e-9; gz+=zs){ if(fabs(gz)<zs/4) continue;
		projw(0,0,gz,&tx,&ty); char b[12]; snprintf(b,sizeof b,"%g",gz); blit_str(tx+4,ty-3,b,C_TICK); }
}

static int qcmp(const void *a,const void *b){
	double da=s_qd[*(const int*)a], db=s_qd[*(const int*)b];
	return da<db ? 1 : da>db ? -1 : 0;          /* far (larger depth) first */
}

/* re-project the cached mesh for the current camera and blit it strip-by-strip */
static void render3(void){
	if(!strip) return;
	setup_view();
	double zh=(s_zmax-s_zmin)/2; if(!(zh>1e-9)) zh=1;
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double wx=X0+(X1-X0)*i/(NG-1), wy=Y0+(Y1-Y0)*j/(NG-1), wz=s_z[i][j];
		double nx=(wx-v_cx)/v_hx, ny=(wy-v_cy)/v_hy, nz=(wz-v_cz)/v_hz;
		double xr=nx*v_ca - ny*v_sa, yr=nx*v_sa + ny*v_ca;
		double scr=yr*v_sb + nz*v_cb;
		s_sx[i][j]=(int)lround(GW/2 + xr*v_scl);
		s_sy[i][j]=(int)lround(GH/2 - scr*v_scl);
		s_vd[i][j]=yr*v_cb - nz*v_sb;            /* depth into the screen */
	}
	if(shaded){
		s_qn=0;
		for(int i=0;i<NG-1;i++) for(int j=0;j<NG-1;j++){
			if(s_ok[i][j]&&s_ok[i+1][j]&&s_ok[i][j+1]&&s_ok[i+1][j+1]){
				int q=i*(NG-1)+j; s_ord[s_qn]=q;
				s_qd[q]=(s_vd[i][j]+s_vd[i+1][j]+s_vd[i][j+1]+s_vd[i+1][j+1])*0.25;
				s_qn++;
			}
		}
		qsort(s_ord, s_qn, sizeof(int), qcmp);
	}
	disp_pause_core1();
	for(cur_y0 = 0; cur_y0 < GH; cur_y0 += STRIP_H){
		cur_h = (GH - cur_y0 < STRIP_H) ? (GH - cur_y0) : STRIP_H;
		for(int k = 0; k < GW*cur_h; k++) strip[k] = C_BG;
		draw_frame();                            /* gizmo + ground plane (behind the surface) */
		if(shaded){
			for(int o=0;o<s_qn;o++){ int q=s_ord[o], i=q/(NG-1), j=q%(NG-1);
				double az=(s_z[i][j]+s_z[i+1][j]+s_z[i][j+1]+s_z[i+1][j+1])*0.25;
				uint16_t c=shade((az-s_zmin)/(2*zh+1e-9));
				tri(s_sx[i][j],s_sy[i][j], s_sx[i+1][j],s_sy[i+1][j], s_sx[i+1][j+1],s_sy[i+1][j+1], c);
				tri(s_sx[i][j],s_sy[i][j], s_sx[i+1][j+1],s_sy[i+1][j+1], s_sx[i][j+1],s_sy[i][j+1], c);
			}
		} else {
			for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
				if(!s_ok[i][j]) continue;
				uint16_t c=shade((s_z[i][j]-s_zmin)/(2*zh+1e-9));
				if(i<NG-1 && s_ok[i+1][j]) line(s_sx[i][j],s_sy[i][j],s_sx[i+1][j],s_sy[i+1][j],c);
				if(j<NG-1 && s_ok[i][j+1]) line(s_sx[i][j],s_sy[i][j],s_sx[i][j+1],s_sy[i][j+1],c);
			}
		}
		blit_str(2,2,"F1 shade  F2 plane  space spin  R reset  ESC", RGB(0x9a,0x8d,0x7a));
		draw_buffer_spi(0, KF_CONTENT_Y + cur_y0, GW-1, KF_CONTENT_Y + cur_y0 + cur_h - 1,
		                (unsigned char*)strip);
	}
	disp_resume_core1();
}

/* one velocity step: accelerate toward ±vmax while held, else coast to 0 */
static double vel_step(double v, int dir, double acc, double vmax, double fric, double dt){
	if(dir){ v += dir*acc*dt; if(v>vmax)v=vmax; if(v<-vmax)v=-vmax; }
	else { double d=fric*dt; if(v>0){ v-=d; if(v<0)v=0; } else if(v<0){ v+=d; if(v>0)v=0; } }
	return v;
}

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

	yaw   += (vyaw + (auto_rot ? 0.9 : 0.0)) * dt;
	pitch += vpitch * dt;
	if(pitch >  1.55){ pitch =  1.55; if(vpitch>0) vpitch=0; }
	if(pitch < -1.55){ pitch = -1.55; if(vpitch<0) vpitch=0; }
	if(vzoomr){ zoom *= exp(vzoomr*dt); if(zoom<0.2) zoom=0.2; if(zoom>8) zoom=8; }
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
		case DK_ESC: case DK_BREAK:
			cn_free(fn3); fn3=NULL; free(strip); strip=NULL;
			held=0; vyaw=vpitch=vzoomr=0; auto_rot=0;
			lv_obj_delete(scr3); scr3=NULL; calc_show_worksheet(); return;
		case DK_F1:           shaded     = !shaded;     render3(); return;   /* F1 */
		case DK_F1+1:         show_plane = !show_plane; render3(); return;   /* F2 */
		case ' ':             auto_rot   = !auto_rot;   last_us = time_us_64(); return;
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
