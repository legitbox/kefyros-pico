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
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

extern char font8x8_basic[128][8];
#define GW 320
#define GH KF_CONTENT_H          /* fit under the persistent OS top bar */
#define NG 24                     /* 24x24 grid: arena ~47 KB (26x26 was ~55 KB and OOM'd on
                                     the 147 KB heap with the form screen still resident) */
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

/* cached surface (recomputed only when the function changes — not while moving).
   These working buffers total ~42 KB. They used to be static .bss — permanently
   resident, they starved the shared SRAM heap so that other apps' allocations failed
   (notably the music player's 32 KB audio ring malloc, which left playback stuck on
   the play button with no sound). They now live in ONE heap arena allocated on open
   (g3d_alloc) and freed on close (g3d_free); pointer-to-array types keep s_z[i][j]. */
static double (*s_z)[NG];  static int (*s_ok)[NG];
static double (*s_wx)[NG], (*s_wy)[NG];          /* world x,y per vertex (spherical mode; in
                                                    Cartesian mode x,y come straight from i,j) */
static int    s_sph = 0;                          /* F3: 0 = z=f(x,y) Cartesian, 1 = r=f(x,y) spherical */
static double s_zmin, s_zmax, s_zlo, s_zhi;     /* data range + nice box range */
static int    (*s_sx)[NG], (*s_sy)[NG];          /* per-frame projected screen coords */
static double (*s_vd)[NG];                        /* per-frame vertex depth (for sorting) */
static double (*s_nx)[NG], (*s_ny)[NG], (*s_nz)[NG];  /* normalised model coords (for normals) */
static int    *s_ord; static double *s_qd; static int s_qn;   /* shaded quad order */
static uint16_t *s_qc;                            /* per-quad lit colour (computed once/frame) */
static void   *g3d_arena;                         /* single heap block backing all of the above */

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
	int yl=y0<y1?y0:y1, yh=y0<y1?y1:y0;
	if(yh<cur_y0 || yl>=cur_y0+cur_h) return;     /* whole line outside this strip — skip */
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
		if(xl<0) xl=0; if(xr>=GW) xr=GW-1;
		uint16_t *row = strip + (size_t)(y-cur_y0)*GW;   /* y already clamped to the strip */
		for(int x=xl; x<=xr; x++) row[x]=col;
	}
}
static void blit_ch(int x,int y,char ch,uint16_t c){ if((unsigned char)ch>=128) return;
	const char *g=font8x8_basic[(int)ch]; for(int j=0;j<8;j++){ uint8_t b=(uint8_t)g[j]; for(int i=0;i<8;i++) if((b>>i)&1) px(x+i,y+j,c); } }
static void blit_str(int x,int y,const char*s,uint16_t c){ for(;*s;s++,x+=6) blit_ch(x,y,*s,c); }

static uint16_t shade(double t){ /* t in [0,1] -> dim amber .. hot (wireframe) */
	if(t<0)t=0; if(t>1)t=1;
	int r=(int)(0xb8 + t*(0xff-0xb8)), g=(int)(0x86 + t*(0xc9-0x86)), b=(int)(0x0b + t*(0x4d-0x0b));
	return RGB(r,g,b);
}
/* lit: the amber height-ramp modulated by a directional light (+ ambient) */
static uint16_t shade_lit(double t, double inten){
	if(t<0)t=0; if(t>1)t=1; if(inten<0)inten=0; if(inten>1.15)inten=1.15;
	int r=(int)((0xb8 + t*(0xff-0xb8))*inten);
	int g=(int)((0x86 + t*(0xc9-0x86))*inten);
	int b=(int)((0x0b + t*(0x4d-0x0b))*inten);
	if(r>255)r=255; if(g>255)g=255; if(b>255)b=255;
	return RGB(r,g,b);
}

