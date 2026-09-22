// port/clock.c — RP2350 / Pico 2 W overclock + clock reporting for Kefyros.
//
// Sequence (per board.h: KF_SYS_KHZ=360000, KF_VREG_MV=1200):
//   1. Raise the core regulator to 1.20 V so the silicon can sustain 360 MHz.
//   2. Let the rail settle (~2 ms) before pushing the PLL.
//   3. set_sys_clock_khz(360000): reconfigures sys PLL + clk_sys. On failure
//      (no exact PLL/divider solution, or rejected) fall back to a rock-solid
//      150 MHz at the default 1.10 V — the RP2350 boots and runs there with no
//      voltage bump.
//
// ---- FLASH / QSPI DIVIDER NOTE (important) ----------------------------------
// On the RP2350 the XIP/QSPI interface (QMI) is clocked from clk_sys through an
// integer divider (QMI M0/M1 CLKDIV). The pico-sdk bootrom/runtime programs a
// conservative default divider sized for the *boot* clock, NOT for 360 MHz. The
// SDK's set_sys_clock_khz() does NOT retune the flash divider — so if clk_sys
// jumps from the ~150 MHz boot rate up to 360 MHz, the effective QSPI rate more
// than doubles and can exceed the flash part's rated read speed, corrupting
// instruction fetches from XIP.
//
// To stay safe we explicitly clamp the QMI clock divider for the window 0 (XIP
// read) timing block right after the overclock succeeds: we pick the smallest
// even divider that keeps the QSPI clock at or below KF_QSPI_MAX_HZ. With
// clk_sys=360 MHz and a /4 divider the flash runs at 90 MHz, comfortably inside
// the 100–133 MHz envelope of the common W25Qxx parts used on Pico-class boards
// while leaving margin for SPI setup/hold at the bus. We also set the matching
// RXDELAY so sampled read data stays aligned at the higher core clock.
//
// If you bring up a board whose flash is characterised for higher rates you can
// relax KF_QSPI_MAX_HZ. If you would rather trust the SDK default and accept the
// risk, delete the qmi_set_flash_div() call below — it is the only place this
// file touches flash timing.
// -----------------------------------------------------------------------------

#include <stdint.h>
#include <stdbool.h>
#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/pll.h"
#include "hardware/vreg.h"
#include "hardware/spi.h"
#include "hardware/uart.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/regs/addressmap.h"

#include "port/board.h"
#include "port/clock.h"
#include "port/lcdspi/lcdspi.h"   /* LCD_SPI_SPEED — keep the reclock in sync with the crank */

/* re-derived after a clk_sys change (defined in psram.c / disp.c) */
extern void kf_psram_reclock(void);
extern void kf_net_reclock(void);                /* retune cyw43 gSPI bus for the new clk_sys */
extern void disp_pause_core1(void);
extern void disp_resume_core1(void);
extern void kf_audio_clock_change_begin(void);   /* tristate speaker pins across the switch (anti-pop) */
extern void kf_audio_clock_change_end(void);

/* Keep the XIP/QSPI read clock at or below this. 90 MHz (clk_sys/4 @ 360 MHz)
 * is safe for the W25Q-class parts these boards ship with. */
#define KF_QSPI_MAX_HZ   90000000u

/* Prepare QMI window 0 for the highest clk_sys we will use. RP2350 supports odd
 * divisors. Crucially, the divider must be increased BEFORE clk_sys is raised;
 * the datasheet also requires a fenced dummy memory access after the write.
 * Run from SRAM with interrupts masked so no XIP code can execute in the timing
 * transition. Preserve boot2's board-qualified RXDELAY instead of replacing it
 * with one hard-coded value for two different flash/bus layouts. */
static void __no_inline_not_in_flash_func(qmi_prepare_flash_div)(uint32_t sys_hz){
	uint32_t div = (sys_hz + KF_QSPI_MAX_HZ - 1) / KF_QSPI_MAX_HZ;   /* ceil */
	if(div < 2) div = 2;
	if(div > 255) div = 255;

	uint32_t irq = save_and_disable_interrupts();
	uint32_t v = qmi_hw->m[0].timing;
	v &= ~QMI_M0_TIMING_CLKDIV_BITS;
	v |= div << QMI_M0_TIMING_CLKDIV_LSB;
	qmi_hw->m[0].timing = v;
	__compiler_memory_barrier();
	/* The uncached alias guarantees a real QMI transaction rather than a cache hit. */
	volatile uint32_t dummy = *(volatile const uint32_t *)XIP_NOCACHE_NOALLOC_BASE;
	(void)dummy;
	__dsb();
	__isb();
	restore_interrupts(irq);
}

