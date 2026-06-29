/* calc_app.c — Phase-3 milestone: the calc COMPUTE CORE running as a class-1 .kx.
 *
 * Links the 7 pure calc files (eval/parse/exact/num/bignum/sym/solve) UNMODIFIED against
 * libkapi, then runs a battery of expressions through parse -> eval -> format. If the
 * numbers are right on hardware, it proves the core works end-to-end via the shimmed
 * strtod/snprintf and the k_math vtable — i.e. the whole arithmetic stack is sound.
 *
 * Tiny canvas (arena-carved, no heap malloc) — the full REPL/forms UI comes next. */
#include "kapi.h"
#include "kapi_rt.h"
#include "calc.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

static const kapi *K;
static kf_canvas C;
static kf_font   F;

static int eval_num(const char *expr, double *out){
    cnode *n = calc_parse(expr);
    if(!n) return 0;
    int ok = 1;                      /* calc_eval only CLEARS ok on error; seed it to 1 */
    double v = calc_eval(n, &ok);
    cn_free(n);
    if(ok) *out = v;
    return ok;
}

static void on_key(void *ud, int key, int down){ (void)ud; if(down && key == KF_KEY_ESC) K->sys->exit(0); }
static void on_frame(void *ud){ (void)ud; }

int app_main(const kapi *k){
    K = k;
    kapi_rt_init(k);                 /* arm libkapi (malloc/printf/math) */
    calc_reset_env();                /* constants pi/e, empty var/fun tables */

    int w, h; k->gfx->screen_size(&w, &h);
    C = k->gfx->canvas(0, 28, w, 46);   /* 320x46x2 = 29 KB — fits the arena, no heap */
    k->gfx->clear(C, 0x0000);
    F = k->txt->open("mono", 13);

    /* known-answer battery (exercises +,*,^,!, and the k_math forwards) */
    static const struct { const char *e; double want; } T[] = {
        {"2+2", 4}, {"3*7+1", 22}, {"2^10", 1024}, {"5!", 120},
        {"1/3+1/6", 0.5}, {"sin(0)", 0}, {"cos(0)", 1},
        {"sqrt(2)", 1.4142135623730951}, {"exp(1)", 2.718281828459045},
        {"ln(exp(1))", 1}, {"log(1000)", 3},     /* ln = natural, log = log10 */
        {"atan(1)*4", 3.141592653589793},
    };
    int N = (int)(sizeof T / sizeof T[0]), pass = 0;
    for(int i = 0; i < N; i++){ double v; if(eval_num(T[i].e, &v) && fabs(v - T[i].want) < 1e-9) pass++; }

    char line[80]; double v; char nb[32];

    snprintf(line, sizeof line, "CALC CORE %d/%d OK   atan(1)*4=%.8g", pass, N,
             (eval_num("atan(1)*4", &v), v));
    k->txt->draw(C, F, 4, 2, line, pass == N ? 0x07E0 : 0xF800);

    eval_num("sqrt(2)", &v); calc_fmt(v, nb, sizeof nb);
    snprintf(line, sizeof line, "sqrt(2)=%s   1/3+1/6=%g", nb, (eval_num("1/3+1/6", &v), v));
    k->txt->draw(C, F, 4, 16, line, 0xFFFF);

    snprintf(line, sizeof line, "2^10=%g  5!=%g  ln(e)=%g  log(1k)=%g",
             (eval_num("2^10", &v), v), (eval_num("5!", &v), v),
             (eval_num("ln(exp(1))", &v), v), (eval_num("log(1000)", &v), v));
    k->txt->draw(C, F, 4, 30, line, 0xFFFF);

    k->gfx->present(C);
    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("calc.kx: compute-core battery done");
    return 0;
}
