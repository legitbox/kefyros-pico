// port/kapi.c — Kefyros App Programming Interface (KAPI) runtime: loader + vtable glue.
//
// Loads a class-1 app (.kx) from SD into a fixed SRAM arena and runs it. The app reaches
// the OS ONLY through the `kapi` function-pointer table built here — it links no kernel
// symbols. See KAPI.md / sdk/kapi.h.
//
// ARENA: the top 48 KiB of RP2350 SRAM, reserved by CMake's generated linker map at the
// stable address 0x20074000. Apps can therefore be built once for a KAPI ABI and copied to
// SD; unrelated kernel rebuilds no longer move their load address. Code executes from SRAM
// (RP2350 SRAM is executable); __dsb/__isb make the freshly-copied code visible.
// (96/64 KiB were tried; both starved the built-ins' heap. See CMakeLists.txt.)
#include "kefyros.h"
#include "kapi.h"
#include "theme.h"           /* KF_FONT / KF_FONT_BIG */
#include "deskconf.h"
#include "lcdspi/lcdspi.h"
#include "disp.h"
#include "imgdec.h"
#include "http.h"
#include "tls.h"
#include "ssh.h"
#include "ssh_tcp.h"
#include "hardware/spi.h"
#include "hardware/sync.h"
#include "pico/rand.h"
#include "pico/time.h"
#include "lwip/tcp.h"
#include "lwip/udp.h"
#include "lwip/dns.h"
#include "lwip/ip4_addr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

/* ===================== arena + app state ===================== */
#define KAPI_ARENA_BYTES (48 * 1024)
extern uint8_t g_kapi_arena[];       /* absolute symbols from memmap_kapi.ld */
extern uint8_t g_kapi_arena_end[];

typedef int (*app_main_fn)(const kapi *);

static int   s_active = 0, s_exit = 0;
static int   s_last_error = KF_OK;
static enum kf_perf s_perf_req = KF_PERF_NORMAL;
static int s_net_eco_hold,s_ssh_eco_hold;
static struct kf_ssh_s *s_kssh;
static lv_obj_t *s_scr = NULL;
static char  s_app_dir[256] = "/apps";

static void (*s_on_frame)(void*);                 static void *s_on_frame_ud;
static void (*s_on_key)(void*, int, int);         static void *s_on_key_ud;
static void (*s_on_close)(void*);                 static void *s_on_close_ud;
static void tone_pump(void);
static void net_clock_tick(void);
static int sh_poll(kf_ssh h);

enum { KUI_SCREEN, KUI_LABEL, KUI_BUTTON, KUI_LIST, KUI_TEXTAREA, KUI_CHECKBOX, KUI_SLIDER };
struct kui_obj_s {
    lv_obj_t *obj;
    uint8_t kind;
    void (*click)(void *);
    void *click_ud;
    void (*change)(void *);
    void *change_ud;
};
#define MAX_KUI 96
static struct kui_obj_s *s_kui[MAX_KUI];
static int s_nkui;
static lv_group_t *s_ui_group;

struct khttp_req_s { kf_mem mem; uint32_t base, cap, read_off; int active; };
static struct khttp_req_s *s_hreq;

#define KAPI_PMEM_MAX 48
static struct { uint32_t base,size; uint8_t used; } s_pmem[KAPI_PMEM_MAX];
static uint32_t s_pmem_mark;
static kf_mem s_locked_mem;static void *s_locked_ptr;static size_t s_locked_size;

static uint32_t kx_crc32(const uint8_t *p, size_t n){
    uint32_t c=0xffffffffu;
    while(n--){ c^=*p++; for(int b=0;b<8;b++) c=(c>>1)^((0u-(c&1u))&0xedb88320u); }
    return c^0xffffffffu;
}

static void u_deleted(lv_event_t *e);
static struct kui_obj_s *u_wrap(lv_obj_t *obj, int kind){
    if(!obj || s_nkui>=MAX_KUI) return NULL;
    struct kui_obj_s *w=malloc(sizeof *w); if(!w) return NULL;
    *w=(struct kui_obj_s){obj,(uint8_t)kind,NULL,NULL,NULL,NULL};
    s_kui[s_nkui++]=w;lv_obj_add_event_cb(obj,u_deleted,LV_EVENT_DELETE,w);
    return w;
}
static lv_obj_t *u_parent(kui_obj p){ return p ? ((struct kui_obj_s*)p)->obj : s_scr; }
static void u_click_event(lv_event_t *e){
    struct kui_obj_s *w=lv_event_get_user_data(e);
    if(w && w->click) w->click(w->click_ud);
}
static void u_change_event(lv_event_t *e){
    struct kui_obj_s *w=lv_event_get_user_data(e);
    if(w && w->change) w->change(w->change_ud);
}
static void u_deleted(lv_event_t *e){struct kui_obj_s*w=lv_event_get_user_data(e);if(w)w->obj=NULL;}

/* ===================== canvas implementation ===================== */
struct kf_canvas_s {
    lv_obj_t *obj;
    uint16_t *buf;
    int w, h, from_arena;
    int cx0, cy0, cx1, cy1;
};
#define MAXCANV 4
static struct kf_canvas_s *s_canv[MAXCANV];
static int s_ncanv;
static void canvas_deleted(lv_event_t *e){struct kf_canvas_s*c=lv_event_get_user_data(e);if(c)c->obj=NULL;}
/* Canvas buffers are carved from the ARENA's free space (after the loaded app image):
   guaranteed-contiguous, fragmentation-proof, and costing the desktop heap NOTHING (the
   arena is reserved either way — so the launcher's icon decodes keep their heap). Overflow
   falls back to malloc. s_arena_top is the bump cursor, set by the loader after image+bss. */
static uint8_t *s_arena_top;

static inline lv_color_t c565(kf_color c){
    uint8_t r = (c >> 11) & 0x1f, g = (c >> 5) & 0x3f, b = c & 0x1f;
    return lv_color_make((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2));
}

static kf_canvas g_canvas(int x, int y, int w, int h){
    if(s_ncanv >= MAXCANV || !s_scr || w<=0 || h<=0 || w>LCD_W || h>LCD_H) return NULL;
    struct kf_canvas_s *c = malloc(sizeof *c);
    if(!c) return NULL;
    size_t need = (size_t)w * h * 2;
    uint8_t *aend = g_kapi_arena_end;
    if(s_arena_top && s_arena_top + need <= aend){    /* carve from the arena (preferred) */
        c->buf = (uint16_t*)s_arena_top; s_arena_top += (need + 3) & ~(size_t)3; c->from_arena = 1;
    } else {                                          /* arena full -> heap */
        c->buf = malloc(need); if(!c->buf){ free(c); return NULL; } c->from_arena = 0;
    }
    c->w = w; c->h = h; c->cx0 = 0; c->cy0 = 0; c->cx1 = w - 1; c->cy1 = h - 1;
    c->obj = lv_canvas_create(s_scr);if(!c->obj){if(!c->from_arena)free(c->buf);free(c);return NULL;}
    lv_obj_add_event_cb(c->obj,canvas_deleted,LV_EVENT_DELETE,c);
    lv_canvas_set_buffer(c->obj, c->buf, w, h, LV_COLOR_FORMAT_RGB565);   /* lv_image auto-sizes */
    lv_obj_set_pos(c->obj, x, y);
    s_canv[s_ncanv++] = c;
    return (kf_canvas)c;
}
static void g_canvas_destroy(kf_canvas h){
    struct kf_canvas_s *c = (struct kf_canvas_s*)h; if(!c) return;
    for(int i = 0; i < s_ncanv; i++) if(s_canv[i] == c){ s_canv[i] = s_canv[--s_ncanv]; break; }
    if(c->obj) lv_obj_delete(c->obj);
    if(!c->from_arena) free(c->buf);
    free(c);
}
static void g_present(kf_canvas h){ struct kf_canvas_s *c = (void*)h; if(c&&c->obj) lv_obj_invalidate(c->obj); }

static void g_clear(kf_canvas h, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    for(int y=c->cy0;y<=c->cy1;y++){ uint16_t *row=c->buf+y*c->w;
        for(int x=c->cx0;x<=c->cx1;x++) row[x]=col; }
}
static void g_pixel(kf_canvas h, int x, int y, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    if(x>=c->cx0 && x<=c->cx1 && y>=c->cy0 && y<=c->cy1) c->buf[y * c->w + x] = col;
}
static void g_fill(kf_canvas h, int x, int y, int w, int hh, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c) return;
    if(w<=0||hh<=0) return;
    int x0 = x < c->cx0 ? c->cx0 : x, y0 = y < c->cy0 ? c->cy0 : y;
    int x1 = x + w > c->cx1+1 ? c->cx1+1 : x + w, y1 = y + hh > c->cy1+1 ? c->cy1+1 : y + hh;
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
    struct kf_canvas_s *c = (void*)h; if(!c||!px||w<=0||hh<=0) return;
    for(int r = 0; r < hh; r++){ int yy = y + r; if(yy<c->cy0||yy>c->cy1) continue;
        for(int cc = 0; cc < w; cc++){ int xx = x + cc; if(xx>=c->cx0&&xx<=c->cx1) c->buf[yy*c->w + xx] = px[r*w + cc]; } }
}
static void g_clip(kf_canvas h, int x, int y, int w, int hh){
    struct kf_canvas_s *c=(void*)h;if(!c)return;
    if(w<=0||hh<=0){c->cx0=1;c->cy0=1;c->cx1=0;c->cy1=0;return;}
    c->cx0=x<0?0:x;c->cy0=y<0?0:y;
    c->cx1=x+w-1>=c->w?c->w-1:x+w-1;c->cy1=y+hh-1>=c->h?c->h-1:y+hh-1;
    if(c->cx0>c->cx1||c->cy0>c->cy1){c->cx0=1;c->cy0=1;c->cx1=0;c->cy1=0;}
}
static void g_screen_size(int *w, int *h){ if(w) *w = LCD_W; if(h) *h = LCD_H; }

