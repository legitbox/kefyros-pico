// tools/calc_test/test_exact.c — end-to-end: source -> parse -> exact eval -> string.
#include "calc.h"
#include "calc_num.h"
#include "calc_exact.h"
#include <stdio.h>
#include <string.h>

static int fails = 0, total = 0;

/* expect an exact rational result equal to `want` */
static void EX(const char *src, const char *want){
	total++;
	cnode *n = calc_parse(src);
	if(!n){ fails++; printf("FAIL %-16s parse error: %s\n", src, calc_err); return; }
	cnum r; cnum_init(&r);
	int ok = calc_eval_exact(n, &r);
	if(!ok){ fails++; printf("FAIL %-16s not exact (wanted %s)\n", src, want); }
	else { char buf[256]; cnum_to_str(&r, buf, sizeof buf);
		if(strcmp(buf, want)){ fails++; printf("FAIL %-16s got %s  want %s\n", src, buf, want); } }
	cnum_free(&r); cn_free(n);
}
/* expect the exact path to DECLINE (so the numeric path would handle it) */
static void INEX(const char *src){
	total++;
	cnode *n = calc_parse(src);
	if(!n){ fails++; printf("FAIL %-16s parse error: %s\n", src, calc_err); return; }
	cnum r; cnum_init(&r);
	if(calc_eval_exact(n, &r)){ fails++; printf("FAIL %-16s should be inexact\n", src); }
	cnum_free(&r); cn_free(n);
}

int main(void){
	/* exact rational arithmetic */
	EX("1/3+1/6", "1/2");
	EX("(1/2+1/3)*6", "5");
	EX("2/4", "1/2");
	EX("7/2", "7/2");
	EX("2^10", "1024");
	EX("2^100", "1267650600228229401496703205376");
	EX("2^-3", "1/8");
	EX("5^0", "1");
	EX("-3^2", "-9");           /* unary binds looser than ^: -(3^2) */
	EX("10!", "3628800");
	EX("(2/3)^3", "8/27");
	EX("17%5", "2");

	/* Pythonic operators */
	EX("2**100", "1267650600228229401496703205376");   /* ** == ^ */
	EX("7//2", "3");                                     /* floor division */
	EX("(-7)//2", "-4");                                 /* floors toward -inf */

	/* exactness-preserving builtins */
	EX("gcd(48,36)", "12");
	EX("lcm(4,6)", "12");
	EX("abs(-7/2)", "7/2");
	EX("floor(-7/2)", "-4");
	EX("ceil(7/2)", "4");
	EX("round(-5/2)", "-3");
	EX("ncr(10,3)", "120");
	EX("npr(5,2)", "20");
	EX("min(3/2,4/3,5)", "4/3");
	EX("pow(2,10)", "1024");

	/* special-angle trig (RAD mode default): rational results only (Niven) */
	EX("sin(pi/6)", "1/2");
	EX("sin(5*pi/6)", "1/2");
	EX("sin(7*pi/6)", "-1/2");
	EX("cos(pi/3)", "1/2");
	EX("cos(pi)", "-1");
	EX("sin(pi/2)", "1");
	EX("tan(pi/4)", "1");
	EX("tan(3*pi/4)", "-1");
	EX("sin(0)", "0");
	EX("cos(0)", "1");
	EX("sin(2*pi)", "0");

	/* must fall back to numeric (NOT exact) */
	INEX("0.5+0.5");      /* decimal literals */
	INEX("1/2+0.25");     /* mixed */
	INEX("pi");           /* irrational constant */
	INEX("sqrt(2)");      /* surd — next V2.0-b increment, not here */
	INEX("2^(1/2)");      /* non-integer power */
	INEX("sin(pi/4)");    /* sqrt(2)/2 — irrational */
	INEX("tan(pi/3)");    /* sqrt(3) — irrational */
	INEX("sin(1)");       /* 1 radian — transcendental */
	INEX("x");            /* unset variable */
	INEX("2^100000");     /* RAM guard: result too big -> numeric path */
	INEX("100000!");      /* work guard: factorial beyond the cap */

	/* exact variables: x = 1/2 stays exact through later expressions */
	{ cnum half; cnum_init(&half); cnum_set_i64(&half, 1);
	  cnum two; cnum_init(&two); cnum_set_i64(&two, 2); cnum_div(&half, &half, &two);
	  calc_set_var_exact("x", &half); cnum_free(&half); cnum_free(&two); }
	EX("x", "1/2");
	EX("x+x", "1");
	EX("x^2", "1/4");
	EX("2*x+1/3", "4/3");

	/* parser sanity for the pythonic equality marker */
	cnode *eq = calc_parse("x==4");
	total++; if(!eq || eq->type != CN_EQ || eq->op != 'c'){ fails++; printf("FAIL x==4 marker\n"); }
	cn_free(eq);
	cnode *as = calc_parse("x=4");
	total++; if(!as || as->type != CN_EQ || as->op == 'c'){ fails++; printf("FAIL x=4 marker\n"); }
	cn_free(as);

	printf("\nexact: %d/%d passed\n", total - fails, total);
	return fails ? 1 : 0;
}
