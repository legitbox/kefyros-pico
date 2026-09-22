#!/usr/bin/env python3
import sys

with open("/home/legitbox/fake86/src/fake86/cpu.c", "r", encoding="latin-1") as f:
    text = f.read()

# 1. ModRegRM definition
modregrm_code = '''
#define modregrm() { \\
	addrbyte = getmem8(segregs[regcs], ip); \\
	StepIP(1); \\
	mode = addrbyte >> 6; \\
	reg = (addrbyte >> 3) & 7; \\
	rm = addrbyte & 7; \\
	switch(mode) \\
	{ \\
	case 0: \\
		if(rm == 6) { \\
			disp16 = getmem16(segregs[regcs], ip); \\
			StepIP(2); \\
		} \\
		if(((rm == 2) || (rm == 3)) && !segoverride) { \\
			useseg = segregs[regss]; \\
		} \\
		break; \\
	case 1: \\
		disp16 = signext(getmem8(segregs[regcs], ip)); \\
		StepIP(1); \\
		if(((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) { \\
			useseg = segregs[regss]; \\
		} \\
		break; \\
	case 2: \\
		disp16 = getmem16(segregs[regcs], ip); \\
		StepIP(2); \\
		if(((rm == 2) || (rm == 3) || (rm == 6)) && !segoverride) { \\
			useseg = segregs[regss]; \\
		} \\
		break; \\
	default: \\
		disp8 = 0; \\
		disp16 = 0; \\
	} \\
}
'''

# 2. Extract helpers from flag_szp8 to intcall86
start_flag_szp8 = text.find("void flag_szp8")
start_intcall = text.find("void intcall86 (uint8_t intnum) {")

if start_flag_szp8 == -1 or start_intcall == -1:
    print("Error finding flag_szp8 or intcall86")
    sys.exit(1)

middle_helpers = text[start_flag_szp8:start_intcall]

# 3. Extract the switch statement from exec86
start_switch = text.find("switch (opcode) {", start_intcall)
end_switch = text.find("skipexecution:", start_switch)

switch_code = text[start_switch:end_switch].rstrip()

header = f'''// apps/px3_cpu.c - Adapted 8086 CPU core for Planet X3
#include "px3_cpu.h"
#include <string.h>
#include <stdio.h>

uint8_t byteregtable[8] = {{ regal, regcl, regdl, regbl, regah, regch, regdh, regbh }};

static const uint8_t parity[0x100] = {{
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0,
	0, 1, 1, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0, 1, 1, 0, 1, 0, 0, 1, 0, 1, 1, 0, 0, 1, 1, 0, 1, 0, 0, 1
}};

uint8_t *g_px3_ram = 0;
uint8_t opcode, segoverride, reptype, hltstate = 0;
uint16_t segregs[4], savecs, saveip, ip, useseg, oldsp;
uint8_t tempcf, oldcf, cf, pf, af, zf, sf, tf, ifl, df, of, mode, reg, rm;
uint16_t oper1, oper2, res16, disp16, temp16, dummy, stacksize, frametemp;
uint8_t oper1b, oper2b, res8, disp8, temp8, nestlev, addrbyte;
uint32_t temp1, temp2, temp3, temp4, temp5, temp32, tempaddr32, ea;
int32_t result;
uint64_t totalexec = 0;
union _bytewordregs_ regs;

#define StepIP(x) ip += (x)
#define segbase(x) ((uint32_t)(x) << 4)
#define signext(value) (int16_t)(int8_t)(value)
#define signext32(value) (int32_t)(int16_t)(value)
#define getreg16(regid) regs.wordregs[regid]
#define getreg8(regid) regs.byteregs[byteregtable[regid]]
#define putreg16(regid, writeval) regs.wordregs[regid] = (writeval)
#define putreg8(regid, writeval) regs.byteregs[byteregtable[regid]] = (writeval)
#define getsegreg(regid) segregs[regid]
#define putsegreg(regid, writeval) segregs[regid] = (writeval)

#define makeflagsword() \\
	(2 | (uint16_t)cf | ((uint16_t)pf << 2) | ((uint16_t)af << 4) | ((uint16_t)zf << 6) | ((uint16_t)sf << 7) | \\
	((uint16_t)tf << 8) | ((uint16_t)ifl << 9) | ((uint16_t)df << 10) | ((uint16_t)of << 11))

#define decodeflagsword(x) {{ \\
	temp16 = (x); \\
	cf = temp16 & 1; \\
	pf = (temp16 >> 2) & 1; \\
	af = (temp16 >> 4) & 1; \\
	zf = (temp16 >> 6) & 1; \\
	sf = (temp16 >> 7) & 1; \\
	tf = (temp16 >> 8) & 1; \\
	ifl = (temp16 >> 9) & 1; \\
	df = (temp16 >> 10) & 1; \\
	of = (temp16 >> 11) & 1; \\
}}

static inline void write86(uint32_t addr32, uint8_t value) {{
	if(g_px3_ram) {{
		g_px3_ram[addr32 & 0xFFFFF] = value;
	}}
}}

static inline void writew86(uint32_t addr32, uint16_t value) {{
	write86(addr32, (uint8_t)value);
	write86(addr32 + 1, (uint8_t)(value >> 8));
}}

static inline uint8_t read86(uint32_t addr32) {{
	if(g_px3_ram) {{
		return g_px3_ram[addr32 & 0xFFFFF];
	}}
	return 0xFF;
}}

static inline uint16_t readw86(uint32_t addr32) {{
	return (uint16_t)read86(addr32) | ((uint16_t)read86(addr32 + 1) << 8);
}}

#define getmem8(x, y) read86(segbase(x) + (y))
#define getmem16(x, y) readw86(segbase(x) + (y))
#define putmem8(x, y, z) write86(segbase(x) + (y), z)
#define putmem16(x, y, z) writew86(segbase(x) + (y), z)

static inline void portout(uint16_t portnum, uint8_t value) {{
	px3_port_out(portnum, value);
}}

static inline void portout16(uint16_t portnum, uint16_t value) {{
	px3_port_out(portnum, (uint8_t)value);
	px3_port_out(portnum + 1, (uint8_t)(value >> 8));
}}

static inline uint8_t portin(uint16_t portnum) {{
	return px3_port_in(portnum);
}}

static inline uint16_t portin16(uint16_t portnum) {{
	return (uint16_t)px3_port_in(portnum) | ((uint16_t)px3_port_in(portnum + 1) << 8);
}}

{modregrm_code}

void push(uint16_t pushval);
uint16_t pop(void);

void intcall86(uint8_t intnum) {{
	if(px3_handle_interrupt(intnum)) {{
		return;
	}}
	push(makeflagsword());
	push(segregs[regcs]);
	push(ip);
	segregs[regcs] = getmem16(0, (uint16_t)intnum * 4 + 2);
	ip = getmem16(0, (uint16_t)intnum * 4);
	ifl = 0;
	tf = 0;
}}
'''

