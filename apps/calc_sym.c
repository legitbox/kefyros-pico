// apps/calc_sym.c — light symbolic engine: AST pretty-printer, differentiation,
// simplification, expansion (canonical single-variable poly), and polynomial factoring.
// Differentiation assumes RADIANS (standard calculus). Pure C.
#include "calc.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.71828182845904523536
#endif

/* ============ pretty-printer (AST -> infix string) ============ */
static int prec(const cnode *n){
	if(!n) return 9;
	if(n->type==CN_EQ) return 0;
	if(n->type==CN_BINOP){ switch(n->op){ case '+': case '-': return 1; case '*': case '/': case '%': return 2; case '^': return 4; } }
	if(n->type==CN_NEG) return 3;
	return 9;
}
static void ap(char *o,int *p,int sz,const char *s){ while(*s && *p<sz-1) o[(*p)++]=*s++; o[*p]=0; }
static void pr(const cnode *n, char *o, int *p, int sz){
	if(!n) return;
	switch(n->type){
	case CN_NUM:{ char b[40]; calc_fmt(n->num,b,sizeof b); ap(o,p,sz,b); break; }
	case CN_VAR: ap(o,p,sz,n->name); break;
	case CN_NEG:{ ap(o,p,sz,"-"); int c=prec(n->a); if(c<3){ ap(o,p,sz,"("); pr(n->a,o,p,sz); ap(o,p,sz,")"); } else pr(n->a,o,p,sz); break; }
	case CN_FACT:{ int c=prec(n->a); if(c<9){ ap(o,p,sz,"("); pr(n->a,o,p,sz); ap(o,p,sz,")"); } else pr(n->a,o,p,sz); ap(o,p,sz,"!"); break; }
	case CN_BINOP:{ int mp=prec(n); char ops[2]={n->op,0};
		int lp=prec(n->a); if(lp<mp){ ap(o,p,sz,"("); pr(n->a,o,p,sz); ap(o,p,sz,")"); } else pr(n->a,o,p,sz);
		ap(o,p,sz,ops);
		int rp=prec(n->b), needp = rp<mp || (rp==mp && (n->op=='-'||n->op=='/'||n->op=='%'||n->op=='^'));
		if(needp){ ap(o,p,sz,"("); pr(n->b,o,p,sz); ap(o,p,sz,")"); } else pr(n->b,o,p,sz); break; }
	case CN_CALL:{ ap(o,p,sz,n->name); ap(o,p,sz,"("); for(int i=0;i<n->nargs;i++){ if(i) ap(o,p,sz,","); pr(n->args[i],o,p,sz); } ap(o,p,sz,")"); break; }
	case CN_EQ:{ pr(n->a,o,p,sz); ap(o,p,sz,"="); pr(n->b,o,p,sz); break; }
	}
}
void calc_sym_str(const cnode *n, char *out, int outsz){ int p=0; if(outsz>0) out[0]=0; pr(n,out,&p,outsz); }

/* ============ builders + helpers ============ */
static cnode *num(double v){ return cn_num(v); }
static cnode *bin(char op,cnode*a,cnode*b){ return cn_bin(op,a,b); }
static cnode *neg(cnode*a){ return cn_neg(a); }
static cnode *vnode(const char*n){ return cn_var(n); }
static cnode *call1(const char*nm,cnode*a){ cnode*c=cn_new(CN_CALL); strncpy(c->name,nm,CN_NAMELEN-1); c->args[0]=a; c->nargs=1; return c; }
static int isnum(const cnode*n,double*v){ if(n&&n->type==CN_NUM){ if(v)*v=n->num; return 1; } return 0; }
static int constval(const char*n,double*v){
	if(!strcmp(n,"pi")){*v=M_PI;return 1;} if(!strcmp(n,"e")){*v=M_E;return 1;}
	if(!strcmp(n,"tau")){*v=2*M_PI;return 1;} if(!strcmp(n,"phi")){*v=1.61803398874989484820;return 1;} return 0;
}
static int same(const cnode*a,const cnode*b){
	if(a==b) return 1; if(!a||!b||a->type!=b->type) return 0;
	switch(a->type){
	case CN_NUM: return a->num==b->num;
	case CN_VAR: return !strcmp(a->name,b->name);
	case CN_BINOP: return a->op==b->op && same(a->a,b->a) && same(a->b,b->b);
	case CN_NEG: case CN_FACT: return same(a->a,b->a);
	case CN_EQ: return same(a->a,b->a) && same(a->b,b->b);
	case CN_CALL: if(strcmp(a->name,b->name)||a->nargs!=b->nargs) return 0;
		for(int i=0;i<a->nargs;i++) if(!same(a->args[i],b->args[i])) return 0; return 1;
	}
	return 0;
}
static int hasv(const cnode*n,const char*v){
	if(!n) return 0; if(n->type==CN_VAR) return !strcmp(n->name,v);
	if(hasv(n->a,v)||hasv(n->b,v)) return 1; for(int i=0;i<n->nargs;i++) if(hasv(n->args[i],v)) return 1; return 0;
}

