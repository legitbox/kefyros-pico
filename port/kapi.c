// port/kapi.c — Kefyros App Programming Interface (KAPI) runtime: loader + vtable glue.
//
// Loads a class-1 app (.kx) from SD into a fixed SRAM arena and runs it. The app reaches
// the OS ONLY through the `kapi` function-pointer table built here — it links no kernel
// symbols. See KAPI.md / sdk/kapi.h.
//
// ARENA: a static array (NO linker-memmap surgery -> zero boot-map risk). The app is built
// to load at &g_kapi_arena (the build script reads its address via nm and links the app there),
// and the loader refuses any .kx whose header load_base != &g_kapi_arena. Code executes from
// SRAM (RP2350 SRAM is executable); __dsb/__isb make the freshly-copied code visible.
#include "kefyros.h"
#include "kapi.h"
#include "theme.h"           /* KF_FONT / KF_FONT_BIG */
#include "lcdspi/lcdspi.h"
#include "disp.h"
#include "imgdec.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

/* ===================== arena + app state ===================== */
#define KAPI_ARENA_BYTES (64 * 1024)
static uint8_t g_kapi_arena[KAPI_ARENA_BYTES] __attribute__((aligned(32)));

typedef int (*app_main_fn)(const kapi *);

static int   s_active = 0, s_exit = 0;
static lv_obj_t *s_scr = NULL;
static char  s_app_dir[256] = "/apps";

static void (*s_on_frame)(void*);                 static void *s_on_frame_ud;
static void (*s_on_key)(void*, int, int);         static void *s_on_key_ud;
static void (*s_on_close)(void*);                 static void *s_on_close_ud;

/* ===================== canvas implementation ===================== */
struct kf_canvas_s { lv_obj_t *obj; uint16_t *buf; int w, h, from_static; };
#define MAXCANV 4
static struct kf_canvas_s *s_canv[MAXCANV];
static int s_ncanv;
/* one windowed canvas backed by a STATIC buffer (up to 256x176) so it can never fail to
   allocate / fragment the heap while the launcher screen is still resident. Bigger or
   additional canvases fall back to malloc. */
static uint16_t s_cbuf[256 * 176] __attribute__((aligned(4)));
static int s_cbuf_used = 0;

