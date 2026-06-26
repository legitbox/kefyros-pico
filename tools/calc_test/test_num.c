// tools/calc_test/test_num.c — host unit tests for exact rationals.
#include "calc_num.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, total = 0;

static void frac(cnum *x, long pn, long qd){
	cnum t; cnum_init(&t); cnum_set_i64(x, pn); cnum_set_i64(&t, qd);
	cnum_div(x, x, &t); cnum_free(&t);
}
static void chk(const char *label, const cnum *x, const char *want){
	char buf[256]; cnum_to_str(x, buf, sizeof buf);
	total++;
	if(strcmp(buf, want)){ fails++; printf("FAIL %-18s got %s  want %s\n", label, buf, want); }
}

int main(void){
	cnum a, b, r; cnum_init(&a); cnum_init(&b); cnum_init(&r);

	frac(&a, 1, 3); frac(&b, 1, 6); cnum_add(&r, &a, &b); chk("1/3+1/6", &r, "1/2");
	frac(&a, 1, 2); frac(&b, 1, 3); cnum_add(&r, &a, &b);
	cnum_set_i64(&b, 6); cnum_mul(&r, &r, &b);            chk("(1/2+1/3)*6", &r, "5");
	frac(&a, 2, 4);                                       chk("2/4 reduce", &a, "1/2");
	frac(&a, -6, 8);                                      chk("-6/8 reduce", &a, "-3/4");
	frac(&a, 6, -8);                                      chk("6/-8 sign", &a, "-3/4");
	frac(&a, 1, 3); frac(&b, 1, 3); cnum_sub(&r, &a, &b); chk("1/3-1/3", &r, "0");
	frac(&a, 7, 1); frac(&b, 2, 1); cnum_div(&r, &a, &b); chk("7/2", &r, "7/2");

	/* pow: integer + negative exponents on rationals */
	frac(&a, 2, 3); cnum_pow_i(&r, &a, 3);   chk("(2/3)^3", &r, "8/27");
	frac(&a, 2, 3); cnum_pow_i(&r, &a, -2);  chk("(2/3)^-2", &r, "9/4");
	cnum_set_i64(&a, 5); cnum_pow_i(&r, &a, 0); chk("5^0", &r, "1");
	cnum_set_i64(&a, 2); cnum_pow_i(&r, &a, 100);
	chk("2^100", &r, "1267650600228229401496703205376");

	/* compare */
	frac(&a, 1, 3); frac(&b, 1, 2);
	total++; if(cnum_cmp(&a,&b) >= 0){ fails++; printf("FAIL cmp 1/3<1/2\n"); }
	frac(&a, 3, 1); frac(&b, 6, 2);
	total++; if(cnum_cmp(&a,&b) != 0){ fails++; printf("FAIL cmp 3==6/2\n"); }

	/* alias safety */
	frac(&a, 3, 4); cnum_add(&a, &a, &a); chk("alias 3/4+3/4", &a, "3/2");
	frac(&a, 2, 5); cnum_mul(&a, &a, &a); chk("alias (2/5)^2", &a, "4/25");

	cnum_free(&a); cnum_free(&b); cnum_free(&r);
	printf("\nnum: %d/%d passed\n", total - fails, total);
	return fails ? 1 : 0;
}
