# KAPI — Kefyros App Programming Interface (spec v0.2)

> The contract a Kefyros app programs against. The kernel (class 0) hands a loaded
> app (class 1) one pointer — `const kapi *k` — and that is the app's entire universe.
> No linked kernel symbols, no globals: every capability is a function pointer in a
> versioned struct.

## 0. Philosophy (what this is, and what it is NOT)

KAPI is a **homebrew-console SDK with a productivity layer on top**, *not* a widget
toolkit. The conformance test is the existing app suite: a Game Boy emulator, a FLAC
player, a TLS browser, and a calculator must **all** be portable to it. They demand
opposite things — the emulator wants the raw panel and an audio ring; Files wants
stock widgets — so KAPI is split into two layers and serves two execution profiles.

- **Layer 0 — the console floor (mechanism, mandatory):** framebuffer + exclusive
  display, streaming audio ring, input, files, sockets+TLS, timing, performance/clock
  control, image codec, memory (SRAM + PSRAM). Everything demanding apps stand on.
- **Layer 1 — productivity sugar (policy, optional):** the `kui` widget toolkit, an
  `http` helper, a `doc` (markdown/HTML) renderer. May be `NULL`.

The GB emulator touches *zero* of Layer 1. Files touches almost nothing *but* Layer 1.
Same API, opposite ends.

**What migrates:** the complete app suite becomes KAPI bundles on SD, **including Calculator,
Settings and WiFi**. Class 0 retains only the kernel/drivers, launcher, recovery UI and stable
services. System apps configure hardware through `k_device`; they do not link drivers or touch
registers directly.

> **Design note:** KAPI invents no capabilities — it draws a stable vtable around
> kernel facilities that already exist (`kf_audio_*`, `disp_pause_core1`,
> `define_region_spi`/`spi_write_fast`/`spi_set_baudrate`, `kf_clock_*`, `imgdec`,
> `kf_net_poll`, `uart_pop_key`, the PSRAM block store). See the conformance matrix (§12).

### 0.1 Parity is the ship gate

Loading from SD is not a migration by itself. A KAPI app replaces its built-in version only
after the same user-visible app passes all of these on hardware:

1. Same screens, controls, key-up/key-down behavior, topbar integration and error reporting.
2. Same render rate and compute performance at the same clock tier (target: no measurable
   regression; Calculator graphs are the first reference workload).
3. Same filesystem, network, TLS, image, PSRAM and audio features; no stub-backed substitute.
4. Same screen-off, `keep_clock`/`keep_awake`, exit/cleanup and low-memory behavior.
5. The `.kx` can be replaced on the SD card without rebuilding or reflashing the kernel.

**Current implementation status (2026-08-04): service-complete enough to begin full ports,
not yet hardware parity-certified.** ABI 1 minor 2 provides a stable 48 KiB arena, managed UI,
HTTP, document rendering, sockets/DNS/TLS, image codecs, app-scoped PSRAM, audio/tone, clipped
canvas/direct-panel views, filesystem, math, system information, WiFi/config/backlight/power
device services, manifest discovery and lifecycle cleanup. Firmware and SDK conformance apps
build. Remaining ship gates are on-device service tests, full-app ports and side-by-side parity;
flash-scratch staging remains capability-disabled and TLS retains the kernel's existing trust
limitations. The small `sdk/examples/calc` REPL remains a toolchain test, not a replacement.

This rule also determines implementation order: close shared service gaps first, port one app
without cutting features, compare it to the built-in, and only then remove the built-in source.

## 1. The two execution profiles

An app declares its profile in the manifest (`"mode": "windowed" | "exclusive"`).

### Windowed (managed) — Files, Settings, Notes, Help, Electronics, Spineko, DeepSeek, Calc
- `app_main()` builds UI, registers `sys->on_frame/on_key/on_close`, then **returns**.
- The **kernel owns the superloop**: keys → app callback → `net_poll` → battery → topbar
  → LVGL render → audio → sleep. The app is one cooperative step; it cannot stall
  background tasks, and the **topbar persists** above the app's inset screen.

