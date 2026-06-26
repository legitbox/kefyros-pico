// apps/calc_exact.c — exact rational evaluator (see calc_exact.h).
// Covers the part of the language that stays in Q: rational literals, + - * / % , integer
// powers, factorial, and the exactness-preserving builtins (abs/sign/floor/ceil/round/trunc/
// gcd/lcm/mod/min/max/ncr/npr/pow). Anything else (decimals, pi/e, variables, sqrt of a
// non-square, trig, …) returns 0 so the caller uses the numeric path. Irrational simplify
// (√8 -> 2√2, sin(pi/6) -> 1/2) is V2.0-b, not here.
#include "calc_exact.h"
#include "calc_bignum.h"
#include <string.h>

/* ---- small helpers on exact values ---- */

#define EXACT_WORK_CAP 20000      /* max iterations for factorial/ncr/npr (RAM + time guard) */

static int as_nonneg_i64(const cnum *x, int64_t *out){
	if(!cnum_is_int(x) || x->p.sign < 0 || !bi_fits_i64(&x->p)) return 0;
	*out = bi_to_i64(&x->p); return 1;
}
/* decline a^e in the exact path when the result would be enormous: result bits grow as
   |e| * bitlen(base). Cap at ~1e6 bits (~125 KB / ~300k digits) so the numeric path takes it. */
static int pow_too_big(const cnum *a, int64_t e){
	int64_t ae = e < 0 ? -e : e;
	int bbits = (a->p.n > a->q.n ? a->p.n : a->q.n) * 32;
	if(bbits < 1) bbits = 1;
	return (double)ae * (double)bbits > 1.0e6;
}

static void ratfloor(bigint *out, const cnum *x){       /* floor(p/q), q>0 */
	bigint rt; bi_init(&rt);
	bi_divmod(out, &rt, &x->p, &x->q);                  /* trunc toward zero */
	if(!bi_is_zero(&rt) && x->p.sign < 0){
		bigint one; bi_init(&one); bi_set_i64(&one, 1);
		bi_sub(out, out, &one); bi_free(&one);
	}
	bi_free(&rt);
}
static void set_int(cnum *out, const bigint *v){ bi_copy(&out->p, v); bi_set_i64(&out->q, 1); }

static void bi_fact(bigint *r, long n){
	bigint t; bi_init(&t); bi_set_i64(r, 1);
	for(long i = 2; i <= n; i++){ bi_set_i64(&t, i); bi_mul(r, r, &t); }
	bi_free(&t);
}
/* n!/(r!(n-r)!) via the multiplicative formula with exact (even) divisions */
static int bi_ncr(bigint *out, long n, long r){
	if(r < 0 || r > n){ bi_set_i64(out, 0); return 1; }
	if(r > n - r) r = n - r;
	bigint num, den, t; bi_init(&num); bi_init(&den); bi_init(&t);
	bi_set_i64(out, 1);
	for(long i = 1; i <= r; i++){
		bi_set_i64(&t, n - r + i); bi_mul(out, out, &t);   /* *(n-r+i) */
		bi_set_i64(&t, i);         bi_divmod(out, NULL, out, &t); /* /i  (always exact) */
	}
	bi_free(&num); bi_free(&den); bi_free(&t);
	return 1;
}

/* ---- exact trig of special angles (rational results only; Niven's theorem) ---- */

