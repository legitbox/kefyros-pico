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

    /* A live RGB565 canvas consumes 2 bytes/pixel. Keep this tiny demo inside the
       app arena; full-screen animation should use gfx->view_* strip streaming. */
    C = k->gfx->canvas(10, 26, w - 20, 96);
    if(!C){ k->sys->log("hello.kx: canvas allocation failed"); return -1; }
    k->gfx->clear(C, 0x0000);
    k->gfx->fill (C, 2, 2, w - 24, 36, 0xF420);            /* amber banner            */

    kf_font big = k->txt->open("mono", 20);
    kf_font reg = k->txt->open("mono", 13);
    k->txt->draw(C, big, 12, 8, "hello.kx", 0x0000);         /* dark text on amber      */
    k->txt->draw(C, reg, 6, 50, "first-class SD app - ESC quits", 0xFFFF);
    k->txt->draw(C, reg, 6, 70, "no firmware reflash required", 0xCE59);
    k->gfx->present(C);

    k->sys->on_key(on_key, 0);
    k->sys->on_frame(on_frame, 0);
    k->sys->log("hello.kx: app_main done");
    return 0;
}