### Exclusive (takeover) — Game Boy, games, anything real-time/full-screen
- `app_main()` calls `gfx->lease()` (parks Core 1, hands over the panel) and runs its
  **own tight loop**, which must:
  - drain input itself (`in->poll`),
  - feed the **audio ring** (`aud->out_*`),
  - call **`sys->pump()` once per frame** to keep net/SNTP/battery/sleep alive
    (this is exactly today's `kf_net_poll()`-every-frame in `gameboy.c`),
  - draw straight to the panel (`gfx->region` + `gfx->push`).
- On exit it calls `gfx->release()` (restores Core 1, panel clock, topbar).
- The topbar is **hidden** while leased (the app owns the whole screen).

A windowed app *may* lease temporarily and release; exclusivity is a runtime state,
the manifest flag is just the loader's hint.

## 2. Constitution (rules that keep the ABI from rotting)

1. C ABI, one struct of function pointers, passed to `int app_main(const kapi *k)`.
2. Layer 0 = mechanism/mandatory; Layer 1 = policy/optional (`NULL` if absent).
3. **Opaque handles** for all stateful objects; apps never see kernel structs.
4. One error model: handle calls return `0/NULL` on failure; `int` calls return `0` ok /
   negative `kf_err`; `KF_AGAIN` (-11) means "non-blocking, not ready"; `sys->last_error()`
   for detail.
5. **Nothing blocks the superloop.** All I/O is non-blocking + polled.
6. **Append-only** evolution: new funcs at the END of a sub-struct; never reorder/remove
   within an ABI major. `abi` (major) gates loading; `minor` advertises additive funcs.
7. Caller owns its buffers; KAPI never frees them. Handles freed by explicit `close/destroy`.
8. **Capability-gated optionals.** Hardware-dependent features (mic, PSRAM, WiFi, flash
   scratch) are advertised in `sys->caps()`; apps check before use and degrade gracefully.

## 3. Types

```c
#define KAPI_ABI    1            /* major; app's manifest "abi" must == kernel's      */
typedef uint16_t kf_color;       /* RGB565 (panel-native)                            */
typedef int      kf_err;         /* 0 ok, negative = error, KF_AGAIN = not-ready     */

/* opaque handles */
typedef struct kf_canvas* kf_canvas;
typedef struct kf_font*   kf_font;
typedef struct kf_img*    kf_img;
typedef struct kf_sock*   kf_sock;
typedef struct kf_tls*    kf_tls;
typedef struct kf_file*   kf_file;
typedef struct kf_dir*    kf_dir;
typedef struct kf_view*   kf_view;    /* windowed direct-blit content viewport (turbo)*/
typedef uint32_t          kf_mem;     /* PSRAM block handle (0 = invalid)            */
typedef struct kui_obj*   kui_obj;    /* Layer-1 widget handle                       */

enum { KF_OK=0, KF_ERR=-1, KF_ENOMEM=-2, KF_EIO=-5, KF_ENOENT=-2, KF_EINVAL=-22,
       KF_EUNSUPP=-95, KF_AGAIN=-11 };

/* sys->caps() bitfield */
enum { KF_CAP_PSRAM=1, KF_CAP_WIFI=2, KF_CAP_AUDIO_OUT=4, /* 8 reserved (no audio-in HW) */
       KF_CAP_FLASH_SCRATCH=16, KF_CAP_IMG=32, KF_CAP_RNG=64, KF_CAP_TLS=128 };

enum kf_perf { KF_PERF_ECO, KF_PERF_NORMAL, KF_PERF_BOOST };  /* app-requestable tiers, §7.3   */
/* KF_PERF_SLEEP (150 MHz) is kernel-only — entered on idle, never app-requestable.            */
enum kf_idle { KF_IDLE_NORMAL,        /* default: dim + drop to sleep tier when idle           */
               KF_IDLE_KEEP_CLOCK,    /* allow dim, but DON'T downclock (audio apps)           */
               KF_IDLE_KEEP_AWAKE };  /* no dim, no downclock (games / active playback)        */
```

## 4. Root table

```c
typedef struct kapi {
  uint16_t abi, minor;
  /* ---- Layer 0: the console floor (never NULL) ---- */
  const struct k_sys  *sys;   /* lifecycle, scheduling, perf, sysinfo, rng, errors  */
  const struct k_mem  *mem;   /* SRAM heap + PSRAM handle tier                       */
  const struct k_gfx  *gfx;   /* windowed canvas AND exclusive raw panel            */
  const struct k_txt  *txt;   /* font metrics + glyph/text rasterization            */
  const struct k_img  *img;   /* image decode/encode (cap KF_CAP_IMG)               */
  const struct k_in   *in;    /* keyboard                                           */
  const struct k_aud  *aud;   /* streaming audio ring out (+ in, cap-gated)         */
  const struct k_fs   *fs;    /* SD storage + flash-scratch staging                 */
  const struct k_net  *net;   /* sockets + DNS + WiFi mgmt (cap KF_CAP_WIFI)        */
  const struct k_tls  *tls;   /* TLS socket wrap (cap KF_CAP_TLS)                    */
  const struct k_time *time;  /* ms/us clocks, cooperative sleep, RTC               */
  /* ---- Layer 1: productivity sugar (may be NULL) ---- */
  const struct k_ui   *ui;    /* widget toolkit (amber theme)                       */
  const struct k_http *http;  /* convenience GET/POST over TLS                       */
  const struct k_doc  *doc;   /* markdown/HTML -> canvas                             */
  const struct k_math *math;  /* minor 1: kernel libm                                */
  const struct k_device *device; /* minor 2: mediated system-app services             */
  const struct k_ssh *ssh;    /* minor 2: kernel SSH/crypto service                  */
} kapi;

int app_main(const kapi *k);   /* entry; return value is exit code                  */
```

---

## 5. Layer 0 — the console floor

### k_sys — lifecycle, scheduling, performance, system
```c
struct k_sys {
  /* lifecycle */
  void (*exit)(int code);
  /* WINDOWED scheduling: register, then return from app_main */
  void (*on_frame)(void(*fn)(void*), void* ud);            /* each superloop tick   */
  void (*on_key)  (void(*fn)(void*, int key, int down), void* ud);
  void (*on_close)(void(*fn)(void*), void* ud);            /* teardown before reset */
  /* EXCLUSIVE scheduling: app owns the loop, pumps the kernel itself */
  void (*pump)(void);             /* one kernel housekeeping pass: net/SNTP/battery/sleep
                                     (does NOT touch the leased panel) — call ~each frame */
  /* performance — a TRANSIENT request, NOT system config: the kernel arbitrates/caps it
     for thermal & battery and restores the prior tier on app exit. */
  void     (*perf)(enum kf_perf);  /* hint clock tier (kf_clock_eco/normal/boost)         */
  uint32_t (*clock_hz)(void);
  void     (*idle_policy)(enum kf_idle);  /* override the manifest idle policy at runtime
                                             (a wakelock: e.g. KEEP_CLOCK while audio plays) */
  /* system info — READ-ONLY. Apps observe; they do NOT configure the system. Setting
     brightness, volume, keyboard backlight, and all WiFi config live ONLY in the
     class-0 Settings/WiFi apps (see §7.1 Privilege model). */
  int  (*battery_pct)(void); int (*charging)(void);
  int  (*get_brightness)(void); int (*get_volume)(void);
  uint32_t (*caps)(void);
  uint32_t (*rng)(void);                                   /* HW TRNG (cap KF_CAP_RNG)   */
  const char* (*kernel_version)(void);
  /* errors / debug */
  int  (*last_error)(void);
  const char* (*err_str)(int);
  void (*log)(const char*);                                /* -> debug UART             */
};
```

### k_mem — memory (SRAM heap + PSRAM tier)
```c
struct k_mem {
  void*  (*alloc)(size_t); void* (*realloc)(void*, size_t); void (*free)(void*);
  size_t (*avail)(void);                                   /* free SRAM heap bytes      */
  /* PSRAM 8 MB block store (cap KF_CAP_PSRAM): handles, not pointers */
  kf_mem (*psram_alloc)(size_t); void (*psram_free)(kf_mem);
  void   (*psram_read)(kf_mem, size_t off, void* dst, size_t n);
  void   (*psram_write)(kf_mem, size_t off, const void* src, size_t n);
  void*  (*lock)(kf_mem); void (*unlock)(kf_mem);          /* page into an SRAM window  */
};
```

### k_gfx — display (windowed canvas + exclusive raw panel)
```c
struct k_gfx {
  void (*screen_size)(int* w, int* h);                     /* 320x320 here              */
  /* --- WINDOWED, zero-copy (the calculator's path) --- A LIVE on-screen canvas, inset
     below the topbar. The app draws straight into its buffer with the primitives below;
     the kernel composites it in its normal LVGL flush — there is NO present-copy. Cost is
     identical to drawing into LVGL today => ~0% rendering loss vs the baked-in app. */
  kf_canvas (*canvas)(int x, int y, int w, int h);        /* live, composited             */
  void (*canvas_destroy)(kf_canvas);
  void (*present)(kf_canvas);                             /* mark dirty -> composite      */
  void (*clear)(kf_canvas, kf_color);
  void (*pixel)(kf_canvas, int, int, kf_color);
  void (*line)(kf_canvas, int, int, int, int, kf_color);
  void (*rect)(kf_canvas, int, int, int, int, kf_color);
  void (*fill)(kf_canvas, int, int, int, int, kf_color);
  void (*blit)(kf_canvas, int x, int y, int w, int h, const kf_color* px);
  void (*clip)(kf_canvas, int, int, int, int);
  /* --- WINDOWED TURBO (optional) --- direct panel SPI into a content sub-rect, bypassing
     the compositor for windowed apps that must EXCEED LVGL throughput (a windowed game,
     a fast scope). Topbar persists; the kernel arbitrates the SPI bus so its ~1 Hz refresh
     still lands. Same raw path as exclusive mode, scoped to a rectangle below the topbar. */
  kf_view (*view_open)(int x, int y, int w, int h);
  void (*view_region)(kf_view, int x0, int y0, int x1, int y1);
  void (*view_push)(kf_view, const void* px, size_t nbytes);
  void (*view_flush)(kf_view); void (*view_close)(kf_view);
  /* --- EXCLUSIVE: own the panel (parks Core 1) for max-throughput full-screen --- */
  kf_err (*lease)(void);          /* disp_pause_core1 + take SPI; topbar hidden         */
  void   (*release)(void);        /* restore Core 1, panel clock, topbar                */
  void   (*region)(int x0, int y0, int x1, int y1);        /* define_region_spi         */
  void   (*push)(const void* px, size_t nbytes);           /* spi_write_fast raw pixels */
  void   (*flush)(void);                                   /* spi_finish + raise CS     */
  kf_err (*set_panel_hz)(uint32_t hz);                     /* spi_set_baudrate (<=110M) */
};
```

### k_txt — fonts (lets a custom renderer lay out text)
```c
struct k_txt {
  kf_font (*open)(const char* name, int px); void (*close)(kf_font);
  int  (*text_w)(kf_font, const char*); int (*line_h)(kf_font);
  void (*draw)(kf_canvas, kf_font, int x, int y, const char*, kf_color);
  int  (*glyph)(kf_font, uint32_t cp, int* w, int* h, const uint8_t** bitmap); /* full custom layout */
};
```

### k_img — image codec (cap KF_CAP_IMG)  *(music album art, wallpaper, any image)*
```c
struct k_img {
  kf_img (*decode)(const void* buf, size_t len);           /* PNG/JPEG -> image         */
  kf_img (*decode_file)(const char* path);
  void   (*info)(kf_img, int* w, int* h);
  void   (*to_canvas)(kf_img, kf_canvas, int x, int y);    /* scaled blit               */
  void   (*free)(kf_img);
  kf_err (*encode_file)(const char* path, const kf_color* px, int w, int h); /* RGB565 .bin/JPEG */
};
```

### k_in — input
```c
struct k_in {
  int  (*poll)(int* key, int* down, int* mods);            /* 0 if none; key = kf_key  */
  int  (*mods)(void);
};
```

### k_aud — audio (streaming ring out, + input if present)
```c
struct k_aud {
  /* output ring (cap KF_CAP_AUDIO_OUT) — the model gameboy.c/music.c already use */
  kf_err (*out_start)(int rate);                           /* kf_audio_start           */
  int    (*out_space)(void);                               /* samples free in the ring */
  int    (*out_write)(const int16_t* buf, int nsamp);      /* kf_audio_write           */
  int    (*out_running)(void);
  void   (*out_stop)(void);
  void   (*tone)(int hz, int ms);                          /* convenience beep         */
  /* NOTE: the PicoCalc has NO mic/ADC, so there is no audio input. If a future board ever
     gains one, an aud->in_* group is appended here under a new cap + minor bump (§6, §11). */
};
```

### k_fs — storage (+ flash-scratch staging)
```c
struct k_fs {
  kf_file (*open)(const char* path, int mode);             /* "r/w/a", returns NULL/err */
  int  (*read)(kf_file, void*, int); int (*write)(kf_file, const void*, int);
  int  (*seek)(kf_file, long, int);  long (*tell)(kf_file);  void (*close)(kf_file);
  kf_dir (*opendir)(const char*); int (*readdir)(kf_dir, char* name, int n, int* is_dir);
  void (*closedir)(kf_dir);
  kf_err (*remove)(const char*); kf_err (*rename)(const char*, const char*);
  kf_err (*mkdir)(const char*);
  const char* (*app_dir)(void);                            /* this app's /apps/<id>/data */
  /* flash-scratch: stage a big read-only blob to a reserved flash region and XIP it
     (cap KF_CAP_FLASH_SCRATCH) — this is the gbflash path the GB emulator uses for ROMs */
  kf_err (*stage)(const char* path, const void** xip_ptr, size_t* size);
  void   (*unstage)(void);
};
```

### k_net — sockets + DNS + WiFi management
```c
struct k_net {
  /* sockets (non-blocking) */
  kf_sock (*connect)(const char* host, int port, int udp);  /* does DNS                 */
  int  (*send)(kf_sock, const void*, int); int (*recv)(kf_sock, void*, int); /* KF_AGAIN */
  int  (*status)(kf_sock); void (*close)(kf_sock);
  kf_err (*resolve)(const char* host, uint32_t* ip4);
  /* WiFi STATUS — read-only. Apps consume the connection; they do NOT manage it.
     scan/join/forget are class-0 Settings ONLY (see §7.1). */
  int  (*online)(void); int  (*rssi)(void); uint32_t (*ip)(void);
};
```

### k_tls — TLS socket wrap (cap KF_CAP_TLS; BearSSL stays kernel-side)
```c
struct k_tls {
  kf_tls (*wrap)(kf_sock, const char* sni);
  int  (*handshake)(kf_tls);                               /* KF_AGAIN until done       */
  int  (*send)(kf_tls, const void*, int); int (*recv)(kf_tls, void*, int);
  void (*close)(kf_tls);
};
/* NOTE: no certificate verification (no trusted clock/root store) — same caveat as
   today's port/tls.c. Mechanism only: you get an encrypted byte pipe. */
```

### k_time
```c
struct k_time {
  uint32_t (*millis)(void); uint64_t (*micros)(void);
  void     (*sleep_ms)(uint32_t);                          /* cooperative (yields)      */
  uint32_t (*now_unix)(void);                              /* SNTP-synced wall clock    */
};
```

---

## 6. Layer 1 — productivity sugar (optional; may be NULL)

### k_ui — stock widgets (amber theme, LVGL-backed in class 0)
```c
struct k_ui {
  kui_obj (*screen)(void);                                 /* inset below the topbar    */
  kui_obj (*label)(kui_obj, const char*);
  kui_obj (*button)(kui_obj, const char*, void(*)(void*), void*);
  kui_obj (*list)(kui_obj);  kui_obj (*list_add)(kui_obj, const char*, void(*)(void*), void*);
  kui_obj (*textarea)(kui_obj); kui_obj (*checkbox)(kui_obj, const char*);
  kui_obj (*slider)(kui_obj, int min, int max);
  void (*set_text)(kui_obj, const char*); const char* (*get_text)(kui_obj);
  void (*msgbox)(const char* title, const char* msg);
  /* a kui_obj canvas bridges to k_gfx for custom drawing inside a managed app */
  kf_canvas (*canvas)(kui_obj, int w, int h);
  /* minor 2 append: lifecycle, geometry, layout, state, focus, events and styling */
  void (*destroy)(kui_obj);
  void (*set_pos)(kui_obj,int,int); void (*set_size)(kui_obj,int,int);
  void (*align)(kui_obj,int,int,int); void (*flex)(kui_obj,int,int); void (*grow)(kui_obj,int);
  void (*hidden)(kui_obj,int); void (*enabled)(kui_obj,int);
  void (*set_value)(kui_obj,int); int (*get_value)(kui_obj); void (*focus)(kui_obj);
  void (*on_change)(kui_obj,void(*)(void*),void*);
  void (*set_colors)(kui_obj,kf_color,kf_color); void (*set_font)(kui_obj,int);
};
```

### k_http — convenience over k_tls
```c
struct k_http { void* (*get)(const char* url); void* (*post)(const char* url, const void*, int);
                int (*poll)(void* req, void* buf, int n); void (*free)(void* req);
                void* (*post_headers)(const char*,const char*,const void*,int);
                int (*status)(void*); const char* (*error)(void*); const char* (*final_url)(void*); };
```

### k_doc — markdown/HTML renderer (Spineko uses it; bingus ignores it)
```c
struct k_doc { void (*render)(kf_canvas, const char* markup, int fmt); };
```

### k_device — mediated services for SD-hosted system apps (minor 2)

`k_device` exposes persisted desktop configuration, LCD/keyboard backlights, WiFi
scan/join/forget, system sounds, and shutdown/reboot/BOOTSEL. Drivers and registers remain
kernel-owned; Settings, WiFi and Wallpaper no longer need to be compiled into the firmware.

### k_ssh — kernel SSH/crypto service (minor 2)

Term keeps its VT/UI app-side but uses the kernel's existing SSH-2 and Monocypher implementation
through a high-level nonblocking session handle. This avoids spending most of the app arena on a
second crypto implementation and preserves host-key callbacks, password/key auth, resize and data I/O.

---

## 7. Conventions

- **Memory ownership:** anything `k_mem->alloc` returns is the app's to `free`. Kernel
  services that need a buffer take a pointer the app owns; single address space, no MMU,
  so it's zero-copy (the cost is no isolation — see §10).
- **Handles** are closed/destroyed explicitly; the arena reset on app exit is a backstop,
  not a license to leak.
- **Errors:** check the documented sentinel; pull detail from `sys->last_error()/err_str()`.
- **Capabilities:** never call a cap-gated module without checking `sys->caps()` first.

## 7.1 Hardware ownership and system apps

SD apps never own hardware drivers or raw registers. Ordinary apps consume KAPI services;
Settings/WiFi/Wallpaper-class apps use the mediated `k_device` table for persisted config,
backlights, network management, system sounds and power actions. This keeps those apps
replaceable from SD without making driver ABI or register layout public.

| Apps MAY (through KAPI) | Apps MAY NOT (driver/kernel internals) |
|---|---|
| open sockets, TLS, fetch | direct CYW43/lwIP ownership |
| read battery %, charge, online, RSSI, IP | raw STM32/register access |
| system apps: scan/join/forget through `k_device` | bypass KAPI hardware arbitration |
| system apps: persisted config/backlights/power through `k_device` | link kernel driver symbols |
| **request** a perf tier (`sys->perf`, arbitrated) | set PLL/QMI clocks or voltage directly |
| play audio, draw, read input, read/write their own files | touch another app's files / GPIO / raw registers |

`sys->perf` is the one "control"-looking call, and it's deliberately *not* configuration:
it's a transient, kernel-arbitrated hint (the kernel may cap it for thermal/battery) that
is reverted when the app exits. This is what lets the calculator request `BOOST` for a 3D
plot without being able to dim the screen or drop your WiFi.

## 7.2 Performance — what "~0% loss" means, honestly

A ported app's hot paths cost the same as baked-in, because:
- **Pure compute** (calc bignum/CAS, an emulator's CPU core) makes **zero KAPI calls** in
  its inner loops — it runs native from SRAM, which is as fast as flash XIP or faster.
- **Rendering** goes through a **live composited canvas** (windowed) or **direct SPI**
  (turbo/exclusive) — no extra buffer copy, and draw calls are *coarse* (a whole line/row
  per call), so the one added pointer-indirection is amortized to nothing.

The only real costs are a ~few-ms one-time **load from SD at launch** (not runtime) and a
1–2-cycle vtable indirection on coarse calls — imperceptible. "Smooth as shit" holds.

## 7.3 Performance modes (clock tiers, idle, arbitration)

### The tiers (kernel-defined; `port/clock.c`)
| tier | clock / volt | SPI | role |
|---|---|---|---|
| `KF_PERF_ECO` | 250 MHz @ 1.20 V | 62.5 M | WiFi-safe; brief — wraps radio join |
| `KF_PERF_NORMAL` | 400 MHz @ 1.30 V | 100 M | default for UI / apps |
| `KF_PERF_BOOST` | 420 MHz @ 1.35 V | 105 M | turbo: GB blit, calc 3D, decode |
| *SLEEP* (kernel-only) | 150 MHz @ 1.10 V | — | idle screen-off; never app-requested |

Note the panel-SPI coupling: SPI = clk_sys/2 (even prescaler), dies above ~110 MHz, so
`BOOST` is also what buys the fast blit. Apps don't pick SPI — they pick a tier.

### `sys->perf()` is a *request*, not a setting — the kernel arbitrates against a stack:
1. **Hardware constraints win.** A WiFi *join* needs ≤ ~270 MHz, so a `BOOST` request is
   deferred to ECO until the radio associates (a *joined* link then rides 400 with a bus
   retune). The panel can't exceed ~110 MHz SPI. Low battery / thermal caps the ceiling.
2. **System power profile** (class-0 Settings: *Performance / Balanced / Saver*) sets the
   ceiling. *Saver* caps app requests at `NORMAL` and idles aggressively. Apps **cannot**
   change this (privilege model, §7.1) — they live under it.
3. **The app's request** is honored within 1–2. On exit the kernel restores the prior tier.

So the calculator can ask for `BOOST` to render a 3D plot and the kernel grants it — unless
WiFi is mid-join or you're in Saver, in which case it's capped and the app still works,
just slower. The app never has to know why.

### Idle policy — the thing that broke the music player
The kernel dims the backlight and drops to the SLEEP tier after inactivity. SLEEP's 150 MHz
collapses the PWM-audio carrier into the audible band, so an app playing sound must opt out.
Two controls, manifest default + runtime override:

- **Manifest:** `"idle": "normal" | "keep_clock" | "keep_awake"` (default `normal`).
- **Runtime:** `sys->idle_policy(KF_IDLE_*)` — a wakelock the app raises while busy and drops
  when not (e.g. `KEEP_CLOCK` during playback, back to `NORMAL` when paused).

`keep_clock` (audio apps: music, Morse sidetone) allows the screen to dim but holds the
clock up. `keep_awake` (games, active video) holds both. This generalizes the old
`APP_NO_SLEEP` tag into a first-class, per-app policy.

### Manifest perf hint
`"perf": "eco" | "normal" | "boost"` sets the tier the loader requests at launch (still
arbitrated). A reader app ships `eco` for battery; the GB emulator ships `boost`.

## 8. Capability flags

| flag | guards | absent ⇒ |
|---|---|---|
| `KF_CAP_PSRAM` | `mem->psram_*`, `mem->lock` | use SRAM only |
| `KF_CAP_WIFI` | `net->*` | offline app |
| `KF_CAP_TLS` | `tls->*`, `http` | http:// only / no net |
| `KF_CAP_AUDIO_OUT` | `aud->out_*`, `tone` | silent |
| `KF_CAP_IMG` | `img->*` | no image display |
| `KF_CAP_FLASH_SCRATCH` | `fs->stage` | big blobs via PSRAM paging instead |
| `KF_CAP_RNG` | `sys->rng` | seed from `time->micros` |

## 9. Anti-scope (deliberately absent)

No threads (cooperative single task), no raw register/GPIO access (privileged kernel
only), no launching other apps, no arbitrary LVGL passthrough. Each omission keeps the
surface small and the ABI stable.

## 10. Isolation

Single address space, no MMU paging. v1 ships **no isolation** (a wild app pointer can
corrupt the kernel). Optional hardening: the Cortex-M33 **MPU** marks kernel regions
no-access while an app runs, so a stray write faults into `port/fault.c`, which resets
the arena and returns to the launcher instead of bricking. Recommended but not v1-blocking.

## 11. Versioning

`abi` (major) is the hard gate: the manifest's `abi` must equal the kernel's `KAPI_ABI`
or the loader refuses the app. `minor` only grows (new funcs appended); an app built
against minor *N* runs on kernel minor ≥ *N*. Breaking changes bump `abi`.

## 12. Conformance matrix — the existing app suite must map cleanly

| App | Profile | KAPI surface used |
|---|---|---|
| Files / Settings / Notes / Help / Electronics | windowed | `ui`, `fs`, `in`, `sys` |
| Calculator | windowed + live canvas + perf | `ui` (keypad/chrome), `gfx->canvas` (live, 0%-loss graphs), `txt`, `sys->perf(BOOST)` for 3D, native compute |
| Spineko | windowed + net | `net`, `tls`, `http`+`doc` **or** `gfx`+`txt` (custom) |
| DeepSeek | windowed + net | `net`, `tls`, `http`, `ui` |
| Music (FLAC) | windowed + real-time audio | `ui`, **`aud->out_*`**, `fs` (streaming), **`img`** (album art), app-side FLAC decode |
| Morse | windowed + tone | `aud->tone`/`out_*` (TX sidetone), `time` (timing), `in` (RX = keyed on the keyboard — no mic on this HW) |
| Game Boy / games | **exclusive** | **`gfx->lease/region/push/set_panel_hz`**, **`aud->out_*`**, `in`, `sys->pump`+`perf`, `mem->psram`/`fs->stage` (ROM), `time->micros` |
| Wallpaper | windowed + image | **`img->decode/encode_file`**, `gfx`, `fs` |

Every app maps. The pieces that were missing in spec v0.1 and are now in Layer 0:
**exclusive display (`gfx->lease/region/push`), the audio ring (`aud->out_*`), the image
codec (`k_img`), perf/clock (`sys->perf`), the own-loop pump (`sys->pump`), audio input
(`aud->in_*`, cap-gated), WiFi mgmt + sysinfo.**

## 13. Open decisions

1. **FLAC/codec libs:** ship as Layer-1 kernel services or as static SDK libs the app
   bundles? (size/sharing vs version freedom.)
2. **Apps above 48 KiB:** split optional codecs into kernel services or add a safe overlay/XIP
   format without turning ordinary app replacement into firmware reflashing.

**Resolved:** stable app arena → 48 KiB at `0x20074000` (96/64 KiB arenas starved the built-ins'
kernel heap — missing launcher icons, Spineko "..." low-memory page, empty Music library, calc 3D
OOM; 48 KiB keeps every shipped .kx with 2x headroom); windowed perf → live composited
canvas + optional direct-SPI turbo, ~0% loss (§7.2, §gfx); hardware ownership → mediated
KAPI device services rather than app-linked drivers (§7.1);
performance modes → arbitrated tier requests + per-app idle policy (§7.3); audio input →
none (no mic/ADC on this hardware), Morse RX keys off the keyboard.
