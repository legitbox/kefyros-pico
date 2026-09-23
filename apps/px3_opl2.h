// apps/px3_opl2.h - High quality integer OPL2 (AdLib) synthesizer for Planet X3
#ifndef PX3_OPL2_H
#define PX3_OPL2_H

#include <stdint.h>

void    px3_opl2_init(uint32_t sample_rate);
void    px3_opl2_write_addr(uint8_t val);
void    px3_opl2_write_data(uint8_t val);
void    px3_opl2_write_raw(uint8_t reg, uint8_t val);
uint8_t px3_opl2_read_status(void);
void    px3_opl2_render_mono(int16_t *mono_buf, int frames);
void    px3_opl2_reset(void);

#endif
