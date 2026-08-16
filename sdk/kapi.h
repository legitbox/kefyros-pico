/* sdk/kapi.h — Kefyros App Programming Interface (KAPI), ABI v1.
 *
 * The ONLY header a Kefyros app (.kx) includes. The kernel hands app_main() a single
 * `const kapi*`; it is the app's entire universe — no linked kernel symbols, no globals.
 * Every capability is a function pointer in a versioned struct.
 *
 * Spec: KAPI.md. RULE: append-only. New funcs go at the END of a sub-struct (bump
 * `minor`); never reorder/remove within an ABI major (that needs a `KAPI_ABI` bump).
 */
#ifndef KAPI_H
#define KAPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KAPI_ABI    1
#define KAPI_MINOR  2     /* managed UI, HTTP/device/SSH services */

/* ===== scalar types ===== */
typedef uint16_t kf_color;      /* RGB565, panel-native                              */
typedef int      kf_err;        /* 0 ok, <0 error, KF_AGAIN = non-blocking not-ready  */

/* ===== opaque handles (apps hold, never dereference) ===== */
typedef struct kf_canvas_s* kf_canvas;
typedef struct kf_font_s*   kf_font;
typedef struct kf_img_s*    kf_img;
typedef struct kf_sock_s*   kf_sock;
typedef struct kf_tls_s*    kf_tls;
typedef struct kf_file_s*   kf_file;
typedef struct kf_dir_s*    kf_dir;
typedef struct kf_view_s*   kf_view;   /* windowed direct-blit content viewport       */
typedef struct kf_ssh_s*    kf_ssh;
typedef struct kui_obj_s*   kui_obj;   /* Layer-1 widget                              */
typedef uint32_t            kf_mem;    /* PSRAM block handle (0 = invalid)            */

/* ===== error codes ===== */
enum { KF_OK=0, KF_ERR=-1, KF_ENOMEM=-2, KF_ENOENT=-3, KF_EIO=-5,
       KF_EAGAIN=-11, KF_EINVAL=-22, KF_EUNSUPP=-95 };
#define KF_AGAIN KF_EAGAIN

/* ===== capability bits (sys->caps) ===== */
enum { KF_CAP_PSRAM=1, KF_CAP_WIFI=2, KF_CAP_AUDIO_OUT=4, /* 8 reserved (no audio-in HW) */
       KF_CAP_FLASH_SCRATCH=16, KF_CAP_IMG=32, KF_CAP_RNG=64, KF_CAP_TLS=128 };

/* ===== performance + idle (see KAPI.md §7.3) ===== */
enum kf_perf { KF_PERF_ECO, KF_PERF_NORMAL, KF_PERF_BOOST };       /* app-requestable  */
enum kf_idle { KF_IDLE_NORMAL, KF_IDLE_KEEP_CLOCK, KF_IDLE_KEEP_AWAKE };

/* ===== keys (in->poll / on_key). Printable keys arrive as their ASCII code;
 *       special keys use these >0xFF codes. F1..F10 = KF_KEY_F1 + 0..9. ===== */
enum {
  KF_KEY_ESC=0x100, KF_KEY_ENTER, KF_KEY_BKSP, KF_KEY_TAB, KF_KEY_DEL,
  KF_KEY_UP, KF_KEY_DOWN, KF_KEY_LEFT, KF_KEY_RIGHT,
  KF_KEY_HOME, KF_KEY_END, KF_KEY_PGUP, KF_KEY_PGDN,
  KF_KEY_F1, KF_KEY_F2, KF_KEY_F3, KF_KEY_F4, KF_KEY_F5,
  KF_KEY_F6, KF_KEY_F7, KF_KEY_F8, KF_KEY_F9, KF_KEY_F10
};
enum { KF_MOD_SHIFT=1, KF_MOD_CTRL=2, KF_MOD_ALT=4 };

/* ===================================================================== */
/* Layer 0 — the console floor (never NULL)                              */
/* ===================================================================== */

