/* sdk/include/math.h — KAPI <math.h>.
 *
 * The polynomial-approximation transcendentals are extern functions implemented in libkapi
 * (kapi_math.c), each a one-line forward to the kernel's libm via k->math. The trivial ops
 * (fabs/fmin/fmax/copysign/classification) are GCC builtins — inline, no libm, no vtable. */
#ifndef KAPI_MATH_H
#define KAPI_MATH_H

#ifdef __cplusplus
extern "C" {
#endif

/* ---- constants ---- */
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_1_PI     0.31830988618379067154
#define M_2_PI     0.63661977236758134308
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440

#define HUGE_VAL   (__builtin_huge_val())
#define INFINITY   (__builtin_inff())
#define NAN        (__builtin_nanf(""))

/* ---- trivial ops: GCC builtins (inline, freestanding) ---- */
#define fabs(x)        __builtin_fabs(x)
#define fmin(a,b)      __builtin_fmin((a),(b))
#define fmax(a,b)      __builtin_fmax((a),(b))
#define copysign(a,b)  __builtin_copysign((a),(b))
#define signbit(x)     __builtin_signbit(x)
#define isnan(x)       __builtin_isnan(x)
#define isinf(x)       __builtin_isinf(x)
#define isfinite(x)    __builtin_isfinite(x)
#define isnormal(x)    __builtin_isnormal(x)

/* ---- forwarded to kernel libm (kapi_math.c) ---- */
double sin(double);   double cos(double);   double tan(double);
double asin(double);  double acos(double);  double atan(double);
double atan2(double, double);
double sinh(double);  double cosh(double);  double tanh(double);
double asinh(double); double acosh(double); double atanh(double);
double exp(double);   double expm1(double);
double log(double);   double log1p(double); double log10(double); double log2(double);
double pow(double, double);
double sqrt(double);  double cbrt(double);  double hypot(double, double);
double fmod(double, double);
double floor(double); double ceil(double);  double round(double); double trunc(double);
double lgamma(double); double tgamma(double);
double erf(double);   double erfc(double);

#ifdef __cplusplus
}
#endif
#endif /* KAPI_MATH_H */