/* evaluate the surface once + derive a nice box range. Two modes:
     Cartesian  z = f(x,y)   over X0..X1, Y0..Y1   (s_z = height; x,y from the grid index)
     Spherical  r = f(x,y)   with x = azimuth th in [0,2pi], y = polar phi in [0,pi]; the
                point is (r sinphi costh, r sinphi sinth, r cosphi). All three world coords
                are stored per vertex (s_wx,s_wy,s_z) and the box is a symmetric cube so a
                plain r=const renders as a true sphere. s_zmin/s_zmax stay the z-extent so the
                amber height-ramp colouring is unchanged. */
static void mesh_eval(void){
	s_zmin=1e300; s_zmax=-1e300;
	if(!s_sph){
		X0=-5; X1=5; Y0=-5; Y1=5;                /* fixed Cartesian domain (spherical clobbers these) */
		for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
			double wx=X0+(X1-X0)*i/(NG-1), wy=Y0+(Y1-Y0)*j/(NG-1);
			calc_set_var("x",wx); calc_set_var("y",wy);
			int o=1; double v=calc_eval(fn3,&o); s_ok[i][j]= o && isfinite(v);
			s_z[i][j]=v; s_wx[i][j]=wx; s_wy[i][j]=wy;
			if(s_ok[i][j]){ if(v<s_zmin) s_zmin=v; if(v>s_zmax) s_zmax=v; }
		}
		if(s_zmin>s_zmax){ s_zmin=0; s_zmax=1; }     /* no finite samples */
		double pad=(s_zmax-s_zmin)*0.08; if(!(pad>1e-9)) pad=1;
		s_zlo=floor(s_zmin-pad); s_zhi=ceil(s_zmax+pad);
		if(s_zhi-s_zlo < 1) s_zhi = s_zlo + 1;
		return;
	}
	/* spherical: x = theta, y = phi; r = f(theta,phi) */
	double m=0;                                      /* half-extent of the bounding cube */
	for(int i=0;i<NG;i++) for(int j=0;j<NG;j++){
		double th=2*M_PI*i/(NG-1), ph=M_PI*j/(NG-1);
		calc_set_var("x",th); calc_set_var("y",ph);
		int o=1; double r=calc_eval(fn3,&o); int ok = o && isfinite(r);
		s_ok[i][j]=ok;
		double wx=0,wy=0,wz=0;
		if(ok){ double sp=sin(ph); wx=r*sp*cos(th); wy=r*sp*sin(th); wz=r*cos(ph);
			if(fabs(wx)>m)m=fabs(wx); if(fabs(wy)>m)m=fabs(wy); if(fabs(wz)>m)m=fabs(wz);
			if(wz<s_zmin)s_zmin=wz; if(wz>s_zmax)s_zmax=wz; }
		s_wx[i][j]=wx; s_wy[i][j]=wy; s_z[i][j]=wz;
	}
	if(!(m>1e-9)) m=1;
	if(s_zmin>s_zmax){ s_zmin=-m; s_zmax=m; }
	X0=-m; X1=m; Y0=-m; Y1=m; s_zlo=-m; s_zhi=m;     /* symmetric cube box */
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

static void perp_of(int x0,int y0,int x1,int y1,double*pxx,double*pyy){
	double dx=x1-x0, dy=y1-y0, L=sqrt(dx*dx+dy*dy);
	if(L<1){ *pxx=0; *pyy=0; } else { *pxx=-dy/L; *pyy=dx/L; }
}
static void tick_mark(int tx,int ty,double pxx,double pyy,uint16_t c){
	int ox=(int)lround(3*pxx), oy=(int)lround(3*pyy);
	line(tx-ox,ty-oy, tx+ox,ty+oy, c);
}
/* a "nice" tick step (1/2/5 x 10^k) giving roughly `target` ticks across `range`. Keeps the
   z-axis tick COUNT bounded no matter how huge the data range is (e.g. x^4*y^4 spans ~400k):
   the old capped step (max 10) drew tens of thousands of marks per frame and tanked FPS. */
static double nice_step(double range, int target){
	if(!(range > 0) || target < 1) return 1;
	double raw = range / target;
	double mag = pow(10.0, floor(log10(raw)));
	double n = raw / mag;
	double s = (n <= 1.5) ? 1 : (n <= 3) ? 2 : (n <= 7) ? 5 : 10;
	return s * mag;
}

