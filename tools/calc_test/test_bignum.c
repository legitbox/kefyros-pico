// tools/calc_test/test_bignum.c — host unit tests for the CAS bignum.
// Build+run (WSL):  gcc -O2 -I../../apps test_bignum.c ../../apps/calc_bignum.c -o /tmp/tb && /tmp/tb
#include "calc_bignum.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, total = 0;

static void check_str(const char *label, const bigint *a, const char *want){
	char buf[1024];
	bi_to_str(a, buf, sizeof buf);
	total++;
	if(strcmp(buf, want) != 0){ fails++; printf("FAIL %-22s got %s  want %s\n", label, buf, want); }
}

static void from(bigint *a, const char *s){ bi_from_str(a, s, (int)strlen(s)); }

int main(void){
	bigint a, b, r, q, rem; bi_init(&a); bi_init(&b); bi_init(&r); bi_init(&q); bi_init(&rem);

	/* basic i64 round-trips */
	bi_set_i64(&a, 0);            check_str("zero", &a, "0");
	bi_set_i64(&a, 123456789);    check_str("pos", &a, "123456789");
	bi_set_i64(&a, -987654321);   check_str("neg", &a, "-987654321");
	bi_set_i64(&a, (int64_t)-9223372036854775807LL - 1); check_str("INT64_MIN", &a, "-9223372036854775808");

	/* add / sub with sign mixing */
	from(&a, "99999999999999999999"); from(&b, "1");
	bi_add(&r, &a, &b);          check_str("carry-chain", &r, "100000000000000000000");
	from(&a, "100"); from(&b, "100"); bi_sub(&r, &a, &b); check_str("sub-zero", &r, "0");
	from(&a, "5"); from(&b, "8"); bi_sub(&r, &a, &b);     check_str("sub-neg", &r, "-3");
	from(&a, "-5"); from(&b, "-8"); bi_add(&r, &a, &b);   check_str("add-neg", &r, "-13");

	/* multiply */
	from(&a, "12345678901234567890"); from(&b, "98765432109876543210");
	bi_mul(&r, &a, &b);          check_str("mul-big", &r, "1219326311370217952237463801111263526900");

	/* 2^64, 2^100 exact (the golden tests) */
	bi_set_i64(&a, 2); bi_pow_u(&r, &a, 64);  check_str("2^64",  &r, "18446744073709551616");
	bi_set_i64(&a, 2); bi_pow_u(&r, &a, 100); check_str("2^100", &r, "1267650600228229401496703205376");
	bi_set_i64(&a, 2); bi_pow_u(&r, &a, 0);   check_str("2^0",   &r, "1");

	/* divmod: truncation toward zero + remainder sign == dividend sign */
	from(&a, "17"); from(&b, "5"); bi_divmod(&q, &rem, &a, &b);
	check_str("17/5 q", &q, "3"); check_str("17/5 r", &rem, "2");
	from(&a, "-17"); from(&b, "5"); bi_divmod(&q, &rem, &a, &b);
	check_str("-17/5 q", &q, "-3"); check_str("-17/5 r", &rem, "-2");
	from(&a, "17"); from(&b, "-5"); bi_divmod(&q, &rem, &a, &b);
	check_str("17/-5 q", &q, "-3"); check_str("17/-5 r", &rem, "2");
	from(&a, "1219326311370217952237463801111263526900"); from(&b, "98765432109876543210");
	bi_divmod(&q, &rem, &a, &b);
	check_str("big/big q", &q, "12345678901234567890"); check_str("big/big r", &rem, "0");

	/* gcd (drives rational normalisation) */
	from(&a, "1071"); from(&b, "462"); bi_gcd(&r, &a, &b); check_str("gcd", &r, "21");
	from(&a, "-48"); from(&b, "36");  bi_gcd(&r, &a, &b);  check_str("gcd-neg", &r, "12");
	from(&a, "0"); from(&b, "7");     bi_gcd(&r, &a, &b);  check_str("gcd-0", &r, "7");

	/* big round-trip: exercises the dynamic to_str buffer (well past the old 700-byte cap) */
	bi_set_i64(&a, 2); bi_pow_u(&r, &a, 4000);
	{ char s1[2048]; bi_to_str(&r, s1, sizeof s1);
	  total++; if((int)strlen(s1) != 1205){ fails++; printf("FAIL 2^4000 len %d want 1205\n",(int)strlen(s1)); }
	  from(&b, s1); total++; if(bi_cmp(&r,&b)!=0){ fails++; printf("FAIL 2^4000 roundtrip\n"); } }

	/* alias safety: r == a == b */
	from(&a, "12345"); bi_mul(&a, &a, &a); check_str("alias-sq", &a, "152399025");
	from(&a, "99"); bi_add(&a, &a, &a);    check_str("alias-add", &a, "198");

	bi_free(&a); bi_free(&b); bi_free(&r); bi_free(&q); bi_free(&rem);
	printf("\nbignum: %d/%d passed\n", total - fails, total);
	return fails ? 1 : 0;
}