/* exclusive (raw panel) — wrappers over the same path the GB emulator uses. push() takes
   already-formatted RGB565 bytes (panel-native), like spi_write_fast in gameboy.c. */
static int s_panel_leased;
static kf_err g_lease(void){
    if(s_panel_leased)return KF_EAGAIN;
    disp_pause_core1();spi_set_baudrate(Pico_LCD_SPI_MOD,LCD_SPI_SPEED);s_panel_leased=1;return KF_OK;
}
static void   g_region(int x0,int y0,int x1,int y1){if(s_panel_leased)define_region_spi(x0,y0,x1,y1,1);}
static void   g_push(const void *px, size_t n){if(s_panel_leased&&px&&n)spi_write_fast(Pico_LCD_SPI_MOD,(const uint8_t*)px,n);}
static void   g_flush(void){if(s_panel_leased){spi_finish(Pico_LCD_SPI_MOD);lcd_spi_raise_cs();}}
static void g_release(void){
    if(!s_panel_leased)return;g_flush();s_panel_leased=0;disp_resume_core1();
    if(s_scr)lv_obj_invalidate(s_scr);lv_obj_invalidate(lv_layer_top());
}
static kf_err g_set_panel_hz(uint32_t hz){
    if(!s_panel_leased)return KF_EINVAL;if(hz<1000000u||hz>LCD_SPI_SPEED)return KF_EINVAL;
    spi_set_baudrate(Pico_LCD_SPI_MOD,hz);return KF_OK;
}

/* windowed turbo (view): direct panel access clipped below the persistent topbar */
struct kf_view_s { int x,y,w,h,open; };
static struct kf_view_s s_view;
static kf_view g_view_open(int x,int y,int w,int h){
    if(s_view.open||w<=0||h<=0)return NULL;
    if(x<0){w+=x;x=0;}if(y<0){h+=y;y=0;}
    if(x+w>LCD_W)w=LCD_W-x;if(y+h>KF_CONTENT_H)h=KF_CONTENT_H-y;
    if(w<=0||h<=0||g_lease()!=KF_OK)return NULL;
    s_view=(struct kf_view_s){x,y+KF_TOPBAR_H,w,h,1};return &s_view;
}
static void g_view_region(kf_view h,int x0,int y0,int x1,int y1){
    struct kf_view_s*v=(void*)h;if(!v||!v->open)return;
    if(x0<0)x0=0;if(y0<0)y0=0;if(x1>=v->w)x1=v->w-1;if(y1>=v->h)y1=v->h-1;
    if(x0<=x1&&y0<=y1)g_region(v->x+x0,v->y+y0,v->x+x1,v->y+y1);
}
static void g_view_push(kf_view v,const void*p,size_t n){struct kf_view_s*w=(void*)v;if(w&&w->open&&p&&n)g_push(p,n);}
static void g_view_flush(kf_view v){struct kf_view_s*w=(void*)v;if(w&&w->open)g_flush();}
static void g_view_close(kf_view v){struct kf_view_s*w=(void*)v;if(!w||w!=&s_view||!w->open)return;g_flush();w->open=0;g_release();}