# Clean up middle helpers
middle_helpers = middle_helpers.replace("void intcall86 (uint8_t intnum);", "")
middle_helpers = middle_helpers.replace("init_intel();", "")
middle_helpers = middle_helpers.replace("timing();", "")

# Clean up verbose print in switch_code
switch_code = switch_code.replace("if (verbose) {", "if (0) {")

exec_func = f'''
void exec86(uint32_t execloops) {{
	uint32_t loopcount;
	uint8_t docontinue;
	static uint16_t firstip;

	for (loopcount = 0; loopcount < execloops; loopcount++) {{
		if (hltstate) return;

		reptype = 0;
		segoverride = 0;
		useseg = segregs[regds];
		docontinue = 0;
		firstip = ip;

		while (!docontinue) {{
			segregs[regcs] = segregs[regcs] & 0xFFFF;
			ip = ip & 0xFFFF;
			savecs = segregs[regcs];
			saveip = ip;
			opcode = getmem8(segregs[regcs], ip);
			StepIP(1);

			{switch_code}
		}}
		totalexec++;
	}}
}}

void px3_cpu_init(uint8_t *ram_ptr) {{
	g_px3_ram = ram_ptr;
}}

void px3_cpu_reset(uint16_t cs_val, uint16_t ip_val, uint16_t ss_val, uint16_t sp_val) {{
	reset86();
	segregs[regcs] = cs_val;
	segregs[regds] = cs_val;
	segregs[reges] = cs_val;
	segregs[regss] = ss_val;
	ip = ip_val;
	regs.wordregs[regsp] = sp_val;
	decodeflagsword(0x7202);
	hltstate = 0;
}}

void px3_cpu_exec(uint32_t cycles) {{
	exec86(cycles);
}}

void px3_cpu_interrupt(uint8_t intnum) {{
	hltstate = 0;
	intcall86(intnum);
}}

uint16_t px3_cpu_makeflagsword(void) {{
	return makeflagsword();
}}

void px3_cpu_decodeflagsword(uint16_t x) {{
	decodeflagsword(x);
}}
'''

full_code = header + middle_helpers + exec_func

with open("/home/legitbox/kefyros-pico/apps/px3_cpu.c", "w", encoding="utf-8") as f:
    f.write(full_code)

print("Regenerated apps/px3_cpu.c successfully!")
