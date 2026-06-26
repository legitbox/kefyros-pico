// apps/calc_eval.c — AST nodes, the variable/function environment, the numeric
// evaluator (full scientific function table + constants + angle mode), and number
// formatting. Pure C, uses math.h (-lm). Part of the Kefyros scientific calculator.
#include "calc.h"
#include "calc_num.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ctype.h>

char calc_err[160] = "";
#define KPI  3.14159265358979323846
#define KTAU 6.28318530717958647692
#define KE   2.71828182845904523536
#define KPHI 1.61803398874989484820

/* ===================== AST ===================== */
cnode *cn_new(cn_type t){ cnode *n = calloc(1, sizeof *n); if(n) n->type = t; return n; }
cnode *cn_num(double v){ cnode *n = cn_new(CN_NUM); if(n) n->num = v; return n; }
cnode *cn_var(const char *name){ cnode *n = cn_new(CN_VAR);
	if(n){ strncpy(n->name, name, CN_NAMELEN-1); } return n; }
cnode *cn_bin(char op, cnode *a, cnode *b){ cnode *n = cn_new(CN_BINOP);
	if(n){ n->op = op; n->a = a; n->b = b; } return n; }
cnode *cn_neg(cnode *a){ cnode *n = cn_new(CN_NEG); if(n) n->a = a; return n; }

void cn_free(cnode *n){
	if(!n) return;
	if(n->exact){ cnum_free(n->exact); free(n->exact); }
	cn_free(n->a); cn_free(n->b);
	for(int i=0;i<n->nargs;i++) cn_free(n->args[i]);
	free(n);
}
cnode *cn_clone(const cnode *n){
	if(!n) return NULL;
	cnode *c = cn_new(n->type); if(!c) return NULL;
	c->num = n->num; c->op = n->op; memcpy(c->name, n->name, CN_NAMELEN);
	if(n->exact){ c->exact = malloc(sizeof(cnum)); if(c->exact){ cnum_init(c->exact); cnum_copy(c->exact, n->exact); } }
	c->a = cn_clone(n->a); c->b = cn_clone(n->b);
	c->nargs = n->nargs;
	for(int i=0;i<n->nargs;i++) c->args[i] = cn_clone(n->args[i]);
	return c;
}

/* ===================== environment ===================== */
struct cfun { char name[CN_NAMELEN]; char params[CN_MAXPARAMS][CN_NAMELEN]; int nparams; cnode *body; };
/* val is always set (numeric view); exact!=NULL when the variable holds an exact rational. */
typedef struct cvar { char name[CN_NAMELEN]; double val; cnum *exact; } cvar;

static struct {
	cvar  vars[80]; int nvars;
	struct cfun funs[32]; int nfuns;
	int   angle;
} ENV = { .angle = CALC_RAD };

int  calc_angle(void){ return ENV.angle; }
void calc_set_angle(int m){ ENV.angle = m; }

int calc_get_var(const char *name, double *out){
	for(int i=0;i<ENV.nvars;i++) if(!strcmp(ENV.vars[i].name,name)){ if(out)*out=ENV.vars[i].val; return 1; }
	return 0;
}
/* find an existing slot or make a new one; returns NULL if the table is full. */
static cvar *var_slot(const char *name){
	for(int i=0;i<ENV.nvars;i++) if(!strcmp(ENV.vars[i].name,name)) return &ENV.vars[i];
	if(ENV.nvars >= (int)(sizeof ENV.vars/sizeof ENV.vars[0])) return NULL;
	cvar *s = &ENV.vars[ENV.nvars++];
	strncpy(s->name, name, CN_NAMELEN-1); s->name[CN_NAMELEN-1]=0; s->exact = NULL;
	return s;
}
static void var_clear_exact(cvar *s){ if(s->exact){ cnum_free(s->exact); free(s->exact); s->exact = NULL; } }

