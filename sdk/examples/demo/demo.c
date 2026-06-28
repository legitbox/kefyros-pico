/* demo.c — a self-contained demoscene for the Kefyros handheld (RP2350).
 *
 * A "class-1" .kx app: it compiles against ONE header (kapi.h) and reaches the
 * OS only through the passed `const kapi *k`. It links NOTHING from the kernel,
 * calls NO libc (no math, no malloc) — everything is fixed-point + static arrays.
 *
 * It is WINDOWED: it owns a single 256x176 canvas that the kernel composites
 * BELOW its live topbar (clock/battery/wifi). So while these scenes animate, the
 * OS clock above keeps ticking — proof the kernel stays alive under us.
 *
 * Scenes (cycle with LEFT/RIGHT or keys 1-4, SPACE pauses, ESC quits):
 *   1 PLASMA     — sin-field colour wash, computed at half-res then 2x upscaled.
 *   2 STARFIELD  — a few hundred stars streaming toward the viewer in 3D.
 *   3 COPPER     — Amiga-style raster bars sliding on sine paths.
 *   4 SCROLLER   — big-font greetz scrolling horizontally with a sine wobble.
 *
 * Static RAM budget is dominated by the canvas (90 KB, kernel-owned) plus our own
 * little buffers: a 256-entry sin table, the half-res plasma line buffer, and the
 * star array. Total app-side static data is well under 4 KB (see tally at EOF).
 */
#include "kapi.h"
#include <stdint.h>

/* ===================================================================== */
/* Globals — the app's entire mutable state lives here (no malloc).       */
/* ===================================================================== */

static const kapi *K;               /* the one and only door to the OS    */
static kf_canvas   CV;              /* our live windowed canvas           */
static kf_font     F_HUD;           /* mono 13 — HUD line + labels        */
static kf_font     F_BIG;           /* mono 20 — scroller text            */

#define CW   256                    /* canvas width  (RGB565, 90 KB cap)  */
#define CH   176                    /* canvas height                      */
#define HW   (CW/2)                 /* half-res width  = 128              */
#define HH   ((CH-12)/2)            /* half-res height (leave HUD row)    */

static int      g_scene   = 0;      /* 0..3 active scene                  */
#define NSCENES 4
static int      g_paused  = 0;      /* SPACE toggles                      */
static uint32_t g_t0;               /* millis() at start, our time base   */
static uint32_t g_last_ms;          /* last rendered frame time (pacing)  */
static uint32_t g_phase;            /* animation phase, ms while running  */

/* ===================================================================== */
/* Tiny fixed-point trig. 256-step table, amplitude +/-255 (int8 range    */
/* widened to int16). sin256[i] = round(255*sin(2*pi*i/256)).             */
/* We generate it once at startup from a quarter-wave seed to avoid any   */
/* libm call — fully freestanding.                                        */
/* ===================================================================== */

static int16_t SIN[256];

/* Build SIN[] with a fixed-point CORDIC-free incremental method: we step
 * around the circle using the rotation recurrence
 *     x' = x - (y >> k)*... — but simplest robust approach here is the
 * small-angle integrator:  s += c*dθ ; c -= s*dθ.
 * Done in Q15 with dθ = 2π/256, this drifts a hair, so we renormalise by
 * symmetry: compute only the first quadrant accurately and mirror it. */
static void build_sin(void){
    /* Q15 small-step oscillator for the first quadrant (64 steps = 90deg). */
    /* dθ = (2π/256) in Q15 ≈ 0.0245437 -> 804 */
    int32_t s = 0;          /* sin, Q15 */
    int32_t c = 32767;      /* cos, Q15 */
    const int32_t dth = 804;
    /* Quadrant 0: indices 0..64, sin rising 0->1 */
    for(int i = 0; i <= 64; i++){
        SIN[i] = (int16_t)((s * 255) >> 15);
        /* integrate */
        int32_t ns = s + ((c * dth) >> 15);
        int32_t nc = c - ((ns * dth) >> 15);   /* semi-implicit: stays stable */
        s = ns; c = nc;
    }
    SIN[64] = 255;                              /* pin the peak exactly        */
    /* Mirror to fill the rest of the circle from the quadrant we have:
     *   sin[128-i] =  sin[i]        (i in 0..64)
     *   sin[128+i] = -sin[i]
     *   sin[256-i] = -sin[i]                                          */
    for(int i = 0; i <= 64; i++){
        int16_t v = SIN[i];
        SIN[128 - i] = v;
        SIN[128 + i] = (int16_t)-v;
        SIN[(256 - i) & 255] = (int16_t)-v;
    }
}
/* angle wraps mod 256; returns -255..255 */
static inline int isin(int a){ return SIN[(uint8_t)a]; }
static inline int icos(int a){ return SIN[(uint8_t)(a + 64)]; }

