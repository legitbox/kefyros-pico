// port/disp.h — Kefyros display HAL (ILI9488 320x320 over spi1) with a
// dedicated Core 1 flush pump. Core 0 renders into one of two LVGL partial
// buffers; Core 1 owns the SPI blit so Core 0 never blocks on a flush.
#ifndef KF_DISP_H
#define KF_DISP_H

#include "lvgl/lvgl.h"

/* Bring up the panel + LVGL display and register the Core 1 flush pump.
 * Returns the created lv_display_t (also cached internally for flush_ready). */
lv_display_t *disp_init(void);

/* Core 1 entry point. src/main.c launches this via multicore_launch_core1().
 * It loops forever, draining the single-slot flush mailbox: blit -> flush_ready.
 * Must NOT be called before disp_init(). */
void disp_core1_main(void);

/* Park / release the Core 1 flush pump (used around a clk_sys change so no SPI
 * blit is in flight while clk_peri shifts). disp_pause_core1() blocks until Core 1
 * is idle; pair every pause with a resume. */
void disp_pause_core1(void);
void disp_resume_core1(void);

/* Halt Core 1 entirely for a flash write, then relaunch a fresh flush pump. Used by the
 * Game Boy ROM loader (port/gbflash.c) so neither core executes from XIP mid-erase. */
void disp_core1_reset(void);
void disp_core1_relaunch(void);

#endif /* KF_DISP_H */