/* Reference frame: a BIG ground grid plane + the y-axis spine (both gated by F2/show_plane),
   plus the always-on x axis. The z-axis is NOT here — it's drawn on top after the surface by
   draw_zaxis(). Tick + grid spacing use nice_step so the count stays bounded no matter how big
   the box gets (spherical can blow the box out to tens of units -> the old g+=1 loop drew
   hundreds of ticks/grid-lines and tanked FPS). Cartesian's fixed -5..5 box -> step 1, unchanged. */
static void draw_frame(void){
	double zp = (s_zlo<=0.0 && 0.0<=s_zhi) ? 0.0 : s_zlo;
	int e0x,e0y,e1x,e1y,tx,ty; double pxx,pyy;
	double xs = nice_step(X1-X0, 10), ys = nice_step(Y1-Y0, 10);

	if(show_plane){
		double Ex=2.5*v_hx, Ey=2.5*v_hy;                 /* a big floor, ~5x the data span */
		double gx0=v_cx-Ex, gx1=v_cx+Ex, gy0=v_cy-Ey, gy1=v_cy+Ey;
		for(double g=ceil(gx0/xs)*xs; g<=gx1+1e-9; g+=xs) linw(g,gy0,zp, g,gy1,zp, C_GRID);
		for(double g=ceil(gy0/ys)*ys; g<=gy1+1e-9; g+=ys) linw(gx0,g,zp, gx1,g,zp, C_GRID);
		arrow(0,Y0,zp, 0,Y1,zp, C_AXIS);                 /* y-axis spine (with the plane) */
		projw(0,Y0,zp,&e0x,&e0y); projw(0,Y1,zp,&e1x,&e1y); perp_of(e0x,e0y,e1x,e1y,&pxx,&pyy);
		for(double g=ceil(Y0/ys)*ys; g<=Y1+1e-9; g+=ys){ if(fabs(g)<ys/4) continue; projw(0,g,zp,&tx,&ty); tick_mark(tx,ty,pxx,pyy,C_TICK); }
		projw(0,Y1,zp,&tx,&ty); blit_str(tx+4,ty-4,"y",C_LBL);
	}
	/* x-axis (always) */
	arrow(X0,0,zp, X1,0,zp, C_AXIS);
	projw(X0,0,zp,&e0x,&e0y); projw(X1,0,zp,&e1x,&e1y); perp_of(e0x,e0y,e1x,e1y,&pxx,&pyy);
	for(double g=ceil(X0/xs)*xs; g<=X1+1e-9; g+=xs){ if(fabs(g)<xs/4) continue; projw(g,0,zp,&tx,&ty); tick_mark(tx,ty,pxx,pyy,C_TICK); }
	projw(X1,0,zp,&tx,&ty); blit_str(tx+4,ty-4,"x",C_LBL);
}
/* The z-axis pole at x=0,y=0: spine + perpendicular ticks + arrowhead + "z" label. Drawn AFTER
   the surface (on top) so the filled surface can't overpaint it. Drawing it behind made it read
   as a layer *under* the model; depth-splitting it broke concave shapes (it got cut off inside a
   bowl). On-top is simple and consistent in both wireframe and shaded modes. */
