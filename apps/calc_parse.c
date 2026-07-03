// apps/calc_parse.c — tokenizer + recursive-descent parser -> AST.
// Precedence: '=' < (+ -) < (* / // %) < unary(- +) < ('^' | '**') right-assoc < postfix '!'.
// Python-compatible: '**' aliases '^' (power); '//' is floor division -> floor(a/b).
// Implicit multiply: 2x, 2(x+1), 2sin(x), (a)(b), 3pi. Function calls name(a,b,...).
// Identifiers allow high-bit bytes so UTF-8 names (e.g. theta) parse. Part of the
// Kefyros scientific calculator.
#include "calc.h"
#include "calc_num.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

static const char *P;        /* current parse position */

/* Recursion-depth guard: the descent chain (parse_expr..parse_atom->parse_expr on '(') and the
   unary self-recursion are unbounded; deep "((((" or a "-----" run would overflow the 4 KB
   core-0 stack (no guard page). Counted in parse_atom + parse_unary (both lie on every nesting
   step), reset per top-level parse. Cap 32 => ~16 paren levels, well inside the stack. */
#define CALC_MAXDEPTH 32
static int calc_depth = 0;

static void seterr(const char *m){ snprintf(calc_err, sizeof calc_err, "%s", m); }
static void skip(void){ while(*P==' '||*P=='\t') P++; }
static int  is_namestart(int c){ return isalpha((unsigned char)c) || c=='_' || (unsigned char)c>=0x80; }
static int  is_namechar (int c){ return isalnum((unsigned char)c) || c=='_' || (unsigned char)c>=0x80; }

static cnode *parse_expr(void);

/* can the current position begin a factor? (for implicit multiply) */
static int starts_factor(void){
	skip();
	int c = *P;
	return isdigit((unsigned char)c) || c=='.' || c=='(' || is_namestart(c);
}

static cnode *parse_atom_body(void);
static cnode *parse_atom(void){                   /* depth-guarded wrapper */
	if(++calc_depth > CALC_MAXDEPTH){ calc_depth--; seterr("nesting too deep"); return NULL; }
	cnode *r = parse_atom_body();
	calc_depth--;
	return r;
}
static cnode *parse_atom_body(void){
	skip();
	int c = *P;
	if(c=='('){
		P++;
		cnode *e = parse_expr();
		if(!e) return NULL;
		skip();
		if(*P!=')'){ seterr("missing ')'"); cn_free(e); return NULL; }
		P++;
		return e;
	}
	if(isdigit((unsigned char)c) || c=='.'){
		const char *start = P;
		char *end; double v = strtod(P, &end);
		if(end==P){ seterr("bad number"); return NULL; }
		P = end;
		cnode *node = cn_num(v);
		if(!node){ seterr("out of memory"); return NULL; }
		/* exact iff a pure integer literal (no '.', no exponent) — keeps 1/3, 2^100 exact */
		int isint = 1;
		for(const char *s = start; s < end; s++) if(*s=='.'||*s=='e'||*s=='E'){ isint = 0; break; }
		if(isint){
			node->exact = malloc(sizeof(cnum));
			if(node->exact){ cnum_init(node->exact); cnum_set_str_int(node->exact, start, (int)(end-start)); }
		}
		return node;
	}
	if(is_namestart(c)){
		char name[CN_NAMELEN]; int i=0;
		while(is_namechar(*P) && i<CN_NAMELEN-1) name[i++]=*P++;
		name[i]=0;
		skip();
		if(*P=='('){                                  /* function call */
			P++;
			cnode *call = cn_new(CN_CALL);
			if(!call) return NULL;
			memcpy(call->name, name, (size_t)i + 1);   /* name is NUL-terminated, i < CN_NAMELEN */
			skip();
			if(*P!=')'){
				for(;;){
					cnode *arg = parse_expr();
					if(!arg){ cn_free(call); return NULL; }
					if(call->nargs >= CN_MAXARGS){ seterr("too many args"); cn_free(arg); cn_free(call); return NULL; }
					call->args[call->nargs++] = arg;
					skip();
					if(*P==','){ P++; continue; }
					break;
				}
			}
			skip();
			if(*P!=')'){ seterr("missing ')'"); cn_free(call); return NULL; }
			P++;
			return call;
		}
		return cn_var(name);                          /* variable / constant */
	}
	seterr("unexpected character");
	return NULL;
}