/* ===================================================================== */
/* Colour helpers — 8:8:8 -> RGB565.                                      */
/* ===================================================================== */

static inline kf_color rgb(int r, int g, int b){
    if(r < 0) r = 0;
    if(r > 255) r = 255;
    if(g < 0) g = 0;
    if(g > 255) g = 255;
    if(b < 0) b = 0;
    if(b > 255) b = 255;
    return (kf_color)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* ===================================================================== */
/* Scene buffers (static; no malloc).                                     */
/* ===================================================================== */

/* PLASMA: render one half-res ROW at a time into a small line buffer,
 * then blit it twice (top & bottom of a 2px-high band) to upscale 2x.    */
static kf_color plasma_row[CW];     /* one full-res row = 512 B           */

/* STARFIELD: a few hundred stars, each with x,y in a centred coord space
 * and z depth. Streaming toward viewer => z shrinks; reset when it passes.*/
#define NSTARS 256
static struct { int16_t x, y; uint16_t z; } stars[NSTARS];
static uint32_t g_rng = 0x1234abcdu;          /* local xorshift PRNG       */
static inline uint32_t xr(void){
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 17; g_rng ^= g_rng << 5;
    return g_rng;
}

/* SCROLLER text — wraps around endlessly. */
static const char SCROLL_MSG[] =
    "KEFYROS KAPI v1 -- FIRST DEMOSCENE ON THE PICOCALC -- "
    "GREETINGS TO ALL WHO MADE IT THIS FAR ...   ";

/* ===================================================================== */
/* Scene 1 — PLASMA                                                       */
/* Classic 4-source sin plasma. Computed at 128x82 then 2x upscaled.      */
/* v = sin(x+t) + sin(y+t) + sin((x+y+t)/2) + sin(dist-ish) ; palette it. */
/* ===================================================================== */
static void scene_plasma(int t){
    int pa =  t & 255;                 /* three phase accumulators          */
    int pb = (t * 3 >> 1) & 255;
    int pc = (t >> 1) & 255;

    for(int hy = 0; hy < HH; hy++){
        /* per-row terms that don't depend on x */
        int ty = isin((hy * 4) + pb);                 /* -255..255         */
        for(int hx = 0; hx < HW; hx++){
            int tx  = isin((hx * 4) + pa);
            int txy = isin(((hx + hy) * 3 + pc));
            int v = tx + ty + txy;                    /* -765..765         */
            /* map to a smooth fire/ocean palette via three phase-shifted
             * sines of the value — gives that liquid demoscene look.       */
            int r = 128 + (isin(v / 3 + pa) >> 1);
            int g = 128 + (isin(v / 3 + 85 + pc) >> 1);
            int b = 128 + (isin(v / 3 + 170 + pb) >> 1);
            kf_color col = rgb(r, g, b);
            /* write the upscaled pair of pixels into the row buffer         */
            int x2 = hx * 2;
            plasma_row[x2]     = col;
            plasma_row[x2 + 1] = col;
        }
        /* blit this row to both scanlines of its 2px band (vertical 2x)    */
        K->gfx->blit(CV, 0, hy * 2,     CW, 1, plasma_row);
        K->gfx->blit(CV, 0, hy * 2 + 1, CW, 1, plasma_row);
    }
}

/* ===================================================================== */
/* Scene 2 — STARFIELD                                                    */
/* Perspective projection: sx = cx + x*FOV/z. Star nears => z drops =>    */
/* it spreads out and brightens, then wraps to the far plane.             */
/* ===================================================================== */
static void star_respawn(int i){
    stars[i].x = (int16_t)((int)(xr() % 4096) - 2048);   /* -2048..2047   */
    stars[i].y = (int16_t)((int)(xr() % 4096) - 2048);
    stars[i].z = (uint16_t)(1024 + (xr() % 3072));       /* 1024..4095    */
}
static void scene_starfield(int dt_steps){
    K->gfx->clear(CV, rgb(2, 2, 10));                     /* near-black sky */
    const int cx = CW / 2, cy = (CH - 12) / 2;
    const int FOV = 200;
    for(int i = 0; i < NSTARS; i++){
        /* advance toward viewer (z shrinks). dt_steps scales with frame.   */
        int z = stars[i].z - (dt_steps * 24);
        if(z < 16){ star_respawn(i); continue; }
        stars[i].z = (uint16_t)z;
        int sx = cx + (stars[i].x * FOV) / z;
        int sy = cy + (stars[i].y * FOV) / z;
        if(sx < 0 || sx >= CW || sy < 0 || sy >= CH - 12) continue;
        /* brightness: closer (smaller z) => brighter. z 16..4095          */
        int br = 255 - (z >> 4);
        if(br < 30)  br = 30;
        if(br > 255) br = 255;
        kf_color c = rgb(br, br, br + 16 > 255 ? 255 : br + 16);
        K->gfx->pixel(CV, sx, sy, c);
        /* a 2nd pixel for the very closest stars makes them pop            */
        if(z < 600 && sx + 1 < CW) K->gfx->pixel(CV, sx + 1, sy, c);
    }
}

/* ===================================================================== */
/* Scene 3 — COPPER / RASTER BARS                                         */
/* A handful of horizontal gradient bars whose centres ride sine paths.   */
/* Each bar is drawn as fill() strips with a vertical light->dark ramp.   */
/* ===================================================================== */
static void copper_bar(int cy, int half, int br, int bg, int bb){
    /* draw a symmetric gradient bar centred at cy, +/- half tall.         */
    for(int dy = -half; dy <= half; dy++){
        int y = cy + dy;
        if(y < 0 || y >= CH - 12) continue;
        /* intensity peaks at centre (dy=0), falls to edges -> glossy bar.  */
        int k = 255 - (dy < 0 ? -dy : dy) * 255 / (half + 1);
        kf_color c = rgb(br * k / 255, bg * k / 255, bb * k / 255);
        K->gfx->fill(CV, 0, y, CW, 1, c);
    }
}
static void scene_copper(int t){
    K->gfx->clear(CV, rgb(0, 0, 0));
    const int midy = (CH - 12) / 2;
    const int amp  = (CH - 12) / 2 - 14;
    /* 4 coloured bars, phase-spread around the circle, different speeds.   */
    int y0 = midy + (isin((t * 2)       & 255) * amp >> 8);
    int y1 = midy + (isin((t * 2 + 64)  & 255) * amp >> 8);
    int y2 = midy + (isin((t * 3 + 128) & 255) * amp >> 8);
    int y3 = midy + (isin((t * 3 + 192) & 255) * amp >> 8);
    /* draw far-to-near so overlaps look layered                           */
    copper_bar(y0, 11, 255,  40,  40);   /* red    */
    copper_bar(y1, 11,  40, 255,  90);   /* green  */
    copper_bar(y2, 11,  60, 120, 255);   /* blue   */
    copper_bar(y3, 11, 255, 210,  40);   /* amber  */
}

/* ===================================================================== */
/* Scene 4 — SCROLLER                                                     */
/* The greetz string slides right->left; each character bobs on a sine    */
/* whose phase advances with x, giving a travelling wave.                 */
/* We measure char width via text_w() and draw glyph-by-glyph.            */
/* ===================================================================== */
static int g_scroll_x;              /* current left edge offset (px)       */
static void scene_scroller(int t){
    /* gradient backdrop so the text reads against motion                  */
    for(int y = 0; y < CH - 12; y++){
        int sh = isin((y * 3 + t) & 255);
        K->gfx->fill(CV, 0, y, CW, 1, rgb(8 + (sh >> 4) + 8, 4, 20 + (sh >> 3)));
    }
    int line_h = K->txt->line_h(F_BIG);
    int basey  = (CH - 12 - line_h) / 2;
    int x = -g_scroll_x;
    /* walk the message; draw each character at its wobbled y.             */
    char buf[2]; buf[1] = 0;
    int total_w = 0;
    for(const char *p = SCROLL_MSG; *p; ++p){
        buf[0] = *p;
        int cw = K->txt->text_w(F_BIG, buf);
        total_w += cw;
        if(x > -32 && x < CW){                 /* cull off-canvas glyphs    */
            int wob = (isin((x * 2 + t * 3) & 255) * 14) >> 8;  /* +/-14px  */
            /* simple rainbow cycling on character position                 */
            int hue = (x + t) & 255;
            kf_color c = rgb(128 + (isin(hue) >> 1),
                             128 + (isin(hue + 85) >> 1),
                             128 + (isin(hue + 170) >> 1));
            K->txt->draw(CV, F_BIG, x, basey + wob, buf, c);
        }
        x += cw;
    }
    /* advance & wrap once the whole message has passed                     */
    g_scroll_x += 3;
    if(g_scroll_x > total_w) g_scroll_x -= total_w;
}

/* ===================================================================== */
/* HUD — persistent bottom line (mono 13): scene name + controls.         */
/* ===================================================================== */
static const char *scene_name(int s){
    switch(s){
        case 0: return "PLASMA";
        case 1: return "STARFIELD";
        case 2: return "COPPER BARS";
        default:return "SCROLLER";
    }
}
static void draw_hud(void){
    int y = CH - 11;
    K->gfx->fill(CV, 0, CH - 12, CW, 12, rgb(0, 0, 0));   /* black footer   */
    /* left: scene name (amber). right: controls (grey).                   */
    K->txt->draw(CV, F_HUD, 2, y, scene_name(g_scene), rgb(255, 200, 40));
    const char *ctl = "<- -> scene  SPACE pause  ESC quit";
    int tw = K->txt->text_w(F_HUD, ctl);
    K->txt->draw(CV, F_HUD, CW - tw - 2, y, ctl, rgb(160, 160, 170));
    if(g_paused)
        K->txt->draw(CV, F_HUD, CW/2 - 18, y, "[PAUSE]", rgb(255, 80, 80));
}

/* ===================================================================== */
/* Frame callback — paced to ~30 FPS off millis().                        */
/* ===================================================================== */
static void frame_cb(void *ud){
    (void)ud;
    uint32_t now = K->time->millis();
    if(now - g_last_ms < 33) return;            /* cap at ~30 FPS           */
    uint32_t dt = now - g_last_ms;
    if(dt > 100) dt = 100;                      /* clamp after a long stall */
    g_last_ms = now;

    if(!g_paused) g_phase += dt;                /* freeze time when paused  */

    /* derive an integer "tick" from elapsed ms — scenes use it as phase.   */
    int t = (int)(g_phase >> 3);                /* ~125 ticks/sec           */
    int dt_steps = (int)(dt >> 3);              /* per-frame advance amount */
    if(dt_steps < 1) dt_steps = 1;

    switch(g_scene){
        case 0: scene_plasma(t);            break;
        case 1: scene_starfield(g_paused ? 0 : dt_steps); break;
        case 2: scene_copper(t);            break;
        default: if(!g_paused) scene_scroller(t);
                 else          scene_scroller(t);   /* still redraw, frozen */
                 break;
    }
    draw_hud();
    K->gfx->present(CV);                        /* tell the OS to composite */
}

/* ===================================================================== */
/* Key callback — scene switching, pause, quit.                           */
/* ===================================================================== */
static void set_scene(int s){
    g_scene = ((s % NSCENES) + NSCENES) % NSCENES;
    g_scroll_x = 0;                             /* reset scroller on entry  */
    K->gfx->clear(CV, rgb(0, 0, 0));            /* wipe leftovers           */
    g_last_ms = 0;                              /* force an immediate redraw*/
}
static void key_cb(void *ud, int key, int down){
    (void)ud;
    if(!down) return;
    switch(key){
        case KF_KEY_ESC:   K->sys->exit(0);                 break;
        case KF_KEY_LEFT:  set_scene(g_scene - 1);          break;
        case KF_KEY_RIGHT: set_scene(g_scene + 1);          break;
        case '1': case '2': case '3': case '4':
                           set_scene(key - '1');            break;
        case ' ':          g_paused = !g_paused;            break;
        default: break;
    }
}

/* ===================================================================== */
/* Entry point — set up the windowed canvas and register callbacks.       */
/* ===================================================================== */
int app_main(const kapi *k){
    K = k;

    build_sin();                                /* freestanding trig table  */

    /* Centre the 256x176 canvas horizontally, just under the 24px topbar.  */
    int sw, sh;
    k->gfx->screen_size(&sw, &sh);
    int cx = (sw - CW) / 2; if(cx < 0) cx = 0;
    int cy = 28;                                /* a few px below topbar    */
    CV = k->gfx->canvas(cx, cy, CW, CH);        /* the one live canvas      */
    k->gfx->clear(CV, rgb(0, 0, 0));

    F_HUD = k->txt->open("mono", 13);
    F_BIG = k->txt->open("mono", 20);

    /* seed the starfield so scene 2 is populated the moment it's shown.    */
    for(int i = 0; i < NSTARS; i++) star_respawn(i);

    k->sys->perf(KF_PERF_BOOST);                /* smooth animation         */

    g_t0 = k->time->millis();
    g_last_ms = 0;                              /* render on first frame    */
    g_phase = 0;

    k->sys->on_frame(frame_cb, 0);
    k->sys->on_key(key_cb, 0);
    k->sys->log("demo.kx: app_main done — 4 scenes armed");
    return 0;                                    /* kernel drives the loop   */
}

/* ----------------------------------------------------------------------
 * App-side static RAM tally (excludes the kernel-owned 90 KB canvas):
 *   SIN[256]      int16   =  512 B
 *   plasma_row    256xu16 =  512 B
 *   stars[256]    6 B each= 1536 B
 *   SCROLL_MSG    rodata  ~  100 B
 *   scalars/ptrs           <  64 B
 *   --------------------------------
 *   total .data/.bss      ~ 3.1 KB   (well under any sane budget)
 * The 90 KB RGB565 canvas (256*176*2) is the kernel's compositor buffer,
 * the documented ceiling, and is NOT double-allocated by us.
 * -------------------------------------------------------------------- */