static bool kf_set_sys_clock_khz(uint32_t khz){
	if(khz == 350000u){
		/* Pico SDK's check_sys_clock_khz assumes PLL_SYS_REFDIV = 1, which cannot
		 * synthesize 350 MHz within the RP2350 PLL VCO envelope [750, 1600 MHz].
		 * With REFDIV = 2 (6 MHz reference), VCO = 1050 MHz (fbdiv = 175), postdiv1 = 3,
		 * postdiv2 = 1, giving 1050 / 3 = 350.0 MHz exact. */
		clock_configure_undivided(clk_sys,
		                          CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
		                          CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_USB,
		                          USB_CLK_HZ);

		pll_init(pll_sys, 2, 1050000000u, 3, 1);

		clock_configure_undivided(clk_ref,
		                          CLOCKS_CLK_REF_CTRL_SRC_VALUE_XOSC_CLKSRC,
		                          0,
		                          XOSC_HZ);

		clock_configure_undivided(clk_sys,
		                          CLOCKS_CLK_SYS_CTRL_SRC_VALUE_CLKSRC_CLK_SYS_AUX,
		                          CLOCKS_CLK_SYS_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
		                          350000000u);

#if PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK
		clock_configure_undivided(clk_peri,
		                          0,
		                          CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLKSRC_PLL_SYS,
		                          350000000u);
#endif
		return true;
	}
	return set_sys_clock_khz(khz, false);
}

void clock_init(void){
	/* 1) Core rail at 1.10 V — stock voltage for 250 MHz cold boot and 300 MHz nominal.
	 *    kf_clock_boost() raises it (to 1.20 V) when stepping up to 350 MHz. */
	vreg_set_voltage(VREG_VOLTAGE_1_10);
	sleep_ms(2);                         /* let the regulator settle */

	/* 2) Slow XIP for the 350 MHz peak BEFORE changing clk_sys. /4 gives 62.5 MHz
	 *    at cold init, 75 MHz normal, and 87.5 MHz boost (all safely <=90 MHz). */
	qmi_prepare_flash_div(350000000u);

	/* 3) Bring up the 250 MHz default. set_sys_clock_khz(.., false) returns false rather
	 *    than faulting if it can't hit the rate. */
	if(kf_set_sys_clock_khz(KF_SYS_KHZ)){
		/* Divider is already valid for every runtime tier. */
	} else {
		/* 4) Fallback: 150 MHz is the RP2350's happy default.
		 *    Pass required=true so a failure here is a real (rare) hard fault. */
		set_sys_clock_khz(150000, true);
	}
}

uint32_t clock_sys_mhz(void){
	return clock_get_hz(clk_sys) / 1000000u;
}

/* ===== runtime dynamic clock/voltage switching =====
 * With PICO_CLOCK_ADJUST_PERI_CLOCK_WITH_SYS_CLOCK=1 (see CMakeLists), set_sys_clock_khz()
 * re-points clk_peri at clk_sys on every call (the SDK default would instead park clk_peri
 * on the 48 MHz USB PLL, hard-capping every SPI at 24 MHz). So SPI/UART baud (display, SD,
 * keyboard) and the PSRAM PIO bus all need re-deriving afterwards. The
 * QMI flash timing is deliberately left at its boot value — chosen conservative enough
 * (CLKDIV keeps flash <=87.5 MHz even at 350) to stay valid down to 150 MHz, so there is
 * no XIP-mismatch window and this code can run from flash normally.
 *
 * Voltage ordering: raise the rail BEFORE clocking up, lower it AFTER clocking down. */
static uint32_t s_cur_khz = KF_SYS_KHZ;
static uint32_t s_lcd_hz   = LCD_SPI_SPEED;   /* panel SPI target; reclock honours it so a
                                                 single app (calc) can run faster than the UI */

static void reclock_peripherals(void){
	spi_set_baudrate(KF_LCD_SPI, s_lcd_hz);     /* LCD — honour the cranked panel clock  */
	spi_set_baudrate(KF_SD_SPI,  KF_SD_SPI_HZ); /* SD target is board-specific */
	uart_set_baudrate(KF_KBD_UART, KF_KBD_BAUD);
	kf_psram_reclock();                          /* re-derive the active PSRAM bus timing */
	kf_net_reclock();                            /* cyw43 gSPI bus back to ~28 MHz (keeps a live
	                                                link alive across a clk_sys change) */
}

static void clock_apply(uint32_t khz, enum vreg_voltage v, uint32_t lcd_hz, bool up){
	disp_pause_core1();                          /* no SPI blit in flight during the switch */
	kf_audio_clock_change_begin();               /* mute the speaker across the PLL relock (anti-pop) */
	if(v > VREG_VOLTAGE_1_30) vreg_disable_voltage_limit();   /* allow 1.35+ V (RP2350) */
	if(up){ vreg_set_voltage(v); sleep_ms(2); }  /* rail up before clock up */
	if(kf_set_sys_clock_khz(khz)){
		s_lcd_hz = lcd_hz;
		reclock_peripherals();
		s_cur_khz = khz;
		if(!up) vreg_set_voltage(v);             /* rail down only after a good downclock */
	}
	/* if the clock change failed we keep the (higher/safe) voltage — never undervolt */
	kf_audio_clock_change_end();
	disp_resume_core1();
}

