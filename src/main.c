// src/main.c — Kefyros PDA entry point (bare-metal RP2350 / Pico 2 W).
//
// Boot order matters:
//   * clock_init() runs first so every later peripheral (SPI, UART, DMA) is
//     configured against the final clk_sys rate.
//   * lv_init() before any LVGL object is created (disp_init makes a display).
//   * disp_init() builds the LVGL display + flush_cb on Core 0; only AFTER it
//     returns do we launch disp_core1_main on Core 1 — Core 1 drains the flush
//     mailbox and must never run before the mailbox/display exist.
//   * kfs_mount() is non-fatal: the PDA still boots without an SD card.
//   * uart_init() here is the kefyros.h keyboard-link initialiser (uart1 +
//     PING), NOT the pico-sdk uart_init — main.c deliberately does not include
//     hardware/uart.h so there is no symbol clash.
//
// Then the superloop pumps the keyboard link and the raw-key app consumers
// (calc/electronics/editor are no-ops unless one of them has grabbed input),
// and finally services LVGL timers. The 2 ms sleep keeps the loop from busy-
// spinning while still hitting LVGL's default ~30 ms refresh comfortably.

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "lvgl/lvgl.h"

#include "kefyros.h"
#include "port/disp.h"
#include "port/clock.h"
#include "ui/theme.h"

int main(void){
	stdio_init_all();          /* debug stdio on uart0 (GP0/GP1) — see CMake */

	clock_init();              /* overclock to 360 MHz (or 150 MHz fallback) */
	kf_fault_init();           /* precise fault traps -> amber screen of death */

	lv_init();                 /* LVGL core — before any lv_* object/display  */
	lv_display_t *disp = disp_init();          /* ILI9488 + LVGL display       */
	kf_theme_init(disp);       /* install the amber CRT theme (flat inputs!)   */
	multicore_launch_core1(disp_core1_main);   /* start the LCD flush pump     */

	kfs_mount();               /* mount SD at "/" — non-fatal if it fails      */
	kbd_init();                /* keyboard / STM32 link (uart1 + PING)         */
	kf_audio_init();           /* PWM audio (GP26/27) + DMA — idle until played */
	kf_psram_init();           /* 8 MB PSRAM (PIO SPI) — probe + self-test     */
	/* cyw43 is brought up lazily by the WiFi app (kf_net_init), not here. The default
	   250 MHz clock is already WiFi-safe (<=~270 MHz), so this is just lazy init — but
	   any app that boosts to 400 MHz must drop back to <=270 before touching the radio. */

	indev_init();              /* LVGL keypad input device                     */
	tick_init();               /* lv_tick from the 64-bit us timer             */

	kf_wallpaper_init();       /* register the PSRAM streaming wallpaper decoder */
	launcher_init();           /* build + show the app launcher (loads config)  */
	topbar_init();             /* persistent OS top bar (mem/clock/battery/wifi) */

	/* Cold boot ran at the rock-solid 250 MHz default; now that everything's up, ramp to
	   360 MHz for a smooth UI. Doing it warm (not at cold boot) avoids the marginal-XIP
	   boot lottery that booting high caused. WiFi apps drop back to 250 as needed. */
	kf_clock_normal();

	kf_sfx_play("boot");       /* startup chime from /kefyros/sfx/boot.wav (silent if absent) */

	for(;;){
		uart_poll();           /* drain keyboard RX, push key events           */
		calc_poll();           /* raw-key app pumps (no-op unless grabbed)     */
		electronics_poll();
		editor_poll();
		music_poll();          /* FLAC decode pump + player keys (no-op idle)  */
		browser_poll();        /* Spineko keys + HTTP pump (no-op unless open) */
		deepseek_poll();       /* DeepSeek chat keys + request pump (no-op idle) */
		cakespark_poll();      /* CakeSpark REPL keys (no-op unless open) */
		morse_poll();          /* Morse keys + TX keyer/audio state machine (no-op idle) */
		sfx_poll();            /* pump an in-flight UI sound effect (no-op idle)         */
		kf_net_poll();         /* pump CYW43 + lwIP + reconnect watchdog       */
		lv_timer_handler();    /* render + dispatch LVGL timers                */
		sleep_ms(2);
	}

	return 0;                  /* unreachable */
}
