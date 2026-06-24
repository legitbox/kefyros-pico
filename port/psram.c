// port/psram.c — driver for the PicoCalc's 8 MB ESP-PSRAM64H / APS6404L PSRAM.
//
// The chip is on its own GPIO bus (SIO0=GP2, SIO1=GP3, SIO2=GP4, SIO3=GP5,
// CS=GP20, SCK=GP21) — NOT on the RP2350 XIP/QMI flash bus, so it can't be
// memory-mapped. It's a software block store: explicit read/write over a PIO
// engine. The public API (read/write/alloc) is byte-oriented and stable.
//
// The chip powers up in 1-bit SPI mode and must be told to enter QPI (quad)
// manually. So bring-up runs 1-bit (reset 0x66/0x99, read-id 0x9F), sends 0x35
// "enter quad", then switches the PIO to a quad engine and self-tests. After
// that EVERYTHING is QPI: fast-quad read 0xEB (6 dummy cycles) and quad write
// 0x38 (0 dummy), 4 bits per SCK — ~4x the 1-bit throughput at the same ~18 MHz
// bus, lifting wallpaper streaming, the browser arena, the music index, etc.
//
// Fail-hard: if the quad read-back self-test doesn't come back byte-exact, we
// treat PSRAM as absent (s_size = 0), exactly like a failed chip probe. No
// runtime 1-bit fallback — quad is the contract.
//
// Burst wraps at the chip's 1024-byte page, so every transfer is split at 1 KB
// boundaries (safe regardless of the chip's wrap config).
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/pio_instructions.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "../kefyros.h"
#include "board.h"
#include "psram.pio.h"

/* The QPI bus clock is auto-calibrated at boot (kf_psram_init): we sweep the PIO
   divider from fast to slow and keep the fastest that passes the bit-verify self-
   test, so the board picks its own safe ceiling instead of us hard-coding one. The
   chip is rated 133 MHz; the real limit is the PicoCalc's GPIO traces + the PIO
   read sample point (bit-banged, so no hardware RX-delay trim like the QMI flash). */
#define KF_BUS_HZ_MAX    45000000u   /* fastest divider we'll try (÷-rounded to achievable) */
#define KF_BUS_HZ_MIN    12000000u   /* slowest we'll accept before declaring the chip dead */
#define QSPI_DUMMY       6           /* default 0xEB fast-quad-read dummy cycles (datasheet) */
#define QR_DUMMY_IDX     6           /* index of the `set y, N-1` dummy-count instr in psram_qr */

static PIO  s_pio;
static int  s_sm = -1;
static uint s_qr_off, s_qw_off;      /* loaded offsets of the quad read/write programs   */
static int  s_active = -1;           /* offset of the program currently armed on the SM  */
static uint32_t s_size = 0;          /* detected size in bytes (0 = absent/failed)        */
static uint32_t s_brk  = 0;          /* bump allocator cursor                             */
static uint32_t s_bus_hz = 16000000u;/* calibrated bus speed; 16 MHz is the safe bring-up rate */

static inline void cs_lo(void){ gpio_put(KF_PSRAM_CS, 0); }
static inline void cs_hi(void){ gpio_put(KF_PSRAM_CS, 1); }

/* ---- integer clock divider (frac=0): a fractional divider jitters the sample point
   and corrupts reads — see the long note in kf_psram_reclock(). Derived from the
   calibrated s_bus_hz, so at any clk_sys the actual SCK is <= the validated speed. ---- */
static uint16_t bus_div(void){
	uint32_t hz = clock_get_hz(clk_sys);
	uint32_t div = (hz + (2u*s_bus_hz) - 1u) / (2u*s_bus_hz);   /* ceil -> integer */
	if(div < 1u) div = 1u;
	if(div > 65535u) div = 65535u;
	return (uint16_t)div;
}

/* =====================================================================
   1-bit SPI — used ONLY during bring-up (chip is still in SPI mode).
   ===================================================================== */