void calc_set_var(const char *name, double v){
	cvar *s = var_slot(name);
	if(s){ var_clear_exact(s); s->val = v; }     /* plain numeric assignment drops any exact value */
}
void calc_set_var_exact(const char *name, const struct cnum *x){
	cvar *s = var_slot(name);
	if(!s) return;
	var_clear_exact(s);
	s->val = cnum_to_double(x);
	s->exact = malloc(sizeof(cnum));
	if(s->exact){ cnum_init(s->exact); cnum_copy(s->exact, x); }
}
int calc_get_var_exact(const char *name, struct cnum *out){
	for(int i=0;i<ENV.nvars;i++)
		if(!strcmp(ENV.vars[i].name,name) && ENV.vars[i].exact){ cnum_copy(out, ENV.vars[i].exact); return 1; }
	return 0;
}
const struct cfun *calc_find_fun(const char *name){
	for(int i=0;i<ENV.nfuns;i++) if(!strcmp(ENV.funs[i].name,name)) return &ENV.funs[i];
	return NULL;
}
void calc_def_fun(const char *name, char params[][CN_NAMELEN], int nparams, const cnode *body){
	struct cfun *f = NULL;
	for(int i=0;i<ENV.nfuns;i++) if(!strcmp(ENV.funs[i].name,name)) f = &ENV.funs[i];
	if(!f){ if(ENV.nfuns >= (int)(sizeof ENV.funs/sizeof ENV.funs[0])) return; f = &ENV.funs[ENV.nfuns++]; f->body = NULL; }
	cn_free(f->body);
	strncpy(f->name, name, CN_NAMELEN-1); f->name[CN_NAMELEN-1]=0;
	f->nparams = nparams;
	for(int i=0;i<nparams && i<CN_MAXPARAMS;i++){ strncpy(f->params[i], params[i], CN_NAMELEN-1); f->params[i][CN_NAMELEN-1]=0; }
	f->body = cn_clone(body);
}
void calc_reset_env(void){
	for(int i=0;i<ENV.nfuns;i++){ cn_free(ENV.funs[i].body); ENV.funs[i].body=NULL; }
	for(int i=0;i<ENV.nvars;i++) var_clear_exact(&ENV.vars[i]);
	ENV.nvars = 0; ENV.nfuns = 0; ENV.angle = CALC_RAD;
}

/* ===================== eval helpers ===================== */
static double to_rad(double x){ return ENV.angle==CALC_DEG ? x*KPI/180.0 : ENV.angle==CALC_GRAD ? x*KPI/200.0 : x; }
static double from_rad(double x){ return ENV.angle==CALC_DEG ? x*180.0/KPI : ENV.angle==CALC_GRAD ? x*200.0/KPI : x; }
static void err(const char *fmt, const char *s){ snprintf(calc_err, sizeof calc_err, fmt, s); }

static long gcdl(long a, long b){ a=labs(a); b=labs(b); while(b){ long t=a%b; a=b; b=t; } return a; }

/* nCr / nPr (real n,r via gamma for non-integers) */
static double ncr(double n, double r){ return tgamma(n+1.0)/(tgamma(r+1.0)*tgamma(n-r+1.0)); }
static double npr(double n, double r){ return tgamma(n+1.0)/tgamma(n-r+1.0); }

static int g_depth = 0;

static double call_builtin(const char *nm, double *v, int n, int *ok);

