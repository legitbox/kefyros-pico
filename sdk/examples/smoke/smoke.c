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
    C = k->gfx->canvas(0, 0, w, h);
    k->gfx->clear(C, 0x0000);
    F = k->txt->open("mono", 13);

    char line[80];

    snprintf(line, sizeof line, "sin(1)=%.5f  pow(2,10)=%.0f", sin(1.0), pow(2.0, 10.0));
    k->txt->draw(C, F, 8, 20, line, 0xFFFF);

    snprintf(line, sizeof line, "sqrt(2)=%g  M_PI=%g", sqrt(2.0), M_PI);
    k->txt->draw(C, F, 8, 40, line, 0xFFFF);

    snprintf(line, sizeof line, "exp(1)=%.6g  log(M_E)=%.3f", exp(1.0), log(M_E));
    k->txt->draw(C, F, 8, 60, line, 0xFFFF);

    char *buf = malloc(48);
    strcpy(buf, "malloc + strcpy + free: ok");
    k->txt->draw(C, F, 8, 84, buf, 0xF420);
    free(buf);

    double d = strtod("3.14159", 0);
    snprintf(line, sizeof line, "strtod=%.5f  int=%d  hex=%x", d, 42, 0xBEEF);
    k->txt->draw(C, F, 8, 104, line, 0xFFFF);

    k->txt->draw(C, F, 8, 140, "smoke.kx OK  -  ESC to quit", 0xCE59);
    k->gfx->present(C);

    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("smoke.kx: libkapi end-to-end OK");
    return 0;
}
