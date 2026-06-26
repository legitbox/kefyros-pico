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

static int eval_call_exact(const cnode *n, cnum *out){
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
	default:
		return 0;   /* CN_VAR (pi/e/user vars), CN_EQ — not an exact rational here */
	}
}
