// port/tick.c — LVGL tick source from the RP2350 64-bit microsecond timer.
#include "pico/stdlib.h"
#include "lvgl/lvgl.h"

static uint32_t now_ms(void){
	return (uint32_t)(time_us_64() / 1000ULL);
}

void tick_init(void){
	lv_tick_set_cb(now_ms);
}
