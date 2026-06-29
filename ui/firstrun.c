// ui/firstrun.c — boot-time SD card health gate + first-launch guidance.
//
// Kefyros lives on the SD card: every app icon, the boot/UI sound effects, the Help docs and
// the wallpaper all stream off it. Without a properly serviced card the OS is an empty grid, so
// at boot we verify the card BEFORE building the launcher and, if it's missing/blank/incomplete,
// show a full-screen amber warning telling the user exactly what to do. The screen re-checks the
// card every ~0.5 s, so inserting a good card lets boot continue without a power-cycle.
//
// Three states (kfs_mount + a manifest of everything the service pack installs):
//   NO_CARD     - no card, wrong format, or unreadable  -> "insert a FAT32 SD card"
//   EMPTY       - card mounts but none of our assets exist -> "unzip the service pack onto it"
//   INCOMPLETE  - some assets present, some missing       -> "re-service the card"
//
// The gate is no longer a dead end: it offers a menu (Up/Down to move, Enter to pick) so the
// user is never stuck if they don't have a serviced card handy —
//   CONTINUE  - boot into an SD-less environment anyway (icons/SFX/Help just won't load)
//   RESTART   - hard-reboot the device (watchdog reset)
//   SHUTDOWN  - power the device off (STM32 southbridge cuts the rail)
//   BOOTSEL   - drop into the UF2 bootloader to re-flash firmware
// Keys here are handled manually (drained straight from the UART queue) rather than via the LVGL
// group/indev, because the indev's ESC->launcher and POWER->power-menu shortcuts target screens
// that don't exist yet this early in boot. Inserting a healthy card still auto-continues.
#include "../kefyros.h"
#include "theme.h"
#include "pico/stdlib.h"
#include <sys/stat.h>
#include <string.h>
#include <stdio.h>

/* Everything kefyros_sd.zip (the SD service pack) installs and the OS cannot regenerate itself.
   The gate stat()s each; a miss means the card needs (re)servicing. KEEP THIS IN SYNC with the
   contents of kefyros_sd.zip and the app list in ui/launcher.c. */
static const char *REQUIRED[] = {
	"/kefyros/icons/calc.png",
	"/kefyros/icons/files.png",
	"/kefyros/icons/wifi.png",
	"/kefyros/icons/settings.png",
	"/kefyros/icons/appearance.png",
	"/kefyros/icons/notes.png",
	"/kefyros/icons/music.png",
	"/kefyros/icons/electronics.png",
	"/kefyros/icons/spineko.png",
	"/kefyros/icons/deepseek.png",
	"/kefyros/icons/help.png",
	"/kefyros/help/00 Welcome/01 About Kefyros.md",
	/* SFX (/kefyros/sfx/*.wav) are intentionally NOT required — they're optional polish and the
	   sound engine plays silence when a file is absent, so a card without them is still healthy. */
};
#define NREQ ((int)(sizeof REQUIRED / sizeof REQUIRED[0]))

enum { SD_OK = 0, SD_NO_CARD, SD_EMPTY, SD_INCOMPLETE };

static int exists(const char *p){ struct stat st; return stat(p, &st) == 0; }

/* Health of the card right now. Fills *miss with the count of absent assets and *first with the
   first absent path (for the INCOMPLETE readout). */
static int sd_health(int *miss, const char **first){
	if(!kfs_ready()){ *miss = NREQ; *first = NULL; return SD_NO_CARD; }
	int m = 0; const char *fm = NULL;
	for(int i = 0; i < NREQ; i++) if(!exists(REQUIRED[i])){ if(!fm) fm = REQUIRED[i]; m++; }
	*miss = m; *first = fm;
	if(m == 0)    return SD_OK;
	if(m == NREQ) return SD_EMPTY;        /* nothing of ours present -> blank/fresh card */
	return SD_INCOMPLETE;
}

/* The escape hatches, in menu order. */
enum { OPT_CONTINUE = 0, OPT_RESTART, OPT_SHUTDOWN, OPT_BOOTSEL, NOPT };
static const char *OPT_LABEL[NOPT] = {
	"Continue without SD card",
	"Restart",
	"Shut down",
	"BOOTSEL (re-flash firmware)",
};

