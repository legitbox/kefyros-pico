// apps/calc_num.h — exact rational number (p/q) over the CAS bignum.
// Always kept normalised: q > 0, gcd(|p|, q) == 1, zero stored as 0/1. Board-agnostic.
#ifndef KF_CALC_NUM_H
#define KF_CALC_NUM_H
#include <stdint.h>
#include "calc_bignum.h"

typedef struct cnum { bigint p, q; } cnum;     /* value = p / q */

void   cnum_init(cnum *x);                /* -> 0/1 */
void   cnum_free(cnum *x);
void   cnum_set_i64(cnum *x, int64_t v);
int    cnum_set_str_int(cnum *x, const char *s, int len);  /* integer literal -> p/1; -1 err */
void   cnum_copy(cnum *d, const cnum *s);

int    cnum_is_zero(const cnum *x);
int    cnum_is_int(const cnum *x);        /* true when q == 1 */
int    cnum_sign(const cnum *x);          /* -1/0/+1 */
int    cnum_cmp(const cnum *a, const cnum *b);

void   cnum_neg(cnum *x);
int    cnum_add(cnum *r, const cnum *a, const cnum *b);
int    cnum_sub(cnum *r, const cnum *a, const cnum *b);
int    cnum_mul(cnum *r, const cnum *a, const cnum *b);
int    cnum_div(cnum *r, const cnum *a, const cnum *b);     /* -1 on divide-by-zero */
int    cnum_pow_i(cnum *r, const cnum *a, int64_t e);       /* integer exponent; -1 on 0^(neg) */

double cnum_to_double(const cnum *x);
int    cnum_to_str(const cnum *x, char *out, int outsz);    /* "p" or "p/q"; -1 if too small */

#endif
