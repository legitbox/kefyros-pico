// port/clock.h — RP2350 clock/overclock control for Kefyros.
// clock_init() raises the core voltage and overclocks clk_sys to KF_SYS_KHZ
// (360 MHz target), with a safe 150 MHz fallback. clock_sys_mhz() reports the
// rate the chip is actually running at.
#ifndef KF_CLOCK_H
#define KF_CLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/vreg.h"

/* Bring the RP2350 up to the overclock target (or the safe fallback).
 * Call once, early in main(), before any peripheral clock setup. */
void clock_init(void);

/* Actual clk_sys frequency in MHz (clock_get_hz(clk_sys)/1000000). */
uint32_t clock_sys_mhz(void);

/* Change clk_sys + core voltage WITHOUT pausing Core1 — for callers (e.g. the
 * Settings Screen Test) that already own the panel and have Core1 parked. Re-derives
 * SD/UART/PSRAM bauds so the keyboard survives the switch; the caller sets the LCD
 * baud itself afterwards. Voltages above 1.30 V are unlocked automatically. Raise the
 * rail BEFORE clocking up / lower it AFTER clocking down (pass `up` accordingly).
 * Returns the new clk_sys in Hz, or 0 if the PLL couldn't hit `khz` (rail untouched
 * on the way up, so the caller can simply stay at the previous rung). */
uint32_t kf_clock_set_bare(uint32_t khz, enum vreg_voltage v, bool up);

#endif /* KF_CLOCK_H */
