// kefyros.h — shared interfaces for the Kefyros LVGL launcher.
#ifndef KEFYROS_H
#define KEFYROS_H
#include <stdint.h>
#include "lvgl/lvgl.h"

/* ---- board hardware map (one header per board; all board-specific pins/paths
   live there). Select with -DBOARD_DUO256M or -DBOARD_MODULE01 (set by the
   Makefile's BOARD= switch). ---- */
#if   defined(BOARD_DUO256M)
#  include "port/board_duo256m.h"
#elif defined(BOARD_MODULE01)
#  include "port/board_module01.h"
#else
#  error "define BOARD_DUO256M or BOARD_MODULE01 (e.g. make BOARD=duo256m)"
#endif

/* ---- display port (port/disp.c) ---- */
#define LCD_W 320
#define LCD_H 320
lv_display_t *disp_init(void);          /* opens KF_SPIDEV, inits ILI9488, creates lv_display */
void disp_release(void);                /* free spidev + DC/RST gpio (hand panel to a child) */
void disp_reacquire(void);              /* re-open spidev + gpio + re-init ILI9488 on return */

/* ---- tick (port/tick.c) ---- */
void tick_init(void);                   /* lv_tick_set_cb -> monotonic ms */

/* ---- UART link to the PicoCalc STM32 (port/uart.c) ---- */
/* device keycodes (from picocalc_BIOS Core/Inc/keyboard.h) */
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
#define DK_F1        0x81   /* F1..F10 = 0x81..0x90 (note F10=0x90) */
#define DK_MOD_ALT   0xA1
#define DK_MOD_SHL   0xA2
#define DK_MOD_SHR   0xA3
#define DK_MOD_SYM   0xA4
#define DK_MOD_CTRL  0xA5
/* key event states (protocol) */
#define KS_PRESS   1
#define KS_HOLD    2
#define KS_RELEASE 3
/* modifier bitmask returned by uart_mods() */
#define MOD_ALT  0x01
#define MOD_SHL  0x02
#define MOD_SHR  0x04
#define MOD_SYM  0x08
#define MOD_CTRL 0x10
#define MOD_SHIFT (MOD_SHL|MOD_SHR)

int  uart_init(const char *dev);                 /* open /dev/ttyS1, returns 0 ok; sends PING */
void uart_poll(void);                            /* drain available bytes (non-blocking) */
int  uart_pop_key(uint8_t *state, uint8_t *key); /* 1 if a key event dequeued, else 0 */
int  uart_mods(void);                            /* current modifier bitmask */
uint32_t uart_last_activity(void);               /* lv_tick of the last key event (for idle dim) */
void uart_release(void);                         /* close /dev/ttyS1 (hand keyboard to a child) */
void uart_reacquire(void);                        /* re-open + re-init /dev/ttyS1 on return */
/* register access (request/response, blocking with timeout). returns #bytes read, <0 on fail */
int  reg_read(uint8_t reg, uint8_t *out, int maxlen);
int  reg_write(uint8_t reg, const uint8_t *data, int len);
/* PicoCalc register ids (subset, from UART_PROTOCOL.md) */
#define REG_TYP 0x00
#define REG_VER 0x01
#define REG_BKL 0x05   /* LCD backlight 0-9 */
#define REG_BK2 0x0A   /* keyboard backlight */
#define REG_BAT 0x0B   /* battery %, bit7=charging */
#define REG_RTC_DATE 0x14
#define REG_RTC_TIME 0x15

/* ---- keypad indev (port/indev.c) ---- */
void indev_init(void);
lv_indev_t *indev_get(void);
uint32_t map_key(uint8_t devkey);   /* device keycode -> LVGL key (0 if unmapped) */
void kf_grab_input(int on);         /* when on, indev yields keys to a raw consumer (terminal) */

/* ---- app framework (ui/launcher.c, ui/app.c) ---- */
typedef struct {
	const char *name;
	const char *icon;          /* an LVGL symbol or short label */
	void (*open)(void);        /* build + load the app screen, set its group active */
} kf_app_t;

void kf_boot_splash(void);         /* brief amber CRT splash, skippable; runs before the home screen */
void launcher_init(void);          /* build + show the home screen */
void launcher_show(void);          /* return to home (used by the global hotkey) */
lv_group_t *kf_use_group(void);    /* create a fresh group, make it the indev's active group */
void kf_back_to_launcher(void);    /* close current app screen -> launcher */
void kf_power_menu(void);          /* power screen: Shutdown / Reboot / Cancel */

/* apps (each provides an open() for the registry) */
void app_terminal_open(void);
void app_calc_open(void);
void app_settings_open(void);
void app_files_open(void);
void app_wifi_open(void);       /* native WiFi manager (wpa_cli backend) */
void app_wallpaper_open(void);  /* wallpaper chooser (fit/crop/fill, decodes files) */
void app_editor_open(void);     /* text editor / notepad over /root/documents */
void app_editor_open_path(const char *path);  /* open an arbitrary file in the editor */
void app_web_open(void);        /* NetSurf web browser (separate full-screen process) */
void app_electronics_open(void); /* electronics toolkit (resistor colour, SMD, Ohm, 555, ...) */

/* Apply a wallpaper to an lv_image: src=="" -> baked default, else an "A:/path"
   file; fit = fill|crop|fit|center|stretch. Used by launcher + chooser preview. */
void kf_wallpaper_apply(lv_obj_t *img, const char *src, const char *fit);

/* pumped every main-loop tick; no-op unless the terminal is active */
void term_poll(void);
/* pumped every main-loop tick; no-op unless the editor is active (grabs raw keys) */
void editor_poll(void);
/* pumped every main-loop tick; no-op unless the calculator is active (grabs raw keys) */
void calc_poll(void);
/* pumped every main-loop tick; no-op unless the electronics app is active (grabs raw keys) */
void electronics_poll(void);
/* open the terminal pre-loaded with a command (e.g. "cakespark foo.cake"); NULL = plain shell */
void app_terminal_run(const char *cmd);

#endif