/* full-duplex 1-bit byte transfer (MSB-first). tx/rx may be NULL. */
static void xfer1(const uint8_t *tx, uint8_t *rx, int n){
	for(int i = 0; i < n; i++){
		pio_sm_put_blocking(s_pio, s_sm, ((uint32_t)(tx ? tx[i] : 0)) << 24);
		uint32_t v = pio_sm_get_blocking(s_pio, s_sm);
		if(rx) rx[i] = (uint8_t)(v & 0xff);
	}
}
static void cmd1(uint8_t cmd){ cs_lo(); xfer1(&cmd, NULL, 1); cs_hi(); }

/* =====================================================================
   Quad (QPI) engine.
   ===================================================================== */
/* Build the shared quad SM config for whichever program lives at `off`.
   Read and write use IDENTICAL pin mappings & shift settings — they differ only
   in their instructions — so the only thing that changes on a read<->write
   switch is which program the PC runs. clkdiv is taken from the live clk_sys. */
static pio_sm_config qcfg(uint off){
	pio_sm_config c = (off == s_qr_off)
	                ? psram_qr_program_get_default_config(off)
	                : psram_qw_program_get_default_config(off);
	sm_config_set_out_pins(&c, KF_PSRAM_SIO0, 4);
	sm_config_set_in_pins(&c,  KF_PSRAM_SIO0);
	sm_config_set_set_pins(&c, KF_PSRAM_SIO0, 4);
	sm_config_set_sideset_pins(&c, KF_PSRAM_SCK);
	sm_config_set_out_shift(&c, false /*MSB-first*/, true /*autopull*/, 32);
	sm_config_set_in_shift(&c,  false /*MSB-first*/, true /*autopush*/, 8);
	sm_config_set_clkdiv_int_frac(&c, bus_div(), 0);
	return c;
}

/* Arm the SM to run program `off` from a clean state (PC at start, FIFOs + shift
   registers cleared). A full re-init only when the program actually changes;
   otherwise just restart + jump, which is cheap. The clear is what drops any
   leftover OSR nibbles from a previous partial-word write. */
static void qarm(uint off){
	pio_sm_set_enabled(s_pio, s_sm, false);
	if(off != (uint)s_active){
		pio_sm_config c = qcfg(off);
		pio_sm_init(s_pio, s_sm, off, &c);   /* sets pins/shift/clkdiv/wrap, jmp off, clears */
		s_active = (int)off;
	} else {
		pio_sm_clear_fifos(s_pio, s_sm);
		pio_sm_restart(s_pio, s_sm);
		pio_sm_exec(s_pio, s_sm, pio_encode_jmp(off));
	}
	pio_sm_set_enabled(s_pio, s_sm, true);
}

/* one quad transfer confined to a single 1 KB page */
static void qread_page(uint32_t addr, uint8_t *rd, int n){
	qarm(s_qr_off);
	cs_lo();
	pio_sm_put_blocking(s_pio, s_sm, (0xEBu << 24) | (addr & 0xFFFFFFu)); /* cmd + addr   */
	pio_sm_put_blocking(s_pio, s_sm, (uint32_t)(2*n - 1));                /* data nibbles */
	for(int i = 0; i < n; i++)
		rd[i] = (uint8_t)(pio_sm_get_blocking(s_pio, s_sm) & 0xFF);
	cs_hi();
}
static void qwrite_page(uint32_t addr, const uint8_t *wr, int n){
	qarm(s_qw_off);
	cs_lo();
	pio_sm_put_blocking(s_pio, s_sm, (0x38u << 24) | (addr & 0xFFFFFFu)); /* cmd + addr   */
	pio_sm_put_blocking(s_pio, s_sm, (uint32_t)(2*n - 1));                /* data nibbles */
	int i = 0;
	while(i < n){
		uint32_t w = 0;                                /* pack big-endian, byte0 in bits 31..24 */
		for(int b = 0; b < 4; b++){ w <<= 8; if(i < n) w |= wr[i++]; }
		pio_sm_put_blocking(s_pio, s_sm, w);
	}
	pio_sm_get_blocking(s_pio, s_sm);                  /* completion barrier (last nibble clocked) */
	cs_hi();
}

