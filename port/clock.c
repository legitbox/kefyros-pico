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
#include "hardware/vreg.h"
#include "hardware/spi.h"
#include "hardware/uart.h"

#include "port/board.h"
#include "port/clock.h"
#include "port/lcdspi/lcdspi.h"   /* LCD_SPI_SPEED — keep the reclock in sync with the crank */

/* re-derived after a clk_sys change (defined in psram.c / disp.c) */
extern void kf_psram_reclock(void);
extern void disp_pause_core1(void);
extern void disp_resume_core1(void);

/* Keep the XIP/QSPI read clock at or below this. 90 MHz (clk_sys/4 @ 360 MHz)
 * is safe for the W25Q-class parts these boards ship with. */
#define KF_QSPI_MAX_HZ   90000000u

/* QMI window-0 read-timing register, raw access (XIP_QMI_BASE=0x400d0000,
 * QMI_M0_TIMING at +0x0c). Done raw so this TU doesn't need the RP2350 struct
 * header on its include path. CLKDIV=bits[7:0], RXDELAY=bits[10:8]. */
#define KF_QMI_M0_TIMING    (*(volatile uint32_t *)(0x400d0000u + 0x0cu))
#define KF_QMI_CLKDIV_BITS  0x000000ffu
#define KF_QMI_RXDELAY_BITS 0x00000700u

/* Clamp the QMI window-0 (XIP read) clock divider so the flash never runs
 * faster than KF_QSPI_MAX_HZ for the current clk_sys. The divider is an even
 * integer in [2..N]; div==1 (full speed) is avoided on purpose. */
static void qmi_set_flash_div(uint32_t sys_hz){
	uint32_t div = (sys_hz + KF_QSPI_MAX_HZ - 1) / KF_QSPI_MAX_HZ;   /* ceil */
	if(div < 2) div = 2;
	if(div & 1u) div++;                 /* QMI CLKDIV wants an even value */
	if(div > 255) div = 255;

	/* RXDELAY: at higher clk_sys the round-trip sample point shifts; one extra
	 * delay stage keeps read data aligned. Conservative and divider-agnostic. */
	uint32_t rxdelay = 1;

	/* read-modify-write the CLKDIV + RXDELAY fields. We only ever SLOW flash down
	 * here, so the next XIP fetch under the new (valid) timing is safe. */
	uint32_t v = KF_QMI_M0_TIMING;
	v &= ~(KF_QMI_CLKDIV_BITS | KF_QMI_RXDELAY_BITS);
	v |= (div & 0xffu) | ((rxdelay & 0x7u) << 8);
	KF_QMI_M0_TIMING = v;
}