/* ===== Clock tiers — the WHOLE OS uses these. =====
     sleep   150 MHz @ 1.10 V / 37.5-75 MHz SPI - idle screen-off; kf_clock_wake() restores prior
     eco     250 MHz @ 1.10 V / 62.5   MHz SPI - WiFi-safe (radio won't bring up/associate >~270 MHz).
                                                 clk_sys/4 = 62.5 MHz. Brief: only wraps cyw43 JOIN.
     normal  300 MHz @ 1.10 V / 75     MHz SPI - the default the UI / apps / audio sit at in RGB565.
                                                 clk_sys/4 = 75.0 MHz (~45.8 FPS wire flush).
                                                 A WiFi link joined at eco RIDES 300 (the cyw43 bus
                                                 divider is retuned on the switch).
     boost   350 MHz @ 1.20 V / 87.5   MHz SPI - the fastest mode (Music decode, the calc's 3D
                                                 render); clk_sys/4 = 87.5 MHz. Callers drop back to normal on exit. */

void kf_clock_eco(void){
	if(s_cur_khz == 250000u) return;   /* == not <=, so kf_clock_wake() can restore eco from 150 */
	/* 250 MHz: under the ~270 MHz WiFi ceiling. Used briefly to (re)join the radio; once joined,
	   the OS ramps to normal and the link rides 300 (kf_net_reclock retunes the cyw43 bus). */
	clock_apply(250000u, VREG_VOLTAGE_1_10, LCD_SPI_SPEED, false);
}
/* The steady-state default the whole UI returns to: 300 MHz / 75 MHz SPI (in RGB565).
   Warm-ramped from the 250 MHz cold boot. */
void kf_clock_normal(void){
	if(s_cur_khz == 300000u) return;
	clock_apply(300000u, VREG_VOLTAGE_1_10, LCD_SPI_SPEED, 300000u > s_cur_khz);
}
/* The fastest mode: 350 MHz @ 1.20 V -> SPI = 350/4 = 87.5 MHz. Brief use only (Music FLAC decode +
   13-bit audio carrier, the calc's 3D render); callers return to normal on exit. */
void kf_clock_boost(void){
	if(s_cur_khz == 350000u) return;
	clock_apply(350000u, VREG_VOLTAGE_1_20, 87500000u, 350000u > s_cur_khz);
}
/* Deep low-power sleep: 150 MHz @ the stock 1.10 V rail (the QMI flash divider stays valid down
   here), panel SPI 37.5 MHz. The launcher's idle timer drops here (and kills both backlights) after
   the screen-off timeout; kf_clock_wake() restores the tier we slept FROM. */
static uint32_t s_pre_sleep_khz = 0;
void kf_clock_sleep(void){
	if(s_cur_khz == 150000u) return;
	s_pre_sleep_khz = s_cur_khz;
	clock_apply(150000u, VREG_VOLTAGE_1_10, LCD_SPI_SPEED, false);
}
void kf_clock_wake(void){
	if(s_cur_khz != 150000u) return;             /* only meaningful when actually asleep */
	uint32_t k = s_pre_sleep_khz ? s_pre_sleep_khz : 300000u;
	if(k >= 350000u)      kf_clock_boost();
	else if(k <= 250000u) kf_clock_eco();
	else                  kf_clock_normal();
}
uint32_t kf_clock_khz(void){ return clock_get_hz(clk_sys) / 1000u; }

/* Bare clk_sys/voltage change that does NOT pause Core1 — the Screen Test already
 * parks Core1 and drives the panel directly, so re-pausing here would let the flush
 * pump resume mid-test. We still re-derive SD/UART/PSRAM (reclock_peripherals) so the
 * keyboard keeps working across the switch; the caller re-baudrates the LCD itself.
 * Voltages above the 1.30 V default cap require unlocking the regulator limit first,
 * else vreg_set_voltage silently clamps to 1.30 V. */
uint32_t kf_clock_set_bare(uint32_t khz, enum vreg_voltage v, bool up){
	if(v > VREG_VOLTAGE_1_30) vreg_disable_voltage_limit();   /* allow 1.35–1.65 V (RP2350) */
	if(up){ vreg_set_voltage(v); sleep_ms(2); }               /* rail up before clock up */
	if(kf_set_sys_clock_khz(khz)){
		reclock_peripherals();                               /* SD/UART/PSRAM follow clk_sys */
		s_cur_khz = khz;
		if(!up) vreg_set_voltage(v);                         /* rail down only after a good downclock */
		return khz * 1000u;
	}
	return 0;                                                /* PLL rejected the rate; rail left as-is */
}