struct k_sys {
  void (*exit)(int code);
  /* WINDOWED scheduling: register, then return from app_main */
  void (*on_frame)(void (*fn)(void*), void* ud);
  void (*on_key)  (void (*fn)(void*, int key, int down), void* ud);
  void (*on_close)(void (*fn)(void*), void* ud);
  /* EXCLUSIVE scheduling: app owns the loop, pumps the kernel itself ~each frame */
  void (*pump)(void);
  /* performance — TRANSIENT request, kernel-arbitrated, reverted on exit */
  void     (*perf)(enum kf_perf);
  uint32_t (*clock_hz)(void);
  void     (*idle_policy)(enum kf_idle);       /* wakelock (KEEP_CLOCK while audio plays) */
  /* system info — READ ONLY (apps observe, never configure: §7.1) */
  int  (*battery_pct)(void); int (*charging)(void);
  int  (*get_brightness)(void); int (*get_volume)(void);
  uint32_t (*caps)(void);
  uint32_t (*rng)(void);                        /* HW TRNG (cap KF_CAP_RNG)              */
  const char* (*kernel_version)(void);
  /* errors / debug */
  int  (*last_error)(void);
  const char* (*err_str)(int);
  void (*log)(const char*);                     /* -> debug UART                         */
};

struct k_mem {
  void*  (*alloc)(size_t); void* (*realloc)(void*, size_t); void (*free)(void*);
  size_t (*avail)(void);
  /* PSRAM 8 MB block store (cap KF_CAP_PSRAM): handles, not pointers */
  kf_mem (*psram_alloc)(size_t); void (*psram_free)(kf_mem);
  void   (*psram_read)(kf_mem, size_t off, void* dst, size_t n);
  void   (*psram_write)(kf_mem, size_t off, const void* src, size_t n);
  void*  (*lock)(kf_mem); void (*unlock)(kf_mem);   /* page into an SRAM window          */
};

struct k_gfx {
  void (*screen_size)(int* w, int* h);
  /* WINDOWED, zero-copy: a LIVE on-screen canvas the kernel composites (calc's path) */
  kf_canvas (*canvas)(int x, int y, int w, int h);
  void (*canvas_destroy)(kf_canvas);
  void (*present)(kf_canvas);                       /* mark dirty -> composite           */
  void (*clear)(kf_canvas, kf_color);
  void (*pixel)(kf_canvas, int, int, kf_color);
  void (*line)(kf_canvas, int, int, int, int, kf_color);
  void (*rect)(kf_canvas, int, int, int, int, kf_color);
  void (*fill)(kf_canvas, int, int, int, int, kf_color);
  void (*blit)(kf_canvas, int x, int y, int w, int h, const kf_color* px);
  void (*clip)(kf_canvas, int, int, int, int);
  /* WINDOWED TURBO: direct panel SPI into a content sub-rect (beat the compositor) */
  kf_view (*view_open)(int x, int y, int w, int h);
  void (*view_region)(kf_view, int x0, int y0, int x1, int y1);
  void (*view_push)(kf_view, const void* px, size_t nbytes);
  void (*view_flush)(kf_view);
  void (*view_close)(kf_view);
  /* EXCLUSIVE: own the whole panel (parks Core 1, hides topbar) — games/emulators */
  kf_err (*lease)(void); void (*release)(void);
  void (*region)(int x0, int y0, int x1, int y1);
  void (*push)(const void* px, size_t nbytes);
  void (*flush)(void);
  kf_err (*set_panel_hz)(uint32_t hz);
};

struct k_txt {
  kf_font (*open)(const char* name, int px); void (*close)(kf_font);
  int  (*text_w)(kf_font, const char*); int (*line_h)(kf_font);
  void (*draw)(kf_canvas, kf_font, int x, int y, const char*, kf_color);
  int  (*glyph)(kf_font, uint32_t cp, int* w, int* h, const uint8_t** bitmap);
};