static void qchunked(int is_write, uint32_t addr, const uint8_t *wr, uint8_t *rd, uint32_t n){
	while(n){
		uint32_t page_left = 1024u - (addr & 1023u);
		uint32_t c = n < page_left ? n : page_left;
		if(is_write) qwrite_page(addr, wr, (int)c);
		else         qread_page (addr, rd, (int)c);
		addr += c; n -= c;
		if(wr) wr += c;
		if(rd) rd += c;
	}
}

/* =====================================================================
   Public API
   ===================================================================== */
void kf_psram_write(uint32_t addr, const void *buf, uint32_t n){
	if(!s_size) return;
	qchunked(1, addr, (const uint8_t*)buf, NULL, n);
}
void kf_psram_read(uint32_t addr, void *buf, uint32_t n){
	if(!s_size){ if(buf) memset(buf, 0, n); return; }
	qchunked(0, addr, NULL, (uint8_t*)buf, n);
}

uint32_t kf_psram_size(void){ return s_size; }

/* dumb bump allocator for big long-lived blobs (wallpaper, browser page). */
uint32_t kf_psram_alloc(uint32_t n){
	n = (n + 3u) & ~3u;
	if(!s_size || s_brk + n > s_size) return 0xFFFFFFFFu;   /* out of PSRAM */
	uint32_t a = s_brk; s_brk += n; return a;
}
void kf_psram_reset_alloc(void){ s_brk = 0; }

/* Re-derive the PIO clock divider for the current clk_sys (call after a runtime
   clk_sys change so the PSRAM bus stays ~18 MHz instead of scaling with the CPU).
   MUST be an INTEGER divider: a heavy fractional divider stretches ~94% of SCK
   periods by a sys cycle at the 250 MHz eco clock, walking the MISO sample point
   off its valid window -> garbage reads. frac=0 keeps SCK periodic at any clk_sys.
   In quad this matters even more — a nibble sampled a hair early shifts every byte. */
void kf_psram_reclock(void){
	if(!s_size || s_sm < 0) return;
	pio_sm_set_clkdiv_int_frac(s_pio, s_sm, bus_div(), 0);
	pio_sm_clkdiv_restart(s_pio, s_sm);
}

/* Patch the read program's dummy-cycle count live (instruction memory write).
   Used by the self-test to localise a nibble-shift if 6 is wrong on this board. */
static void qr_set_dummy(int dummy){
	s_pio->instr_mem[s_qr_off + QR_DUMMY_IDX] = pio_encode_set(pio_y, dummy - 1);
}

/* Round-trip self-test at the current clock. Writes varied patterns (incl. a
   non-multiple-of-4 length and a 1 KB page-cross) and reads them back byte-exact.
   Returns 1 if clean. */
static int qr_selftest(void){
	static const uint32_t probes[] = { 0, 1003, 0x100000u, KF_PSRAM_SIZE - 40u };
	static const uint32_t lens[]   = { 16,  40,   64,        37 };
	/* probe[1]=1003,len40 straddles the 1 KB page boundary into 21- and 19-byte
	   sub-chunks (both non-multiple-of-4) — exercises chunk-splitting + the OSR
	   flush between writes. probe[3] is odd-length near the top of the chip. */
	uint8_t w[80], r[80];
	for(unsigned p = 0; p < sizeof probes/sizeof probes[0]; p++){
		uint32_t a = probes[p], n = lens[p];
		for(uint32_t i = 0; i < n; i++) w[i] = (uint8_t)(a + i*37u + 0x5Au);
		kf_psram_write(a, w, n);
		memset(r, 0xAA, sizeof r);
		kf_psram_read(a, r, n);
		if(memcmp(w, r, n) != 0) return 0;
	}
	return 1;
}

