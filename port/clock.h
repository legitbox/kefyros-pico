// port/clock.h — RP2350 clock/overclock control for Kefyros.
// clock_init() raises the core voltage and overclocks clk_sys to KF_SYS_KHZ
// (360 MHz target), with a safe 150 MHz fallback. clock_sys_mhz() reports the
// rate the chip is actually running at.
#ifndef KF_CLOCK_H
#define KF_CLOCK_H

#include <stdint.h>

/* Bring the RP2350 up to the overclock target (or the safe fallback).
 * Call once, early in main(), before any peripheral clock setup. */
void clock_init(void);

/* Actual clk_sys frequency in MHz (clock_get_hz(clk_sys)/1000000). */
uint32_t clock_sys_mhz(void);

#endif /* KF_CLOCK_H */
