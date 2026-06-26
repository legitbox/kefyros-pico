// apps/calc_bignum.h — arbitrary-precision signed integer for the Kefyros CAS.
// Sign-magnitude, base 2^32 little-endian limbs. Pure C, board-agnostic (host-testable).
// All binary ops are alias-safe: r may be the same object as a and/or b.
#ifndef KF_CALC_BIGNUM_H
#define KF_CALC_BIGNUM_H
#include <stdint.h>

typedef struct {
	int       sign;   /* -1, 0, +1   (0 <=> the value is zero) */
	int       n;      /* significant limbs (no leading zeros; 0 when value is zero) */
	int       cap;    /* allocated limbs */
	uint32_t *limb;   /* little-endian magnitude; may be NULL while cap==0 */
} bigint;

void   bi_init(bigint *a);                 /* -> 0 (no alloc) */
void   bi_free(bigint *a);
void   bi_set_i64(bigint *a, int64_t v);
void   bi_copy(bigint *dst, const bigint *src);
void   bi_swap(bigint *a, bigint *b);

int    bi_is_zero(const bigint *a);
int    bi_fits_i64(const bigint *a);
int64_t bi_to_i64(const bigint *a);        /* valid only if bi_fits_i64 */
double bi_to_double(const bigint *a);

int    bi_cmp(const bigint *a, const bigint *b);   /* signed: -1/0/+1 */

void   bi_neg(bigint *a);
void   bi_abs(bigint *a);
void   bi_add(bigint *r, const bigint *a, const bigint *b);
void   bi_sub(bigint *r, const bigint *a, const bigint *b);
void   bi_mul(bigint *r, const bigint *a, const bigint *b);
/* truncated division toward zero: a = q*b + rem, sign(rem)==sign(a) (or rem 0).
   q and/or rem may be NULL if not needed. Division by zero leaves q,rem = 0. */
void   bi_divmod(bigint *q, bigint *rem, const bigint *a, const bigint *b);
void   bi_gcd(bigint *r, const bigint *a, const bigint *b);   /* result >= 0 */
void   bi_pow_u(bigint *r, const bigint *base, uint32_t e);   /* r = base^e (0^0 = 1) */

/* base-10 string. bi_to_str returns the written length (excluding NUL), or -1 if outsz too
   small. bi_from_str parses an optional sign then digits; returns 0 on success, -1 on error. */
int    bi_to_str(const bigint *a, char *out, int outsz);
int    bi_from_str(bigint *a, const char *s, int len);

#endif
