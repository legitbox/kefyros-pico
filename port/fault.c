// port/fault.c — Kefyros "amber screen of death" + broad error handling.
//
// Installs strong overrides for the Cortex-M33 (RP2350) fault vectors
// (isr_hardfault / isr_memmanage / isr_busfault / isr_usagefault /
// isr_securefault — all .weak in the pico-sdk crt0) plus a software panic
// entry (kf_panic) and hooks for newlib assert()/abort() and LVGL asserts.
//
// When something goes wrong we DO NOT silently reboot. We render a full-screen
// diagnostic DIRECTLY to the panel via lcdspi — no LVGL, no malloc, no IRQs —
// because at fault time the heap may be corrupt and LVGL/Core-1 may be wedged.
// The screen shows the faulting PC (run `arm-none-eabi-addr2line -e kefyros.elf
// <PC>` to pin the exact source line), the decoded fault cause and the register
// frame, then halts so it can actually be read. Power-cycle to restart.  :3
#include <stdint.h>
#include <stddef.h>
#include <malloc.h>

#include "lcdspi/lcdspi.h"
#include "../kefyros.h"

/* 8x8 bitmap font (defined once in ui/font8x8.c). */
extern char font8x8_basic[128][8];

/* ---- palette (0xRRGGBB; lcdspi sends R,G,B which the panel renders 1:1) ---- */
#define C_BG    0x0a0806   /* near-black */
#define C_AMBER 0xd4940a   /* primary */
#define C_HOT   0xffc94d   /* headings */
#define C_RED   0xe03c32   /* fault */
#define C_DIM   0x9a8d7a   /* secondary */
#define C_GREEN 0xb6f000   /* the friendly face */

/* ============================ tiny text renderer ============================ */
/* draw_bitmap_spi() walks bits MSB-first within each row byte; font8x8 is LSB-
   first, so reverse each row byte before handing it over (one define_region +
   streamed pixels per glyph = fast enough for a panic screen). */
static void cputc(int x, int y, unsigned char ch, int scale, int fg, int bg){
	unsigned char t[8];
	const unsigned char *g = (const unsigned char*)font8x8_basic[ch & 0x7f];
	for(int i = 0; i < 8; i++){
		unsigned char v = g[i], r = 0;
		for(int b = 0; b < 8; b++) if((v >> b) & 1) r |= (unsigned char)(1 << (7 - b));
		t[i] = r;
	}
	draw_bitmap_spi(x, y, 8, 8, scale, fg, bg, t);
}
static int cputs(int x, int y, const char *s, int scale, int fg, int bg){
	for(; *s; s++, x += 8*scale) cputc(x, y, (unsigned char)*s, scale, fg, bg);
	return x;
}

/* ---- self-contained number formatting (no snprintf: it may touch malloc) ---- */
static char *hex32(uint32_t v, char *out){   /* "0x........", out >= 11 bytes */
	static const char H[] = "0123456789abcdef";
	out[0] = '0'; out[1] = 'x';
	for(int i = 0; i < 8; i++) out[2+i] = H[(v >> ((7-i)*4)) & 0xf];
	out[10] = 0; return out;
}
static char *u32(uint32_t v, char *out){      /* decimal, out >= 11 bytes */
	char tmp[11]; int n = 0;
	if(!v){ out[0]='0'; out[1]=0; return out; }
	while(v){ tmp[n++] = (char)('0' + v%10); v/=10; }
	for(int i = 0; i < n; i++) out[i] = tmp[n-1-i];
	out[n] = 0; return out;
}

/* ============================ SCB / fault registers ========================= */
#define SCB_CFSR   (*(volatile uint32_t*)0xE000ED28u)
#define SCB_HFSR   (*(volatile uint32_t*)0xE000ED2Cu)
#define SCB_MMFAR  (*(volatile uint32_t*)0xE000ED34u)
#define SCB_BFAR   (*(volatile uint32_t*)0xE000ED38u)
#define SCB_SHCSR  (*(volatile uint32_t*)0xE000ED24u)
#define SCB_CCR    (*(volatile uint32_t*)0xE000ED14u)
#define SCB_AIRCR  (*(volatile uint32_t*)0xE000ED0Cu)

/* spin ~N ms with no SDK calls (≈clk/1000 loops per ms; clk≈360 MHz). */
static void spin_ms(uint32_t ms){
	volatile uint32_t n = ms * 60000u;
	while(n--) __asm volatile("nop");
}

/* RAM bounds for the backtrace scan (RP2350: 520 KB SRAM @ 0x20000000). */
#define RAM_BASE 0x20000000u
#define RAM_TOP  0x20082000u
#define IS_CODE(w) (((w) & 0xff000000u) == 0x10000000u && ((w) & 1u))  /* flash + thumb */

