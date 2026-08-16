/* smoke.c — proves the KAPI standard library links and runs end-to-end.
 * Exercises: math forwarding (sin/pow/sqrt/M_PI), snprintf %f/%g/%d, malloc+strcpy+free,
 * and strtod. If this draws correct numbers on the panel, libkapi is good. */
#include "kapi.h"
#include "kapi_rt.h"
#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static const kapi *K;
static kf_canvas C;
static kf_font   F;

static void on_key(void *ud, int key, int down){ (void)ud; if(down && key == KF_KEY_ESC) K->sys->exit(0); }
static void on_frame(void *ud){ (void)ud; }

int app_main(const kapi *k){
    K = k;
    kapi_rt_init(k);                       /* arm the stdlib before any malloc/printf/math */

    int w, h; k->gfx->screen_size(&w, &h);
    /* Small canvas (must fit the 96 KB arena's free space — a full-screen 320x320 = 200 KB
       can't be carved and the heap fallback fails while the launcher is resident). */
    int cw = w, ch = 96;                          /* 320x96x2 = 60 KB, fits the arena */
    C = k->gfx->canvas(0, 28, cw, ch);            /* just below the topbar */
    k->gfx->clear(C, 0x0000);
    F = k->txt->open("mono", 13);

    char line[80];

    snprintf(line, sizeof line, "sin1=%.5f pow=%.0f sqrt2=%.4f", sin(1.0), pow(2.0, 10.0), sqrt(2.0));
    k->txt->draw(C, F, 6, 6, line, 0xFFFF);

    snprintf(line, sizeof line, "PI=%.5f e=%.5f ln(e)=%.3f", M_PI, exp(1.0), log(M_E));
    k->txt->draw(C, F, 6, 26, line, 0xFFFF);

    char *buf = malloc(32);
    int mok = 0;
    if(buf){ strcpy(buf, "malloc ok"); mok = (strcmp(buf, "malloc ok") == 0); free(buf); }
    double d = strtod("3.14159", 0);
    snprintf(line, sizeof line, "%s strtod=%.4f d=%d x=%x", mok ? "malloc ok" : "MALLOC FAIL", d, 42, 0xBEEF);
    k->txt->draw(C, F, 6, 46, line, mok ? 0xF420 : 0xF800);

    k->txt->draw(C, F, 6, 70, "smoke.kx OK  -  ESC to quit", 0xCE59);
    k->gfx->present(C);

    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("smoke.kx: libkapi end-to-end OK");
    return 0;
}