/* ============ simplify ============ */
static cnode *simp(const cnode*n){
	if(!n) return NULL;
	switch(n->type){
	case CN_NUM: return num(n->num);
	case CN_VAR: return vnode(n->name);
	case CN_NEG:{ cnode*a=simp(n->a); double v; if(isnum(a,&v)){ cn_free(a); return num(-v); }
		if(a->type==CN_NEG){ cnode*r=cn_clone(a->a); cn_free(a); return r; } return neg(a); }
	case CN_FACT:{ cnode*a=simp(n->a); double v; if(isnum(a,&v)){ cn_free(a); return num(tgamma(v+1)); } cnode*f=cn_new(CN_FACT); f->a=a; return f; }
	case CN_EQ:{ cnode*e=cn_new(CN_EQ); e->a=simp(n->a); e->b=simp(n->b); return e; }
	case CN_CALL:{ cnode*c=cn_new(CN_CALL); strncpy(c->name,n->name,CN_NAMELEN-1); c->nargs=n->nargs; int alln=1;
		for(int i=0;i<n->nargs;i++){ c->args[i]=simp(n->args[i]); if(c->args[i]->type!=CN_NUM) alln=0; }
		if(alln && n->nargs>0){ int ok=1; double r=calc_eval(c,&ok); if(ok&&isfinite(r)){ cn_free(c); return num(r); } } return c; }
	case CN_BINOP:{
		cnode*a=simp(n->a), *b=simp(n->b); char op=n->op; double va,vb; int na=isnum(a,&va), nb=isnum(b,&vb);
		if(na&&nb){ double r=0; switch(op){case '+':r=va+vb;break;case '-':r=va-vb;break;case '*':r=va*vb;break;
			case '/':r=va/vb;break;case '^':r=pow(va,vb);break;case '%':r=fmod(va,vb);break;} cn_free(a);cn_free(b); return num(r); }
		switch(op){
		case '+': if(na&&va==0){cn_free(a);return b;} if(nb&&vb==0){cn_free(b);return a;} if(same(a,b)){cn_free(b);return bin('*',num(2),a);} break;
		case '-': if(nb&&vb==0){cn_free(b);return a;} if(na&&va==0){cn_free(a);return neg(b);} if(same(a,b)){cn_free(a);cn_free(b);return num(0);} break;
		case '*': if((na&&va==0)||(nb&&vb==0)){cn_free(a);cn_free(b);return num(0);}
			if(na&&va==1){cn_free(a);return b;} if(nb&&vb==1){cn_free(b);return a;}
			if(na&&va==-1){cn_free(a);return neg(b);} if(nb&&vb==-1){cn_free(b);return neg(a);} break;
		case '/': if(nb&&vb==1){cn_free(b);return a;} if(na&&va==0){cn_free(a);cn_free(b);return num(0);} if(same(a,b)){cn_free(a);cn_free(b);return num(1);} break;
		case '^': if(nb&&vb==1){cn_free(b);return a;} if(nb&&vb==0){cn_free(a);cn_free(b);return num(1);}
			if(na&&va==1){cn_free(a);cn_free(b);return num(1);} if(na&&va==0){cn_free(a);cn_free(b);return num(0);} break;
		}
		return bin(op,a,b);
	}
	}
	return cn_clone(n);
}
cnode *calc_simplify(const cnode*n){ return simp(n); }

