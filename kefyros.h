// kefyros.h — Kefyros PDA framework interface (Pico 2 W / RP2350 bare-metal port).
// One board only (PicoCalc). HAL is pico-sdk based: SPI+DMA display (core1 pump),
// uart1 keyboard (STM32 southbridge), pico-vfs POSIX FS over the SD card.
#ifndef KEFYROS_H
#define KEFYROS_H
#include <stdint.h>
#include "lvgl/lvgl.h"
#include "port/board.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ===== display (port/disp.c) ===== */
#define LCD_W 320
#define LCD_H 320
lv_display_t *disp_init(void);     /* lcd_init() + lv_display + flush_cb (core1 DMA pump) */

/* ===== persistent OS top bar (ui/topbar.c) =====
   Always-on bar on lv_layer_top() (shows on EVERY screen). Apps must render in the
   area BELOW it: a content rect of LCD_W x KF_CONTENT_H at y = KF_TOPBAR_H. Use
   kf_inset_top() on an app's root screen to push its content clear of the bar. */
#define KF_TOPBAR_H  24
#define KF_CONTENT_Y KF_TOPBAR_H
#define KF_CONTENT_H (LCD_H - KF_TOPBAR_H)
void topbar_init(void);                  /* build the bar once (after launcher_init) */
void kf_inset_top(lv_obj_t *scr);        /* pad an app screen's content below the bar */
/* RAM-bar bucketing (leak hunting aid): launcher calls open/close around each app
   session; net.c calls tb_net_up() on the radio stack's first bring-up. The top bar
   renders a stacked bar: amber=OS base, green=radio stack, purple=current app,
   gray=unbucketed/leaked heap. */
void tb_app_open(void);
void tb_app_close(void);
void tb_net_up(void);

/* ===== tick (port/tick.c) ===== */
void tick_init(void);              /* lv_tick_set_cb -> time_us_64()/1000 */

/* ===== keyboard / STM32 link (port/kbd_uart.c) ===== */
/* device keycodes (picocalc_BIOS Core/Inc/keyboard.h) */
#define DK_BACKSPACE 0x08
#define DK_TAB       0x09
#define DK_ENTER     0x0A
#define DK_ESC       0xB1
#define DK_UP        0xB5
#define DK_DOWN      0xB6
#define DK_LEFT      0xB4
#define DK_RIGHT     0xB7
#define DK_BREAK     0xD0
#define DK_INSERT    0xD1
#define DK_HOME      0xD2
#define DK_DEL       0xD4
#define DK_END       0xD5
#define DK_PGUP      0xD6
#define DK_PGDN      0xD7
#define DK_CAPS      0xC1
#define DK_POWER     0x91
#define DK_F1        0x81   /* F1..F10 = 0x81..0x90 (F10 = 0x90) */
#define DK_MOD_ALT   0xA1
#define DK_MOD_SHL   0xA2
#define DK_MOD_SHR   0xA3
#define DK_MOD_SYM   0xA4
#define DK_MOD_CTRL  0xA5
/* key event states (protocol) */
#define KS_PRESS   1
#define KS_HOLD    2
#define KS_RELEASE 3
/* modifier bitmask from uart_mods() */
#define MOD_ALT  0x01
#define MOD_SHL  0x02
#define MOD_SHR  0x04
#define MOD_SYM  0x08
#define MOD_CTRL 0x10
#define MOD_SHIFT (MOD_SHL|MOD_SHR)

void     kbd_init(void);                           /* init uart1 + PING (keyboard/STM32 link) */
void     uart_poll(void);                          /* drain RX bytes (non-blocking), push events */
int      uart_pop_key(uint8_t *state, uint8_t *key);
int      uart_mods(void);
uint32_t uart_last_activity(void);
/* register access (request/response, blocking with timeout); #bytes read, <0 on fail */
int  reg_read(uint8_t reg, uint8_t *out, int maxlen);
int  reg_write(uint8_t reg, const uint8_t *data, int len);
/* PicoCalc register ids (UART_PROTOCOL.md) */
#define REG_TYP 0x00
#define REG_VER 0x01
#define REG_BKL 0x05   /* LCD backlight 0-9 */
#define REG_RST 0x08   /* bit6 = Pico reset */
#define REG_BK2 0x0A   /* keyboard backlight */
#define REG_BAT 0x0B   /* battery %, bit7 = charging */
#define REG_OFF 0x0E   /* bit6 = sleep, bit7 = shutdown */
#define REG_RTC_DATE 0x14
#define REG_RTC_TIME 0x15