/* match a node to r*pi with r rational: pi, c*pi, pi/n, c*pi/n, sums/negs. 1 on success. */
static int match_pi(const cnode *n, cnum *r){
	if(!n) return 0;
	if(n->type==CN_VAR && (!strcmp(n->name,"pi")||!strcmp(n->name,"\xcf\x80"))){ cnum_set_i64(r,1); return 1; }
	if(n->type==CN_NEG){ if(!match_pi(n->a,r)) return 0; cnum_neg(r); return 1; }
	if(n->type==CN_BINOP){
		if(n->op=='*'){
			cnum c; cnum_init(&c); int ok=0;
			if(calc_eval_exact(n->a,&c) && match_pi(n->b,r)){ cnum_mul(r,r,&c); ok=1; }
			else if(match_pi(n->a,r) && calc_eval_exact(n->b,&c)){ cnum_mul(r,r,&c); ok=1; }
			cnum_free(&c); return ok;
		}
		if(n->op=='/'){
			cnum d; cnum_init(&d); int ok=0;
			if(match_pi(n->a,r) && calc_eval_exact(n->b,&d) && !cnum_is_zero(&d)){ cnum_div(r,r,&d); ok=1; }
			cnum_free(&d); return ok;
		}
		if(n->op=='+'||n->op=='-'){
			cnum rb; cnum_init(&rb); int ok=0;
			if(match_pi(n->a,r) && match_pi(n->b,&rb)){ if(n->op=='+') cnum_add(r,r,&rb); else cnum_sub(r,r,&rb); ok=1; }
			cnum_free(&rb); return ok;
		}
	}
	return 0;
}
/* the trig argument as a multiple of pi ("half-turns"), honouring the angle mode. */
static int arg_half_turns(const cnode *n, cnum *r){
	int mode = calc_angle();
	if(mode==CALC_RAD){
		if(match_pi(n, r)) return 1;
		cnum a; cnum_init(&a); int z = 0;     /* a bare 0 angle is exact (sin0=0, cos0=1) */
		if(calc_eval_exact(n, &a) && cnum_is_zero(&a)){ cnum_set_i64(r, 0); z = 1; }
		cnum_free(&a); return z;
	}
	cnum a; cnum_init(&a);
	int ok = calc_eval_exact(n, &a);          /* DEG/GRAD: arg is a plain number of deg/grad */
	if(ok){ cnum d; cnum_init(&d); cnum_set_i64(&d, mode==CALC_DEG ? 180 : 200); cnum_div(r,&a,&d); cnum_free(&d); }
	cnum_free(&a); return ok;
}
/* out = r mod m, in [0,m) */
static void rat_mod(cnum *out, const cnum *r, int m){
	cnum md; cnum_init(&md); cnum_set_i64(&md, m);
	cnum q; cnum_init(&q); cnum_div(&q, r, &md);
	bigint fl; bi_init(&fl); ratfloor(&fl, &q);
	cnum f; cnum_init(&f); set_int(&f, &fl);
	cnum_mul(&f, &f, &md); cnum_sub(out, r, &f);
	cnum_free(&md); cnum_free(&q); bi_free(&fl); cnum_free(&f);
}
static int eq_frac(const cnum *s, long a, long b){
	cnum f, d; cnum_init(&f); cnum_init(&d);
	cnum_set_i64(&f, a); cnum_set_i64(&d, b); cnum_div(&f, &f, &d);
	int e = (cnum_cmp(s, &f) == 0); cnum_free(&f); cnum_free(&d); return e;
}
static void set_half(cnum *out, int sign){ cnum_set_i64(out, sign); cnum d; cnum_init(&d); cnum_set_i64(&d,2); cnum_div(out,out,&d); cnum_free(&d); }
/* sin(r*pi): rational only at 0, +-1/2, +-1 (s = r mod 2 in [0,2)) */
static int sin_pi(const cnum *r, cnum *out){
	cnum s; cnum_init(&s); rat_mod(&s, r, 2); int got = 1;
	if(cnum_is_int(&s))                       cnum_set_i64(out, 0);    /* 0, 1 -> 0 */
	else if(eq_frac(&s,1,2))                  cnum_set_i64(out, 1);
	else if(eq_frac(&s,3,2))                  cnum_set_i64(out, -1);
	else if(eq_frac(&s,1,6)||eq_frac(&s,5,6)) set_half(out, 1);
	else if(eq_frac(&s,7,6)||eq_frac(&s,11,6))set_half(out, -1);
	else got = 0;
	cnum_free(&s); return got;
}
/* cos(r*pi) = sin((r+1/2)*pi) */
static int cos_pi(const cnum *r, cnum *out){
	cnum h; cnum_init(&h); set_half(&h, 1);
	cnum rr; cnum_init(&rr); cnum_add(&rr, r, &h);
	int g = sin_pi(&rr, out); cnum_free(&h); cnum_free(&rr); return g;
}
/* tan(r*pi): rational only at 0, +-1 (s = r mod 1; 1/2 is undefined) */
static int tan_pi(const cnum *r, cnum *out){
	cnum s; cnum_init(&s); rat_mod(&s, r, 1); int got = 1;
	if(cnum_is_zero(&s))     cnum_set_i64(out, 0);
	else if(eq_frac(&s,1,4)) cnum_set_i64(out, 1);
	else if(eq_frac(&s,3,4)) cnum_set_i64(out, -1);
	else got = 0;            /* 1/2 undefined; pi/3 etc. irrational */
	cnum_free(&s); return got;
}