/* ============ differentiate (radians) ============ */
static cnode *dd(const cnode*n,const char*v){
	switch(n->type){
	case CN_NUM: return num(0);
	case CN_VAR: return num(!strcmp(n->name,v)?1:0);
	case CN_NEG: return neg(dd(n->a,v));
	case CN_FACT: return num(0);
	case CN_EQ: return bin('-', dd(n->a,v), dd(n->b,v));
	case CN_BINOP:{ cnode*a=n->a,*b=n->b;
		switch(n->op){
		case '+': return bin('+',dd(a,v),dd(b,v));
		case '-': return bin('-',dd(a,v),dd(b,v));
		case '*': return bin('+', bin('*',dd(a,v),cn_clone(b)), bin('*',cn_clone(a),dd(b,v)));
		case '/': return bin('/', bin('-', bin('*',dd(a,v),cn_clone(b)), bin('*',cn_clone(a),dd(b,v))), bin('^',cn_clone(b),num(2)));
		case '^':{ int av=hasv(a,v), bv=hasv(b,v);
			if(!bv) return bin('*', bin('*', cn_clone(b), bin('^',cn_clone(a),bin('-',cn_clone(b),num(1)))), dd(a,v));
			if(!av) return bin('*', bin('*', bin('^',cn_clone(a),cn_clone(b)), call1("ln",cn_clone(a))), dd(b,v));
			return bin('*', bin('^',cn_clone(a),cn_clone(b)), bin('+', bin('*',dd(b,v),call1("ln",cn_clone(a))), bin('/', bin('*',cn_clone(b),dd(a,v)), cn_clone(a)))); }
		default: return num(0);
		}
	}
	case CN_CALL:{
		if(n->nargs!=1) return num(0);
		const cnode*u=n->args[0]; const char*f=n->name; cnode*outer=NULL;
		if(!strcmp(f,"sin")) outer=call1("cos",cn_clone(u));
		else if(!strcmp(f,"cos")) outer=neg(call1("sin",cn_clone(u)));
		else if(!strcmp(f,"tan")) outer=bin('/',num(1),bin('^',call1("cos",cn_clone(u)),num(2)));
		else if(!strcmp(f,"exp")) outer=call1("exp",cn_clone(u));
		else if(!strcmp(f,"ln")) outer=bin('/',num(1),cn_clone(u));
		else if(!strcmp(f,"log")||!strcmp(f,"log10")) outer=bin('/',num(1),bin('*',cn_clone(u),call1("ln",num(10))));
		else if(!strcmp(f,"log2")) outer=bin('/',num(1),bin('*',cn_clone(u),call1("ln",num(2))));
		else if(!strcmp(f,"sqrt")) outer=bin('/',num(1),bin('*',num(2),call1("sqrt",cn_clone(u))));
		else if(!strcmp(f,"cbrt")) outer=bin('/',num(1),bin('*',num(3),bin('^',call1("cbrt",cn_clone(u)),num(2))));
		else if(!strcmp(f,"asin")) outer=bin('/',num(1),call1("sqrt",bin('-',num(1),bin('^',cn_clone(u),num(2)))));
		else if(!strcmp(f,"acos")) outer=neg(bin('/',num(1),call1("sqrt",bin('-',num(1),bin('^',cn_clone(u),num(2))))));
		else if(!strcmp(f,"atan")) outer=bin('/',num(1),bin('+',num(1),bin('^',cn_clone(u),num(2))));
		else if(!strcmp(f,"sinh")) outer=call1("cosh",cn_clone(u));
		else if(!strcmp(f,"cosh")) outer=call1("sinh",cn_clone(u));
		else if(!strcmp(f,"tanh")) outer=bin('-',num(1),bin('^',call1("tanh",cn_clone(u)),num(2)));
		else if(!strcmp(f,"abs")) outer=call1("sign",cn_clone(u));
		else return num(0);
		return bin('*', outer, dd(u,v));
	}
	}
	return num(0);
}
cnode *calc_diff(const cnode*n,const char*v){ cnode*d=dd(n,v); cnode*s=simp(d); cn_free(d); return s; }