/* Software-reset the chip in QPI format, before any PIO is set up. Critical for
   warm reboots: a previous run left the chip in QPI mode, and on the PicoCalc the
   PSRAM rail stays powered across an RP2350 reset (and across a "power off" while
   USB is attached), so QPI mode is sticky. Our 1-bit bring-up (ID read etc.) only
   works if the chip is in SPI, so we first bit-bang 0x66/0x99 as quad to knock a
   QPI-stuck chip back to SPI. A chip already in SPI sees a 2-clock fragment and
   discards it (incomplete command) — harmless either way. Plain GPIO; one-time. */
static void qpi_reset_to_spi(void){
	const int sio[4] = { KF_PSRAM_SIO0, KF_PSRAM_SIO1, KF_PSRAM_SIO2, KF_PSRAM_SIO3 };
	for(int i = 0; i < 4; i++){ gpio_init(sio[i]); gpio_set_dir(sio[i], GPIO_OUT); }
	gpio_init(KF_PSRAM_SCK); gpio_set_dir(KF_PSRAM_SCK, GPIO_OUT); gpio_put(KF_PSRAM_SCK, 0);
	gpio_init(KF_PSRAM_CS);  gpio_set_dir(KF_PSRAM_CS,  GPIO_OUT); gpio_put(KF_PSRAM_CS,  1);

	static const uint8_t cmds[2] = { 0x66, 0x99 };   /* reset-enable, reset */
	for(int c = 0; c < 2; c++){
		gpio_put(KF_PSRAM_CS, 0);
		for(int half = 0; half < 2; half++){         /* high nibble first */
			uint8_t n = half ? (cmds[c] & 0xF) : (cmds[c] >> 4);
			for(int b = 0; b < 4; b++) gpio_put(sio[b], (n >> b) & 1);  /* SIO3=MSB */
			__asm volatile("nop\nnop\nnop\nnop");
			gpio_put(KF_PSRAM_SCK, 1);               /* sample edge (mode 0) */
			__asm volatile("nop\nnop\nnop\nnop");
			gpio_put(KF_PSRAM_SCK, 0);
		}
		gpio_put(KF_PSRAM_CS, 1);
		for(volatile int i = 0; i < 64; i++) __asm volatile("nop");
	}
	sleep_us(100);   /* chip needs time to complete the reset before next command */
}