/* ===================== fonts / text ===================== */
static kf_font g_font_open(const char *name, int px){ (void)name; return (kf_font)(px >= 18 ? KF_FONT_BIG : KF_FONT); }
static void    g_font_close(kf_font f){ (void)f; }
static int     g_line_h(kf_font f){ return f ? ((const lv_font_t*)f)->line_height : 13; }
static int     g_text_w(kf_font f, const char *s){
    if(!f)f=(kf_font)KF_FONT;if(!s)return 0;
    lv_point_t sz; lv_text_get_size(&sz, s, (const lv_font_t*)f, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return sz.x;
}
static void    g_draw_text(kf_canvas h, kf_font f, int x, int y, const char *s, kf_color col){
    struct kf_canvas_s *c = (void*)h; if(!c || !c->obj || !s) return;
    if(!f)f=(kf_font)KF_FONT;
    lv_layer_t layer; lv_canvas_init_layer(c->obj, &layer);
    layer._clip_area=(lv_area_t){c->cx0,c->cy0,c->cx1,c->cy1};
    lv_draw_label_dsc_t d; lv_draw_label_dsc_init(&d);
    d.text = s; d.font = (const lv_font_t*)f; d.color = c565(col);
    lv_area_t a = { x, y, c->w - 1, y + ((const lv_font_t*)f)->line_height };
    lv_draw_label(&layer, &d, &a);
    lv_canvas_finish_layer(c->obj, &layer);
}
static uint8_t s_glyph_alpha[64*64];
static int g_glyph(kf_font f, uint32_t cp, int *w, int *h, const uint8_t **bm){
    if(w)*w=0;if(h)*h=0;if(bm)*bm=NULL;if(!f)return 0;
    lv_font_glyph_dsc_t d;
    if(!lv_font_get_glyph_dsc((const lv_font_t*)f,&d,cp,0)||d.box_w>64||d.box_h>64)return 0;
    const uint8_t *src=lv_font_get_glyph_bitmap(&d,NULL);if(!src)return 0;
    int bpp=(d.format==LV_FONT_GLYPH_FORMAT_A1)?1:(d.format==LV_FONT_GLYPH_FORMAT_A2)?2:
            (d.format==LV_FONT_GLYPH_FORMAT_A4)?4:(d.format==LV_FONT_GLYPH_FORMAT_A8)?8:0;
    if(!bpp){lv_font_glyph_release_draw_data(&d);return 0;}
    int np=d.box_w*d.box_h;
    for(int i=0;i<np;i++){int bit=i*bpp,shift=8-bpp-(bit&7);uint8_t v=(src[bit>>3]>>shift)&((1u<<bpp)-1u);
        s_glyph_alpha[i]=(uint8_t)(v*255u/((1u<<bpp)-1u));}
    lv_font_glyph_release_draw_data(&d);
    if(w)*w=d.box_w;if(h)*h=d.box_h;if(bm)*bm=s_glyph_alpha;return d.adv_w;
}

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
static uint32_t map_lv_key(uint8_t k){
    switch(k){
    case DK_ENTER:return LV_KEY_ENTER; case DK_BACKSPACE:return LV_KEY_BACKSPACE; case DK_TAB:return LV_KEY_NEXT;
    case DK_DEL:return LV_KEY_DEL; case DK_UP:return LV_KEY_UP; case DK_DOWN:return LV_KEY_DOWN;
    case DK_LEFT:return LV_KEY_LEFT; case DK_RIGHT:return LV_KEY_RIGHT; case DK_HOME:return LV_KEY_HOME;
    case DK_END:return LV_KEY_END; default:return (k>=0x20&&k<0x7f)?k:0;
    }
}
static int g_in_poll(int *key, int *down, int *mods){
    uart_poll();                       /* exclusive apps run inside app_main(), outside the kernel superloop */
    uint8_t st, k; if(!uart_pop_key(&st, &k)) return 0;
    if(key) *key = map_dk(k); if(down) *down = (st != KS_RELEASE); if(mods) *mods = map_mods(uart_mods()); return 1;
}
static int g_in_mods(void){ return map_mods(uart_mods()); }

/* ===================== sys ===================== */
static void     y_exit(int code){ (void)code; s_exit = 1; }
static void     y_on_frame(void (*fn)(void*), void *ud){ s_on_frame = fn; s_on_frame_ud = ud; }
static void     y_on_key(void (*fn)(void*,int,int), void *ud){ s_on_key = fn; s_on_key_ud = ud; }
static void     y_on_close(void (*fn)(void*), void *ud){ s_on_close = fn; s_on_close_ud = ud; }
static void     y_pump(void){ uart_poll(); kf_net_poll(); net_clock_tick(); tone_pump(); }
static void y_perf(enum kf_perf p){
    s_perf_req=p;if(s_net_eco_hold||s_ssh_eco_hold)return;
    if(p==KF_PERF_ECO)kf_clock_eco();else if(p==KF_PERF_BOOST)kf_clock_boost();else kf_clock_normal();
}
static uint32_t y_clock_hz(void){ return kf_clock_khz() * 1000u; }
static void     y_idle_policy(enum kf_idle p){ kf_app_idle_policy((int)p); }
static int      y_battery_pct(void){ uint8_t b = 0; if(reg_read(REG_BAT, &b, 1) > 0) return b & 0x7f; return -1; }
static int      y_charging(void){ uint8_t b = 0; if(reg_read(REG_BAT, &b, 1) > 0) return (b >> 7) & 1; return 0; }
static int      y_get_brightness(void){ uint8_t v=0; return reg_read(REG_BKL,&v,1)>0 ? v : -1; }
static int      y_get_volume(void){ return -1; } /* no global mixer/volume control exists on this hardware */
static uint32_t y_caps(void){ uint32_t c = KF_CAP_AUDIO_OUT | KF_CAP_RNG | KF_CAP_WIFI | KF_CAP_TLS | KF_CAP_IMG;
    if(kf_psram_size()) c |= KF_CAP_PSRAM; return c; }
static uint32_t y_rng(void){ return get_rand_32(); }
static const char *y_kernel_version(void){ return KF_VERSION; }
static int       y_last_error(void){ return s_last_error; }
static const char *y_err_str(int e){ switch(e){case KF_OK:return "ok";case KF_ERR:return "error";case KF_ENOMEM:return "out of memory";case KF_ENOENT:return "not found";case KF_EIO:return "I/O error";case KF_EAGAIN:return "try again";case KF_EINVAL:return "invalid argument";case KF_EUNSUPP:return "unsupported";default:return "unknown error";} }
static void      y_log(const char *s){ if(s) printf("[kapi-app] %s\n", s); }

/* ===================== time ===================== */
static uint32_t t_millis(void){ return (uint32_t)(time_us_64() / 1000u); }
static uint64_t t_micros(void){ return time_us_64(); }
static void t_sleep_ms(uint32_t ms){
    uint32_t end=t_millis()+ms;
    while((int32_t)(end-t_millis())>0){uint32_t left=end-t_millis();sleep_ms(left>2?2:left);y_pump();}
}
static uint32_t t_now_unix(void){ return kf_time_unix(); }

/* ===================== mem ===================== */
static void  *m_alloc(size_t n){ void*p=malloc(n);s_last_error=p||!n?KF_OK:KF_ENOMEM;return p; }
static void  *m_realloc(void *p, size_t n){ void*q=realloc(p,n);s_last_error=q||!n?KF_OK:KF_ENOMEM;return q; }
static void   m_free(void *p){ free(p); }
static size_t m_avail(void){ struct mallinfo mi=mallinfo(); return mi.fordblks>0?(size_t)mi.fordblks:0; }
static int pm_index(kf_mem h){ return h>0 && h<=KAPI_PMEM_MAX && s_pmem[h-1].used ? (int)h-1 : -1; }
static uint32_t pm_base(kf_mem h){ int i=pm_index(h); return i<0?0xffffffffu:s_pmem[i].base; }
static void m_unlock(kf_mem h);
static kf_mem m_psram_alloc(size_t n){
    if(!n || n>0xffffffc0u) return 0; uint32_t need=((uint32_t)n+31u)&~31u;
    int slot=-1;
    for(int i=0;i<KAPI_PMEM_MAX;i++) if(!s_pmem[i].used){ if(slot<0) slot=i; if(s_pmem[i].size>=need){ slot=i; break; } }
    if(slot<0) return 0;
    if(s_pmem[slot].size<need){ uint32_t o=kf_psram_alloc(need); if(o==0xffffffffu) return 0; s_pmem[slot].base=o; s_pmem[slot].size=need; }
    s_pmem[slot].used=1; return (kf_mem)(slot+1);
}
static void m_psram_free(kf_mem h){ int i=pm_index(h);if(i>=0){if(h==s_locked_mem)m_unlock(h);s_pmem[i].used=0;} }
static void m_psram_read(kf_mem h,size_t off,void *d,size_t n){ int i=pm_index(h); if(i<0||!d||off>s_pmem[i].size||n>s_pmem[i].size-off) return; kf_psram_read(s_pmem[i].base+(uint32_t)off,d,(uint32_t)n); }
static void m_psram_write(kf_mem h,size_t off,const void *s,size_t n){ int i=pm_index(h); if(i<0||!s||off>s_pmem[i].size||n>s_pmem[i].size-off) return; kf_psram_write(s_pmem[i].base+(uint32_t)off,s,(uint32_t)n); }
static void *m_lock(kf_mem h){
    int i=pm_index(h);if(i<0||s_locked_ptr)return NULL;
    void*p=malloc(s_pmem[i].size);if(!p){s_last_error=KF_ENOMEM;return NULL;}
    kf_psram_read(s_pmem[i].base,p,s_pmem[i].size);s_locked_mem=h;s_locked_ptr=p;s_locked_size=s_pmem[i].size;return p;
}
static void m_unlock(kf_mem h){
    if(!s_locked_ptr||h!=s_locked_mem)return;int i=pm_index(h);
    if(i>=0)kf_psram_write(s_pmem[i].base,s_locked_ptr,(uint32_t)s_locked_size);
    free(s_locked_ptr);s_locked_ptr=NULL;s_locked_mem=0;s_locked_size=0;
}

/* ===================== fs (POSIX) ===================== */
static kf_file f_open(const char *p, const char *m){ FILE*f=fopen(p,m);s_last_error=f?KF_OK:KF_ENOENT;return(kf_file)f; }
static int  f_read(kf_file f, void *b, int n){ return f ? (int)fread(b, 1, n, (FILE*)f) : -1; }
static int  f_write(kf_file f, const void *b, int n){ return f ? (int)fwrite(b, 1, n, (FILE*)f) : -1; }
static int  f_seek(kf_file f, long o, int w){ return f ? fseek((FILE*)f, o, w) : -1; }
static long f_tell(kf_file f){ return f ? ftell((FILE*)f) : -1; }
static void f_close(kf_file f){ if(f) fclose((FILE*)f); }
struct kf_dir_s { DIR *dir; char path[256]; };
static kf_dir f_opendir(const char *p){
    if(!p)return NULL;DIR*d=opendir(p);if(!d)return NULL;
    struct kf_dir_s*h=malloc(sizeof*h);if(!h){closedir(d);return NULL;}
    h->dir=d;snprintf(h->path,sizeof h->path,"%s",p);return h;
}
static int  f_readdir(kf_dir d, char *name, int n, int *is_dir){
    struct kf_dir_s*h=(void*)d;if(!h||!h->dir||!name||n<=0)return 0;
    struct dirent *e = readdir(h->dir); if(!e) return 0;
    snprintf(name, n, "%s", e->d_name);
    if(is_dir){ char p[520]; struct stat stt; snprintf(p, sizeof p, "%s/%s", h->path, e->d_name);
        *is_dir = (stat(p, &stt) == 0 && S_ISDIR(stt.st_mode)); }
    return 1;
}
static void f_closedir(kf_dir d){struct kf_dir_s*h=(void*)d;if(h){if(h->dir)closedir(h->dir);free(h);}}
static kf_err f_remove(const char *p){ return remove(p) == 0 ? KF_OK : KF_EIO; }
static kf_err f_rename(const char *a, const char *b){ return rename(a, b) == 0 ? KF_OK : KF_EIO; }
static kf_err f_mkdir(const char *p){ return mkdir(p, 0777) == 0 ? KF_OK : KF_EIO; }
static const char *f_app_dir(void){ return s_app_dir; }
static kf_err f_stage(const char *p, const void **xip, size_t *sz){ (void)p;(void)xip;(void)sz; return KF_EUNSUPP; }
static void   f_unstage(void){}

/* ===================== aud (stereo ring) ===================== */
static int s_tone_frames;static uint32_t s_tone_phase,s_tone_step,s_tone_stop_at;
static int16_t s_tone_buf[128*2];
static kf_err a_out_start(int rate){s_tone_frames=0;s_tone_stop_at=0;return kf_audio_start_buffered(rate,8192)?KF_OK:KF_ENOMEM;}
static int    a_out_space(void){ return kf_audio_space(); }
static int    a_out_write(const int16_t *s, int nf){ return kf_audio_write(s, nf); }
static int    a_out_running(void){ return kf_audio_running(); }
static void a_out_stop(void){s_tone_frames=0;kf_audio_stop();}
static void tone_pump(void){
    if(!s_tone_frames){if(s_tone_stop_at&&(int32_t)(t_millis()-s_tone_stop_at)>=0){s_tone_stop_at=0;kf_audio_stop();}return;}
    int n=kf_audio_space();if(n>128)n=128;if(n>s_tone_frames)n=s_tone_frames;if(n<=0)return;
    for(int i=0;i<n;i++){int16_t v=(s_tone_phase&0x80000000u)?6500:-6500;s_tone_phase+=s_tone_step;s_tone_buf[i*2]=v;s_tone_buf[i*2+1]=v;}
    int z=kf_audio_write(s_tone_buf,n);if(z>0)s_tone_frames-=z;
}
static void a_tone(int hz, int ms){
    if(hz<20||hz>8000||ms<=0){a_out_stop();return;}
    if(ms>60000)ms=60000;if(!kf_audio_start_buffered(16000,1024)){s_last_error=KF_ENOMEM;return;}
    s_tone_phase=0;s_tone_step=(uint32_t)(((uint64_t)(uint32_t)hz<<32)/16000u);
    s_tone_frames=ms*16;s_tone_stop_at=t_millis()+(uint32_t)ms+40u;tone_pump();
}

/* ===================== generic nonblocking sockets ===================== */
#define KSOCK_MAX 4
#define KSOCK_RX 4096
struct kf_sock_s { uint8_t used,udp,state; int err; uint16_t port; struct tcp_pcb *tcp; struct udp_pcb *up; uint8_t *rx; uint16_t rr,rw,count; };
static struct kf_sock_s s_sock[KSOCK_MAX];
static void sock_start_ip(struct kf_sock_s *s,const ip_addr_t *ip);
static int n_online(void);
static err_t sock_tcp_recv(void *arg,struct tcp_pcb *pcb,struct pbuf *p,err_t err){
    struct kf_sock_s *s=arg;if(!s||!s->used){if(p)pbuf_free(p);return ERR_OK;}
    if(!p){s->state=2;s->tcp=NULL;return ERR_OK;} if(err!=ERR_OK){pbuf_free(p);s->err=KF_EIO;s->state=3;return ERR_OK;}
    if(p->tot_len>KSOCK_RX-s->count)return ERR_MEM;
    for(struct pbuf*q=p;q;q=q->next){const uint8_t*d=q->payload;for(uint16_t i=0;i<q->len;i++){s->rx[s->rw++]=d[i];if(s->rw==KSOCK_RX)s->rw=0;}}
    s->count+=(uint16_t)p->tot_len;tcp_recved(pcb,p->tot_len);pbuf_free(p);return ERR_OK;
}
static err_t sock_tcp_connected(void *arg,struct tcp_pcb *pcb,err_t err){struct kf_sock_s*s=arg;if(!s||!s->used)return ERR_ABRT;if(err!=ERR_OK){s->err=KF_EIO;s->state=3;return err;}s->tcp=pcb;s->state=1;return ERR_OK;}
static void sock_tcp_err(void *arg,err_t err){struct kf_sock_s*s=arg;if(s&&s->used){s->tcp=NULL;s->err=err==ERR_MEM?KF_ENOMEM:KF_EIO;s->state=3;}}
static void sock_udp_recv(void *arg,struct udp_pcb *pcb,struct pbuf *p,const ip_addr_t *addr,u16_t port){
    (void)pcb;(void)addr;(void)port;struct kf_sock_s*s=arg;if(!s||!s->used||!p){if(p)pbuf_free(p);return;}
    uint16_t take=p->tot_len;if(take>KSOCK_RX-s->count)take=(uint16_t)(KSOCK_RX-s->count);
    uint8_t tmp[256];uint16_t off=0;while(off<take){uint16_t n=take-off;if(n>sizeof tmp)n=sizeof tmp;pbuf_copy_partial(p,tmp,n,off);for(uint16_t i=0;i<n;i++){s->rx[s->rw++]=tmp[i];if(s->rw==KSOCK_RX)s->rw=0;}off+=n;}s->count+=take;pbuf_free(p);
}
static void sock_dns_cb(const char *name,const ip_addr_t *ip,void *arg){(void)name;struct kf_sock_s*s=arg;if(!s||!s->used)return;if(!ip){s->err=KF_ENOENT;s->state=3;return;}sock_start_ip(s,ip);}
static void sock_start_ip(struct kf_sock_s *s,const ip_addr_t *ip){
    if(s->udp){s->up=udp_new_ip_type(IPADDR_TYPE_V4);if(!s->up||udp_connect(s->up,ip,s->port)!=ERR_OK){if(s->up)udp_remove(s->up);s->up=NULL;s->state=3;s->err=KF_EIO;return;}udp_recv(s->up,sock_udp_recv,s);s->state=1;}
    else{s->tcp=tcp_new_ip_type(IPADDR_TYPE_V4);if(!s->tcp){s->state=3;s->err=KF_ENOMEM;return;}tcp_arg(s->tcp,s);tcp_recv(s->tcp,sock_tcp_recv);tcp_err(s->tcp,sock_tcp_err);err_t e=tcp_connect(s->tcp,ip,s->port,sock_tcp_connected);if(e!=ERR_OK){tcp_abort(s->tcp);s->tcp=NULL;s->state=3;s->err=KF_EIO;}}
}
static kf_sock n_connect(const char *host,int port,int udp){
    if(!host||!*host||port<1||port>65535||!n_online())return NULL;int idx=-1;for(int i=0;i<KSOCK_MAX;i++)if(!s_sock[i].used){idx=i;break;}if(idx<0)return NULL;
    struct kf_sock_s*s=&s_sock[idx];memset(s,0,sizeof*s);s->rx=malloc(KSOCK_RX);if(!s->rx)return NULL;s->used=1;s->udp=udp!=0;s->port=(uint16_t)port;s->state=0;
    ip_addr_t ip;err_t e=dns_gethostbyname(host,&ip,sock_dns_cb,s);if(e==ERR_OK)sock_start_ip(s,&ip);else if(e!=ERR_INPROGRESS){free(s->rx);memset(s,0,sizeof*s);return NULL;}return s;
}
static int n_send(kf_sock h,const void *buf,int n){struct kf_sock_s*s=(void*)h;if(!s||!s->used||n<0||(n&&!buf))return KF_EINVAL;if(s->state==0)return KF_AGAIN;if(s->state!=1)return s->err?s->err:KF_EIO;if(!n)return 0;
    if(s->udp){if(n>65507)n=65507;struct pbuf*p=pbuf_alloc(PBUF_TRANSPORT,(u16_t)n,PBUF_RAM);if(!p)return KF_AGAIN;pbuf_take(p,buf,(u16_t)n);err_t e=udp_send(s->up,p);pbuf_free(p);return e==ERR_OK?n:e==ERR_MEM?KF_AGAIN:KF_EIO;}
    if(n>65535)n=65535;
    if((u16_t)n>tcp_sndbuf(s->tcp))n=tcp_sndbuf(s->tcp);if(!n)return KF_AGAIN;err_t e=tcp_write(s->tcp,buf,(u16_t)n,TCP_WRITE_FLAG_COPY);if(e==ERR_OK){tcp_output(s->tcp);return n;}return e==ERR_MEM?KF_AGAIN:KF_EIO;
}
static int n_recv(kf_sock h,void *buf,int n){struct kf_sock_s*s=(void*)h;if(!s||!s->used||n<0||(n&&!buf))return KF_EINVAL;if(s->count){if(n>s->count)n=s->count;uint8_t*d=buf;for(int i=0;i<n;i++){d[i]=s->rx[s->rr++];if(s->rr==KSOCK_RX)s->rr=0;}s->count-=(uint16_t)n;return n;}if(s->state==2)return 0;if(s->state==3)return s->err?s->err:KF_EIO;return KF_AGAIN;}
static int n_status(kf_sock h){struct kf_sock_s*s=(void*)h;if(!s||!s->used)return KF_EINVAL;if(s->state==1)return KF_OK;if(s->state==0)return KF_AGAIN;return s->err?s->err:KF_EIO;}
static void n_close(kf_sock h){struct kf_sock_s*s=(void*)h;if(!s||!s->used)return;s->used=0;if(s->tcp){tcp_arg(s->tcp,NULL);tcp_recv(s->tcp,NULL);tcp_err(s->tcp,NULL);tcp_abort(s->tcp);}if(s->up)udp_remove(s->up);free(s->rx);memset(s,0,sizeof*s);}
static char s_resolve_host[128];static uint32_t s_resolve_ip;static int s_resolve_state;
static void resolve_cb(const char*n,const ip_addr_t*ip,void*a){(void)n;(void)a;if(ip){s_resolve_ip=ip4_addr_get_u32(ip_2_ip4(ip));s_resolve_state=2;}else s_resolve_state=3;}
static kf_err n_resolve(const char *host,uint32_t *out){if(!host||!out)return KF_EINVAL;ip_addr_t ip;if(ipaddr_aton(host,&ip)){*out=ip4_addr_get_u32(ip_2_ip4(&ip));return KF_OK;}if(s_resolve_state&&strcmp(host,s_resolve_host)){s_resolve_state=0;}if(s_resolve_state==2){*out=s_resolve_ip;return KF_OK;}if(s_resolve_state==3)return KF_ENOENT;if(!s_resolve_state){snprintf(s_resolve_host,sizeof s_resolve_host,"%s",host);err_t e=dns_gethostbyname(host,&ip,resolve_cb,NULL);if(e==ERR_OK){*out=ip4_addr_get_u32(ip_2_ip4(&ip));return KF_OK;}if(e!=ERR_INPROGRESS)return KF_EIO;s_resolve_state=1;}return KF_AGAIN;}
static int n_online(void){ return kf_net_present() && kf_net_state() == KF_NET_ONLINE; }
static int n_rssi(void){ return kf_net_rssi(); }
static uint32_t n_ip(void){ ip_addr_t ip;return ipaddr_aton(kf_net_ip(),&ip)?ip4_addr_get_u32(ip_2_ip4(&ip)):0; }
static void net_clock_tick(void){
    if(!s_net_eco_hold)return;
    if(kf_net_state()==KF_NET_ONLINE||
       (!kf_net_autoconnect_active()&&!kf_net_scan_active()&&kf_net_state()!=KF_NET_CONNECTING)){
        s_net_eco_hold=0;y_perf(s_perf_req);
    }
}

/* BearSSL over a KAPI TCP socket. BearSSL itself is a kernel singleton, matching the
   one-active-app model; HTTP and raw TLS reject simultaneous ownership. */
struct kf_tls_s { struct kf_sock_s *sock; br_ssl_engine_context *eng; int failed; };
static struct kf_tls_s *s_ktls;
static int tl_pump(struct kf_tls_s *t){
    if(!t||t!=s_ktls||t->failed)return KF_EINVAL;
    for(int spins=0;spins<16;spins++){
        unsigned st=br_ssl_engine_current_state(t->eng);
        if(st&BR_SSL_CLOSED){t->failed=1;return KF_EIO;}
        if(st&BR_SSL_SENDREC){size_t n;unsigned char*p=br_ssl_engine_sendrec_buf(t->eng,&n);int z=n_send(t->sock,p,(int)n);if(z>0){br_ssl_engine_sendrec_ack(t->eng,(size_t)z);continue;}if(z!=KF_AGAIN)return z;}
        if(st&BR_SSL_RECVREC){size_t cap;unsigned char*p=br_ssl_engine_recvrec_buf(t->eng,&cap);int z=n_recv(t->sock,p,(int)cap);if(z>0){br_ssl_engine_recvrec_ack(t->eng,(size_t)z);continue;}if(z==0){t->failed=1;return KF_EIO;}if(z!=KF_AGAIN)return z;}
        if(st&(BR_SSL_SENDAPP|BR_SSL_RECVAPP))return KF_OK;
        return KF_AGAIN;
    }
    return KF_AGAIN;
}
static kf_tls tl_wrap(kf_sock h,const char *sni){struct kf_sock_s*s=(void*)h;if(!s||!s->used||s->udp||s_ktls||s_hreq||!sni)return NULL;if(n_status(h)!=KF_OK)return NULL;struct kf_tls_s*t=malloc(sizeof*t);if(!t)return NULL;kf_tls_begin(sni);*t=(struct kf_tls_s){s,kf_tls_eng(),0};s_ktls=t;return t;}
static int tl_handshake(kf_tls h){struct kf_tls_s*t=(void*)h;int e=tl_pump(t);if(e<0)return e;unsigned st=br_ssl_engine_current_state(t->eng);return(st&BR_SSL_SENDAPP)?KF_OK:KF_AGAIN;}
static int tl_send(kf_tls h,const void *buf,int n){struct kf_tls_s*t=(void*)h;if(!t||n<0||(n&&!buf))return KF_EINVAL;int e=tl_pump(t);if(e<0&&e!=KF_AGAIN)return e;unsigned st=br_ssl_engine_current_state(t->eng);if(!(st&BR_SSL_SENDAPP))return KF_AGAIN;size_t cap;unsigned char*p=br_ssl_engine_sendapp_buf(t->eng,&cap);if((size_t)n>cap)n=(int)cap;if(!n)return KF_AGAIN;memcpy(p,buf,(size_t)n);br_ssl_engine_sendapp_ack(t->eng,(size_t)n);tl_pump(t);return n;}
static int tl_recv(kf_tls h,void *buf,int n){struct kf_tls_s*t=(void*)h;if(!t||n<0||(n&&!buf))return KF_EINVAL;int e=tl_pump(t);if(e<0&&e!=KF_AGAIN)return e;unsigned st=br_ssl_engine_current_state(t->eng);if(!(st&BR_SSL_RECVAPP))return KF_AGAIN;size_t have;unsigned char*p=br_ssl_engine_recvapp_buf(t->eng,&have);if((size_t)n>have)n=(int)have;if(!n)return KF_AGAIN;memcpy(buf,p,(size_t)n);br_ssl_engine_recvapp_ack(t->eng,(size_t)n);return n;}
static void tl_close(kf_tls h){struct kf_tls_s*t=(void*)h;if(!t)return;if(t==s_ktls){br_ssl_engine_close(t->eng);tl_pump(t);s_ktls=NULL;}free(t);}

/* ===================== high-level SSH service ===================== */
struct kf_ssh_s {
    ssh_t *core;char user[64],pass[128];int started,eco_hold;
    void(*data)(void*,const uint8_t*,int);void(*state_cb)(void*,int,const char*);
    int(*hostkey)(void*,const uint8_t[32],const char*);void*ud;
};
static void sh_rng(uint8_t*b,int n,void*ud){(void)ud;while(n>0){uint32_t r=get_rand_32();int z=n>4?4:n;memcpy(b,&r,z);b+=z;n-=z;}}
static uint32_t sh_now(void*ud){(void)ud;return t_millis();}
static int sh_hostkey(const uint8_t pub[32],const char*fp,void*ud){struct kf_ssh_s*s=ud;return s&&s->hostkey?s->hostkey(s->ud,pub,fp):0;}
static void sh_data(const uint8_t*b,int n,void*ud){struct kf_ssh_s*s=ud;if(s&&s->data)s->data(s->ud,b,n);}
static void sh_state(ssh_state_t st,const char*d,void*ud){struct kf_ssh_s*s=ud;if(s&&s->state_cb)s->state_cb(s->ud,(int)st,d?d:"");}
static kf_ssh sh_connect(const char*host,int port,const char*user,const char*pass,const uint8_t*seed,
                         void(*data)(void*,const uint8_t*,int),void(*state)(void*,int,const char*),
                         int(*hostkey)(void*,const uint8_t[32],const char*),void*ud){
    if(s_kssh||!host||!*host||port<1||port>65535||!user||!*user||!n_online())return NULL;
    struct kf_ssh_s*s=calloc(1,sizeof*s);if(!s)return NULL;snprintf(s->user,sizeof s->user,"%s",user);snprintf(s->pass,sizeof s->pass,"%s",pass?pass:"");
    s->data=data;s->state_cb=state;s->hostkey=hostkey;s->ud=ud;ssh_cb_t cb={ssh_tcp_tx,sh_rng,sh_now,sh_hostkey,sh_data,sh_state,s};
    s->core=ssh_create(&cb,malloc,free);if(!s->core){free(s);return NULL;}if(seed)ssh_set_key(s->core,seed);
    ssh_tcp_init(s->core);kf_clock_eco();s->eco_hold=1;s_ssh_eco_hold=1;
    if(ssh_tcp_connect(host,(uint16_t)port)<0){s->eco_hold=0;s_ssh_eco_hold=0;y_perf(s_perf_req);ssh_tcp_close();ssh_destroy(s->core);free(s);return NULL;}
    s_kssh=s;return s;
}
static int sh_poll(kf_ssh h){struct kf_ssh_s*s=(void*)h;if(!s||s!=s_kssh)return KF_EINVAL;kf_net_poll();ssh_tcp_poll();
    if(s->eco_hold&&(ssh_tcp_is_up()||ssh_tcp_is_dead())){s->eco_hold=0;s_ssh_eco_hold=0;y_perf(s_perf_req);}
    if(ssh_tcp_is_up()&&!s->started){ssh_start(s->core,s->user);s->started=1;}
    if(s->started){ssh_tick(s->core);if(ssh_wants_password(s->core))ssh_auth_password(s->core,s->pass);}
    ssh_state_t st=ssh_state(s->core);if(st==SSH_ST_RUNNING)return KF_OK;if(st==SSH_ST_ERROR||st==SSH_ST_CLOSED||ssh_tcp_is_dead())return KF_EIO;return KF_AGAIN;}
static int sh_send(kf_ssh h,const void*b,int n){struct kf_ssh_s*s=(void*)h;if(!s||s!=s_kssh||n<0||(n&&!b))return KF_EINVAL;return ssh_state(s->core)==SSH_ST_RUNNING?ssh_send_channel(s->core,b,n):KF_AGAIN;}
static void sh_resize(kf_ssh h,int c,int r){struct kf_ssh_s*s=(void*)h;if(s&&s==s_kssh&&c>0&&r>0)ssh_window_change(s->core,c,r);}
static int sh_get_state(kf_ssh h){struct kf_ssh_s*s=(void*)h;return s&&s==s_kssh?(int)ssh_state(s->core):SSH_ST_ERROR;}
static const char*sh_error(kf_ssh h){struct kf_ssh_s*s=(void*)h;if(!s||s!=s_kssh)return "invalid session";const char*e=ssh_error(s->core);return e&&*e?e:ssh_tcp_err();}
static void sh_close(kf_ssh h){struct kf_ssh_s*s=(void*)h;if(!s)return;if(s==s_kssh){if(ssh_state(s->core)==SSH_ST_RUNNING)ssh_disconnect(s->core,"bye");ssh_tcp_close();ssh_destroy(s->core);if(s->eco_hold){s_ssh_eco_hold=0;y_perf(s_perf_req);}s_kssh=NULL;}memset(s,0,sizeof*s);free(s);}

/* ===================== image codec ===================== */
struct kf_img_s { kf_mem px,alpha; int w,h; };
static uint8_t s_img_io[1024];
static uint16_t s_img_row[320];
static uint8_t s_alpha_row[320];
static kf_img im_store(uint16_t *px,uint8_t *alpha,int w,int h){
    if(!px || w<1 || h<1){ free(px);free(alpha);return NULL; }
    size_t np=(size_t)w*h; kf_mem hp=m_psram_alloc(np*2),ha=alpha?m_psram_alloc(np):0;
    if(!hp || (alpha&&!ha)){ m_psram_free(hp);m_psram_free(ha);free(px);free(alpha);return NULL; }
    m_psram_write(hp,0,px,np*2); if(alpha)m_psram_write(ha,0,alpha,np);
    free(px);free(alpha);
    struct kf_img_s *im=malloc(sizeof *im); if(!im){m_psram_free(hp);m_psram_free(ha);return NULL;}
    *im=(struct kf_img_s){hp,ha,w,h}; return im;
}
static kf_img im_decode_psram(uint32_t base,uint32_t len){
    uint16_t *px=NULL;uint8_t *alpha=NULL;int w=0,h=0;
    if(!kf_img_decode(base,len,LCD_W,KF_CONTENT_H,120u*1024u,16u*1024u,&px,&alpha,&w,&h)) return NULL;
    return im_store(px,alpha,w,h);
}
static kf_img im_decode(const void *buf,size_t len){
    if(!buf || len<8 || len>0xffffffffu) return NULL;
    kf_mem src=m_psram_alloc(len); if(!src) return NULL; m_psram_write(src,0,buf,len);
    kf_img im=im_decode_psram(pm_base(src),(uint32_t)len); m_psram_free(src); return im;
}
static kf_img im_decode_file(const char *path){
    FILE *f=fopen(path,"rb");if(!f)return NULL;
    if(fseek(f,0,SEEK_END)||ftell(f)<8){fclose(f);return NULL;} long z=ftell(f);rewind(f);
    if(z<=0)z=ftell(f); if(z<=0 || (unsigned long)z>0xffffffffu){fclose(f);return NULL;}
    kf_mem src=m_psram_alloc((size_t)z);if(!src){fclose(f);return NULL;}
    uint32_t off=0; while(off<(uint32_t)z){size_t want=(uint32_t)z-off;if(want>sizeof s_img_io)want=sizeof s_img_io;
        size_t n=fread(s_img_io,1,want,f);if(!n)break;m_psram_write(src,off,s_img_io,n);off+=(uint32_t)n;}
    fclose(f); kf_img im=off==(uint32_t)z?im_decode_psram(pm_base(src),(uint32_t)z):NULL; m_psram_free(src); return im;
}
static void im_info(kf_img h,int *w,int *hh){struct kf_img_s *im=(void*)h;if(w)*w=im?im->w:0;if(hh)*hh=im?im->h:0;}
static uint16_t blend565(uint16_t d,uint16_t s,uint8_t a){
    if(a==255)return s;if(!a)return d;uint32_t ia=255-a;
    uint32_t r=(((s>>11)&31)*a+((d>>11)&31)*ia+127)/255;
    uint32_t g=(((s>>5)&63)*a+((d>>5)&63)*ia+127)/255;
    uint32_t b=((s&31)*a+(d&31)*ia+127)/255;return(uint16_t)((r<<11)|(g<<5)|b);
}
static void im_to_canvas(kf_img h,kf_canvas ch,int x,int y){
    struct kf_img_s *im=(void*)h;struct kf_canvas_s *c=(void*)ch;if(!im||!c)return;
    for(int sy=0;sy<im->h;sy++){int dy=y+sy;if((unsigned)dy>=(unsigned)c->h)continue;
        int n=im->w;if(n>320)n=320;m_psram_read(im->px,(size_t)sy*im->w*2,s_img_row,n*2);
        if(im->alpha)m_psram_read(im->alpha,(size_t)sy*im->w,s_alpha_row,n);
        for(int sx=0;sx<n;sx++){int dx=x+sx;if((unsigned)dx>=(unsigned)c->w)continue;
            uint16_t s=s_img_row[sx];c->buf[dy*c->w+dx]=im->alpha?blend565(c->buf[dy*c->w+dx],s,s_alpha_row[sx]):s;}
    }
    g_present(ch);
}
static void im_free(kf_img h){struct kf_img_s *im=(void*)h;if(!im)return;m_psram_free(im->alpha);m_psram_free(im->px);free(im);}
static void put16(uint8_t *p,uint16_t v){p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}static void put32(uint8_t*p,uint32_t v){put16(p,(uint16_t)v);put16(p+2,(uint16_t)(v>>16));}
static kf_err im_encode_file(const char *path,const kf_color *px,int w,int h){
    if(!path||!px||w<1||h<1||w>320||h>320)return KF_EINVAL;FILE*f=fopen(path,"wb");if(!f)return KF_EIO;
    uint32_t stride=((uint32_t)w*3+3)&~3u,bytes=stride*(uint32_t)h;uint8_t hdr[54]={0};hdr[0]='B';hdr[1]='M';put32(hdr+2,54+bytes);put32(hdr+10,54);put32(hdr+14,40);put32(hdr+18,w);put32(hdr+22,h);put16(hdr+26,1);put16(hdr+28,24);put32(hdr+34,bytes);
    if(fwrite(hdr,1,sizeof hdr,f)!=sizeof hdr){fclose(f);return KF_EIO;}
    static uint8_t row[960];for(int y=h-1;y>=0;y--){memset(row,0,stride);for(int x=0;x<w;x++){uint16_t v=px[y*w+x];row[x*3]=(uint8_t)((v&31)<<3);row[x*3+1]=(uint8_t)(((v>>5)&63)<<2);row[x*3+2]=(uint8_t)(((v>>11)&31)<<3);}if(fwrite(row,1,stride,f)!=stride){fclose(f);return KF_EIO;}}
    return fclose(f)==0?KF_OK:KF_EIO;
}

/* ===================== math (forward to kernel libm) ===================== */
/* Transcendentals are referenced directly (signature double(double[,double])). Only the
   float<->text helpers need a wrapper, since they shape a printf format / call strtod. */
static int mm_fmt_double(char *out, int n, double v, int prec, char fmt){
    if(fmt != 'f' && fmt != 'e' && fmt != 'g') fmt = 'g';
    if(prec < 0) prec = 6;
    char spec[8]; snprintf(spec, sizeof spec, "%%.%d%c", prec, fmt);
    return snprintf(out, n, spec, v);
}
static double mm_parse_double(const char *s, char **end){ return strtod(s, end); }

/* ===================== managed UI (Layer 1) ===================== */
static kui_obj u_screen(void){
    if(!s_scr) return NULL;
    for(int i=0;i<s_nkui;i++) if(s_kui[i] && s_kui[i]->kind==KUI_SCREEN) return (kui_obj)s_kui[i];
    kf_inset_top(s_scr);
    lv_obj_set_style_bg_color(s_scr,KF_BG_DEEP,0);
    lv_obj_set_style_bg_opa(s_scr,LV_OPA_COVER,0);
    return (kui_obj)u_wrap(s_scr,KUI_SCREEN);
}
static kui_obj u_label(kui_obj parent,const char *text){
    lv_obj_t *o=lv_label_create(u_parent(parent)); if(!o) return NULL;
    lv_label_set_text(o,text?text:"");
    struct kui_obj_s*w=u_wrap(o,KUI_LABEL);if(!w)lv_obj_delete(o);return (kui_obj)w;
}
static kui_obj u_button(kui_obj parent,const char *text,void (*cb)(void*),void *ud){
    lv_obj_t *o=lv_button_create(u_parent(parent)); if(!o) return NULL;
    lv_obj_t *l=lv_label_create(o); lv_label_set_text(l,text?text:""); lv_obj_center(l);
    struct kui_obj_s *w=u_wrap(o,KUI_BUTTON); if(!w){ lv_obj_delete(o); return NULL; }
    w->click=cb; w->click_ud=ud; lv_obj_add_event_cb(o,u_click_event,LV_EVENT_CLICKED,w);
    if(s_ui_group) lv_group_add_obj(s_ui_group,o);
    return (kui_obj)w;
}
static kui_obj u_list(kui_obj parent){
    lv_obj_t *o=lv_list_create(u_parent(parent)); if(!o) return NULL;
    lv_obj_set_width(o,LV_PCT(100)); lv_obj_set_flex_grow(o,1);
    struct kui_obj_s*w=u_wrap(o,KUI_LIST);if(!w)lv_obj_delete(o);return (kui_obj)w;
}
static kui_obj u_list_add(kui_obj list,const char *text,void (*cb)(void*),void *ud){
    lv_obj_t *o=lv_list_add_button(u_parent(list),NULL,text?text:""); if(!o) return NULL;
    struct kui_obj_s *w=u_wrap(o,KUI_BUTTON); if(!w){ lv_obj_delete(o); return NULL; }
    w->click=cb; w->click_ud=ud; lv_obj_add_event_cb(o,u_click_event,LV_EVENT_CLICKED,w);
    if(s_ui_group) lv_group_add_obj(s_ui_group,o);
    return (kui_obj)w;
}
static kui_obj u_textarea(kui_obj parent){
    lv_obj_t *o=lv_textarea_create(u_parent(parent)); if(!o) return NULL;
    if(s_ui_group) lv_group_add_obj(s_ui_group,o);
    struct kui_obj_s*w=u_wrap(o,KUI_TEXTAREA);if(!w)lv_obj_delete(o);return (kui_obj)w;
}
static kui_obj u_checkbox(kui_obj parent,const char *text){
    lv_obj_t *o=lv_checkbox_create(u_parent(parent)); if(!o) return NULL;
    lv_checkbox_set_text(o,text?text:""); if(s_ui_group) lv_group_add_obj(s_ui_group,o);
    struct kui_obj_s*w=u_wrap(o,KUI_CHECKBOX);if(!w)lv_obj_delete(o);return (kui_obj)w;
}
static kui_obj u_slider(kui_obj parent,int min,int max){
    lv_obj_t *o=lv_slider_create(u_parent(parent)); if(!o) return NULL;
    lv_slider_set_range(o,min,max); if(s_ui_group) lv_group_add_obj(s_ui_group,o);
    struct kui_obj_s*w=u_wrap(o,KUI_SLIDER);if(!w)lv_obj_delete(o);return (kui_obj)w;
}
static void u_set_text(kui_obj h,const char *text){
    struct kui_obj_s *w=(void*)h; if(!w || !w->obj) return; if(!text) text="";
    if(w->kind==KUI_LABEL) lv_label_set_text(w->obj,text);
    else if(w->kind==KUI_TEXTAREA) lv_textarea_set_text(w->obj,text);
    else if(w->kind==KUI_CHECKBOX) lv_checkbox_set_text(w->obj,text);
    else if(w->kind==KUI_BUTTON){ lv_obj_t *l=lv_obj_get_child(w->obj,0); if(l) lv_label_set_text(l,text); }
}
static const char *u_get_text(kui_obj h){
    struct kui_obj_s *w=(void*)h; if(!w || !w->obj) return "";
    if(w->kind==KUI_LABEL) return lv_label_get_text(w->obj);
    if(w->kind==KUI_TEXTAREA) return lv_textarea_get_text(w->obj);
    if(w->kind==KUI_CHECKBOX) return lv_checkbox_get_text(w->obj);
    if(w->kind==KUI_BUTTON){ lv_obj_t *l=lv_obj_get_child(w->obj,0); return l?lv_label_get_text(l):""; }
    return "";
}
static void u_msgbox(const char *title,const char *msg){
    if(!s_scr) return;
    lv_obj_t *m=lv_msgbox_create(s_scr);
    lv_msgbox_add_title(m,title?title:""); lv_msgbox_add_text(m,msg?msg:""); lv_msgbox_add_close_button(m);
    lv_obj_center(m);
}
static kf_canvas u_canvas(kui_obj parent,int w,int h){
    kf_canvas c=g_canvas(0,0,w,h); if(c && parent) lv_obj_set_parent(((struct kf_canvas_s*)c)->obj,u_parent(parent));
    return c;
}
static struct kui_obj_s *u_valid(kui_obj h){struct kui_obj_s*w=(void*)h;return w&&w->obj?w:NULL;}
static void u_destroy(kui_obj h){struct kui_obj_s*w=u_valid(h);if(!w||w->kind==KUI_SCREEN)return;if(s_ui_group)lv_group_remove_obj(w->obj);lv_obj_delete(w->obj);w->obj=NULL;}
static void u_set_pos(kui_obj h,int x,int y){struct kui_obj_s*w=u_valid(h);if(w)lv_obj_set_pos(w->obj,x,y);}
static void u_set_size(kui_obj h,int wv,int hv){struct kui_obj_s*w=u_valid(h);if(w)lv_obj_set_size(w->obj,wv,hv);}
static void u_align(kui_obj h,int a,int x,int y){
    static const lv_align_t map[]={LV_ALIGN_CENTER,LV_ALIGN_TOP_LEFT,LV_ALIGN_TOP_MID,LV_ALIGN_TOP_RIGHT,LV_ALIGN_LEFT_MID,LV_ALIGN_RIGHT_MID,LV_ALIGN_BOTTOM_LEFT,LV_ALIGN_BOTTOM_MID,LV_ALIGN_BOTTOM_RIGHT};
    struct kui_obj_s*w=u_valid(h);if(w&&a>=0&&a<(int)(sizeof map/sizeof map[0]))lv_obj_align(w->obj,map[a],x,y);
}
static void u_flex(kui_obj h,int f,int gap){
    static const lv_flex_flow_t map[]={LV_FLEX_FLOW_ROW,LV_FLEX_FLOW_ROW,LV_FLEX_FLOW_COLUMN,LV_FLEX_FLOW_ROW_WRAP,LV_FLEX_FLOW_COLUMN_WRAP};
    struct kui_obj_s*w=u_valid(h);if(!w)return;if(f<=KUI_FLEX_NONE){lv_obj_set_layout(w->obj,LV_LAYOUT_NONE);return;}
    if(f<(int)(sizeof map/sizeof map[0])){lv_obj_set_flex_flow(w->obj,map[f]);lv_obj_set_style_pad_row(w->obj,gap,0);lv_obj_set_style_pad_column(w->obj,gap,0);}
}
static void u_grow(kui_obj h,int n){struct kui_obj_s*w=u_valid(h);if(w)lv_obj_set_flex_grow(w->obj,n<0?0:n);}
static void u_hidden(kui_obj h,int yes){struct kui_obj_s*w=u_valid(h);if(w){if(yes)lv_obj_add_flag(w->obj,LV_OBJ_FLAG_HIDDEN);else lv_obj_remove_flag(w->obj,LV_OBJ_FLAG_HIDDEN);}}
static void u_enabled(kui_obj h,int yes){struct kui_obj_s*w=u_valid(h);if(w){if(yes)lv_obj_remove_state(w->obj,LV_STATE_DISABLED);else lv_obj_add_state(w->obj,LV_STATE_DISABLED);}}
static void u_set_value(kui_obj h,int v){struct kui_obj_s*w=u_valid(h);if(!w)return;if(w->kind==KUI_SLIDER)lv_slider_set_value(w->obj,v,LV_ANIM_OFF);else if(w->kind==KUI_CHECKBOX){if(v)lv_obj_add_state(w->obj,LV_STATE_CHECKED);else lv_obj_remove_state(w->obj,LV_STATE_CHECKED);}}
static int u_get_value(kui_obj h){struct kui_obj_s*w=u_valid(h);if(!w)return 0;if(w->kind==KUI_SLIDER)return lv_slider_get_value(w->obj);if(w->kind==KUI_CHECKBOX)return lv_obj_has_state(w->obj,LV_STATE_CHECKED);return 0;}
static void u_focus(kui_obj h){struct kui_obj_s*w=u_valid(h);if(w&&s_ui_group)lv_group_focus_obj(w->obj);}
static void u_on_change(kui_obj h,void(*fn)(void*),void*ud){struct kui_obj_s*w=u_valid(h);if(!w)return;w->change=fn;w->change_ud=ud;lv_obj_add_event_cb(w->obj,u_change_event,LV_EVENT_VALUE_CHANGED,w);}
static void u_set_colors(kui_obj h,kf_color fg,kf_color bg){struct kui_obj_s*w=u_valid(h);if(w){lv_obj_set_style_text_color(w->obj,c565(fg),0);lv_obj_set_style_bg_color(w->obj,c565(bg),0);lv_obj_set_style_bg_opa(w->obj,LV_OPA_COVER,0);}}
static void u_set_font(kui_obj h,int px){struct kui_obj_s*w=u_valid(h);if(w)lv_obj_set_style_text_font(w->obj,px>=18?KF_FONT_BIG:KF_FONT,0);}

/* ===================== async HTTP helper (Layer 1) ===================== */
static struct khttp_req_s *h_begin(void){
    if(s_hreq || !kf_psram_size()) return NULL;
    uint32_t mark=kf_psram_brk(), avail=kf_psram_size()>mark?kf_psram_size()-mark:0;
    if(avail>2u*1024u*1024u) avail=2u*1024u*1024u;
    if(avail<4096) return NULL;
    kf_mem mem=m_psram_alloc(avail); uint32_t base=pm_base(mem); if(!mem || base==0xffffffffu) return NULL;
    struct khttp_req_s *r=malloc(sizeof *r); if(!r){ m_psram_free(mem); return NULL; }
    *r=(struct khttp_req_s){mem,base,avail,0,1}; s_hreq=r; kf_http_set_arena(base,avail); return r;
}
static void *h_get(const char *url){
    struct khttp_req_s *r=h_begin(); if(!r) return NULL;
    if(kf_http_get(url)<0){ s_hreq=NULL; m_psram_free(r->mem); free(r); return NULL; }
    return r;
}
static void *h_post(const char *url,const void *body,int len){
    if(len<0 || (len && !body)) return NULL;
    struct khttp_req_s *r=h_begin(); if(!r) return NULL;
    if(kf_http_post(url,"Content-Type: application/octet-stream\r\n",body,(uint32_t)len)<0){
        s_hreq=NULL; m_psram_free(r->mem); free(r); return NULL;
    }
    return r;
}
static void *h_post_headers(const char *url,const char *headers,const void *body,int len){
    if(len<0||(len&&!body))return NULL;struct khttp_req_s*r=h_begin();if(!r)return NULL;
    if(kf_http_post(url,headers,(const char*)body,(uint32_t)len)<0){s_hreq=NULL;m_psram_free(r->mem);free(r);return NULL;}
    return r;
}
static int h_poll(void *req,void *buf,int n){
    struct khttp_req_s *r=req; if(!r || r!=s_hreq || n<0 || (n&&!buf)) return KF_EINVAL;
    kf_net_poll(); kf_http_poll();
    uint32_t have=kf_http_body_len();
    if(r->read_off<have){ uint32_t take=have-r->read_off; if(take>(uint32_t)n) take=(uint32_t)n;
        if(take){ kf_psram_read(kf_http_body_base()+r->read_off,buf,take); r->read_off+=take; return (int)take; } }
    int st=kf_http_state();
    if(st==KF_HTTP_DONE) return 0;
    if(st==KF_HTTP_ERROR) return KF_EIO;
    return KF_AGAIN;
}
static void h_free(void *req){
    struct khttp_req_s *r=req; if(!r) return;
    if(r==s_hreq){ kf_http_abort(); m_psram_free(r->mem); s_hreq=NULL; }
    free(r);
}
static int h_status(void *req){return req&&req==s_hreq?kf_http_status():0;}
static const char *h_error(void *req){return req&&req==s_hreq?kf_http_err():"invalid request";}
static const char *h_final_url(void *req){return req&&req==s_hreq?kf_http_final_url():"";}

/* ===================== system-app device services ===================== */
static const char *dv_cfg_get(const char*k,const char*d){return k?deskconf_get(k,d):d;}
static int dv_cfg_get_int(const char*k,int d){return k?deskconf_get_int(k,d):d;}
static void dv_cfg_set(const char*k,const char*v){if(k&&v)deskconf_set(k,v);}
static void dv_cfg_set_int(const char*k,int v){if(k)deskconf_set_int(k,v);}
static kf_err dv_set_backlight(int lcd,int key){
    if(lcd<0||lcd>9||key<0||key>3)return KF_EINVAL;uint8_t a=(uint8_t)lcd,b=(uint8_t)key;
    if(reg_write(REG_BKL,&a,1)<=0||reg_write(REG_BK2,&b,1)<=0)return KF_EIO;
    deskconf_set_int("bkl",lcd);deskconf_set_int("bk2",key);return KF_OK;
}
static void(*s_scan_found)(void*,const char*,int,int);static void*s_scan_ud;
static void dv_scan_cb(const char*ssid,int rssi,int secure){if(s_scan_found)s_scan_found(s_scan_ud,ssid,rssi,secure);}
static kf_err dv_wifi_scan(void(*fn)(void*,const char*,int,int),void*ud){
    s_scan_found=fn;s_scan_ud=ud;kf_clock_eco();kf_net_init();s_net_eco_hold=1;
    int e=kf_net_scan_start(dv_scan_cb);if(e<0){s_net_eco_hold=0;y_perf(s_perf_req);return KF_EIO;}return KF_OK;
}
static int dv_wifi_scan_active(void){return kf_net_scan_active();}
static kf_err dv_wifi_connect(const char*ssid,const char*pass){
    if(!ssid||!*ssid)return KF_EINVAL;kf_clock_eco();kf_net_init();s_net_eco_hold=1;kf_net_connect(ssid,pass?pass:"");return KF_OK;
}
static void dv_wifi_forget(void){kf_net_forget();s_net_eco_hold=0;y_perf(s_perf_req);}
static const char*dv_wifi_ssid(void){return kf_net_ssid();}
static int dv_wifi_state(void){return (int)kf_net_state();}
static void dv_sfx_play(const char*id){if(id)kf_sfx_play(id);}
static void dv_power(int a){if(a==0)kf_poweroff();else if(a==1)kf_reboot();else if(a==2)kf_bootsel();}

/* Compact bounded document renderer. It deliberately renders into the caller's canvas
   instead of creating hidden kernel widgets, so both windowed and exclusive apps can use it. */
static void d_line(struct kf_canvas_s*c,char *line,int *ln,int *y,int big){
    if(!*ln){*y+=g_line_h((kf_font)KF_FONT);return;}line[*ln]=0;kf_font f=(kf_font)(big?KF_FONT_BIG:KF_FONT);
    int start=0,lh=g_line_h(f);while(start<*ln&&*y+lh<=c->h){int end=start,last=-1;
        while(end<*ln){if(line[end]==' ')last=end;char save=line[end+1];line[end+1]=0;int tw=g_text_w(f,line+start);line[end+1]=save;if(tw>c->w-8)break;end++;}
        if(end<*ln&&last>=start)end=last;if(end<=start)end=start+1;char save=line[end];line[end]=0;g_draw_text(c,f,4,*y,line+start,0xffff);line[end]=save;
        *y+=lh+1;start=end;while(start<*ln&&line[start]==' ')start++;}
    *ln=0;
}
static void d_render(kf_canvas h,const char *markup,int fmt){
    struct kf_canvas_s*c=(void*)h;if(!c||!markup)return;static char line[512];int ln=0,y=4,big=0;
    for(const char*p=markup;;p++){
        char ch=*p;if(fmt==KDOC_HTML&&ch=='<'){int br=0;p++;if(*p=='/')p++;if(!strncmp(p,"br",2)||!strncmp(p,"p",1)||!strncmp(p,"li",2)||!strncmp(p,"h",1))br=1;while(*p&&*p!='>')p++;if(br)d_line(c,line,&ln,&y,big);if(!*p)break;continue;}
        if((ch=='\n'||ch=='\r'||!ch)){d_line(c,line,&ln,&y,big);big=0;if(!ch||y>=c->h)break;continue;}
        if(fmt==KDOC_MARKDOWN&&ln==0&&ch=='#'){big=1;while(p[1]=='#')p++;while(p[1]==' ')p++;continue;}
        if(fmt==KDOC_MARKDOWN&&(ch=='*'||ch=='_'||ch=='`'))continue;
        if(ch=='&'&&fmt==KDOC_HTML){if(!strncmp(p,"&lt;",4)){ch='<';p+=3;}else if(!strncmp(p,"&gt;",4)){ch='>';p+=3;}else if(!strncmp(p,"&amp;",5)){ch='&';p+=4;}else if(!strncmp(p,"&nbsp;",6)){ch=' ';p+=5;}}
        if(ln<(int)sizeof line-1)line[ln++]=ch;
    }
    g_present(h);
}

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
static const struct k_math K_MATH = {
    sin, cos, tan, asin, acos, atan, atan2,
    sinh, cosh, tanh, asinh, acosh, atanh,
    exp, expm1, log, log1p, log10, log2, pow,
    sqrt, cbrt, hypot, fmod, floor, ceil, round, trunc,
    lgamma, tgamma, erf, erfc,
    mm_fmt_double, mm_parse_double
};
static const struct k_ui K_UI = {
    u_screen,u_label,u_button,u_list,u_list_add,u_textarea,u_checkbox,u_slider,
    u_set_text,u_get_text,u_msgbox,u_canvas,
    u_destroy,u_set_pos,u_set_size,u_align,u_flex,u_grow,u_hidden,u_enabled,u_set_value,u_get_value,
    u_focus,u_on_change,u_set_colors,u_set_font
};
static const struct k_http K_HTTP = { h_get,h_post,h_poll,h_free,h_post_headers,h_status,h_error,h_final_url };
static const struct k_doc K_DOC = { d_render };
static const struct k_device K_DEVICE = {dv_cfg_get,dv_cfg_get_int,dv_cfg_set,dv_cfg_set_int,dv_set_backlight,
    dv_wifi_scan,dv_wifi_scan_active,dv_wifi_connect,dv_wifi_forget,dv_wifi_ssid,dv_wifi_state,dv_sfx_play,dv_power};
static const struct k_ssh K_SSH = {sh_connect,sh_poll,sh_send,sh_resize,sh_get_state,sh_error,sh_close};

static const kapi G_KAPI = {
    KAPI_ABI, KAPI_MINOR,
    &K_SYS, &K_MEM, &K_GFX, &K_TXT, &K_IMG, &K_IN, &K_AUD, &K_FS, &K_NET, &K_TLS, &K_TIME,
    &K_UI, &K_HTTP, &K_DOC,
    &K_MATH,           /* appended at minor 1 */
    &K_DEVICE,         /* appended at minor 2 */
    &K_SSH
};

/* ===================== teardown / poll ===================== */
void *kapi_idle_scratch(size_t bytes){
    /* Built-ins and SD-loaded apps cannot run together. Music may use this
       arena while open, and releases its references before returning to the
       launcher; a .kx image or canvas must never own it at the same time. */
    if(s_active || s_arena_top || bytes > KAPI_ARENA_BYTES) return NULL;
    return g_kapi_arena;
}

static void kapi_teardown(void){
    if(s_on_close) s_on_close(s_on_close_ud);
    if(s_view.open)g_view_close(&s_view);else if(s_panel_leased)g_release();
    a_out_stop();
    if(s_hreq) h_free(s_hreq);
    if(s_ktls) tl_close(s_ktls);
    if(s_kssh) sh_close(s_kssh);
    for(int i=0;i<KSOCK_MAX;i++) if(s_sock[i].used) n_close(&s_sock[i]);
    s_resolve_state=0;s_scan_found=NULL;s_scan_ud=NULL;
    for(int i = 0; i < s_ncanv; i++) if(s_canv[i]){ if(s_canv[i]->obj) lv_obj_delete(s_canv[i]->obj);
        if(!s_canv[i]->from_arena) free(s_canv[i]->buf); free(s_canv[i]); s_canv[i] = NULL; }
    s_ncanv = 0; s_arena_top = NULL;
    if(s_locked_ptr)m_unlock(s_locked_mem);
    kf_psram_free_to(s_pmem_mark); memset(s_pmem,0,sizeof s_pmem);
    if(s_ui_group){ lv_group_delete(s_ui_group); s_ui_group=NULL; }
    for(int i=0;i<s_nkui;i++){if(s_kui[i]&&s_kui[i]->obj)lv_obj_remove_event_cb(s_kui[i]->obj,u_deleted);free(s_kui[i]);s_kui[i]=NULL;}
    s_nkui=0;
    s_on_frame = NULL; s_on_key = NULL; s_on_close = NULL;
    s_active = 0; s_exit = 0;
    s_net_eco_hold=0;s_ssh_eco_hold=0;s_perf_req=KF_PERF_NORMAL;
    kf_grab_input(0);
    kf_clock_normal();
    s_scr = NULL;
    kf_back_to_launcher();   /* loads the desktop, async-deletes our (now empty) screen */
}

void kapi_poll(void){
    if(!s_active) return;
    if(s_exit){ kapi_teardown(); return; }
    tone_pump();net_clock_tick();if(s_kssh)sh_poll(s_kssh);
    uint8_t st, key;
    while(uart_pop_key(&st, &key)){
        if(s_on_key) s_on_key(s_on_key_ud, map_dk(key), st != KS_RELEASE);
        if(!s_active || s_exit) return;          /* app may have exited mid-drain */
        if(st!=KS_RELEASE && s_ui_group){
            if(key==DK_UP) lv_group_focus_prev(s_ui_group);
            else if(key==DK_DOWN || key==DK_TAB) lv_group_focus_next(s_ui_group);
            else { uint32_t lk=map_lv_key(key); if(lk) lv_group_send_data(s_ui_group,lk); }
        }
        if(st!=KS_RELEASE && (key==DK_ESC || key==DK_BREAK) && !s_on_key){ s_exit=1; return; }
    }
    if(s_on_frame) s_on_frame(s_on_frame_ud);
}

/* ===================== loader ===================== */
/* Public entry used by manifest-discovered class-1 apps on the SD card. */
int kapi_run(const char *path){
    if(s_active) return -1;
    FILE *f = fopen(path, "rb");
    if(!f){ printf("kapi: cannot open %s\n", path); return -2; }
    kx_header h;
    if(fread(&h, 1, sizeof h, f) != sizeof h){ fclose(f); return -3; }
    if(memcmp(h.magic, KX_MAGIC, 4) != 0 || h.abi_version != KAPI_ABI){ fclose(f); printf("kapi: bad magic/abi\n"); return -4; }
    if(h.load_base != (uint32_t)(uintptr_t)g_kapi_arena){
        fclose(f); printf("kapi: load_base %08lx != arena %08lx (rebuild app)\n",
                          (unsigned long)h.load_base, (unsigned long)(uintptr_t)g_kapi_arena); return -5; }
    /* Validate sizes WITHOUT overflowing 32-bit size_t: a huge image_size + bss_size
       would wrap and pass a naive sum, then fread would overrun the arena. Check each
       field against the arena using only non-wrapping subtraction. */
    if(h.image_size > KAPI_ARENA_BYTES ||
       h.bss_size   > KAPI_ARENA_BYTES - h.image_size){ fclose(f); printf("kapi: too big\n"); return -6; }
    /* entry_offset must land inside the loaded image (checked before any state setup). */
    if(h.entry_offset >= h.image_size){ fclose(f); printf("kapi: bad entry_offset\n"); return -8; }
    if(fread(g_kapi_arena, 1, h.image_size, f) != h.image_size){ fclose(f); return -7; }
    fclose(f);
    if(kx_crc32(g_kapi_arena,h.image_size)!=h.crc32){ printf("kapi: CRC mismatch\n"); return -9; }
    memset(g_kapi_arena + h.image_size, 0, h.bss_size);
    s_arena_top = g_kapi_arena + (((size_t)h.image_size + h.bss_size + 31) & ~(size_t)31);  /* canvas pool start */
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
    s_nkui=0; memset(s_kui,0,sizeof s_kui);
    s_pmem_mark=kf_psram_brk(); memset(s_pmem,0,sizeof s_pmem);
    s_exit = 0; s_active = 1;s_perf_req=KF_PERF_NORMAL;s_net_eco_hold=0;s_ssh_eco_hold=0;s_panel_leased=0;memset(&s_view,0,sizeof s_view);
    kf_grab_input(1);
    s_ui_group=lv_group_create();
    if(s_ui_group) lv_indev_set_group(indev_get(),s_ui_group);

    /* Network apps get the same lazy radio bring-up as built-ins. Association stays at
       the proven-safe eco clock; the requested app tier is restored once it settles. */
    if(h.flags&KX_FLAG_WANTS_NET){
        kf_clock_eco();kf_net_init();kf_net_autoconnect();
        s_net_eco_hold=kf_net_autoconnect_active();if(!s_net_eco_hold)kf_clock_normal();
    }

    app_main_fn entry = (app_main_fn)(((uintptr_t)g_kapi_arena + h.entry_offset) | 1u);  /* Thumb bit */
    entry(&G_KAPI);
    return 0;
}