/* ============ expand ============ */
static cnode *mul_expand(const cnode*a,const cnode*b){
	if(a->type==CN_BINOP && (a->op=='+'||a->op=='-')) return bin(a->op, mul_expand(a->a,b), mul_expand(a->b,b));
	if(b->type==CN_BINOP && (b->op=='+'||b->op=='-')) return bin(b->op, mul_expand(a,b->a), mul_expand(a,b->b));
	return bin('*', cn_clone(a), cn_clone(b));
}
static cnode *expnd(const cnode*n){
	if(!n) return NULL;
	if(n->type==CN_BINOP){
		if(n->op=='+'||n->op=='-') return bin(n->op, expnd(n->a), expnd(n->b));
		if(n->op=='*'){ cnode*a=expnd(n->a),*b=expnd(n->b); cnode*r=mul_expand(a,b); cn_free(a);cn_free(b); return r; }
		if(n->op=='^' && n->b->type==CN_NUM && n->b->num==floor(n->b->num) && n->b->num>=2 && n->b->num<=10){
			int k=(int)n->b->num; cnode*base=expnd(n->a); cnode*acc=cn_clone(base);
			for(int i=1;i<k;i++){ cnode*t=mul_expand(acc,base); cn_free(acc); acc=t; } cn_free(base); return acc; }
		return bin(n->op, expnd(n->a), expnd(n->b));
	}
	if(n->type==CN_NEG) return neg(expnd(n->a));
	return cn_clone(n);
}
static cnode *expand_raw(const cnode*n){ cnode*e=expnd(n); cnode*s=simp(e); cn_free(e); return s; }

/* single-var polynomial: monomial coeff/degree + additive collection */
static int term_cd(const cnode*n,const char*v,double*c,int*d){
	double cv;
	if(n->type==CN_NUM){ *c=n->num; *d=0; return 1; }
	if(n->type==CN_VAR){ if(!strcmp(n->name,v)){*c=1;*d=1;return 1;} if(constval(n->name,&cv)){*c=cv;*d=0;return 1;} return 0; }
	if(n->type==CN_NEG){ if(term_cd(n->a,v,c,d)){ *c=-*c; return 1; } return 0; }
	if(n->type==CN_BINOP){
		double c1,c2; int d1,d2;
		if(n->op=='*'){ if(term_cd(n->a,v,&c1,&d1)&&term_cd(n->b,v,&c2,&d2)){*c=c1*c2;*d=d1+d2;return 1;} return 0; }
		if(n->op=='/'){ if(term_cd(n->a,v,&c1,&d1)&&term_cd(n->b,v,&c2,&d2)&&d2==0&&c2!=0){*c=c1/c2;*d=d1;return 1;} return 0; }
		if(n->op=='^'){ if(n->a->type==CN_VAR&&!strcmp(n->a->name,v)&&n->b->type==CN_NUM&&n->b->num==floor(n->b->num)&&n->b->num>=0){*c=1;*d=(int)n->b->num;return 1;}
			if(term_cd(n->a,v,&c1,&d1)&&d1==0&&n->b->type==CN_NUM&&n->b->num==floor(n->b->num)&&n->b->num>=0){*c=pow(c1,n->b->num);*d=0;return 1;} return 0; }
	}
	return 0;
}
static int collect(const cnode*n,const char*v,double*coef,int maxd,int sign){
	if(n->type==CN_BINOP&&n->op=='+') return collect(n->a,v,coef,maxd,sign)&&collect(n->b,v,coef,maxd,sign);
	if(n->type==CN_BINOP&&n->op=='-') return collect(n->a,v,coef,maxd,sign)&&collect(n->b,v,coef,maxd,-sign);
	if(n->type==CN_NEG) return collect(n->a,v,coef,maxd,-sign);
	double c; int d; if(term_cd(n,v,&c,&d)){ if(d>maxd) return 0; coef[d]+=sign*c; return 1; }
	return 0;
}
static int poly_collect(const cnode*expanded,const char*v,double*coef,int maxd){
	for(int i=0;i<=maxd;i++) coef[i]=0;
	if(!collect(expanded,v,coef,maxd,1)) return -1;
	int d=0; for(int i=0;i<=maxd;i++) if(fabs(coef[i])>1e-12) d=i; return d;
}
static int detect_var(const cnode*n,char*out){
	if(!n) return 0; double cv;
	if(n->type==CN_VAR && !constval(n->name,&cv)){ strncpy(out,n->name,CN_NAMELEN-1); out[CN_NAMELEN-1]=0; return 1; }
	if(detect_var(n->a,out)||detect_var(n->b,out)) return 1;
	for(int i=0;i<n->nargs;i++) if(detect_var(n->args[i],out)) return 1; return 0;
}
static cnode *mono(double c,int d,const char*v){       /* |c|>0 ; builds c*v^d */
	cnode *p = d==0?NULL : d==1?vnode(v) : bin('^',vnode(v),num(d));
	if(!p) return num(c);
	if(c==1) return p; if(c==-1) return neg(p); return bin('*',num(c),p);
}
static cnode *build_poly(const double*coef,int deg,const char*v){
	cnode *acc=NULL;
	for(int d=deg; d>=0; d--){
		double c=coef[d]; if(fabs(c)<1e-12) continue;
		if(!acc) acc=mono(c,d,v);
		else if(c<0) acc=bin('-', acc, mono(-c,d,v));
		else acc=bin('+', acc, mono(c,d,v));
	}
	return acc?acc:num(0);
}
cnode *calc_expand(const cnode*n){
	cnode *s=expand_raw(n); char v[CN_NAMELEN];
	if(detect_var(s,v)){ double coef[20]; int d=poly_collect(s,v,coef,19); if(d>=0){ cnode*p=build_poly(coef,d,v); if(p){ cn_free(s); return p; } } }
	return s;
}

