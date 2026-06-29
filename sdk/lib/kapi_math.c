/* sdk/lib/kapi_math.c — libkapi math: one-line forwards to the kernel's libm via k->math.
 * Trivial ops (fabs/fmin/fmax/copysign/isnan/...) are builtins in <math.h>, not here. */
#include "kapi_rt.h"
#include <math.h>

#define FWD1(name)      double name(double x){ return g_k->math->name(x); }
#define FWD2(name)      double name(double a, double b){ return g_k->math->name(a, b); }

FWD1(sin)   FWD1(cos)   FWD1(tan)
FWD1(asin)  FWD1(acos)  FWD1(atan)
FWD2(atan2)
FWD1(sinh)  FWD1(cosh)  FWD1(tanh)
FWD1(asinh) FWD1(acosh) FWD1(atanh)
FWD1(exp)   FWD1(expm1)
FWD1(log)   FWD1(log1p) FWD1(log10) FWD1(log2)
FWD2(pow)
FWD1(sqrt)  FWD1(cbrt)  FWD2(hypot)
FWD2(fmod)
FWD1(floor) FWD1(ceil)  FWD1(round) FWD1(trunc)
FWD1(lgamma) FWD1(tgamma)
FWD1(erf)   FWD1(erfc)
