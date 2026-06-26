// apps/calc_bignum.c — arbitrary-precision signed integer (see calc_bignum.h).
// Schoolbook add/sub/mul; binary long division (simple + provably correct over Knuth-D for
// calc-sized operands). Alias-safe: every binary op computes into a local temp, then swaps
// into r, so callers may pass r == a == b freely.
#include "calc_bignum.h"
#include <stdlib.h>
#include <string.h>

/* ---------- low-level magnitude helpers ---------- */

static void bi_reserve(bigint *a, int cap){
	if(a->cap >= cap) return;
	int nc = a->cap ? a->cap : 4;
	while(nc < cap) nc *= 2;
	a->limb = (uint32_t*)realloc(a->limb, (size_t)nc * sizeof(uint32_t));
	a->cap = nc;
}

static void bi_trim(bigint *a){
	while(a->n > 0 && a->limb[a->n-1] == 0) a->n--;
	if(a->n == 0) a->sign = 0;
	else if(a->sign == 0) a->sign = 1;   /* defensive: nonzero magnitude must carry a sign */
}

void bi_init(bigint *a){ a->sign = 0; a->n = 0; a->cap = 0; a->limb = NULL; }
void bi_free(bigint *a){ free(a->limb); a->limb = NULL; a->cap = 0; a->n = 0; a->sign = 0; }

void bi_swap(bigint *a, bigint *b){ bigint t = *a; *a = *b; *b = t; }

void bi_set_i64(bigint *a, int64_t v){
	a->n = 0;
	if(v == 0){ a->sign = 0; return; }
	uint64_t m;
	if(v < 0){ a->sign = -1; m = (uint64_t)(-(v+1)) + 1u; }   /* avoids -INT64_MIN UB */
	else     { a->sign =  1; m = (uint64_t)v; }
	bi_reserve(a, 2);
	a->limb[0] = (uint32_t)(m & 0xffffffffu);
	a->limb[1] = (uint32_t)(m >> 32);
	a->n = 2;
	bi_trim(a);
}

void bi_copy(bigint *dst, const bigint *src){
	if(dst == src) return;
	bi_reserve(dst, src->n);
	memcpy(dst->limb, src->limb, (size_t)src->n * sizeof(uint32_t));
	dst->n = src->n; dst->sign = src->sign;
}

int bi_is_zero(const bigint *a){ return a->sign == 0; }

/* compare magnitudes only: -1/0/+1 */
static int cmp_abs(const bigint *a, const bigint *b){
	if(a->n != b->n) return a->n < b->n ? -1 : 1;
	for(int i = a->n - 1; i >= 0; i--)
		if(a->limb[i] != b->limb[i]) return a->limb[i] < b->limb[i] ? -1 : 1;
	return 0;
}

int bi_cmp(const bigint *a, const bigint *b){
	if(a->sign != b->sign) return a->sign < b->sign ? -1 : 1;
	if(a->sign == 0) return 0;
	int c = cmp_abs(a, b);
	return a->sign > 0 ? c : -c;     /* both negative => reverse */
}

void bi_neg(bigint *a){ if(a->sign) a->sign = -a->sign; }
void bi_abs(bigint *a){ if(a->sign) a->sign = 1; }

/* r = |a| + |b|  (sign set by caller) */
static void add_abs(bigint *r, const bigint *a, const bigint *b){
	if(a->n < b->n){ const bigint *t = a; a = b; b = t; }
	bigint out; bi_init(&out); bi_reserve(&out, a->n + 1);
	uint64_t carry = 0; int i;
	for(i = 0; i < b->n; i++){
		uint64_t s = (uint64_t)a->limb[i] + b->limb[i] + carry;
		out.limb[i] = (uint32_t)s; carry = s >> 32;
	}
	for(; i < a->n; i++){
		uint64_t s = (uint64_t)a->limb[i] + carry;
		out.limb[i] = (uint32_t)s; carry = s >> 32;
	}
	if(carry) out.limb[i++] = (uint32_t)carry;
	out.n = i; out.sign = 1; bi_trim(&out);
	bi_swap(r, &out); bi_free(&out);
}

/* r = |a| - |b|, requires |a| >= |b| (sign set by caller) */
static void sub_abs(bigint *r, const bigint *a, const bigint *b){
	bigint out; bi_init(&out); bi_reserve(&out, a->n);
	int64_t borrow = 0; int i;
	for(i = 0; i < b->n; i++){
		int64_t d = (int64_t)a->limb[i] - b->limb[i] - borrow;
		if(d < 0){ d += (int64_t)1 << 32; borrow = 1; } else borrow = 0;
		out.limb[i] = (uint32_t)d;
	}
	for(; i < a->n; i++){
		int64_t d = (int64_t)a->limb[i] - borrow;
		if(d < 0){ d += (int64_t)1 << 32; borrow = 1; } else borrow = 0;
		out.limb[i] = (uint32_t)d;
	}
	out.n = a->n; out.sign = 1; bi_trim(&out);
	bi_swap(r, &out); bi_free(&out);
}

