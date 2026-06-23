// ui/boot.c — brief amber CRT boot splash ("MAR13 PDA SYSTEM"). ~1.8s, any key skips.
#include "../kefyros.h"
#include "theme.h"
#include "pico/stdlib.h"

static long now_ms(void){ return (long)(time_us_64()/1000ULL); }

static lv_obj_t *line(lv_obj_t *p, const char *s, lv_color_t c, const lv_font_t *f){
	lv_obj_t *l = lv_label_create(p);
	lv_label_set_text(l, s);
	lv_obj_set_style_text_color(l, c, 0);
	if(f) lv_obj_set_style_text_font(l, f, 0);
	return l;
}

void kf_boot_splash(void){
	lv_obj_t *scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_row(scr, 6, 0);
	lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

	line(scr, "MAR13",  KF_AMBER_BR, KF_FONT_BIG);
	line(scr, "PDA",    KF_AMBER,    NULL);
	line(scr, "SYSTEM", KF_AMBER,    NULL);

	lv_screen_load(scr);

	long start = now_ms();
	for(;;){
		uart_poll();
		lv_timer_handler();
		uint8_t st, key; int skip = 0;
		while(uart_pop_key(&st,&key)) if(st!=KS_RELEASE) skip = 1;
		if(skip) break;
		if(now_ms() - start > 1800) break;
		sleep_ms(5);
	}
}
