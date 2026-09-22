// apps/px3_cpu.h - 8086 CPU execution core interface for Planet X3
#ifndef PX3_CPU_H
#define PX3_CPU_H

#include <stdint.h>

#define regax 0
#define regcx 1
#define regdx 2
#define regbx 3
#define regsp 4
#define regbp 5
#define regsi 6
#define regdi 7

#define reges 0
#define regcs 1
#define regss 2
#define regds 3

#define regal 0
#define regah 1
#define regcl 2
#define regch 3
#define regdl 4
#define regdh 5
#define regbl 6
#define regbh 7

union _bytewordregs_ {
	uint16_t wordregs[8];
	uint8_t  byteregs[8];
};

extern union _bytewordregs_ regs;
extern uint16_t segregs[4];
extern uint16_t ip;
extern uint8_t  cf, pf, af, zf, sf, tf, ifl, df, of;
extern uint8_t *g_px3_ram;

void     px3_cpu_init(uint8_t *ram_ptr);
void     px3_cpu_reset(uint16_t cs_val, uint16_t ip_val, uint16_t ss_val, uint16_t sp_val);
void     px3_cpu_exec(uint32_t cycles);
void     px3_cpu_interrupt(uint8_t intnum);
uint16_t px3_cpu_makeflagsword(void);
void     px3_cpu_decodeflagsword(uint16_t x);

// Host hooks implemented in apps/planetx3.c
void     px3_port_out(uint16_t port, uint8_t val);
uint8_t  px3_port_in(uint16_t port);
int      px3_handle_interrupt(uint8_t intnum);

#endif