struct k_img {  /* cap KF_CAP_IMG */
  kf_img (*decode)(const void* buf, size_t len);
  kf_img (*decode_file)(const char* path);
  void   (*info)(kf_img, int* w, int* h);
  void   (*to_canvas)(kf_img, kf_canvas, int x, int y);
  void   (*free)(kf_img);
  kf_err (*encode_file)(const char* path, const kf_color* px, int w, int h);
};

struct k_in {
  int (*poll)(int* key, int* down, int* mods);   /* 0 if none; key = ASCII or KF_KEY_*   */
  int (*mods)(void);
};

struct k_aud {  /* output ring — cap KF_CAP_AUDIO_OUT (no mic/ADC on this HW).
   * Audio is INTERLEAVED STEREO int16 (L,R per frame). rate 8000..48000 Hz.
   * Ring depth is ~8192 frames (~186 ms @ 44.1 kHz); pace work off out_space(). */
  kf_err (*out_start)(int rate);
  int    (*out_space)(void);                      /* free STEREO FRAMES in the ring        */
  int    (*out_write)(const int16_t* stereo, int nframes);  /* returns frames accepted     */
  int    (*out_running)(void);
  void   (*out_stop)(void);
  void   (*tone)(int hz, int ms);
};

struct k_fs {
  kf_file (*open)(const char* path, const char* mode);
  int  (*read)(kf_file, void*, int); int (*write)(kf_file, const void*, int);
  int  (*seek)(kf_file, long, int);  long (*tell)(kf_file); void (*close)(kf_file);
  kf_dir (*opendir)(const char*);
  int  (*readdir)(kf_dir, char* name, int n, int* is_dir);
  void (*closedir)(kf_dir);
  kf_err (*remove)(const char*); kf_err (*rename)(const char*, const char*);
  kf_err (*mkdir)(const char*);
  const char* (*app_dir)(void);                   /* this app's /apps/<id>/data sandbox    */
  /* flash-scratch: stage a big read-only blob to flash and XIP it (cap KF_CAP_FLASH_SCRATCH) */
  kf_err (*stage)(const char* path, const void** xip_ptr, size_t* size);
  void   (*unstage)(void);
};

struct k_net {  /* cap KF_CAP_WIFI; sockets are non-blocking (KF_AGAIN) */
  kf_sock (*connect)(const char* host, int port, int udp);
  int  (*send)(kf_sock, const void*, int); int (*recv)(kf_sock, void*, int);
  int  (*status)(kf_sock); void (*close)(kf_sock);
  kf_err (*resolve)(const char* host, uint32_t* ip4);
  /* WiFi STATUS — read only. Config (scan/join/forget) is class-0 Settings (§7.1). */
  int  (*online)(void); int (*rssi)(void); uint32_t (*ip)(void);
};

struct k_tls {  /* cap KF_CAP_TLS; BearSSL stays kernel-side; no cert verification */
  kf_tls (*wrap)(kf_sock, const char* sni);
  int  (*handshake)(kf_tls);                      /* KF_AGAIN until done                  */
  int  (*send)(kf_tls, const void*, int); int (*recv)(kf_tls, void*, int);
  void (*close)(kf_tls);
};

struct k_time {
  uint32_t (*millis)(void); uint64_t (*micros)(void);
  void     (*sleep_ms)(uint32_t);                 /* cooperative (yields)                 */
  uint32_t (*now_unix)(void);                      /* SNTP-synced wall clock               */
};

/* k_math — the C math library, forwarded to the kernel's already-linked libm. Lets an app
 * #include <math.h> (via the SDK shim) and call sin()/pow()/etc. without bundling libm into
 * its image. Trivial ops (fabs/fmin/fmax/copysign/isnan/isfinite) stay app-side in the shim;
 * only the polynomial-approximation transcendentals + float<->text live here. (minor 1) */