/* ============================ the panic screen ============================= */
/* frame (or NULL for a software panic): r0 r1 r2 r3 r12 lr pc xpsr.
   sp = stack pointer at the fault (for the backtrace scan). */
static void render_panic(const char *kind, const uint32_t *frame, uint32_t sp,
                         const char *msg1, const char *msg2){
	/* give Core 1 a moment to finish any in-flight blit, then take the bus. */
	spin_ms(60);

	char b[12];
	int W = LCD_WIDTH, H = LCD_HEIGHT;
	draw_rect_spi(0, 0, W-1, H-1, C_BG);

	/* big friendly face — this is a crash, not a funeral  :3 */
	cputs(12, 14, ":3", 6, C_GREEN, C_BG);
	cputs(12, 70, "kefyros hit a snag", 2, C_HOT, C_BG);
	cputs(12, 90, "and had to stop.", 2, C_HOT, C_BG);

	int y = 124;
	const int L = 8;                 /* line height at scale 1 */
	cputs(12, y, "fault:", 1, C_DIM, C_BG);
	cputs(64, y, kind, 1, C_RED, C_BG); y += L;

	if(msg1){ cputs(12, y, msg1, 1, C_AMBER, C_BG); y += L; }
	if(msg2){ cputs(12, y, msg2, 1, C_AMBER, C_BG); y += L; }
	y += 4;

	if(frame){
		uint32_t pc = frame[6], lr = frame[5], psr = frame[7];
		/* PC is the headline: addr2line this to find the crash. */
		cputs(12, y, "PC", 1, C_DIM, C_BG); cputs(40, y, hex32(pc, b), 1, C_HOT, C_BG);
		cputs(150, y, "LR", 1, C_DIM, C_BG); cputs(178, y, hex32(lr, b), 1, C_AMBER, C_BG); y += L;
		cputs(12, y, "R0", 1, C_DIM, C_BG); cputs(40, y, hex32(frame[0], b), 1, C_AMBER, C_BG);
		cputs(150, y, "R1", 1, C_DIM, C_BG); cputs(178, y, hex32(frame[1], b), 1, C_AMBER, C_BG); y += L;
		cputs(12, y, "R2", 1, C_DIM, C_BG); cputs(40, y, hex32(frame[2], b), 1, C_AMBER, C_BG);
		cputs(150, y, "R3", 1, C_DIM, C_BG); cputs(178, y, hex32(frame[3], b), 1, C_AMBER, C_BG); y += L;
		cputs(12, y, "12", 1, C_DIM, C_BG); cputs(40, y, hex32(frame[4], b), 1, C_AMBER, C_BG);
		cputs(150, y, "SP", 1, C_DIM, C_BG); cputs(178, y, hex32((uint32_t)(frame+8), b), 1, C_AMBER, C_BG); y += L;
		cputs(12, y, "PSR", 1, C_DIM, C_BG); cputs(40, y, hex32(psr, b), 1, C_AMBER, C_BG); y += L;
		y += 4;

		uint32_t cfsr = SCB_CFSR, hfsr = SCB_HFSR;
		cputs(12, y, "CFSR", 1, C_DIM, C_BG); cputs(56, y, hex32(cfsr, b), 1, C_AMBER, C_BG);
		cputs(150, y, "HFSR", 1, C_DIM, C_BG); cputs(194, y, hex32(hfsr, b), 1, C_AMBER, C_BG); y += L;
		/* decode the common culprits */
		if(cfsr & (1u<<7)){ cputs(12, y, "MMFAR", 1, C_DIM, C_BG); cputs(72, y, hex32(SCB_MMFAR, b), 1, C_RED, C_BG); y += L; }
		if(cfsr & (1u<<15)){ cputs(12, y, "BFAR ", 1, C_DIM, C_BG); cputs(72, y, hex32(SCB_BFAR, b), 1, C_RED, C_BG); y += L; }
		const char *why = 0;
		if(cfsr & (1u<<25)) why = "divide by zero";
		else if(cfsr & (1u<<24)) why = "unaligned access";
		else if(cfsr & (1u<<19)) why = "no coprocessor";
		else if(cfsr & (1u<<18)) why = "invalid PC (bad func ptr/return)";
		else if(cfsr & (1u<<17)) why = "invalid state (bad branch)";
		else if(cfsr & (1u<<16)) why = "undefined instruction";
		else if(cfsr & (1u<<10)) why = "imprecise data bus fault (bad write)";
		else if(cfsr & (1u<<9))  why = "precise data bus fault (bad ptr deref)";
		else if(cfsr & (1u<<8))  why = "instruction bus fault";
		else if(cfsr & (1u<<1))  why = "data access violation";
		else if(cfsr & (1u<<0))  why = "instruction access violation";
		if(why){ cputs(12, y, "cause:", 1, C_DIM, C_BG); cputs(64, y, why, 1, C_HOT, C_BG); y += L; }
	}
	y += 4;

	/* free heap — if this is ~0 the panic is out-of-memory, not a wild pointer. */
	{
		struct mallinfo mi = mallinfo();
		cputs(12, y, "heap free", 1, C_DIM, C_BG); cputs(96, y, u32((uint32_t)mi.fordblks, b), 1, C_HOT, C_BG);
		cputs(200, y, "used", 1, C_DIM, C_BG); cputs(240, y, u32((uint32_t)mi.uordblks, b), 1, C_AMBER, C_BG);
		y += L + 4;
	}

	/* stack backtrace: scan the stack for flash+thumb return addresses. addr2line
	   each to recover the call chain that reached the fault/panic. */
	if(sp >= RAM_BASE && sp < RAM_TOP){
		cputs(12, y, "stack (addr2line these):", 1, C_DIM, C_BG); y += L;
		int shown = 0, col = 12;
		for(uint32_t a = sp; a < RAM_TOP && shown < 12; a += 4){
			uint32_t w = *(volatile uint32_t*)a;
			if(IS_CODE(w)){
				cputs(col, y, hex32(w, b), 1, C_AMBER, C_BG);
				col += 92;
				if(col > 220){ col = 12; y += L; }
				shown++;
			}
		}
		if(col != 12) y += L;
	}

	cputs(12, H-20, "PC + stack pin it; power-cycle.  :3", 1, C_GREEN, C_BG);
}

