// port/disp.c — ILI9488 320x320 display HAL with a dedicated Core 1 flush pump.
//
// Design (per kefyros.h contract):
//   - disp_init() runs on Core 0: lcd_init() brings up the panel, then an
//     lv_display is created with TWO static partial buffers (each LCD_W*40
//     RGB565 pixels) in LV_DISPLAY_RENDER_MODE_PARTIAL.
//   - The flush_cb (Core 0) does NOT touch SPI. It hands {x1,y1,x2,y2,px_map}
//     to Core 1 through a single-slot mailbox and returns *without* calling
//     lv_display_flush_ready().
//   - disp_core1_main() (Core 1) spins on the mailbox; on a pending job it
//     calls draw_buffer_spi() (blocking SPI blit, RGB565->panel) and only then
//     lv_display_flush_ready(g_disp), clearing the slot.
//
// Race correctness:
//   LVGL never calls flush_cb again until flush_ready fires for the previous
//   flush. flush_ready is issued only by Core 1 *after* the blit completes, so
//   the single mailbox slot is never overwritten while Core 1 is mid-draw. The
//   two-buffer scheme lets Core 0 render the next area into the *other* buffer
//   while Core 1 transmits the current one — that is the parallelism win.
//   Exactly one flush_ready is issued per flush_cb.

#include <stdbool.h>
#include <stdint.h>

#include "pico/multicore.h"
#include "pico/stdlib.h"
#include "hardware/sync.h"   /* __wfe / __sev / __dmb barrier + event intrinsics */

#include "lvgl/lvgl.h"
#include "lcdspi/lcdspi.h"

#include "../kefyros.h"   /* LCD_W, LCD_H, disp_init() prototype */
#include "disp.h"

/* Rows per partial buffer. LCD_W * ROWS_PER_BUF * 2 bytes each (RGB565). */
#define DISP_BUF_ROWS   40
#define BYTES_PER_PX    2

/* Two partial render buffers so Core 0 can render into one while Core 1
 * transmits the other. LV_ATTRIBUTE_MEM_ALIGN keeps them DMA-friendly. */
LV_ATTRIBUTE_MEM_ALIGN
static uint8_t s_buf_a[LCD_W * DISP_BUF_ROWS * BYTES_PER_PX];
LV_ATTRIBUTE_MEM_ALIGN
static uint8_t s_buf_b[LCD_W * DISP_BUF_ROWS * BYTES_PER_PX];

/* Cached display handle for flush_ready (used from Core 1). */
static lv_display_t *g_disp = NULL;

/* ---- Single-slot flush mailbox (Core 0 producer, Core 1 consumer) ---- */
typedef struct {
    int      x1, y1, x2, y2;
    uint8_t *px_map;
} flush_job_t;

static volatile flush_job_t s_job;
static volatile bool        s_job_pending = false;

/* ---- Core 1 pause handshake (for runtime clock switching) ----
 * Changing clk_sys/clk_peri while Core 1 is mid-SPI-blit would corrupt the panel
 * transfer. Core 0 calls disp_pause_core1() to park Core 1 at the top of its loop
 * (never mid-blit), does the switch, then disp_resume_core1(). */
static volatile bool s_pause_req = false;
static volatile bool s_paused    = false;

void disp_pause_core1(void){
	s_pause_req = true;
	__sev();
	while(!s_paused) tight_loop_contents();   /* wait until Core 1 is parked + idle */
}
void disp_resume_core1(void){
	s_pause_req = false;
	__sev();
	while(s_paused) tight_loop_contents();
}

/* ---- Core 1 reset/relaunch (for the Game Boy ROM flasher) ----
 * Writing the flash ROM region requires Core 1 to NOT execute from XIP during the
 * erase/program. Rather than the multicore flash-lockout (which dead-locked on a repeat
 * ROM load), we simply halt Core 1 for the whole write and relaunch a fresh flush pump
 * afterwards. disp_core1_reset() first parks Core 1 so no SPI blit is in flight, then
 * resets it. disp_core1_relaunch() clears the pause/mailbox state and starts it again. */
void disp_core1_reset(void){
	disp_pause_core1();          /* finish any in-flight blit, park Core 1 */
	multicore_reset_core1();     /* halt it: no XIP fetches during the flash write */
}
void disp_core1_relaunch(void){
	s_pause_req   = false;       /* fresh pump: not paused, no stale job */
	s_paused      = false;
	s_job_pending = false;
	multicore_launch_core1(disp_core1_main);
}

/* Core 0 flush callback: stage the job for Core 1 and return immediately.
 * Does NOT call lv_display_flush_ready() — Core 1 does that after the blit. */
static void disp_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    (void)disp;

    /* Publish the job, then raise the pending flag last so Core 1 never sees a
     * pending flag with stale coordinates. */
    s_job.x1     = area->x1;
    s_job.y1     = area->y1;
    s_job.x2     = area->x2;
    s_job.y2     = area->y2;
    s_job.px_map = px_map;

    __dmb();                 /* ensure job fields land before the flag */
    s_job_pending = true;
    __sev();                 /* wake Core 1 if it is parked in __wfe() */
}

lv_display_t *disp_init(void)
{
    /* Full ILI9488 bring-up (SPI + panel init). */
    lcd_init();

    g_disp = lv_display_create(LCD_W, LCD_H);
    lv_display_set_flush_cb(g_disp, disp_flush_cb);
    lv_display_set_buffers(g_disp, s_buf_a, s_buf_b,
                           sizeof(s_buf_a),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);

    return g_disp;
}

/* Core 1 entry: drain the flush mailbox forever. */
void disp_core1_main(void)
{
    for (;;) {
        /* Clock-switch pause: ack and hold here (idle, never mid-blit) until released. */
        if (s_pause_req) {
            s_paused = true;  __sev();
            while (s_pause_req) __wfe();
            s_paused = false; __sev();
            continue;
        }
        if (!s_job_pending) {
            __wfe();         /* park until Core 0 signals (or spurious wake) */
            continue;
        }

        __dmb();             /* see coherent job fields after the flag */

        /* Blocking SPI blit of the RGB565 partial buffer to the panel rect. */
        draw_buffer_spi(s_job.x1, s_job.y1, s_job.x2, s_job.y2, s_job.px_map);

        /* Clear the slot, then tell LVGL the buffer is free for reuse. Order:
         * drop pending first so flush_ready can immediately admit the next
         * flush_cb without a window where pending is still set. */
        s_job_pending = false;
        __dmb();
        lv_display_flush_ready(g_disp);
    }
}