double calc_eval(const cnode *node, int *ok){
	if(!node){ *ok=0; err("%s","empty"); return NAN; }
	switch(node->type){
	case CN_NUM: return node->num;
	case CN_NEG: return -calc_eval(node->a, ok);
	case CN_FACT: { double a = calc_eval(node->a, ok); return tgamma(a+1.0); }
	case CN_BINOP: {
		double a = calc_eval(node->a, ok), b = calc_eval(node->b, ok);
		switch(node->op){
		case '+': return a+b; case '-': return a-b; case '*': return a*b;
		case '/': return a/b; case '%': return fmod(a,b); case '^': return pow(a,b);
		}
		*ok=0; err("bad op '%s'", (char[]){node->op,0}); return NAN;
	}
	case CN_EQ: /* an equation evaluated numerically = lhs-rhs (for solver/plot) */
		return calc_eval(node->a, ok) - calc_eval(node->b, ok);
	case CN_VAR: {
		const char *nm = node->name; double v;
		if(calc_get_var(nm, &v)) return v;
		if(!strcmp(nm,"pi")||!strcmp(nm,"\xcf\x80")) return KPI;
		if(!strcmp(nm,"e"))   return KE;
		if(!strcmp(nm,"tau")) return KTAU;
		if(!strcmp(nm,"phi")) return KPHI;
		if(!strcmp(nm,"inf")) return INFINITY;
		if(!strcmp(nm,"nan")) return NAN;
		*ok=0; err("undefined: %s", nm); return NAN;
	}
	case CN_CALL: {
		double v[CN_MAXARGS]; int n = node->nargs;
		for(int i=0;i<n;i++) v[i] = calc_eval(node->args[i], ok);
		/* user-defined function? */
		const struct cfun *f = calc_find_fun(node->name);
		if(f){
			if(n != f->nparams){ *ok=0; err("arg count: %s", node->name); return NAN; }
			if(++g_depth > 180){ g_depth--; *ok=0; err("%s","recursion too deep"); return NAN; }
			double saved[CN_MAXPARAMS]; int had[CN_MAXPARAMS];
			for(int i=0;i<f->nparams;i++){ had[i]=calc_get_var(f->params[i],&saved[i]); calc_set_var(f->params[i], v[i]); }
			double r = calc_eval(f->body, ok);
			for(int i=0;i<f->nparams;i++){ if(had[i]) calc_set_var(f->params[i], saved[i]); }
			g_depth--;
			return r;
		}
		return call_builtin(node->name, v, n, ok);
	}
	}
	*ok=0; err("%s","bad node"); return NAN;
}

