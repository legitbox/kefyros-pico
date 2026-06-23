// apps/calc_solve.c — numeric equation solver + calculus.
//  - calc_solve : real roots of f(var)=0 by fine sign-change scan + bisection + Newton
//    polish (handles polynomials AND transcendentals; integer roots format exactly).
//  - calc_solve2: 2x2 systems via numeric-Jacobian Newton from a seed grid.
//  - calc_nderiv: central difference;  calc_integral: adaptive Simpson.
// A CN_EQ node evaluates as lhs-rhs (see calc_eval), so equations come in directly.
#include "calc.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

static double fval(const cnode *f, const char *v, double x, int *ok){
	calc_set_var(v, x); *ok=1; return calc_eval(f, ok);
}

double calc_nderiv(const cnode *f, const char *var, double at, int *ok){
	double h = 1e-5*(fabs(at)>1?fabs(at):1.0);
	int o1,o2; double a=fval(f,var,at+h,&o1), b=fval(f,var,at-h,&o2);
	if(!o1||!o2 || !isfinite(a) || !isfinite(b)){ *ok=0; snprintf(calc_err,sizeof calc_err,"derivative undefined"); return NAN; }
	*ok=1; return (a-b)/(2*h);
}

static double simpson(const cnode*f,const char*v,double a,double b,double fa,double fb,double fm,
                      double whole,double eps,int depth,int*ok){
	double m=(a+b)/2, lm=(a+m)/2, rm=(m+b)/2;
	int o1,o2; double flm=fval(f,v,lm,&o1), frm=fval(f,v,rm,&o2);
	if(!o1||!o2){ *ok=0; return 0; }
	double left=(m-a)/6*(fa+4*flm+fm), right=(b-m)/6*(fm+4*frm+fb);
	if(depth<=0 || fabs(left+right-whole)<=15*eps) return left+right+(left+right-whole)/15;
	return simpson(f,v,a,m,fa,fm,flm,left,eps/2,depth-1,ok)
	     + simpson(f,v,m,b,fm,fb,frm,right,eps/2,depth-1,ok);
}
double calc_integral(const cnode *f, const char *v, double a, double b, int *ok){
	int o1,o2,o3; double fa=fval(f,v,a,&o1), fb=fval(f,v,b,&o2), m=(a+b)/2, fm=fval(f,v,m,&o3);
	if(!o1||!o2||!o3 || !isfinite(fa)||!isfinite(fb)||!isfinite(fm)){ *ok=0; snprintf(calc_err,sizeof calc_err,"integrand undefined"); return NAN; }
	*ok=1; double whole=(b-a)/6*(fa+4*fm+fb);
	double r = simpson(f,v,a,b,fa,fb,fm,whole,1e-9,40,ok);
	if(!*ok) snprintf(calc_err,sizeof calc_err,"integrand undefined");
	return r;
}

static int dup(double *r, int n, double x){ for(int i=0;i<n;i++) if(fabs(r[i]-x)<1e-6*(1+fabs(x))) return 1; return 0; }

int calc_solve(const cnode *eq, const char *var, double *roots, int maxroots){
	int nr=0; double lo=-200, hi=200; int N=8000;     /* 0.05 resolution over ±200 */
	int ok=0; double px=lo, pf=fval(eq,var,lo,&ok);
	for(int i=1;i<=N && nr<maxroots; i++){
		double x = lo + (hi-lo)*i/(double)N;
		int o; double f = fval(eq,var,x,&o);
		if(o && ok && isfinite(f) && isfinite(pf)){
			double rt; int hit=0;
			if(f==0){ rt=x; hit=1; }
			else if((pf<0&&f>0)||(pf>0&&f<0)){
				double a=px,b=x,fa=pf;                  /* bisection */
				for(int k=0;k<90;k++){ double mid=(a+b)/2; int om; double fmid=fval(eq,var,mid,&om);
					if(!om){ break; } if((fa<0)==(fmid<0)){ a=mid; fa=fmid; } else b=mid; }
				rt=(a+b)/2;
				for(int k=0;k<8;k++){                    /* Newton polish */
					int od; double d=calc_nderiv(eq,var,rt,&od); if(!od||fabs(d)<1e-13) break;
					int of; double fv=fval(eq,var,rt,&of); if(!of) break; double nx=rt-fv/d;
					if(!isfinite(nx)||fabs(nx-rt)>10) break; rt=nx; }
				hit=1;
			}
			if(hit && !dup(roots,nr,rt) && nr<maxroots) roots[nr++]=rt;
		}
		px=x; pf=f; ok=o;
	}
	for(int i=0;i<nr;i++) for(int j=i+1;j<nr;j++) if(roots[j]<roots[i]){ double t=roots[i]; roots[i]=roots[j]; roots[j]=t; }
	return nr;
}

int calc_solve2(const cnode *e1, const cnode *e2, const char *vx, const char *vy, double *sx, double *sy){
	static const double seed[] = {0,1,-1,2,-2,3,-3,5,-5};
	for(unsigned i=0;i<sizeof seed/sizeof seed[0];i++)
	for(unsigned j=0;j<sizeof seed/sizeof seed[0];j++){
		double x=seed[i], y=seed[j];
		for(int it=0; it<80; it++){
			calc_set_var(vx,x); calc_set_var(vy,y);
			int o1,o2; double f1=calc_eval(e1,&o1), f2=calc_eval(e2,&o2);
			if(!o1||!o2||!isfinite(f1)||!isfinite(f2)) break;
			if(fabs(f1)<1e-10 && fabs(f2)<1e-10){ *sx=x; *sy=y; return 1; }
			double h=1e-6; int oo;
			calc_set_var(vx,x+h); double f1x=calc_eval(e1,&oo), f2x=calc_eval(e2,&oo); calc_set_var(vx,x);
			calc_set_var(vy,y+h); double f1y=calc_eval(e1,&oo), f2y=calc_eval(e2,&oo); calc_set_var(vy,y);
			double j11=(f1x-f1)/h, j12=(f1y-f1)/h, j21=(f2x-f2)/h, j22=(f2y-f2)/h;
			double det=j11*j22-j12*j21; if(fabs(det)<1e-13) break;
			double dx=( j22*f1 - j12*f2)/det, dy=(-j21*f1 + j11*f2)/det;
			x-=dx; y-=dy; if(!isfinite(x)||!isfinite(y)||fabs(dx)+fabs(dy)>1e10) break;
		}
	}
	return 0;
}
