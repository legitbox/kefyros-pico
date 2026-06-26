// apps/calc_exact.h — exact (rational) evaluation path for the Kefyros CAS.
// Runs alongside the numeric double evaluator: the REPL tries exact first, falls back to
// numeric when an expression isn't exactly rational. Board-agnostic / host-testable.
#ifndef KF_CALC_EXACT_H
#define KF_CALC_EXACT_H
#include "calc.h"
#include "calc_num.h"

/* Evaluate n to an exact rational. Returns 1 with *out set (caller must cnum_init it first
   and cnum_free it after), or 0 if the expression is not exactly rational — i.e. it contains
   a decimal literal, an irrational constant (pi/e), a variable, a non-integer power, or a
   transcendental call. On 0 the caller falls back to the numeric calc_eval(). */
int calc_eval_exact(const cnode *n, cnum *out);

#endif