/* big scientific function table */
static double call_builtin(const char *nm, double *v, int n, int *ok){
	#define A1 (n>=1?v[0]:NAN)
	#define A2 (n>=2?v[1]:NAN)
	#define NEED(k) do{ if(n!=(k)){ *ok=0; err("arg count: %s", nm); return NAN; } }while(0)
	/* force a decimal: these aren't in the exact table, so the whole expression drops to the
	   numeric path. float(1/3) -> 0.3333333333; N/dec/approx are aliases (Python/SymPy). */
	if(!strcmp(nm,"float")||!strcmp(nm,"dec")||!strcmp(nm,"approx")||!strcmp(nm,"N")){ NEED(1); return A1; }
	/* trig (angle-aware) */
	if(!strcmp(nm,"sin")){ NEED(1); return sin(to_rad(A1)); }
	if(!strcmp(nm,"cos")){ NEED(1); return cos(to_rad(A1)); }
	if(!strcmp(nm,"tan")){ NEED(1); return tan(to_rad(A1)); }
	if(!strcmp(nm,"asin")){ NEED(1); return from_rad(asin(A1)); }
	if(!strcmp(nm,"acos")){ NEED(1); return from_rad(acos(A1)); }
	if(!strcmp(nm,"atan")){ NEED(1); return from_rad(atan(A1)); }
	if(!strcmp(nm,"atan2")){ NEED(2); return from_rad(atan2(A1,A2)); }
	if(!strcmp(nm,"sec")){ NEED(1); return 1.0/cos(to_rad(A1)); }
	if(!strcmp(nm,"csc")){ NEED(1); return 1.0/sin(to_rad(A1)); }
	if(!strcmp(nm,"cot")){ NEED(1); return 1.0/tan(to_rad(A1)); }
	/* hyperbolic */
	if(!strcmp(nm,"sinh")){ NEED(1); return sinh(A1); }
	if(!strcmp(nm,"cosh")){ NEED(1); return cosh(A1); }
	if(!strcmp(nm,"tanh")){ NEED(1); return tanh(A1); }
	if(!strcmp(nm,"asinh")){ NEED(1); return asinh(A1); }
	if(!strcmp(nm,"acosh")){ NEED(1); return acosh(A1); }
	if(!strcmp(nm,"atanh")){ NEED(1); return atanh(A1); }
	/* logs / exp / powers */
	if(!strcmp(nm,"ln")){ NEED(1); return log(A1); }
	if(!strcmp(nm,"log")){ if(n==1) return log10(A1); if(n==2) return log(A2)/log(A1); *ok=0; err("arg count: %s",nm); return NAN; }
	if(!strcmp(nm,"log2")){ NEED(1); return log2(A1); }
	if(!strcmp(nm,"log10")){ NEED(1); return log10(A1); }
	if(!strcmp(nm,"exp")){ NEED(1); return exp(A1); }
	if(!strcmp(nm,"sqrt")){ NEED(1); return sqrt(A1); }
	if(!strcmp(nm,"cbrt")){ NEED(1); return cbrt(A1); }
	if(!strcmp(nm,"pow")){ NEED(2); return pow(A1,A2); }
	if(!strcmp(nm,"root")){ NEED(2); return pow(A1, 1.0/A2); }           /* root(x,n) */
	if(!strcmp(nm,"nthroot")){ NEED(2); return pow(A2, 1.0/A1); }        /* nthroot(n,x) */
	if(!strcmp(nm,"hypot")){ NEED(2); return hypot(A1,A2); }
	/* rounding / misc */
	if(!strcmp(nm,"abs")){ NEED(1); return fabs(A1); }
	if(!strcmp(nm,"sign")){ NEED(1); return (A1>0)-(A1<0); }
	if(!strcmp(nm,"floor")){ NEED(1); return floor(A1); }
	if(!strcmp(nm,"ceil")){ NEED(1); return ceil(A1); }
	if(!strcmp(nm,"round")){ NEED(1); return round(A1); }
	if(!strcmp(nm,"trunc")){ NEED(1); return trunc(A1); }
	if(!strcmp(nm,"frac")){ NEED(1); return A1 - trunc(A1); }
	if(!strcmp(nm,"mod")){ NEED(2); return fmod(A1,A2); }
	if(!strcmp(nm,"deg")){ NEED(1); return A1*180.0/KPI; }
	if(!strcmp(nm,"rad")){ NEED(1); return A1*KPI/180.0; }
	/* combinatorics / gamma */
	if(!strcmp(nm,"fact")||!strcmp(nm,"factorial")){ NEED(1); return tgamma(A1+1.0); }
	if(!strcmp(nm,"gamma")){ NEED(1); return tgamma(A1); }
	if(!strcmp(nm,"ncr")||!strcmp(nm,"comb")||!strcmp(nm,"nCr")){ NEED(2); return ncr(A1,A2); }
	if(!strcmp(nm,"npr")||!strcmp(nm,"perm")||!strcmp(nm,"nPr")){ NEED(2); return npr(A1,A2); }
	/* gcd / lcm (integer) */
	if(!strcmp(nm,"gcd")){ NEED(2); return (double)gcdl((long)A1,(long)A2); }
	if(!strcmp(nm,"lcm")){ NEED(2); long g=gcdl((long)A1,(long)A2); return g? fabs(A1*A2)/g : 0; }
	/* min/max fold */
	if(!strcmp(nm,"min")){ if(n<1){*ok=0;err("arg count: %s",nm);return NAN;} double m=v[0]; for(int i=1;i<n;i++) if(v[i]<m)m=v[i]; return m; }
	if(!strcmp(nm,"max")){ if(n<1){*ok=0;err("arg count: %s",nm);return NAN;} double m=v[0]; for(int i=1;i<n;i++) if(v[i]>m)m=v[i]; return m; }
	*ok=0; err("unknown function: %s", nm); return NAN;
	#undef A1
	#undef A2
	#undef NEED
}

/* ===================== format ===================== */
void calc_fmt(double v, char *out, int outsz){
	if(isnan(v)){ snprintf(out,outsz,"undefined"); return; }
	if(isinf(v)){ snprintf(out,outsz, v<0?"-inf":"inf"); return; }
	if(v==0) v = 0; /* kill -0 */
	/* integers up to 1e15 print without a dot */
	if(fabs(v) < 1e15 && v == floor(v)){ snprintf(out,outsz,"%.0f",v); return; }
	snprintf(out,outsz,"%.10g",v);
}
