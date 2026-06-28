/* hello.c — the smallest real Kefyros app: a windowed class-1 .kx.
 * Draws a banner via the live canvas + font, echoes nothing, quits on ESC.
 * Links NOTHING from the kernel — everything is reached through the passed kapi*. */
#include "kapi.h"

static const kapi *K;
static kf_canvas  C;

static void on_key(void *ud, int key, int down){
    (void)ud;
    if(down && key == KF_KEY_ESC) K->sys->exit(0);
}
static void on_frame(void *ud){ (void)ud; }   /* nothing animated; UI is static */

int app_main(const kapi *k){
    K = k;

    int w, h;
    k->gfx->screen_size(&w, &h);

    C = k->gfx->canvas(0, 0, w, h);
    k->gfx->clear(C, 0x0000);
    k->gfx->fill (C, 12, 20, w - 24, 44, 0xF420);          /* amber banner            */

    kf_font big = k->txt->open("mono", 20);
    kf_font reg = k->txt->open("mono", 13);
    k->txt->draw(C, big, 22, 30, "hello.kx", 0x0000);       /* dark text on amber      */
    k->txt->draw(C, reg, 16, 86, "first class-1 app on KAPI", 0xFFFF);
    k->txt->draw(C, reg, 16, 110, "loaded from /apps/hello", 0xCE59);
    k->txt->draw(C, reg, 16, 150, "ESC to quit", 0xFFFF);
    k->gfx->present(C);

    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("hello.kx: app_main done");
    return 0;
}