uint32_t kf_psram_init(void){
	qpi_reset_to_spi();   /* force the chip to SPI mode regardless of prior state */

	/* CS as a plain GPIO (manual). SIO2/3 start as GPIO-high; they join the PIO
	   only once we go quad (in 1-bit bring-up they're unused / driven high). */
	gpio_init(KF_PSRAM_CS);   gpio_set_dir(KF_PSRAM_CS, GPIO_OUT);   gpio_put(KF_PSRAM_CS, 1);
	gpio_init(KF_PSRAM_SIO2); gpio_set_dir(KF_PSRAM_SIO2, GPIO_OUT); gpio_put(KF_PSRAM_SIO2, 1);
	gpio_init(KF_PSRAM_SIO3); gpio_set_dir(KF_PSRAM_SIO3, GPIO_OUT); gpio_put(KF_PSRAM_SIO3, 1);

	s_pio = pio0;
	s_sm  = pio_claim_unused_sm(s_pio, true);

	/* ---- Stage 1: 1-bit SPI bring-up ---- */
	uint spi_off = pio_add_program(s_pio, &psram_spi_program);
	pio_gpio_init(s_pio, KF_PSRAM_SIO0);
	pio_gpio_init(s_pio, KF_PSRAM_SIO1);
	pio_gpio_init(s_pio, KF_PSRAM_SCK);
	pio_sm_set_pindirs_with_mask(s_pio, s_sm,
		(1u<<KF_PSRAM_SIO0) | (1u<<KF_PSRAM_SCK),
		(1u<<KF_PSRAM_SIO0) | (1u<<KF_PSRAM_SIO1) | (1u<<KF_PSRAM_SCK));
	{
		pio_sm_config c = psram_spi_program_get_default_config(spi_off);
		sm_config_set_out_pins(&c, KF_PSRAM_SIO0, 1);
		sm_config_set_in_pins(&c, KF_PSRAM_SIO1);
		sm_config_set_sideset_pins(&c, KF_PSRAM_SCK);
		sm_config_set_out_shift(&c, false /*MSB-first*/, true /*autopull*/, 8);
		sm_config_set_in_shift(&c,  false /*MSB-first*/, true /*autopush*/, 8);
		sm_config_set_clkdiv_int_frac(&c, bus_div(), 0);
		pio_sm_init(s_pio, s_sm, spi_off, &c);
		pio_sm_set_enabled(s_pio, s_sm, true);
	}

	sleep_us(200);
	cmd1(0x66);   /* reset enable */
	cmd1(0x99);   /* reset       */
	sleep_us(100);

	/* read ID: 0x9F + 3 dummy addr + 8 id bytes. mfg 0x0D (APMemory) = good chip. */
	uint8_t id[8] = {0};
	uint8_t hdr[4] = { 0x9F, 0, 0, 0 };
	cs_lo(); xfer1(hdr, NULL, 4); xfer1(NULL, id, 8); cs_hi();
	if(id[0] != 0x0D){ s_size = 0; return 0; }      /* no/unknown chip -> absent */

	/* ---- Stage 2: enter QPI, then re-arm the SM as a quad engine ---- */
	cmd1(0x35);                                     /* Enter Quad Mode (still 1-bit cmd) */

	pio_sm_set_enabled(s_pio, s_sm, false);
	pio_remove_program(s_pio, &psram_spi_program, spi_off);
	s_qr_off = pio_add_program(s_pio, &psram_qr_program);
	s_qw_off = pio_add_program(s_pio, &psram_qw_program);
	s_active = -1;
	pio_gpio_init(s_pio, KF_PSRAM_SIO2);            /* SIO2/3 now PIO-driven for quad */
	pio_gpio_init(s_pio, KF_PSRAM_SIO3);
	qr_set_dummy(QSPI_DUMMY);

	/* ---- Stage 3: auto-calibrate the bus speed ----
	   Sweep the PIO divider fast -> slow at the current clk_sys and keep the FASTEST
	   that passes the bit-verify self-test. s_bus_hz stores the actual achieved speed
	   (clk/(2*div)), so kf_psram_reclock() reproduces it at any later clk_sys. Fail-
	   hard only if even the slowest candidate is corrupt. */
	s_size = KF_PSRAM_SIZE;                          /* enable read/write for the test */
	uint32_t clk   = clock_get_hz(clk_sys);
	uint32_t dfast = (clk + (2u*KF_BUS_HZ_MAX) - 1u) / (2u*KF_BUS_HZ_MAX);
	uint32_t dslow = (clk + (2u*KF_BUS_HZ_MIN) - 1u) / (2u*KF_BUS_HZ_MIN);
	if(dfast < 2u) dfast = 2u;
	int ok = 0;
	for(uint32_t div = dfast; div <= dslow; div++){
		s_bus_hz = clk / (2u * div);                 /* actual SCK for this divider   */
		s_active = -1;                               /* force qarm to re-init the SM  */
		if(qr_selftest()){ ok = 1; break; }
	}
	if(!ok){ s_size = 0; return 0; }                 /* nothing worked -> treat as absent */

	s_brk = 0;
	return s_size;
}

/* Current QPI bus clock in Hz (actual SCK at the live clk_sys), 0 if PSRAM absent. */
uint32_t kf_psram_bus_hz(void){
	if(!s_size || s_sm < 0) return 0;
	return clock_get_hz(clk_sys) / (2u * bus_div());
}
