// apps/calc_num.c — exact rational arithmetic (see calc_num.h).
// Every result is reduced to lowest terms with a positive denominator. All ops build into
// local temporaries before assigning to r, so r may alias a and/or b.
#include "calc_num.h"
#include <string.h>

void cnum_init(cnum *x){ bi_init(&x->p); bi_init(&x->q); bi_set_i64(&x->q, 1); }
void cnum_free(cnum *x){ bi_free(&x->p); bi_free(&x->q); }

void cnum_set_i64(cnum *x, int64_t v){ bi_set_i64(&x->p, v); bi_set_i64(&x->q, 1); }

void cnum_copy(cnum *d, const cnum *s){
	if(d == s) return;
	bi_copy(&d->p, &s->p); bi_copy(&d->q, &s->q);
}

/* reduce p/q: force q>0, divide both by gcd(|p|,q). q must be nonzero on entry. */
static void normalize(bigint *p, bigint *q){
	if(bi_is_zero(p)){ bi_set_i64(q, 1); return; }
	if(q->sign < 0){ bi_neg(p); bi_neg(q); }
	bigint g; bi_init(&g); bi_gcd(&g, p, q);
	/* gcd >= 1 here; skip the divides when it's already 1 */
	if(!(g.n == 1 && g.limb[0] == 1)){
		bi_divmod(p, NULL, p, &g);
		bi_divmod(q, NULL, q, &g);
	}
	bi_free(&g);
}

int cnum_set_str_int(cnum *x, const char *s, int len){
	if(bi_from_str(&x->p, s, len) != 0) return -1;
	bi_set_i64(&x->q, 1);
	return 0;
}

int cnum_is_zero(const cnum *x){ return bi_is_zero(&x->p); }
int cnum_is_int(const cnum *x){ return x->q.n == 1 && x->q.limb[0] == 1; }
int cnum_sign(const cnum *x){ return x->p.sign; }

int cnum_cmp(const cnum *a, const cnum *b){
	/* a.p/a.q ? b.p/b.q  <=>  a.p*b.q ? b.p*a.q   (q's are positive) */
	bigint l, r; bi_init(&l); bi_init(&r);
	bi_mul(&l, &a->p, &b->q);
	bi_mul(&r, &b->p, &a->q);
	int c = bi_cmp(&l, &r);
	bi_free(&l); bi_free(&r);
	return c;
}

void cnum_neg(cnum *x){ bi_neg(&x->p); }

int cnum_add(cnum *r, const cnum *a, const cnum *b){
	bigint np, nq, t; bi_init(&np); bi_init(&nq); bi_init(&t);
	bi_mul(&np, &a->p, &b->q);     /* a.p*b.q */
	bi_mul(&t,  &b->p, &a->q);     /* b.p*a.q */
	bi_add(&np, &np, &t);
	bi_mul(&nq, &a->q, &b->q);
	normalize(&np, &nq);
	bi_swap(&r->p, &np); bi_swap(&r->q, &nq);
	bi_free(&np); bi_free(&nq); bi_free(&t);
	return 0;
}

int cnum_sub(cnum *r, const cnum *a, const cnum *b){
	cnum nb; cnum_init(&nb); cnum_copy(&nb, b); cnum_neg(&nb);
	int rc = cnum_add(r, a, &nb); cnum_free(&nb); return rc;
}

int cnum_mul(cnum *r, const cnum *a, const cnum *b){
	bigint np, nq; bi_init(&np); bi_init(&nq);
	bi_mul(&np, &a->p, &b->p);
	bi_mul(&nq, &a->q, &b->q);
	normalize(&np, &nq);
	bi_swap(&r->p, &np); bi_swap(&r->q, &nq);
	bi_free(&np); bi_free(&nq);
	return 0;
}

int cnum_div(cnum *r, const cnum *a, const cnum *b){
	if(cnum_is_zero(b)) return -1;
	bigint np, nq; bi_init(&np); bi_init(&nq);
	bi_mul(&np, &a->p, &b->q);
	bi_mul(&nq, &a->q, &b->p);     /* b.p may be negative -> normalize fixes the sign */
	normalize(&np, &nq);
	bi_swap(&r->p, &np); bi_swap(&r->q, &nq);
	bi_free(&np); bi_free(&nq);
	return 0;
}

int cnum_pow_i(cnum *r, const cnum *a, int64_t e){
	if(e == 0){ cnum_set_i64(r, 1); return 0; }
	int neg = e < 0;
	uint64_t m = neg ? (uint64_t)(-(e+1)) + 1u : (uint64_t)e;
	if(m > 0xffffffffu) return -1;            /* absurd exponent; caller falls back */
	bigint np, nq; bi_init(&np); bi_init(&nq);
	bi_pow_u(&np, &a->p, (uint32_t)m);
	bi_pow_u(&nq, &a->q, (uint32_t)m);
	if(neg){                                  /* (p/q)^-m = q^m / p^m */
		if(bi_is_zero(&a->p)){ bi_free(&np); bi_free(&nq); return -1; }
		bi_swap(&np, &nq);
	}
	normalize(&np, &nq);
	bi_swap(&r->p, &np); bi_swap(&r->q, &nq);
	bi_free(&np); bi_free(&nq);
	return 0;
}

double cnum_to_double(const cnum *x){ return bi_to_double(&x->p) / bi_to_double(&x->q); }

int cnum_to_str(const cnum *x, char *out, int outsz){
	int n = bi_to_str(&x->p, out, outsz);
	if(n < 0) return -1;
	if(cnum_is_int(x)) return n;
	if(n + 1 >= outsz) return -1;
	out[n++] = '/';
	int m = bi_to_str(&x->q, out + n, outsz - n);
	if(m < 0) return -1;
	return n + m;
}