static int eval_call_exact(const cnode *n, cnum *out){
	/* trig of special angles: the argument is symbolic (r*pi), so handle it before the
	   generic "evaluate every arg to a rational" step below would reject it. */
	if(n->nargs==1 && (!strcmp(n->name,"sin")||!strcmp(n->name,"cos")||!strcmp(n->name,"tan"))){
		cnum r; cnum_init(&r); int got = 0;
		if(arg_half_turns(n->args[0], &r))
			got = n->name[0]=='s' ? sin_pi(&r,out) : n->name[0]=='c' ? cos_pi(&r,out) : tan_pi(&r,out);
		cnum_free(&r);
		return got;     /* unrecognised angle -> decline -> numeric path */
	}
	const char *nm = n->name;
	int na = n->nargs;
	/* evaluate all args exactly; bail if any isn't exact */
	if(na > CN_MAXARGS) return 0;
	cnum a[CN_MAXARGS];
	for(int i = 0; i < na; i++){ cnum_init(&a[i]); }
	int ok = 1;
	for(int i = 0; i < na && ok; i++) if(!calc_eval_exact(n->args[i], &a[i])) ok = 0;
	if(!ok){ for(int i = 0; i < na; i++) cnum_free(&a[i]); return 0; }

	int done = 1;
	#define A0 (&a[0])
	#define A1 (&a[1])
	if(na == 1 && !strcmp(nm, "abs")){ cnum_copy(out, A0); if(out->p.sign < 0) cnum_neg(out); }
	else if(na == 1 && !strcmp(nm, "sign")){ cnum_set_i64(out, cnum_sign(A0)); }
	else if(na == 1 && !strcmp(nm, "floor")){ ratfloor(&out->p, A0); bi_set_i64(&out->q, 1); }
	else if(na == 1 && !strcmp(nm, "ceil")){ cnum t; cnum_init(&t); cnum_copy(&t, A0); cnum_neg(&t);
		bigint f; bi_init(&f); ratfloor(&f, &t); bi_neg(&f); set_int(out, &f); bi_free(&f); cnum_free(&t); }
	else if(na == 1 && !strcmp(nm, "trunc")){ bi_divmod(&out->p, NULL, &A0->p, &A0->q); bi_set_i64(&out->q, 1); }
	else if(na == 1 && !strcmp(nm, "round")){          /* half away from zero */
		cnum h, t; cnum_init(&h); cnum_init(&t); cnum_set_i64(&h, 1);
		cnum hh; cnum_init(&hh); cnum_set_i64(&hh, 2); cnum_div(&h, &h, &hh);  /* 1/2 */
		if(A0->p.sign >= 0){ cnum_add(&t, A0, &h); ratfloor(&out->p, &t); }
		else { cnum_sub(&t, A0, &h); cnum tn; cnum_init(&tn); cnum_copy(&tn,&t); cnum_neg(&tn);
		       bigint f; bi_init(&f); ratfloor(&f,&tn); bi_neg(&f); bi_copy(&out->p,&f); bi_free(&f); cnum_free(&tn); }
		bi_set_i64(&out->q, 1); cnum_free(&h); cnum_free(&t); cnum_free(&hh);
	}
	else if(na == 2 && (!strcmp(nm,"gcd")||!strcmp(nm,"lcm"))){
		if(!cnum_is_int(A0) || !cnum_is_int(A1)) done = 0;
		else { bigint g; bi_init(&g); bi_gcd(&g, &A0->p, &A1->p);
			if(!strcmp(nm,"gcd")) set_int(out, &g);
			else { if(bi_is_zero(&g)) cnum_set_i64(out,0);
			       else { bigint t; bi_init(&t); bi_mul(&t,&A0->p,&A1->p); if(t.sign<0) bi_neg(&t);
			              bi_divmod(&t,NULL,&t,&g); set_int(out,&t); bi_free(&t); } }
			bi_free(&g); }
	}
	else if(na == 2 && !strcmp(nm,"mod")){
		if(!cnum_is_int(A0) || !cnum_is_int(A1) || cnum_is_zero(A1)) done = 0;
		else { bi_divmod(NULL, &out->p, &A0->p, &A1->p); bi_set_i64(&out->q, 1); }
	}
	else if(na >= 1 && (!strcmp(nm,"min")||!strcmp(nm,"max"))){
		int wantmax = !strcmp(nm,"max"); cnum_copy(out, A0);
		for(int i = 1; i < na; i++){ int c = cnum_cmp(&a[i], out); if((wantmax && c>0)||(!wantmax && c<0)) cnum_copy(out, &a[i]); }
	}
	else if(na == 2 && (!strcmp(nm,"ncr")||!strcmp(nm,"comb")||!strcmp(nm,"nCr")
	                  ||!strcmp(nm,"npr")||!strcmp(nm,"perm")||!strcmp(nm,"nPr"))){
		int64_t nn, rr; int iscomb = (nm[1]=='c'||nm[1]=='C');
		if(!as_nonneg_i64(A0,&nn) || !as_nonneg_i64(A1,&rr) || rr > nn) done = 0;
		else {
			int64_t work = iscomb ? (rr < nn-rr ? rr : nn-rr) : rr;
			if(work > EXACT_WORK_CAP) done = 0;
			else if(iscomb){ bi_ncr(&out->p,(long)nn,(long)rr); bi_set_i64(&out->q,1); }
			else { /* nPr = nCr * r! */ bigint c,f; bi_init(&c); bi_init(&f);
			       bi_ncr(&c,(long)nn,(long)rr); bi_fact(&f,(long)rr);
			       bi_mul(&out->p,&c,&f); bi_set_i64(&out->q,1); bi_free(&c); bi_free(&f); }
		}
	}
	else if(na == 1 && (!strcmp(nm,"fact")||!strcmp(nm,"factorial"))){
		int64_t k; if(!as_nonneg_i64(A0,&k) || k > EXACT_WORK_CAP) done = 0;
		else { bi_fact(&out->p,(long)k); bi_set_i64(&out->q,1); }
	}
	else if(na == 2 && !strcmp(nm,"pow")){
		if(!cnum_is_int(A1) || !bi_fits_i64(&A1->p)) done = 0;
		else { int64_t e = bi_to_i64(&A1->p);
		       if(pow_too_big(A0,e) || cnum_pow_i(out, A0, e) != 0) done = 0; }
	}
	else done = 0;
	#undef A0
	#undef A1
	for(int i = 0; i < na; i++) cnum_free(&a[i]);
	return done;
}