static inline lv_color_t c565(kf_color c){
    uint8_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    return lv_color_make((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static kf_canvas g_canvas(int x, int y, int w, int h){
    if(s_ncanv >= MAXCANV || !s_scr) return NULL;
    struct kf_canvas_s *c = malloc(sizeof *c);
    if(!c) return NULL;
    if(!s_cbuf_used && (size_t)w * h <= sizeof s_cbuf / 2){ c->buf = s_cbuf; s_cbuf_used = 1; c->from_static = 1; }
    else { c->buf = malloc((size_t)w * h * 2); if(!c->buf){ free(c); return NULL; } c->from_static = 0; }
    c->w = w; c->h = h;
    c->obj = lv_canvas_create(s_scr);
    lv_canvas_set_buffer(c->obj, c->buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_size(c->obj, w, h);                /* don't rely on image auto-size */
    lv_obj_set_pos(c->obj, x, y);
    s_canv[s_ncanv++] = c;
    return (kf_canvas)c;
}
static void g_canvas_destroy(kf_canvas h){
    struct kf_canvas_s *c = (struct kf_canvas_s*)h; if(!c) return;
    for(int i = 0; i < s_ncanv; i++) if(s_canv[i] == c){ s_canv[i] = s_canv[--s_ncanv]; break; }
    if(c->obj) lv_obj_delete(c->obj);
    if(c->from_static) s_cbuf_used = 0; else free(c->buf);
    free(c);
}
static void g_present(kf_canvas h){ struct kf_canvas_s *c = (void*)h; if(c) lv_obj_invalidate(c->obj); }

static void g_clear(kf_canvas h, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    int n = c->w * c->h; for(int i = 0; i < n; i++) c->buf[i] = col;
}
static void g_pixel(kf_canvas h, int x, int y, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    if((unsigned)x < (unsigned)c->w && (unsigned)y < (unsigned)c->h) c->buf[y * c->w + x] = col;
}
static void g_fill(kf_canvas h, int x, int y, int w, int hh, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > c->w ? c->w : x + w, y1 = y + hh > c->h ? c->h : y + hh;
    for(int yy = y0; yy < y1; yy++){ uint16_t *row = c->buf + yy * c->w; for(int xx = x0; xx < x1; xx++) row[xx] = col; }
}
static void g_line(kf_canvas h, int x0, int y0, int x1, int y1, kf_color col){
    int dx = abs(x1 - x0), dy = -abs(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, e = dx + dy;
    for(;;){ g_pixel(h, x0, y0, col); if(x0 == x1 && y0 == y1) break; int e2 = 2 * e;
        if(e2 >= dy){ e += dy; x0 += sx; } if(e2 <= dx){ e += dx; y0 += sy; } }
}
static void g_rect(kf_canvas h, int x, int y, int w, int hh, kf_color col){
    g_line(h, x, y, x + w - 1, y, col); g_line(h, x, y + hh - 1, x + w - 1, y + hh - 1, col);
    g_line(h, x, y, x, y + hh - 1, col); g_line(h, x + w - 1, y, x + w - 1, y + hh - 1, col);
}
static void g_blit(kf_canvas h, int x, int y, int w, int hh, const kf_color *px){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    for(int r = 0; r < hh; r++){ int yy = y + r; if((unsigned)yy >= (unsigned)c->h) continue;
        for(int cc = 0; cc < w; cc++){ int xx = x + cc; if((unsigned)xx < (unsigned)c->w) c->buf[yy*c->w + xx] = px[r*w + cc]; } }
}
static void g_clip(kf_canvas h, int x, int y, int w, int hh){ (void)h;(void)x;(void)y;(void)w;(void)hh; }
static void g_screen_size(int *w, int *h){ if(w) *w = LCD_W; if(h) *h = LCD_H; }

/* exclusive (raw panel) — wrappers over the same path the GB emulator uses. push() takes
   already-formatted RGB888 bytes (panel-native), like spi_write_fast in gameboy.c. */
static kf_err g_lease(void){ spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED); disp_pause_core1(); return KF_OK; }
static void   g_release(void){ disp_resume_core1(); }
static void   g_region(int x0,int y0,int x1,int y1){ define_region_spi(x0,y0,x1,y1,1); }
static void   g_push(const void *px, size_t n){ spi_write_fast(Pico_LCD_SPI_MOD, (const uint8_t*)px, n); }
static void   g_flush(void){ spi_finish(Pico_LCD_SPI_MOD); lcd_spi_raise_cs(); }
static kf_err g_set_panel_hz(uint32_t hz){ spi_set_baudrate(Pico_LCD_SPI_MOD, hz); return KF_OK; }

/* windowed turbo (view) — not implemented for v1; safe no-ops returning NULL */
static kf_view g_view_open(int x,int y,int w,int h){ (void)x;(void)y;(void)w;(void)h; return NULL; }
static void g_view_region(kf_view v,int a,int b,int c,int d){ (void)v;(void)a;(void)b;(void)c;(void)d; }
static void g_view_push(kf_view v,const void*p,size_t n){ (void)v;(void)p;(void)n; }
static void g_view_flush(kf_view v){ (void)v; }
static void g_view_close(kf_view v){ (void)v; }

/* ===================== fonts / text ===================== */
static kf_font g_font_open(const char *name, int px){ (void)name; return (kf_font)(px >= 18 ? KF_FONT_BIG : KF_FONT); }
static void    g_font_close(kf_font f){ (void)f; }
static int     g_line_h(kf_font f){ return f ? ((const lv_font_t*)f)->line_height : 13; }
static int     g_text_w(kf_font f, const char *s){
    lv_point_t sz; lv_text_get_size(&sz, s, (const lv_font_t*)f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}
static void    g_draw_text(kf_canvas h, kf_font f, int x, int y, const char *s, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c || !s) return;
    lv_layer_t layer; lv_canvas_init_layer(c->obj, &layer);
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.font = (const lv_font_t*)f; d.color = c565(col);
    lv_area_t a = { x, y, c->w - 1, y + ((const lv_font_t*)f)->line_height };
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(c->obj, &layer);
}
static int g_glyph(kf_font f, uint32_t cp, int *w, int *h, const uint8_t **bm){ (void)f;(void)cp;(void)bm; if(w)*w=0; if(h)*h=0; return 0; }

/* ===================== input ===================== */
static int map_dk(uint8_t k){
    switch(k){
    case DK_ESC: return KF_KEY_ESC;   case DK_ENTER: return KF_KEY_ENTER;
    case DK_BACKSPACE: return KF_KEY_BKSP; case DK_TAB: return KF_KEY_TAB; case DK_DEL: return KF_KEY_DEL;
    case DK_UP: return KF_KEY_UP; case DK_DOWN: return KF_KEY_DOWN; case DK_LEFT: return KF_KEY_LEFT; case DK_RIGHT: return KF_KEY_RIGHT;
    case DK_HOME: return KF_KEY_HOME; case DK_END: return KF_KEY_END; case DK_PGUP: return KF_KEY_PGUP; case DK_PGDN: return KF_KEY_PGDN;
    default: if(k >= DK_F1 && k <= DK_F1 + 9) return KF_KEY_F1 + (k - DK_F1);
             if(k >= 0x20 && k < 0x7f) return k;  return 0;
    }
}
static int map_mods(int m){ int r=0; if(m&MOD_CTRL)r|=KF_MOD_CTRL; if(m&(MOD_SHL|MOD_SHR))r|=KF_MOD_SHIFT; if(m&MOD_ALT)r|=KF_MOD_ALT; return r; }
static int g_in_poll(int *key, int *down, int *mods){
    uint8_t st, k; if(!uart_pop_key(&st, &k)) return 0;
    if(key) *key = map_dk(k); if(down) *down = (st != KS_RELEASE); if(mods) *mods = map_mods(uart_mods()); return 1;
}
static int g_in_mods(void){ return map_mods(uart_mods()); }

/* ===================== sys ===================== */
static void     y_exit(int code){ (void)code; s_exit = 1; }
static void     y_on_frame(void (*fn)(void*), void *ud){ s_on_frame = fn; s_on_frame_ud = ud; }
static void     y_on_key(void (*fn)(void*,int,int), void *ud){ s_on_key = fn; s_on_key_ud = ud; }
static void     y_on_close(void (*fn)(void*), void *ud){ s_on_close = fn; s_on_close_ud = ud; }
static void     y_pump(void){ kf_net_poll(); }
static void     y_perf(enum kf_perf p){ if(p==KF_PERF_ECO) kf_clock_eco(); else if(p==KF_PERF_BOOST) kf_clock_boost(); else kf_clock_normal(); }
static uint32_t y_clock_hz(void){ return kf_clock_khz() * 1000u; }
static void     y_idle_policy(enum kf_idle p){ (void)p; }
static int      y_battery_pct(void){ uint8_t b = 0; if(reg_read(REG_BAT, &b, 1) > 0) return b & 0x7f; return -1; }
static int      y_charging(void){ uint8_t b = 0; if(reg_read(REG_BAT, &b, 1) > 0) return (b >> 7) & 1; return 0; }
static int      y_get_brightness(void){ return 5; }
static int      y_get_volume(void){ return 5; }
static uint32_t y_caps(void){ uint32_t c = KF_CAP_AUDIO_OUT | KF_CAP_RNG;
    if(kf_psram_size()) c |= KF_CAP_PSRAM; if(kf_net_present()) c |= KF_CAP_WIFI | KF_CAP_TLS; c |= KF_CAP_IMG; return c; }
static uint32_t y_rng(void){ static uint32_t s = 0x2545F491; s ^= s<<13; s ^= s>>17; s ^= s<<5; s += (uint32_t)time_us_64(); return s; }
static const char *y_kernel_version(void){ return KF_VERSION; }
static int       y_last_error(void){ return 0; }
static const char *y_err_str(int e){ (void)e; return ""; }
static void      y_log(const char *s){ if(s) printf("[kapi-app] %s\n", s); }

/* ===================== time ===================== */
static uint32_t t_millis(void){ return (uint32_t)(time_us_64() / 1000u); }
static uint64_t t_micros(void){ return time_us_64(); }
static void     t_sleep_ms(uint32_t ms){ sleep_ms(ms); }
static uint32_t t_now_unix(void){ return 0; }

/* ===================== mem ===================== */
static void  *m_alloc(size_t n){ return malloc(n); }
static void  *m_realloc(void *p, size_t n){ return realloc(p, n); }
static void   m_free(void *p){ free(p); }
static size_t m_avail(void){ return 0; }
static kf_mem m_psram_alloc(size_t n){ uint32_t o = kf_psram_alloc((uint32_t)n); return o == 0xFFFFFFFFu ? 0 : o + 1; }
static void   m_psram_free(kf_mem h){ if(h) kf_psram_free_to(h - 1); }
static void   m_psram_read(kf_mem h, size_t off, void *d, size_t n){ if(h) kf_psram_read((h - 1) + off, d, n); }
static void   m_psram_write(kf_mem h, size_t off, const void *s, size_t n){ if(h) kf_psram_write((h - 1) + off, s, n); }
static void  *m_lock(kf_mem h){ (void)h; return NULL; }
static void   m_unlock(kf_mem h){ (void)h; }

/* ===================== fs (POSIX) ===================== */
static kf_file f_open(const char *p, const char *m){ return (kf_file)fopen(p, m); }
static int  f_read(kf_file f, void *b, int n){ return f ? (int)fread(b, 1, n, (FILE*)f) : -1; }
static int  f_write(kf_file f, const void *b, int n){ return f ? (int)fwrite(b, 1, n, (FILE*)f) : -1; }
static int  f_seek(kf_file f, long o, int w){ return f ? fseek((FILE*)f, o, w) : -1; }
static long f_tell(kf_file f){ return f ? ftell((FILE*)f) : -1; }
static void f_close(kf_file f){ if(f) fclose((FILE*)f); }
static kf_dir f_opendir(const char *p){ return (kf_dir)opendir(p); }
static int  f_readdir(kf_dir d, char *name, int n, int *is_dir){
    if(!d) return 0; struct dirent *e = readdir((DIR*)d); if(!e) return 0;
    snprintf(name, n, "%s", e->d_name);
    if(is_dir){ char p[300]; struct stat stt; snprintf(p, sizeof p, "%s/%s", s_app_dir, e->d_name);
        *is_dir = (stat(p, &stt) == 0 && S_ISDIR(stt.st_mode)); }
    return 1;
}
static void f_closedir(kf_dir d){ if(d) closedir((DIR*)d); }
static kf_err f_remove(const char *p){ return remove(p) == 0 ? KF_OK : KF_EIO; }
static kf_err f_rename(const char *a, const char *b){ return rename(a, b) == 0 ? KF_OK : KF_EIO; }
static kf_err f_mkdir(const char *p){ return mkdir(p, 0777) == 0 ? KF_OK : KF_EIO; }
static const char *f_app_dir(void){ return s_app_dir; }
static kf_err f_stage(const char *p, const void **xip, size_t *sz){ (void)p;(void)xip;(void)sz; return KF_EUNSUPP; }
static void   f_unstage(void){}

/* ===================== aud (stereo ring) ===================== */
static kf_err a_out_start(int rate){ kf_audio_start(rate); return KF_OK; }
static int    a_out_space(void){ return kf_audio_space(); }
static int    a_out_write(const int16_t *s, int nf){ return kf_audio_write(s, nf); }
static int    a_out_running(void){ return kf_audio_running(); }
static void   a_out_stop(void){ kf_audio_stop(); }
static void   a_tone(int hz, int ms){ (void)hz; (void)ms; }

/* ===================== net / tls / img : safe stubs (phase 2) ===================== */
static kf_sock n_connect(const char *h, int p, int u){ (void)h;(void)p;(void)u; return NULL; }
static int  n_send(kf_sock s, const void *b, int n){ (void)s;(void)b;(void)n; return KF_EUNSUPP; }
static int  n_recv(kf_sock s, void *b, int n){ (void)s;(void)b;(void)n; return KF_EUNSUPP; }
static int  n_status(kf_sock s){ (void)s; return KF_EUNSUPP; }
static void n_close(kf_sock s){ (void)s; }
static kf_err n_resolve(const char *h, uint32_t *ip){ (void)h;(void)ip; return KF_EUNSUPP; }
static int  n_online(void){ return kf_net_present() && kf_net_state() == KF_NET_ONLINE; }
static int  n_rssi(void){ return 0; }
static uint32_t n_ip(void){ return 0; }

static kf_tls tl_wrap(kf_sock s, const char *sni){ (void)s;(void)sni; return NULL; }
static int  tl_handshake(kf_tls t){ (void)t; return KF_EUNSUPP; }
static int  tl_send(kf_tls t, const void *b, int n){ (void)t;(void)b;(void)n; return KF_EUNSUPP; }
static int  tl_recv(kf_tls t, void *b, int n){ (void)t;(void)b;(void)n; return KF_EUNSUPP; }
static void tl_close(kf_tls t){ (void)t; }

static kf_img im_decode(const void *b, size_t l){ (void)b;(void)l; return NULL; }
static kf_img im_decode_file(const char *p){ (void)p; return NULL; }
static void   im_info(kf_img i, int *w, int *h){ (void)i; if(w)*w=0; if(h)*h=0; }
static void   im_to_canvas(kf_img i, kf_canvas c, int x, int y){ (void)i;(void)c;(void)x;(void)y; }
static void   im_free(kf_img i){ (void)i; }
static kf_err im_encode_file(const char *p, const kf_color *px, int w, int h){ (void)p;(void)px;(void)w;(void)h; return KF_EUNSUPP; }

/* ===================== the vtable ===================== */
static const struct k_sys  K_SYS  = { y_exit, y_on_frame, y_on_key, y_on_close, y_pump, y_perf, y_clock_hz,
    y_idle_policy, y_battery_pct, y_charging, y_get_brightness, y_get_volume, y_caps, y_rng, y_kernel_version,
    y_last_error, y_err_str, y_log };
static const struct k_mem  K_MEM  = { m_alloc, m_realloc, m_free, m_avail, m_psram_alloc, m_psram_free,
    m_psram_read, m_psram_write, m_lock, m_unlock };
static const struct k_gfx  K_GFX  = { g_screen_size, g_canvas, g_canvas_destroy, g_present, g_clear, g_pixel,
    g_line, g_rect, g_fill, g_blit, g_clip, g_view_open, g_view_region, g_view_push, g_view_flush, g_view_close,
    g_lease, g_release, g_region, g_push, g_flush, g_set_panel_hz };
static const struct k_txt  K_TXT  = { g_font_open, g_font_close, g_text_w, g_line_h, g_draw_text, g_glyph };
static const struct k_img  K_IMG  = { im_decode, im_decode_file, im_info, im_to_canvas, im_free, im_encode_file };
static const struct k_in   K_IN   = { g_in_poll, g_in_mods };
static const struct k_aud  K_AUD  = { a_out_start, a_out_space, a_out_write, a_out_running, a_out_stop, a_tone };
static const struct k_fs   K_FS   = { f_open, f_read, f_write, f_seek, f_tell, f_close, f_opendir, f_readdir,
    f_closedir, f_remove, f_rename, f_mkdir, f_app_dir, f_stage, f_unstage };
static const struct k_net  K_NET  = { n_connect, n_send, n_recv, n_status, n_close, n_resolve, n_online, n_rssi, n_ip };
static const struct k_tls  K_TLS  = { tl_wrap, tl_handshake, tl_send, tl_recv, tl_close };
static const struct k_time K_TIME = { t_millis, t_micros, t_sleep_ms, t_now_unix };

static const kapi G_KAPI = {
    KAPI_ABI, KAPI_MINOR,
    &K_SYS, &K_MEM, &K_GFX, &K_TXT, &K_IMG, &K_IN, &K_AUD, &K_FS, &K_NET, &K_TLS, &K_TIME,
    NULL, NULL, NULL   /* Layer 1 (ui/http/doc) not provided in v1 */
};

/* ===================== teardown / poll ===================== */
static void kapi_teardown(void){
    if(s_on_close) s_on_close(s_on_close_ud);
    for(int i = 0; i < s_ncanv; i++) if(s_canv[i]){ if(s_canv[i]->obj) lv_obj_delete(s_canv[i]->obj);
        if(s_canv[i]->from_static) s_cbuf_used = 0; else free(s_canv[i]->buf); free(s_canv[i]); s_canv[i] = NULL; }
    s_ncanv = 0;
    s_on_frame = NULL; s_on_key = NULL; s_on_close = NULL;
    s_active = 0; s_exit = 0;
    kf_grab_input(0);
    kf_clock_normal();
    s_scr = NULL;
    kf_back_to_launcher();   /* loads the desktop, async-deletes our (now empty) screen */
}

void kapi_poll(void){
    if(!s_active) return;
    if(s_exit){ kapi_teardown(); return; }
    uint8_t st, key;
    while(uart_pop_key(&st, &key)){
        if(s_on_key) s_on_key(s_on_key_ud, map_dk(key), st != KS_RELEASE);
        if(!s_active || s_exit) return;          /* app may have exited mid-drain */
    }
    if(s_on_frame) s_on_frame(s_on_frame_ud);
}

/* ===================== loader ===================== */
static int kapi_run(const char *path){
    if(s_active) return -1;
    FILE *f = fopen(path, "rb");
    if(!f){ printf("kapi: cannot open %s\n", path); return -2; }
    kx_header h;
    if(fread(&h, 1, sizeof h, f) != sizeof h){ fclose(f); return -3; }
    if(memcmp(h.magic, KX_MAGIC, 4) != 0 || h.abi_version != KAPI_ABI){ fclose(f); printf("kapi: bad magic/abi\n"); return -4; }
    if(h.load_base != (uint32_t)(uintptr_t)g_kapi_arena){
        fclose(f); printf("kapi: load_base %08lx != arena %08lx (rebuild app)\n",
                          (unsigned long)h.load_base, (unsigned long)(uintptr_t)g_kapi_arena); return -5; }
    if((size_t)h.image_size + h.bss_size > sizeof g_kapi_arena){ fclose(f); printf("kapi: too big\n"); return -6; }
    if(fread(g_kapi_arena, 1, h.image_size, f) != h.image_size){ fclose(f); return -7; }
    fclose(f);
    memset(g_kapi_arena + h.image_size, 0, h.bss_size);
    __dsb(); __isb();

    /* derive the app's data dir from its path: ".../demo/demo.kx" -> ".../demo" */
    snprintf(s_app_dir, sizeof s_app_dir, "%s", path);
    char *slash = strrchr(s_app_dir, '/'); if(slash) *slash = 0;

    /* fresh full-screen black canvas-host; the topbar (layer_top) stays above it */
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_scr, 0, 0);
    lv_obj_clear_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_screen_load(s_scr);

    s_on_frame = NULL; s_on_key = NULL; s_on_close = NULL; s_ncanv = 0;
    s_exit = 0; s_active = 1;
    kf_grab_input(1);

    app_main_fn entry = (app_main_fn)(((uintptr_t)g_kapi_arena + h.entry_offset) | 1u);  /* Thumb bit */
    entry(&G_KAPI);
    return 0;
}

/* desktop tile entry — launch the bundled demo */
void app_demo_open(void){
    if(kapi_run("/apps/demo/demo.kx") != 0){
        /* loader refused: stay on the launcher (screen untouched on pre-entry failure) */
    }
}
