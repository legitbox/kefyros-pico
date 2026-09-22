// apps/px3_opl2.c - Authentic Yamaha YM3812 / AdLib OPL2 synthesizer for Planet X3
#include "px3_opl2.h"
#include "nuked_opl2.h"
#include <string.h>
#include <stdint.h>

static opl2_chip s_chip;
static uint32_t  s_sample_rate = 44100;
static uint8_t   s_adlib_addr = 0;

void px3_opl2_reset(void){
	OPL2_Reset(&s_chip, s_sample_rate);
	s_adlib_addr = 0;
}

void px3_opl2_init(uint32_t sample_rate){
	s_sample_rate = sample_rate ? sample_rate : 44100;
	px3_opl2_reset();
}

void px3_opl2_write_addr(uint8_t val){
	s_adlib_addr = val;
}

void px3_opl2_write_data(uint8_t val){
	OPL2_WriteReg(&s_chip, s_adlib_addr, val);
}

void px3_opl2_write_raw(uint8_t reg, uint8_t val){
	OPL2_WriteReg(&s_chip, reg, val);
}

uint8_t px3_opl2_read_status(void){
	uint8_t st = OPL2_ReadStatus(&s_chip);
	/* When timer 1 or 2 is active, report status bits so DOS AdLib detection passes reliably */
	if(s_chip.t1_start) st |= 0xC0;
	if(s_chip.t2_start) st |= 0xA0;
	return st;
}

void px3_opl2_render_stereo(int16_t *stereo_buf, int frames){
	for(int i = 0; i < frames; i++){
		int16_t s = 0;
		OPL2_GenerateResampled(&s_chip, &s);

		/* 2x gain for rich, full presence on PicoCalc PWM amp and headphones */
		int32_t val = (int32_t)s * 2;
		if(val > 32767) val = 32767;
		else if(val < -32768) val = -32768;

		stereo_buf[i * 2 + 0] = (int16_t)val;
		stereo_buf[i * 2 + 1] = (int16_t)val;
	}
}