static cnode *parse_postfix(void){
	cnode *a = parse_atom();
	if(!a) return NULL;
	for(;;){ skip(); if(*P=='!'){ P++; cnode *f = cn_new(CN_FACT); f->a=a; a=f; } else break; }
	return a;
}

static cnode *parse_unary(void);

static cnode *parse_pow(void){
	cnode *base = parse_postfix();
	if(!base) return NULL;
	skip();
	if(*P=='^' || (P[0]=='*' && P[1]=='*')){          /* '^' or Python '**' */
		P += (*P=='^') ? 1 : 2;
		cnode *e = parse_unary();                     /* right-assoc + allow -exp */
		if(!e){ cn_free(base); return NULL; }
		return cn_bin('^', base, e);
	}
	return base;
}

static cnode *parse_unary_body(void);
static cnode *parse_unary(void){                  /* depth-guarded wrapper */
	if(++calc_depth > CALC_MAXDEPTH){ calc_depth--; seterr("nesting too deep"); return NULL; }
	cnode *r = parse_unary_body();
	calc_depth--;
	return r;
}
static cnode *parse_unary_body(void){
	skip();
	if(*P=='-'){ P++; cnode *u = parse_unary(); return u ? cn_neg(u) : NULL; }
	if(*P=='+'){ P++; return parse_unary(); }
	return parse_pow();
}

static cnode *parse_term(void){
	cnode *a = parse_unary();
	if(!a) return NULL;
	for(;;){
		skip();
		char op = *P;
		if(op=='/' && P[1]=='/'){                     /* Python floor division: a//b -> floor(a/b) */
			P += 2;
			cnode *b = parse_unary();
			if(!b){ cn_free(a); return NULL; }
			cnode *fl = cn_new(CN_CALL);
			strncpy(fl->name, "floor", CN_NAMELEN-1);
			fl->args[fl->nargs++] = cn_bin('/', a, b);
			a = fl;
		} else if(op=='*'||op=='/'||op=='%'){
			P++;
			cnode *b = parse_unary();
			if(!b){ cn_free(a); return NULL; }
			a = cn_bin(op, a, b);
		} else if(op!='+'&&op!='-'&&op!=')'&&op!=','&&op!='='&&op!=0&&op!='!'&&starts_factor()){
			cnode *b = parse_unary();                 /* implicit multiply */
			if(!b){ cn_free(a); return NULL; }
			a = cn_bin('*', a, b);
		} else break;
	}
	return a;
}

static cnode *parse_sum(void){
	cnode *a = parse_term();
	if(!a) return NULL;
	for(;;){
		skip();
		char op = *P;
		if(op=='+'||op=='-'){
			P++;
			cnode *b = parse_term();
			if(!b){ cn_free(a); return NULL; }
			a = cn_bin(op, a, b);
		} else break;
	}
	return a;
}

/* lowest precedence: '=' builds an equation node (so solve(x^2=4,x) parses, and
   top-level a=5 / f(x)=... come through as CN_EQ for the REPL to interpret). */
static cnode *parse_expr(void){
	cnode *a = parse_sum();
	if(!a) return NULL;
	skip();
	if(*P=='='){
		int cmp = (P[1] == '=');          /* Python '==' (equality test) vs '=' (assign/equation) */
		P += cmp ? 2 : 1;
		cnode *b = parse_sum();
		if(!b){ cn_free(a); return NULL; }
		cnode *eq = cn_new(CN_EQ); eq->a = a; eq->b = b;
		if(cmp) eq->op = 'c';
		return eq;
	}
	return a;
}

cnode *calc_parse(const char *src){
	calc_err[0] = 0;
	calc_depth = 0;                    /* fresh parse: a prior deep parse must not poison this one */
	P = src;
	cnode *n = parse_expr();
	if(!n) return NULL;
	skip();
	if(*P){ seterr("trailing characters"); cn_free(n); return NULL; }
	return n;
}