/* ============================ entry points ================================= */
/* Re-entrancy guard: if we fault again WHILE painting the panic screen (e.g. the
   SPI path itself trips), don't recurse forever — just halt with the partial
   screen up. */
static volatile int in_panic = 0;

/* called from the naked vector trampolines below */
void kf_fault_entry(uint32_t *frame, uint32_t exc_return, uint32_t type){
	__asm volatile("cpsid i");          /* no preemption while we paint */
	if(in_panic) for(;;) __asm volatile("wfi");
	in_panic = 1;
	(void)exc_return;
	const char *kind;
	switch(type){
		case 1: kind = "HARD FAULT";    break;
		case 2: kind = "MEMMANAGE";     break;
		case 3: kind = "BUS FAULT";     break;
		case 4: kind = "USAGE FAULT";   break;
		case 5: kind = "SECURE FAULT";  break;
		default: kind = "FAULT";        break;
	}
	render_panic(kind, frame, (uint32_t)(frame + 8), 0, 0);
	for(;;) __asm volatile("wfi");       /* halt — keep the screen up to be read */
}

/* software panic: corruption detected, assert failed, alloc gave up, etc. */
void kf_panic(const char *title, const char *l1, const char *l2){
	__asm volatile("cpsid i");
	if(in_panic) for(;;) __asm volatile("wfi");
	in_panic = 1;
	uint32_t sp; __asm volatile("mov %0, sp" : "=r"(sp));
	render_panic(title ? title : "PANIC", 0, sp, l1, l2);
	for(;;) __asm volatile("wfi");
}

/* newlib assert() / __assert_fail land here instead of _exit-ing silently. */
void __assert_func(const char *file, int line, const char *func, const char *expr){
	char ln[12]; u32((uint32_t)line, ln);
	(void)file; (void)func;
	kf_panic("ASSERT FAILED", expr, ln);
}
void abort(void){ kf_panic("ABORT", "abort() called", 0); for(;;){} }

/* enable the precise fault traps + div0 trap, and turn on the friendly screen.
   Without this, bus/usage/mem faults escalate to a generic HardFault and we
   lose the specific cause. */
void kf_fault_init(void){
	SCB_SHCSR |= (1u<<16) | (1u<<17) | (1u<<18);  /* MEM/BUS/USGFAULTENA */
	SCB_CCR   |= (1u<<4);                          /* DIV_0_TRP */
	__asm volatile("dsb; isb");
}

/* ============================ vector trampolines =========================== */
/* Each grabs the active stack pointer (MSP/PSP per EXC_RETURN bit 2), the
   EXC_RETURN in r1 and a type code in r2, then tail-calls kf_fault_entry. */
#define KF_FAULT_VECTOR(name, code)                       \
	__attribute__((naked)) void name(void){               \
		__asm volatile(                                   \
			"tst lr, #4\n"                                \
			"ite eq\n"                                    \
			"mrseq r0, msp\n"                             \
			"mrsne r0, psp\n"                             \
			"mov r1, lr\n"                                \
			"movs r2, #" #code "\n"                       \
			"b kf_fault_entry\n");                         \
	}
KF_FAULT_VECTOR(isr_hardfault,   1)
KF_FAULT_VECTOR(isr_memmanage,   2)
KF_FAULT_VECTOR(isr_busfault,    3)
KF_FAULT_VECTOR(isr_usagefault,  4)
KF_FAULT_VECTOR(isr_securefault, 5)