/* ===== keypad indev (port/indev.c) ===== */
void        indev_init(void);
lv_indev_t *indev_get(void);
uint32_t    map_key(uint8_t devkey);
void        kf_grab_input(int on);   /* raw-key consumer mode (calc/electronics/editor) */

/* ===== release identity ===== */
#define KF_VERSION  "1.0"            /* Kefyros release version (shown in Settings -> About) */

/* ===== storage (port/storage_sd.c, pico-vfs POSIX over FatFs/SD) ===== */
int  kfs_mount(void);                /* mount the SD card at "/"; 0 ok. ensures /kefyros tree */
int  kfs_ready(void);                /* 1 if the card mounted */
void kf_sd_gate(void);               /* boot: block with guidance until the SD card is healthy (ui/firstrun.c) */
/* Apps use standard POSIX (fopen/opendir/...) via pico-vfs; these are convenience helpers. */
#define KF_ROOT     "/kefyros"
#define KF_NOTES    "/kefyros/notes"
#define KF_WALLS    "/kefyros/wallpapers"
#define KF_CONFIG   "/kefyros/config.txt"

/* ===== PSRAM (port/psram.c) — 8 MB ESP-PSRAM64H, PIO SPI block store =====
   Software block device (NOT memory-mapped): use read/write, not direct pointers. */
uint32_t kf_psram_init(void);                         /* probe + self-test; returns bytes (0 = absent) */
uint32_t kf_psram_size(void);                         /* detected size (0 if none) */
uint32_t kf_psram_bus_hz(void);                       /* current QPI SCK in Hz (0 if absent) */
uint32_t kf_psram_brk(void);                          /* allocator high-water; [brk,size) is free */
void     kf_psram_read(uint32_t addr, void *buf, uint32_t n);
void     kf_psram_write(uint32_t addr, const void *buf, uint32_t n);
void    *kf_psram_map(uint32_t addr, uint32_t n);          /* QMI boards: direct pointer; PIO: NULL */
uint32_t kf_psram_alloc(uint32_t n);                  /* bump-allocate a blob; 0xFFFFFFFF if full */
void     kf_psram_reset_alloc(void);
void     kf_psram_free_to(uint32_t addr);             /* LIFO free back to an alloc mark */
void     kf_psram_reclock(void);                      /* re-derive PIO clkdiv after a clk_sys change */

/* ===== dynamic CPU clock + voltage (port/clock.c) =====
   Apps/WiFi bracket their needs with these. boost = full speed; eco = WiFi-safe + low
   power. The switch pauses the Core-1 display pump and re-derives all clk_sys-derived
   peripheral clocks, so it's safe to call from app code (NOT from inside an LVGL flush). */
/* Clock tiers — the whole OS uses these (see port/clock.c). */
void     kf_clock_eco(void);     /* 250 MHz @ 1.20 V / 62.5 MHz SPI - WiFi-safe; brief, wraps radio join */
void     kf_clock_normal(void);  /* 300 MHz @ 1.20 V / 75.0 MHz SPI - UI / apps / audio default in RGB565 */
void     kf_clock_boost(void);   /* 350 MHz @ 1.25 V / 87.5 MHz SPI - turbo: Music decode + calc 3D render  */
void     kf_clock_sleep(void);   /* 150 MHz @ 1.10 V              - idle screen-off low-power     */
void     kf_clock_wake(void);    /* restore the tier active before kf_clock_sleep()             */
uint32_t kf_clock_khz(void);                          /* current clk_sys, kHz */

/* ===== audio (port/audio.c) — PWM DAC on GP26/27, DMA-paced, noise-shaped =====
   Carrier resolution scales with clk_sys (13-bit/~48.8 kHz at the 400 MHz boost);
   sample->duty uses a per-channel 2nd-order noise shaper + TPDF dither. Feed FULL-
   resolution samples (write_s32) to keep 24-bit FLAC intact. */