struct k_math {
  double (*sin)(double);   double (*cos)(double);   double (*tan)(double);
  double (*asin)(double);  double (*acos)(double);  double (*atan)(double);
  double (*atan2)(double, double);
  double (*sinh)(double);  double (*cosh)(double);  double (*tanh)(double);
  double (*asinh)(double); double (*acosh)(double); double (*atanh)(double);
  double (*exp)(double);   double (*expm1)(double);
  double (*log)(double);   double (*log1p)(double); double (*log10)(double); double (*log2)(double);
  double (*pow)(double, double);
  double (*sqrt)(double);  double (*cbrt)(double);  double (*hypot)(double, double);
  double (*fmod)(double, double);
  double (*floor)(double); double (*ceil)(double);  double (*round)(double); double (*trunc)(double);
  double (*lgamma)(double); double (*tgamma)(double);
  double (*erf)(double);   double (*erfc)(double);
  /* float <-> text — the heavy libc bits (newlib's float printf/strtod), kept kernel-side */
  int    (*fmt_double)(char* out, int n, double v, int prec, char fmt);  /* fmt = 'f'|'g'|'e' */
  double (*parse_double)(const char* s, char** end);                     /* strtod            */
};

/* ===================================================================== */
/* Layer 1 — productivity sugar (may be NULL)                            */
/* ===================================================================== */

struct k_ui {
  kui_obj (*screen)(void);                         /* inset below the topbar               */
  kui_obj (*label)(kui_obj, const char*);
  kui_obj (*button)(kui_obj, const char*, void (*)(void*), void*);
  kui_obj (*list)(kui_obj);
  kui_obj (*list_add)(kui_obj, const char*, void (*)(void*), void*);
  kui_obj (*textarea)(kui_obj);
  kui_obj (*checkbox)(kui_obj, const char*);
  kui_obj (*slider)(kui_obj, int min, int max);
  void (*set_text)(kui_obj, const char*); const char* (*get_text)(kui_obj);
  void (*msgbox)(const char* title, const char* msg);
  kf_canvas (*canvas)(kui_obj, int w, int h);      /* bridge to k_gfx for custom drawing   */
  /* appended in minor 2: enough layout/state/event control for first-class apps */
  void (*destroy)(kui_obj);
  void (*set_pos)(kui_obj, int x, int y); void (*set_size)(kui_obj, int w, int h);
  void (*align)(kui_obj, int align, int xoff, int yoff);
  void (*flex)(kui_obj, int flow, int gap); void (*grow)(kui_obj, int amount);
  void (*hidden)(kui_obj, int yes); void (*enabled)(kui_obj, int yes);
  void (*set_value)(kui_obj, int); int (*get_value)(kui_obj);
  void (*focus)(kui_obj);
  void (*on_change)(kui_obj, void (*)(void*), void*);
  void (*set_colors)(kui_obj, kf_color fg, kf_color bg);
  void (*set_font)(kui_obj, int px);
};

enum { KUI_ALIGN_CENTER, KUI_ALIGN_TOP_LEFT, KUI_ALIGN_TOP_MID, KUI_ALIGN_TOP_RIGHT,
       KUI_ALIGN_LEFT_MID, KUI_ALIGN_RIGHT_MID, KUI_ALIGN_BOTTOM_LEFT,
       KUI_ALIGN_BOTTOM_MID, KUI_ALIGN_BOTTOM_RIGHT };
enum { KUI_FLEX_NONE, KUI_FLEX_ROW, KUI_FLEX_COLUMN, KUI_FLEX_ROW_WRAP, KUI_FLEX_COLUMN_WRAP };

struct k_http {
  void* (*get)(const char* url); void* (*post)(const char* url, const void*, int);
  int (*poll)(void* req, void* buf, int n); void (*free)(void* req);
  /* appended in minor 2 */
  void* (*post_headers)(const char* url, const char* headers, const void*, int);
  int (*status)(void* req); const char* (*error)(void* req); const char* (*final_url)(void* req);
};