void kf_sd_gate(void){
	int miss; const char *first;
	kfs_mount();                                  /* (idempotent) ensure we've tried to mount */
	if(sd_health(&miss, &first) == SD_OK) return; /* healthy -> no gate, boot continues */

	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr, 16, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
	lv_obj_set_style_pad_row(scr, 10, 0);

	lv_obj_t *hdr = lv_label_create(scr);
	lv_obj_set_style_text_font(hdr, KF_FONT_BIG, 0);
	lv_obj_set_style_text_color(hdr, KF_AMBER_BR, 0);
	lv_label_set_text(hdr, "SD CARD");

	lv_obj_t *msg = lv_label_create(scr);
	lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(msg, LCD_W - 36);
	lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(msg, KF_AMBER, 0);

	lv_obj_t *hint = lv_label_create(scr);
	lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(hint, LCD_W - 36);
	lv_obj_set_style_text_align(hint, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(hint, KF_TEXT_MUTED, 0);

	/* The 3-way escape menu. Each row is a label; the selected row is bright amber with a "> "
	   marker, the rest dim. Navigated manually below (no LVGL group this early in boot). */
	lv_obj_t *opt[NOPT];
	for(int i = 0; i < NOPT; i++){
		opt[i] = lv_label_create(scr);
		lv_obj_set_style_text_font(opt[i], KF_FONT, 0);
	}

	lv_obj_t *ver = lv_label_create(scr);
	lv_label_set_long_mode(ver, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(ver, LCD_W - 36);
	lv_obj_set_style_text_align(ver, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(ver, KF_TEXT_DIM, 0);
	lv_label_set_text(ver, "Up/Down to choose - Enter to select\nKefyros " KF_VERSION);

	lv_screen_load(scr);

	int sel = OPT_CONTINUE;
	int drawn = -1;                               /* force first paint of the selection */
	uint64_t last = 0;
	int shown = -1;
	for(;;){
		uart_poll();
		uint8_t st, key;
		while(uart_pop_key(&st, &key)){
			if(st == KS_RELEASE) continue;        /* act on press/repeat only */
			if(key == DK_UP)         sel = (sel + NOPT - 1) % NOPT;
			else if(key == DK_DOWN)  sel = (sel + 1) % NOPT;
			else if(key == DK_ENTER){
				switch(sel){
				case OPT_CONTINUE: lv_obj_delete(scr); return;  /* boot SD-less */
				case OPT_RESTART:  kf_reboot();    break;       /* no return */
				case OPT_SHUTDOWN: kf_poweroff();  break;       /* no return */
				case OPT_BOOTSEL:  kf_bootsel();   break;       /* no return */
				}
			}
		}

		if(sel != drawn){                         /* repaint selection highlight */
			drawn = sel;
			for(int i = 0; i < NOPT; i++){
				lv_label_set_text_fmt(opt[i], "%s%s", i == sel ? "> " : "  ", OPT_LABEL[i]);
				lv_obj_set_style_text_color(opt[i], i == sel ? KF_AMBER_BR : KF_TEXT_DIM, 0);
			}
		}

		uint64_t now = time_us_64();
		if(now - last > 500000ull){               /* re-check ~2x/sec */
			last = now;
			kfs_mount();                          /* re-attempt: recovers when a card is inserted */
			int s = sd_health(&miss, &first);
			if(s == SD_OK) break;                 /* card became healthy -> continue boot */
			if(s != shown){                       /* state changed -> refresh the guidance */
				shown = s;
				if(s == SD_NO_CARD){
					lv_label_set_text(msg, "No SD card found.");
					lv_label_set_text(hint, "Insert a FAT32-formatted SD card to start Kefyros,\n"
					                        "or choose an option below.");
				} else if(s == SD_EMPTY){
					lv_label_set_text(msg, "SD card is not set up.");
					lv_label_set_text(hint, "Unzip kefyros_sd.zip (the SD service pack) onto the\n"
					                        "card so it has /kefyros, then reboot.");
				} else {
					lv_label_set_text_fmt(msg, "SD card is incomplete (%d file%s missing).",
					                      miss, miss == 1 ? "" : "s");
					lv_label_set_text_fmt(hint, "Re-service the card: re-unzip kefyros_sd.zip onto it,\n"
					                            "then reboot.\nfirst missing: %s", first ? first : "");
				}
			}
		}
		lv_timer_handler();
		sleep_ms(10);
	}
	lv_obj_delete(scr);
}