/* ============ factor (real linear factors of a 1-var polynomial) ============ */
static double peval(const double*a,int deg,double x){ double r=0; for(int i=deg;i>=0;i--) r=r*x+a[i]; return r; }
static void deflate(double*a,int*deg,double r){        /* a[] ascending; divide by (x-r) */
	int d=*deg; double qd[24]; qd[0]=a[d];
	for(int i=1;i<d;i++) qd[i]=a[d-i]+r*qd[i-1];
	for(int i=0;i<d;i++) a[i]=qd[d-1-i];
	a[d]=0; *deg=d-1;
}
cnode *calc_factor(const cnode*n,const char*vn){
	cnode *s=expand_raw(n); double coef[20]; int deg=poly_collect(s,vn,coef,19);
	if(deg<2) return s;
	double w[24]; int wd=deg; for(int i=0;i<=deg;i++) w[i]=coef[i];
	double lead=coef[deg], roots[24]; int nr=0;
	double lo=-60, hi=60; int N=6000, prevsign=0; double px=lo, pf=peval(w,wd,lo);
	(void)prevsign;
	for(int i=1;i<=N && wd>0; i++){
		double x=lo+(hi-lo)*i/(double)N, f=peval(w,wd,x);
		if(f==0 || (pf<0&&f>0) || (pf>0&&f<0)){
			double a=px,b=x,fa=pf; for(int k=0;k<80;k++){ double m=(a+b)/2,fm=peval(w,wd,m); if((fa<0)==(fm<0)){a=m;fa=fm;}else b=m; }
			double rt=round((a+b)/2*1e6)/1e6;
			while(wd>0 && fabs(peval(w,wd,rt))<1e-5){ if(nr<24) roots[nr++]=rt; deflate(w,&wd,rt); }
			px=x; pf=peval(w,wd,x);
		} else { px=x; pf=f; }
	}
	if(nr==0) return s;
	cn_free(s);
	cnode *acc = (lead!=1)? num(lead) : NULL;
	for(int i=0;i<nr;i++){ cnode *fac = roots[i]<0 ? bin('+', vnode(vn), num(-roots[i])) : bin('-', vnode(vn), num(roots[i]));
		acc = acc? bin('*',acc,fac) : fac; }
	if(wd>0){ cnode *rem=build_poly(w,wd,vn); acc = acc? bin('*',acc,rem) : rem; }
	return acc?acc:num(lead);
}