enum { KDOC_TEXT=0, KDOC_MARKDOWN=1, KDOC_HTML=2 };
struct k_doc { void (*render)(kf_canvas, const char* markup, int fmt); };

/* Kernel-owned hardware/configuration services used by SD-hosted system apps. */
struct k_device {
  const char* (*cfg_get)(const char* key, const char* def);
  int (*cfg_get_int)(const char* key, int def);
  void (*cfg_set)(const char* key, const char* value);
  void (*cfg_set_int)(const char* key, int value);
  kf_err (*set_backlight)(int lcd_0_9, int keyboard_0_3);
  kf_err (*wifi_scan)(void (*found)(void*, const char* ssid, int rssi, int secure), void* ud);
  int (*wifi_scan_active)(void);
  kf_err (*wifi_connect)(const char* ssid, const char* password);
  void (*wifi_forget)(void);
  const char* (*wifi_ssid)(void); int (*wifi_state)(void);
  void (*sfx_play)(const char* id);
  void (*power)(int action); /* 0 shutdown, 1 reboot, 2 BOOTSEL */
};

/* High-level SSH service: keeps Monocypher and the SSH state machine kernel-side. */
struct k_ssh {
  kf_ssh (*connect)(const char* host, int port, const char* user, const char* password,
                    const uint8_t* ed25519_seed_or_null,
                    void (*on_data)(void*, const uint8_t*, int),
                    void (*on_state)(void*, int state, const char* detail),
                    int (*check_hostkey)(void*, const uint8_t pub[32], const char* fingerprint),
                    void* ud);
  int (*poll)(kf_ssh); int (*send)(kf_ssh, const void*, int);
  void (*resize)(kf_ssh, int cols, int rows);
  int (*state)(kf_ssh); const char* (*error)(kf_ssh); void (*close)(kf_ssh);
};

/* ===================================================================== */
/* Root table + entry point                                              */
/* ===================================================================== */

typedef struct kapi {
  uint16_t abi, minor;
  /* Layer 0 (never NULL) */
  const struct k_sys  *sys;
  const struct k_mem  *mem;
  const struct k_gfx  *gfx;
  const struct k_txt  *txt;
  const struct k_img  *img;
  const struct k_in   *in;
  const struct k_aud  *aud;
  const struct k_fs   *fs;
  const struct k_net  *net;
  const struct k_tls  *tls;
  const struct k_time *time;
  /* Layer 1 (may be NULL) */
  const struct k_ui   *ui;
  const struct k_http *http;
  const struct k_doc  *doc;
  /* ---- appended at minor 1 (after doc, so minor-0 offsets are unchanged) ---- */
  const struct k_math *math;   /* C math library (kernel libm) — never NULL on minor>=1 */
  /* ---- appended at minor 2 ---- */
  const struct k_device *device;
  const struct k_ssh *ssh;
} kapi;

/* The app's entry point. Return value is the exit code. */
int app_main(const kapi *k);

/* ===================================================================== */
/* .kx on-disk executable header (also used by the kernel loader)        */
/* ===================================================================== */

#define KX_MAGIC "KX01"
enum { KX_FLAG_EXCLUSIVE=1, KX_FLAG_WANTS_NET=2, KX_FLAG_WANTS_PSRAM=4 };

typedef struct kx_header {
  char     magic[4];        /* "KX01"                                    */
  uint16_t abi_version;     /* must == kernel KAPI_ABI                    */
  uint16_t flags;           /* KX_FLAG_*                                  */
  uint32_t load_base;       /* arena VA the image is linked for           */
  uint32_t entry_offset;    /* app_main offset from load_base             */
  uint32_t image_size;      /* text+rodata+data bytes to copy             */
  uint32_t bss_size;        /* zero-filled after the image                */
  uint32_t stack_size;      /* 0 = run on the kernel stack                */
  uint32_t crc32;           /* over the image bytes                       */
} kx_header;

#ifdef __cplusplus
}
#endif
#endif /* KAPI_H */