static void draw_zaxis(void){
	int e0x,e0y,e1x,e1y,tx,ty; double pxx,pyy;
	double zs = nice_step(s_zhi-s_zlo, 8);
	arrow(0,0,s_zlo, 0,0,s_zhi, C_AXIS);
	projw(0,0,s_zlo,&e0x,&e0y); projw(0,0,s_zhi,&e1x,&e1y); perp_of(e0x,e0y,e1x,e1y,&pxx,&pyy);
	for(double g=ceil(s_zlo/zs)*zs; g<=s_zhi+1e-9; g+=zs){ if(fabs(g)<zs/4) continue; projw(0,0,g,&tx,&ty); tick_mark(tx,ty,pxx,pyy,C_TICK); }
	projw(0,0,s_zhi,&tx,&ty); blit_str(tx+4,ty-8,"z",C_LBL);
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
		double wx=s_wx[i][j], wy=s_wy[i][j], wz=s_z[i][j];   /* both modes store world coords */
		double nx=(wx-v_cx)/v_hx, ny=(wy-v_cy)/v_hy, nz=(wz-v_cz)/v_hz;
		double xr=nx*v_ca - ny*v_sa, yr=nx*v_sa + ny*v_ca;
		double scr=yr*v_sb + nz*v_cb;
		s_sx[i][j]=(int)lround(GW/2 + xr*v_scl);
		s_sy[i][j]=(int)lround(GH/2 - scr*v_scl);
		s_vd[i][j]=yr*v_cb - nz*v_sb;            /* depth into the screen */
		s_nx[i][j]=nx; s_ny[i][j]=ny; s_nz[i][j]=nz;   /* for face normals (lighting) */
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
		/* per-quad lit colour — computed ONCE here, not re-derived for every strip below
		   (that was ~8x the normal + sqrt + lighting math per frame). */
		const double Lx=0.32, Ly=0.42, Lz=0.85;   /* light from above-front-right (model space) */
		for(int o=0;o<s_qn;o++){ int q=s_ord[o], i=q/(NG-1), j=q%(NG-1);
			double az=(s_z[i][j]+s_z[i+1][j]+s_z[i][j+1]+s_z[i+1][j+1])*0.25;
			double ax=s_nx[i+1][j]-s_nx[i][j], ay=s_ny[i+1][j]-s_ny[i][j], aaz=s_nz[i+1][j]-s_nz[i][j];
			double bx=s_nx[i][j+1]-s_nx[i][j], by=s_ny[i][j+1]-s_ny[i][j], bz=s_nz[i][j+1]-s_nz[i][j];
			double nx=ay*bz-aaz*by, ny=aaz*bx-ax*bz, nz=ax*by-ay*bx;
			double nl=sqrt(nx*nx+ny*ny+nz*nz); if(nl<1e-12) nl=1;
			nx/=nl; ny/=nl; nz/=nl;
			double diff;
			if(s_sph){ diff=fabs(nx*Lx+ny*Ly+nz*Lz); }   /* closed surface: light both faces */
			else { if(nz<0){ nx=-nx; ny=-ny; nz=-nz; }    /* height field: force the normal up */
				diff=nx*Lx+ny*Ly+nz*Lz; if(diff<0)diff=0; }
			s_qc[q]=shade_lit((az-s_zmin)/(2*zh+1e-9), 0.32 + 0.78*diff);   /* ambient + diffuse */
		}
	}
	disp_pause_core1();
	for(cur_y0 = 0; cur_y0 < GH; cur_y0 += STRIP_H){
		cur_h = (GH - cur_y0 < STRIP_H) ? (GH - cur_y0) : STRIP_H;
		for(int k = 0; k < GW*cur_h; k++) strip[k] = C_BG;
		draw_frame();                            /* gizmo + ground plane (behind the surface) */
		if(shaded){
			/* surface (painter-sorted); colour precomputed once above, not per strip */
			for(int o=0;o<s_qn;o++){ int q=s_ord[o], i=q/(NG-1), j=q%(NG-1);
				int ya=s_sy[i][j], yb=s_sy[i+1][j], yc=s_sy[i][j+1], yd=s_sy[i+1][j+1];
				int ymin=ya, ymax=ya;                 /* cheap bbox-y cull: skip off-strip quads */
				if(yb<ymin)ymin=yb; else if(yb>ymax)ymax=yb;
				if(yc<ymin)ymin=yc; else if(yc>ymax)ymax=yc;
				if(yd<ymin)ymin=yd; else if(yd>ymax)ymax=yd;
				if(ymax<cur_y0 || ymin>=cur_y0+cur_h) continue;
				uint16_t c=s_qc[q];
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
		draw_zaxis();                    /* z-axis pole, on top of the surface (never cut off) */
		blit_str(2,2, s_sph ? "F1 shade F2 plane F3 xyz  arrows R ESC"
		                    : "F1 shade F2 plane F3 sphere  arrows R ESC", RGB(0x9a,0x8d,0x7a));
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

/* Carve every big per-vertex / per-quad buffer out of one heap block. Doubles are laid
   down first (their block sizes are all multiples of 8, so each stays 8-aligned); the
   int blocks follow. Freed in full by g3d_free() when the plotter closes. */
static int g3d_alloc(void){
	if(g3d_arena) return 1;
	const size_t dbl = sizeof(double[NG][NG]);   /* one NGxNG double grid */
	const size_t ib  = sizeof(int[NG][NG]);      /* one NGxNG int grid    */
	size_t need = dbl*7 + sizeof(double[NQ])      /* s_z,s_vd,s_nx,s_ny,s_nz,s_wx,s_wy + s_qd */
	            + ib*3  + sizeof(int[NQ])         /* s_ok,s_sx,s_sy        + s_ord  */
	            + sizeof(uint16_t[NQ]);           /* s_qc                           */
	char *p = malloc(need);
	if(!p) return 0;
	g3d_arena = p;
	s_z =(double(*)[NG])p; p+=dbl;
	s_vd=(double(*)[NG])p; p+=dbl;
	s_nx=(double(*)[NG])p; p+=dbl;
	s_ny=(double(*)[NG])p; p+=dbl;
	s_nz=(double(*)[NG])p; p+=dbl;
	s_wx=(double(*)[NG])p; p+=dbl;
	s_wy=(double(*)[NG])p; p+=dbl;
	s_qd=(double*)p;       p+=sizeof(double[NQ]);
	s_ok=(int(*)[NG])p;    p+=ib;
	s_sx=(int(*)[NG])p;    p+=ib;
	s_sy=(int(*)[NG])p;    p+=ib;
	s_ord=(int*)p;         p+=sizeof(int[NQ]);
	s_qc=(uint16_t*)p;
	return 1;
}
static void g3d_free(void){
	free(g3d_arena); g3d_arena=NULL;
	s_z=NULL; s_vd=NULL; s_nx=NULL; s_ny=NULL; s_nz=NULL; s_wx=NULL; s_wy=NULL; s_qd=NULL;
	s_ok=NULL; s_sx=NULL; s_sy=NULL; s_ord=NULL; s_qc=NULL;
}

void calc_graph3d_open(const cnode *f){
	if(!f) return;
	if(!strip) strip = malloc((size_t)GW*STRIP_H*2);   /* one strip; freed on exit */
	if(!strip){ calc_note("out of memory"); return; }
	if(!g3d_alloc()){ free(strip); strip=NULL; calc_note("out of memory"); return; }
	kf_clock_boost();             /* 3D render compute: bump to the fastest clock (420 MHz) while open */
	cn_free(fn3); fn3=cn_clone(f);
	yaw=0.7; pitch=0.45; zoom=1.0; vyaw=vpitch=vzoomr=0; held=0; auto_rot=0;
	s_sph=0;                       /* always open in Cartesian; F3 toggles spherical in-viewer */

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
			cn_free(fn3); fn3=NULL; free(strip); strip=NULL; g3d_free();
			held=0; vyaw=vpitch=vzoomr=0; auto_rot=0;
			yaw=0.7; pitch=0.45; zoom=1.0;
			shaded=0; show_plane=1; s_sph=0;
			lv_obj_delete(scr3); scr3=NULL;
			kf_clock_normal();    /* 3D render done: drop back to 400 MHz */
			calc_show_worksheet(); return;
		case DK_F1:           shaded     = !shaded;     render3(); return;   /* F1 */
		case DK_F1+1:         show_plane = !show_plane; render3(); return;   /* F2 */
		case DK_F1+2:         /* F3: toggle Cartesian z=f(x,y) <-> spherical r=f(x,y) */
			s_sph = !s_sph;
			yaw=0.7; pitch=0.45; zoom=1.0; vyaw=vpitch=vzoomr=0;   /* new box -> reset the camera */
			mesh_eval(); render3(); return;
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