void kf_audio_init(void);                 /* set up PWM slice 5 + DMA (call once at boot) */
void kf_audio_start(int hz);              /* begin playback at sample rate hz (<=48000) */
int  kf_audio_start_buffered(int hz, int ring_frames); /* power-of-two 1024..4096; 1 on success */
int  kf_audio_start_buffered_external(int hz, int ring_frames, uint32_t *storage); /* caller owns ring storage */
void kf_audio_stop(void);                 /* stop + silence */
void kf_audio_clock_change_begin(void);   /* tristate speaker pins across a clk_sys change (anti-pop) */
void kf_audio_clock_change_end(void);     /* restore speaker pins after the clock settles */
void kf_audio_idle_park(void);            /* float speaker pins for the whole sleep (kills the 150 MHz carrier whine) */
void kf_audio_idle_unpark(void);          /* restore speaker pins on wake */
void kf_audio_flush(void);                /* drop buffered audio + reset shaper (after a seek) */
int  kf_audio_running(void);
int  kf_audio_space(void);                /* free stereo frames in the ring */
int  kf_audio_buffered(void);             /* frames queued but not yet played (latency gauge) */
typedef struct {
    uint32_t pwm_underrun_events;         /* DMA halves containing unexpected silence */
    uint32_t pwm_underrun_frames;
    uint32_t bt_underrun_frames;
    uint32_t dma_irqs;
    uint32_t max_dma_irq_gap_us;
    uint32_t min_buffered_frames;         /* after first producer write */
} kf_audio_stats_t;
void kf_audio_get_stats(kf_audio_stats_t *out); /* snapshot after stopping producer */

/* OS sound effects: play /kefyros/sfx/<name>.wav on a UI event (ui/sfx.c). Silent if the
   file is missing, effects are disabled (deskconf "sfx"), or music owns the speaker. */
void kf_sfx_play(const char *name);
void sfx_poll(void);                       /* pump an in-flight effect; call every main loop */
int  kf_audio_write_s32(const int32_t *stereo, int frames); /* full-scale L,R; frames accepted */
int  kf_audio_write(const int16_t *stereo, int frames);     /* legacy 16-bit L,R; frames accepted */
int  kf_audio_write_mono(const int16_t *mono, int frames);  /* identical PWM/BT channels; one conversion */
void kf_audio_set_bt_route(int enabled); /* OS sink selection; keeps producer API stable */
int  kf_audio_bt_route(void);
int  kf_audio_bt_read(int16_t *stereo, int frames, int out_rate); /* A2DP backend only */

/* ===== WiFi / networking (port/net.c) — CYW43 + lwIP (poll mode) ===== */
typedef enum { KF_NET_OFF=0, KF_NET_CONNECTING, KF_NET_ONLINE, KF_NET_FAILED } kf_net_state_t;
void           kf_net_init(void);        /* cyw43 arch init + STA mode (once, at boot) */
int            kf_net_present(void);      /* 1 if the radio inited OK */
void           kf_net_poll(void);         /* pump cyw43+lwip + reconnect watchdog (superloop) */
void           kf_net_reclock(void);      /* retune the cyw43 gSPI bus for the current clk_sys (clock-change hook) */
kf_net_state_t kf_net_state(void);
const char    *kf_net_state_str(void);    /* "off"/"connecting"/"online"/"failed" */
const char    *kf_net_ip(void);           /* dotted IPv4, "0.0.0.0" until online */
const char    *kf_net_ssid(void);         /* current/last target SSID ("" if none) */
int            kf_net_rssi(void);         /* current RSSI dBm, or 0 when unavailable */
void           kf_net_connect(const char *ssid, const char *pass);  /* async; persists creds */
void           kf_net_forget(void);       /* disconnect + clear saved creds */
void           kf_net_autoconnect(void);  /* scan + join the strongest saved network (call at boot, at eco) */
int            kf_net_autoconnect_active(void); /* 1 while a boot campaign is still scanning/trying */
int            kf_net_has_saved(void);    /* 1 if any network is remembered */
int            kf_net_saved_pass(const char *ssid, char pass[65]); /* 1 + key if ssid is remembered */
/* scan: cb invoked once per (deduped) AP. secured=0 for open networks. */
typedef void (*kf_scan_cb)(const char *ssid, int rssi, int secured);
int            kf_net_scan_start(kf_scan_cb cb);  /* 0 on start, <0 = cyw43/err code */
int            kf_net_scan_active(void);
int            kf_net_dbg_itf(void);              /* DEBUG: cyw43 itf_state bitmask */
int            kf_net_dbg_link(void);             /* DEBUG: (auth_idx<<8)|link_status */
/* throughput benchmark: HTTP GET of a fixed test file, measures KB/s */
int            kf_net_bench_start(void);          /* 0 started; <0 if offline/busy */
int            kf_net_bench_state(void);          /* 0 idle 1 connecting 2 dl 3 done 4 fail */
uint32_t       kf_net_bench_kbps(void);           /* result KB/s (valid at state 3) */
uint32_t       kf_net_bench_bytes(void);          /* bytes received so far */
/* wall-clock time, synced via SNTP when online (EET/EEST locale). */
int            kf_time_synced(void);              /* 1 once SNTP has set the time */
uint32_t       kf_time_unix(void);                /* current UTC epoch, or 0 if unsynced */
struct tm;
int            kf_time_local(struct tm *out);     /* fill broken-down LOCAL time; 0 if unsynced */
void           kf_time_apply_locale(void);        /* re-derive local time after a tz/DST change in Settings */

