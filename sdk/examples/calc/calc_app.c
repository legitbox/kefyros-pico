/* calc_app.c — Phase-4 step 1: an interactive calc REPL as a class-1 .kx.
 *
 * Links the calc compute core UNMODIFIED and drives it from KAPI input/canvas: type an
 * expression, ENTER evaluates it (parse -> eval -> calc_fmt), the result scrolls into a
 * small history. Windowed (topbar persists); the canvas is intentionally small + arena-
 * carved (no big heap buffer, no arena bump — respects the SRAM budget). Going full-screen
 * later is the direct-SPI text path the plotters use, not a bigger canvas. */
#include "kapi.h"
#include "kapi_rt.h"
#include "calc.h"
#include <stdio.h>
#include <string.h>

#define HISTN  32          /* stored history lines */
#define COLS   56          /* per-line storage (display clips to canvas width) */
#define VISN   4           /* history rows visible above the input line */

static const kapi *K;
static kf_canvas  CV;
static kf_font    F;
static int        CW, CH;

static char hist[HISTN][COLS]; static int nhist = 0;
static char input[96];         static int inlen = 0;
static int  dirty = 1;

static void push_hist(const char *s){
    if(nhist < HISTN){ snprintf(hist[nhist++], COLS, "%s", s); return; }
    for(int i = 1; i < HISTN; i++) memcpy(hist[i-1], hist[i], COLS);
    snprintf(hist[HISTN-1], COLS, "%s", s);
}

static void do_eval(void){
    if(inlen == 0) return;
    char out[COLS];
    cnode *n = calc_parse(input);
    if(!n){
        snprintf(out, COLS, "%s  ! %s", input, calc_err);
    } else {
        int ok = 1; double v = calc_eval(n, &ok); cn_free(n);
        if(!ok) snprintf(out, COLS, "%s  ! %s", input, calc_err);
        else { char nb[40]; calc_fmt(v, nb, sizeof nb); snprintf(out, COLS, "%s = %s", input, nb); }
    }
    push_hist(out);
    inlen = 0; input[0] = 0;
}

static void redraw(void){
    K->gfx->clear(CV, 0x0000);
    int first = nhist > VISN ? nhist - VISN : 0;
    int y = 2;
    for(int i = first; i < nhist; i++){ K->txt->draw(CV, F, 4, y, hist[i], 0xFEA0); y += 13; }
    char line[110];
    snprintf(line, sizeof line, "> %s_", input);
    K->txt->draw(CV, F, 4, CH - 14, line, 0xFFFF);
    K->gfx->present(CV);
}

static void on_key(void *ud, int key, int down){
    (void)ud;
    if(!down) return;
    if(key == KF_KEY_ESC){ K->sys->exit(0); return; }
    if(key == KF_KEY_ENTER){ do_eval(); dirty = 1; return; }
    if(key == KF_KEY_BKSP){ if(inlen > 0){ input[--inlen] = 0; dirty = 1; } return; }
    if(key >= 0x20 && key < 0x7f && inlen < (int)sizeof input - 1){
        input[inlen++] = (char)key; input[inlen] = 0; dirty = 1;
    }
}
static void on_frame(void *ud){ (void)ud; if(dirty){ dirty = 0; redraw(); } }

int app_main(const kapi *k){
    K = k;
    kapi_rt_init(k);
    calc_reset_env();

    k->gfx->screen_size(&CW, &CH);
    CH = 68;                              /* 320x68x2 = 43.5 KB — arena-carved */
    CV = k->gfx->canvas(0, 26, CW, CH);
    F  = k->txt->open("mono", 13);
    k->gfx->clear(CV, 0x0000);

    push_hist("Kefyros calc (KAPI) - type, ENTER, ESC=quit");

    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("calc.kx: REPL ready");
    return 0;
}