int calc_eval_exact(const cnode *n, cnum *out){
	if(!n) return 0;
	switch(n->type){
	case CN_NUM:
		if(!n->exact) return 0;
		cnum_copy(out, n->exact);
		return 1;
	case CN_NEG:
		if(!calc_eval_exact(n->a, out)) return 0;
		cnum_neg(out);
		return 1;
	case CN_FACT: {
		cnum a; cnum_init(&a);
		int ok = calc_eval_exact(n->a, &a);
		int64_t k;
		if(ok && as_nonneg_i64(&a, &k) && k <= EXACT_WORK_CAP){ bi_fact(&out->p, (long)k); bi_set_i64(&out->q, 1); }
		else ok = 0;
		cnum_free(&a);
		return ok;
	}
	case CN_BINOP: {
		cnum a, b; cnum_init(&a); cnum_init(&b);
		if(!calc_eval_exact(n->a, &a) || !calc_eval_exact(n->b, &b)){ cnum_free(&a); cnum_free(&b); return 0; }
		int ok = 1;
		switch(n->op){
		case '+': cnum_add(out, &a, &b); break;
		case '-': cnum_sub(out, &a, &b); break;
		case '*': cnum_mul(out, &a, &b); break;
		case '/': if(cnum_div(out, &a, &b) != 0) ok = 0; break;
		case '%':
			if(cnum_is_int(&a) && cnum_is_int(&b) && !cnum_is_zero(&b)){
				bi_divmod(NULL, &out->p, &a.p, &b.p); bi_set_i64(&out->q, 1);
			} else ok = 0;
			break;
		case '^': {
			if(!cnum_is_int(&b) || !bi_fits_i64(&b.p)){ ok = 0; break; }
			int64_t e = bi_to_i64(&b.p);
			if(pow_too_big(&a, e) || cnum_pow_i(out, &a, e) != 0) ok = 0;
			break;
		}
		default: ok = 0;
		}
		cnum_free(&a); cnum_free(&b);
		return ok;
	}
	case CN_CALL:
		return eval_call_exact(n, out);
	case CN_VAR:
		return calc_get_var_exact(n->name, out);   /* exact-valued user var (pi/e stay symbolic) */
	default:
		return 0;   /* CN_EQ — not an exact rational here */
	}
}