void bi_add(bigint *r, const bigint *a, const bigint *b){
	if(a->sign == 0){ bi_copy(r, b); return; }
	if(b->sign == 0){ bi_copy(r, a); return; }
	if(a->sign == b->sign){ int s = a->sign; add_abs(r, a, b); if(!bi_is_zero(r)) r->sign = s; return; }
	int c = cmp_abs(a, b);                 /* differing signs => subtract smaller magnitude */
	if(c == 0){ r->n = 0; r->sign = 0; return; }
	if(c > 0){ int s = a->sign; sub_abs(r, a, b); if(!bi_is_zero(r)) r->sign = s; }
	else      { int s = b->sign; sub_abs(r, b, a); if(!bi_is_zero(r)) r->sign = s; }
}

void bi_sub(bigint *r, const bigint *a, const bigint *b){
	bigint nb; bi_init(&nb); bi_copy(&nb, b); bi_neg(&nb);
	bi_add(r, a, &nb); bi_free(&nb);
}

void bi_mul(bigint *r, const bigint *a, const bigint *b){
	if(a->sign == 0 || b->sign == 0){ r->n = 0; r->sign = 0; return; }
	bigint out; bi_init(&out); bi_reserve(&out, a->n + b->n);
	memset(out.limb, 0, (size_t)(a->n + b->n) * sizeof(uint32_t));
	for(int i = 0; i < a->n; i++){
		uint64_t carry = 0, ai = a->limb[i];
		for(int j = 0; j < b->n; j++){
			uint64_t s = ai * b->limb[j] + out.limb[i+j] + carry;
			out.limb[i+j] = (uint32_t)s; carry = s >> 32;
		}
		out.limb[i + b->n] += (uint32_t)carry;
	}
	out.n = a->n + b->n; out.sign = a->sign * b->sign; bi_trim(&out);
	bi_swap(r, &out); bi_free(&out);
}

/* ---------- bit helpers for binary long division ---------- */
static int bit_len(const bigint *a){
	if(a->n == 0) return 0;
	uint32_t hi = a->limb[a->n-1]; int b = 0;
	while(hi){ b++; hi >>= 1; }
	return (a->n - 1) * 32 + b;
}
static int get_bit(const bigint *a, int i){ return (a->limb[i>>5] >> (i & 31)) & 1u; }
/* out <<= 1; then OR in `bit` at position 0 */
static void shl1_or(bigint *a, int bit){
	bi_reserve(a, a->n + 1);
	uint32_t carry = (uint32_t)(bit & 1);
	for(int i = 0; i < a->n; i++){
		uint32_t nc = a->limb[i] >> 31;
		a->limb[i] = (a->limb[i] << 1) | carry;
		carry = nc;
	}
	if(carry){ a->limb[a->n] = carry; a->n++; }
	if(a->n == 0 && bit){ a->limb[0] = 1; a->n = 1; }
	if(a->n) a->sign = 1;
}

void bi_divmod(bigint *q, bigint *rem, const bigint *a, const bigint *b){
	if(b->sign == 0){ if(q){ q->n=0; q->sign=0; } if(rem){ rem->n=0; rem->sign=0; } return; }
	if(cmp_abs(a, b) < 0){                 /* |a| < |b| => q=0, rem=a */
		if(q){ q->n = 0; q->sign = 0; }
		if(rem) bi_copy(rem, a);
		return;
	}
	bigint Q, R, babs; bi_init(&Q); bi_init(&R); bi_init(&babs);
	bi_copy(&babs, b); babs.sign = 1;
	int bl = bit_len(a);
	bi_reserve(&Q, (bl>>5) + 1);
	memset(Q.limb, 0, (size_t)Q.cap * sizeof(uint32_t));
	Q.n = (bl>>5) + 1;
	for(int i = bl - 1; i >= 0; i--){
		shl1_or(&R, get_bit(a, i));
		if(cmp_abs(&R, &babs) >= 0){
			sub_abs(&R, &R, &babs);
			Q.limb[i>>5] |= (uint32_t)1 << (i & 31);
		}
	}
	Q.sign = 1; bi_trim(&Q);
	R.sign = R.n ? 1 : 0; bi_trim(&R);
	/* signs: quotient = sign(a)*sign(b); remainder takes sign(a) (truncation toward zero) */
	if(!bi_is_zero(&Q)) Q.sign = a->sign * b->sign;
	if(!bi_is_zero(&R)) R.sign = a->sign;
	if(q) bi_swap(q, &Q);
	if(rem) bi_swap(rem, &R);
	bi_free(&Q); bi_free(&R); bi_free(&babs);
}