void clock_init(void){
	/* 1) Core rail at 1.20 V — enough for the 250 MHz cold-boot clock. kf_clock_normal()
	 *    and kf_clock_boost() raise it (to 1.30 / 1.35 V) when stepping up, so we don't
	 *    hold a high rail at idle. */
	vreg_set_voltage(VREG_VOLTAGE_1_20);
	sleep_ms(2);                         /* let the regulator settle */

	/* 2) Bring up the 250 MHz default. set_sys_clock_khz(.., false) returns false rather
	 *    than faulting if it can't hit the rate. */
	if(set_sys_clock_khz(KF_SYS_KHZ, false)){
		/* Size the QMI flash divider for the PEAK clock we'll ever switch to (420 MHz via
		 * kf_clock_boost) — clock_apply() deliberately never retunes flash timing, so the
		 * boot divider must stay valid at every tier. /6 -> 70 MHz @420, 66 @400, 41 @250:
		 * all under the 90 MHz cap. (420 and 400 both round to the same /6 divider.) */
		qmi_set_flash_div(420000000u);
	} else {
		/* 3) Fallback: 150 MHz is the RP2350's happy default — boots with the
		 *    stock 1.10 V rail and stock flash divider, so no QMI retune needed.
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
 * (CLKDIV keeps flash <=66 MHz even at 400) to stay valid down to 150 MHz, so there is
 * no XIP-mismatch window and this code can run from flash normally.
 *
 * Voltage ordering: raise the rail BEFORE clocking up, lower it AFTER clocking down. */
static uint32_t s_cur_khz = KF_SYS_KHZ;
static uint32_t s_lcd_hz   = LCD_SPI_SPEED;   /* panel SPI target; reclock honours it so a
                                                 single app (calc) can run faster than the UI */

static void reclock_peripherals(void){
	spi_set_baudrate(KF_LCD_SPI, s_lcd_hz);     /* LCD — honour the cranked panel clock  */
	spi_set_baudrate(KF_SD_SPI,  24000000u);    /* SD   (CONF_SD_TRX_FREQUENCY)*/
	uart_set_baudrate(KF_KBD_UART, KF_KBD_BAUD);
	kf_psram_reclock();                          /* PSRAM PIO bus back to ~18 MHz */
}

static void clock_apply(uint32_t khz, enum vreg_voltage v, uint32_t lcd_hz, bool up){
	disp_pause_core1();                          /* no SPI blit in flight during the switch */
	if(v > VREG_VOLTAGE_1_30) vreg_disable_voltage_limit();   /* allow 1.35+ V (RP2350) */
	if(up){ vreg_set_voltage(v); sleep_ms(2); }  /* rail up before clock up */
	if(set_sys_clock_khz(khz, false)){
		s_lcd_hz = lcd_hz;
		reclock_peripherals();
		s_cur_khz = khz;
		if(!up) vreg_set_voltage(v);             /* rail down only after a good downclock */
	}
	/* if the clock change failed we keep the (higher/safe) voltage — never undervolt */
	disp_resume_core1();
}

/* ===== Clock tiers — the WHOLE OS uses these. =====
     sleep   150 MHz @ 1.10 V /  75 MHz SPI  - idle screen-off; kf_clock_wake() restores prior
     eco     250 MHz @ 1.20 V / 100 MHz SPI  - WiFi-safe (radio won't associate >~270 MHz)
     normal  400 MHz @ 1.30 V / 100 MHz SPI  - the default the UI / apps / audio sit at
     boost   420 MHz @ 1.35 V / 105 MHz SPI  - max, one rung below the 440 MHz validated
                                               ceiling; callers drop back to normal on exit. */

void kf_clock_eco(void){
	if(s_cur_khz == 250000u) return;   /* == not <=, so kf_clock_wake() can restore eco from 150 */
	/* 250 MHz: under the ~270 MHz WiFi ceiling, faster than stock 150. The dynamic
	   cyw43 bus divider (port/net.c) auto-tunes to ~31 MHz here. (Voltage 1.20 V was
	   ruled out as the cause of the ECDSA-verify failure — it's a software issue.) */
	clock_apply(250000u, VREG_VOLTAGE_1_20, 100000000u, false);
}
/* Steady-state default the whole UI returns to. main() WARM-ramps here from the 250 MHz
   cold-boot clock (cold-boot 400 is marginal). Panel validated clean to 110 MHz SPI. */
void kf_clock_normal(void){
	if(s_cur_khz == 400000u) return;
	clock_apply(400000u, VREG_VOLTAGE_1_30, 100000000u, 400000u > s_cur_khz);
}
/* Max: 420 MHz @ 1.35 V -> SPI = 420/4 = 105 MHz. One rung below this chip's validated
   440 MHz ceiling (480/500 died even at 1.60 V), one voltage notch above normal. Held
   only while an app needs the headroom (the Calculator); the app restores normal on exit. */
void kf_clock_boost(void){
	if(s_cur_khz == 420000u) return;
	clock_apply(420000u, VREG_VOLTAGE_1_35, 105000000u, 420000u > s_cur_khz);
}
/* Deep low-power sleep: 150 MHz @ the stock 1.10 V rail (the QMI flash divider stays valid
   down here), panel SPI dialed to 75 MHz. The launcher's idle timer drops here (and kills
   both backlights) after the screen-off timeout; kf_clock_wake() restores the tier we slept
   FROM. WiFi is NOT babysat — the cyw43 bus divider isn't retuned, so the bus just slows with
   the clock; if a live link survives that, bonus, otherwise the WiFi app reconnects on entry. */
static uint32_t s_pre_sleep_khz = 0;
void kf_clock_sleep(void){
	if(s_cur_khz == 150000u) return;
	s_pre_sleep_khz = s_cur_khz;
	clock_apply(150000u, VREG_VOLTAGE_1_10, 75000000u, false);
}
void kf_clock_wake(void){
	if(s_cur_khz != 150000u) return;             /* only meaningful when actually asleep */
	uint32_t k = s_pre_sleep_khz ? s_pre_sleep_khz : 400000u;
	if(k >= 420000u)      kf_clock_boost();
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
	if(set_sys_clock_khz(khz, false)){
		reclock_peripherals();                               /* SD/UART/PSRAM follow clk_sys */
		s_cur_khz = khz;
		if(!up) vreg_set_voltage(v);                         /* rail down only after a good downclock */
		return khz * 1000u;
	}
	return 0;                                                /* PLL rejected the rate; rail left as-is */
}