/* ===== fault handling / panic screen (port/fault.c) ===== */
void kf_fault_init(void);    /* enable precise fault traps (call early in main) */
void kf_panic(const char *title, const char *l1, const char *l2);  /* amber screen of death */

/* ===== system (port/sys.c) ===== */
void kf_reboot(void);        /* watchdog/REG_RST reboot */
void kf_bootsel(void);       /* reset_usb_boot -> BOOTSEL mass-storage */
void kf_poweroff(void);      /* REG_OFF shutdown */

/* ===== app framework (ui/launcher.c) ===== */
void        kf_boot_splash(void);
void        launcher_init(void);
void        launcher_show(void);
lv_group_t *kf_use_group(void);
void        kf_back_to_launcher(void);
void        kf_power_menu(void);
void        kf_wallpaper_init(void);   /* register the PSRAM streaming wallpaper decoder */
void        kf_wallpaper_apply(lv_obj_t *img, const char *src, const char *fit);
void        kf_wallpaper_show_raw(lv_obj_t *img, uint32_t psram_off, int w, int h, const char *fit);
void        kf_psram_clients_invalidate(void); /* detach every long-lived PSRAM allocation */

/* apps kept on the PDA (terminal/web dropped; wifi is a stub tile) */
void app_calc_open(void);
void app_electronics_open(void);
void app_editor_open(void);
void app_editor_open_path(const char *path);
void app_files_open(void);
void app_settings_open(void);
void app_wallpaper_open(void);
void app_wifi_open(void);    /* WiFi manager: status, scan, connect, forget */
void app_bluetooth_open(void); /* manual Bluetooth audio manager */
void app_music_open(void);   /* FLAC player (recursively indexes /kefyros/music) */
void app_spineko_open(void); /* Spineko: HTML-only web browser (HTTP/HTTPS) */
void app_deepseek_open(void);/* DeepSeek chat client (HTTPS LLM chat) */
void app_help_open(void);    /* Help: Markdown docs browser, content from SD /kefyros/help */
int  kapi_run(const char *path); /* KAPI: load + run a class-1 .kx by path (no desktop tile in release) */
void *kapi_idle_scratch(size_t bytes); /* idle .kx arena; built-in caller owns until exit */
void kf_app_idle_policy(int policy); /* 0 normal, 1 keep clock, 2 keep awake; KAPI/launcher arbitration */
void app_term_open(void);    /* Term: SSH-2 terminal client (full-screen VT100) */
void app_gameboy_open(void); /* Game Boy / Game Boy Color emulator */
void app_planetx3_open(void);/* Planet X3 real-time strategy */
void app_mem_open(void);     /* Memory monitor: heap census + per-session leak log */

/* topbar leak-tracker API (see ui/topbar.c) — used by the Memory monitor app */
void tb_mem_snapshot(char *out, int cap);
int  tb_session_count(void);
int  tb_session_get(int i, uint32_t *open_used, uint32_t *close_used);

/* pumped every main-loop tick; no-op unless that app grabs raw keys */
void editor_poll(void);
void calc_poll(void);
void electronics_poll(void);
void music_poll(void);       /* key handling + FLAC decode pump */
void browser_poll(void);     /* Spineko: keys + HTTP redirects/timeouts */
void deepseek_poll(void);    /* DeepSeek chat: grabbed keys + HTTP request pump */
void kapi_poll(void);        /* KAPI: drive the loaded class-1 app (no-op unless one runs) */
void term_poll(void);        /* Term: SSH session pump (no-op; modal loop owns the session) */
void gameboy_poll(void);     /* GB/GBC picker + modal emulation loop */

/* Internal participants in kf_psram_clients_invalidate(). Apps should not call these. */
void browser_psram_invalidate(void);
void deepseek_psram_invalidate(void);
void music_psram_invalidate(void);
void wallpaper_psram_invalidate(void);

#ifdef __cplusplus
}
#endif

#endif