void bi_gcd(bigint *r, const bigint *a, const bigint *b){
	bigint x, y, t; bi_init(&x); bi_init(&y); bi_init(&t);
	bi_copy(&x, a); x.sign = x.n ? 1 : 0;
	bi_copy(&y, b); y.sign = y.n ? 1 : 0;
	while(!bi_is_zero(&y)){
		bi_divmod(NULL, &t, &x, &y);   /* t = x mod y (nonneg since x,y >= 0) */
		bi_swap(&x, &y); bi_swap(&y, &t);
	}
	bi_swap(r, &x);
	bi_free(&x); bi_free(&y); bi_free(&t);
}

void bi_pow_u(bigint *r, const bigint *base, uint32_t e){
	bigint result, b; bi_init(&result); bi_init(&b);
	bi_set_i64(&result, 1); bi_copy(&b, base);
	while(e){
		if(e & 1) bi_mul(&result, &result, &b);
		e >>= 1;
		if(e) bi_mul(&b, &b, &b);
	}
	bi_swap(r, &result); bi_free(&result); bi_free(&b);
}

/* ---------- i64 / double / string ---------- */

int bi_fits_i64(const bigint *a){
	if(a->n < 2) return 1;
	if(a->n > 2) return 0;
	uint64_t m = ((uint64_t)a->limb[1] << 32) | a->limb[0];
	if(a->sign >= 0) return m <= (uint64_t)INT64_MAX;
	return m <= (uint64_t)INT64_MAX + 1u;       /* INT64_MIN magnitude */
}
int64_t bi_to_i64(const bigint *a){
	uint64_t m = 0;
	if(a->n > 0) m |= a->limb[0];
	if(a->n > 1) m |= (uint64_t)a->limb[1] << 32;
	return a->sign < 0 ? -(int64_t)m : (int64_t)m;
}
double bi_to_double(const bigint *a){
	double d = 0.0;
	for(int i = a->n - 1; i >= 0; i--) d = d * 4294967296.0 + (double)a->limb[i];
	return a->sign < 0 ? -d : d;
}

int bi_to_str(const bigint *a, char *out, int outsz){
	if(outsz < 2) return -1;
	if(a->sign == 0){ out[0] = '0'; out[1] = 0; return 1; }
	/* size the scratch from the true bit length so we never silently truncate a big value */
	int maxdig = (int)((double)bit_len(a) * 0.30103) + 4;
	char *tmp = (char*)malloc((size_t)maxdig + 8);
	if(!tmp) return -1;
	int ti = 0;
	bigint x, q, rem, base; bi_init(&x); bi_init(&q); bi_init(&rem); bi_init(&base);
	bi_copy(&x, a); x.sign = 1;
	bi_set_i64(&base, 1000000000);
	while(!bi_is_zero(&x)){
		bi_divmod(&q, &rem, &x, &base);
		uint32_t chunk = rem.n ? rem.limb[0] : 0;
		if(bi_is_zero(&q)){
			while(chunk){ tmp[ti++] = (char)('0' + chunk % 10); chunk /= 10; }
		} else {
			for(int k = 0; k < 9; k++){ tmp[ti++] = (char)('0' + chunk % 10); chunk /= 10; }
		}
		bi_swap(&x, &q);
	}
	bi_free(&x); bi_free(&q); bi_free(&rem); bi_free(&base);
	int len = ti + (a->sign < 0 ? 1 : 0);
	if(len + 1 > outsz){ free(tmp); return -1; }   /* caller's buffer too small: clean -1 */
	int o = 0;
	if(a->sign < 0) out[o++] = '-';
	for(int i = ti - 1; i >= 0; i--) out[o++] = tmp[i];
	out[o] = 0;
	free(tmp);
	return o;
}

int bi_from_str(bigint *a, const char *s, int len){
	int i = 0, sign = 1;
	a->n = 0; a->sign = 0;
	if(i < len && (s[i] == '+' || s[i] == '-')){ if(s[i] == '-') sign = -1; i++; }
	if(i >= len) return -1;
	bigint ten, dig, acc; bi_init(&ten); bi_init(&dig); bi_init(&acc);
	bi_set_i64(&ten, 10);
	for(; i < len; i++){
		if(s[i] < '0' || s[i] > '9'){ bi_free(&ten); bi_free(&dig); bi_free(&acc); return -1; }
		bi_mul(&acc, &acc, &ten);
		bi_set_i64(&dig, s[i] - '0');
		bi_add(&acc, &acc, &dig);
	}
	if(!bi_is_zero(&acc)) acc.sign = sign;
	bi_swap(a, &acc);
	bi_free(&ten); bi_free(&dig); bi_free(&acc);
	return 0;
}
