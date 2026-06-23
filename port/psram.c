// port/psram.c — driver for the PicoCalc's 8 MB ESP-PSRAM64H QSPI PSRAM.
//
// The chip is on its own GPIO bus (SIO0=GP2, SIO1=GP3, SIO2=GP4, SIO3=GP5,
// CS=GP20, SCK=GP21) — NOT on the RP2350 XIP/QMI flash bus, so it can't be
// memory-mapped. It's a software block store: explicit read/write over a PIO
// SPI engine. v1 uses plain 1-bit SPI (mode 0); SIO2/SIO3 are unused here. The
// API (read/write/alloc) is stable so the internals can move to quad later.
//
// ESP-PSRAM64H / APS6404L: SPI read 0x03 (<=33 MHz), write 0x02, reset 0x66/0x99,
// read-id 0x9F (-> mfg 0x0D, KGD 0x5D). Burst wraps at the 1024-byte page, so we
// split every transfer at 1 KB boundaries (safe regardless of wrap config).
#include <stdint.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"

#include "../kefyros.h"
#include "board.h"
#include "psram.pio.h"

static PIO  s_pio;
static int  s_sm = -1;
static uint s_off;
static uint32_t s_size = 0;          /* detected size in bytes (0 = absent/failed) */
static uint32_t s_brk  = 0;          /* bump allocator cursor */

static inline void cs_lo(void){ gpio_put(KF_PSRAM_CS, 0); }
static inline void cs_hi(void){ gpio_put(KF_PSRAM_CS, 1); }

/* full-duplex byte transfer (MSB-first). tx/rx may be NULL. */
static void xfer(const uint8_t *tx, uint8_t *rx, int n){
	for(int i = 0; i < n; i++){
		pio_sm_put_blocking(s_pio, s_sm, ((uint32_t)(tx ? tx[i] : 0)) << 24);
		uint32_t v = pio_sm_get_blocking(s_pio, s_sm);
		if(rx) rx[i] = (uint8_t)(v & 0xff);
	}
}

static void cmd_only(uint8_t cmd){
	cs_lo(); xfer(&cmd, NULL, 1); cs_hi();
}

/* one PSRAM op confined to a single 1 KB page: cmd + 24-bit addr + data */
static void op_page(uint8_t cmd, uint32_t addr, const uint8_t *wr, uint8_t *rd, int n){
	uint8_t hdr[4] = { cmd, (uint8_t)(addr>>16), (uint8_t)(addr>>8), (uint8_t)addr };
	cs_lo();
	xfer(hdr, NULL, 4);
	if(wr)      xfer(wr, NULL, n);
	else if(rd) xfer(NULL, rd, n);
	cs_hi();
}

static void chunked(uint8_t cmd, uint32_t addr, const uint8_t *wr, uint8_t *rd, uint32_t n){
	while(n){
		uint32_t page_left = 1024u - (addr & 1023u);
		uint32_t c = n < page_left ? n : page_left;
		op_page(cmd, addr, wr, rd, (int)c);
		addr += c; n -= c;
		if(wr) wr += c;
		if(rd) rd += c;
	}
}

void kf_psram_write(uint32_t addr, const void *buf, uint32_t n){
	if(!s_size) return;
	chunked(0x02, addr, (const uint8_t*)buf, NULL, n);
}
void kf_psram_read(uint32_t addr, void *buf, uint32_t n){
	if(!s_size){ if(buf) memset(buf, 0, n); return; }
	chunked(0x03, addr, NULL, (uint8_t*)buf, n);
}

uint32_t kf_psram_size(void){ return s_size; }

/* dumb bump allocator for big long-lived blobs (wallpaper, browser page). */
uint32_t kf_psram_alloc(uint32_t n){
	n = (n + 3u) & ~3u;
	if(!s_size || s_brk + n > s_size) return 0xFFFFFFFFu;   /* out of PSRAM */
	uint32_t a = s_brk; s_brk += n; return a;
}
void kf_psram_reset_alloc(void){ s_brk = 0; }

/* Re-derive the PIO SPI clock divider for the current clk_sys (call after a runtime
   clk_sys change so the PSRAM bus stays ~16 MHz instead of scaling with the CPU).
   Use an INTEGER divider: a heavy fractional divider corrupts reads. At the 400 MHz boot
   clock the natural divider is ~11.1 (frac 0.1 — nearly integer, harmless), but at the
   250 MHz eco clock it's ~6.94 (frac 0.94), which stretches ~94% of SCK periods by a sys
   cycle. That jitter walks the MISO sample point off its valid window -> garbage reads.
   A pure-integer divider keeps SCK periodic and the sample phase consistent at any clk_sys. */
void kf_psram_reclock(void){
	if(!s_size || s_sm < 0) return;
	uint32_t hz = clock_get_hz(clk_sys);
	uint32_t div = (hz + (2u*16000000u) - 1u) / (2u*16000000u);   /* ceil -> integer, <=16 MHz */
	if(div < 1u) div = 1u; if(div > 65535u) div = 65535u;
	pio_sm_set_clkdiv_int_frac(s_pio, s_sm, (uint16_t)div, 0);    /* frac=0: no jitter */
	pio_sm_clkdiv_restart(s_pio, s_sm);
}

uint32_t kf_psram_init(void){
	/* CS + unused SIO2/3 as plain GPIOs (drive high; ignored in SPI mode). */
	gpio_init(KF_PSRAM_CS);   gpio_set_dir(KF_PSRAM_CS, GPIO_OUT);   gpio_put(KF_PSRAM_CS, 1);
	gpio_init(KF_PSRAM_SIO2); gpio_set_dir(KF_PSRAM_SIO2, GPIO_OUT); gpio_put(KF_PSRAM_SIO2, 1);
	gpio_init(KF_PSRAM_SIO3); gpio_set_dir(KF_PSRAM_SIO3, GPIO_OUT); gpio_put(KF_PSRAM_SIO3, 1);

	s_pio = pio0;
	s_sm  = pio_claim_unused_sm(s_pio, true);
	s_off = pio_add_program(s_pio, &psram_spi_program);

	pio_gpio_init(s_pio, KF_PSRAM_SIO0);
	pio_gpio_init(s_pio, KF_PSRAM_SIO1);
	pio_gpio_init(s_pio, KF_PSRAM_SCK);
	pio_sm_set_pindirs_with_mask(s_pio, s_sm,
		(1u<<KF_PSRAM_SIO0) | (1u<<KF_PSRAM_SCK),                       /* outputs */
		(1u<<KF_PSRAM_SIO0) | (1u<<KF_PSRAM_SIO1) | (1u<<KF_PSRAM_SCK)); /* mask    */

	pio_sm_config c = psram_spi_program_get_default_config(s_off);
	sm_config_set_out_pins(&c, KF_PSRAM_SIO0, 1);
	sm_config_set_in_pins(&c, KF_PSRAM_SIO1);
	sm_config_set_sideset_pins(&c, KF_PSRAM_SCK);
	sm_config_set_out_shift(&c, false /*MSB-first*/, true /*autopull*/, 8);
	sm_config_set_in_shift(&c,  false /*MSB-first*/, true /*autopush*/, 8);
	/* SPI clock = clk_sys / (2*clkdiv), kept <=~18 MHz (under the 33 MHz limit for the
	   0x03 read). MUST be an INTEGER divider: a fractional one is near-integer at the old
	   360/400 MHz boot (~11.1, frac 0.1, harmless) but ~6.94 (frac 0.94) at the 250 MHz
	   default, whose SCK jitter walks the MISO sample point off its window -> garbage reads
	   (corrupt wallpaper + music index). Matches kf_psram_reclock()'s integer divider. */
	uint32_t hz = clock_get_hz(clk_sys);
	uint32_t div = (hz + (2u*18000000u) - 1u) / (2u*18000000u);   /* ceil -> integer */
	if(div < 1u) div = 1u; if(div > 65535u) div = 65535u;
	sm_config_set_clkdiv_int_frac(&c, (uint16_t)div, 0);          /* frac=0: no jitter */
	pio_sm_init(s_pio, s_sm, s_off, &c);
	pio_sm_set_enabled(s_pio, s_sm, true);

	/* wake + reset the chip (it powers up in SPI mode). */
	sleep_us(200);
	cmd_only(0x66);   /* reset enable */
	cmd_only(0x99);   /* reset       */
	sleep_us(100);

	/* read ID: 0x9F + 3 dummy addr + 8 id bytes. mfg 0x0D, KGD 0x5D = good chip. */
	uint8_t id[11] = {0};
	uint8_t hdr[4] = { 0x9F, 0, 0, 0 };
	cs_lo(); xfer(hdr, NULL, 4); xfer(NULL, id, 8); cs_hi();

	s_size = 0;
	if(id[0] == 0x0D /* APMemory */){
		s_size = KF_PSRAM_SIZE;
		/* write/read-back self-test at a few addresses (incl. a page-cross). */
		static const uint32_t probes[] = { 0, 1000, 0x100000, KF_PSRAM_SIZE - 16 };
		uint8_t w[16], r[16];
		for(unsigned p = 0; p < sizeof probes/sizeof probes[0]; p++){
			for(int i = 0; i < 16; i++) w[i] = (uint8_t)(probes[p] + i*7 + 1);
			kf_psram_write(probes[p], w, 16);
			memset(r, 0, 16);
			kf_psram_read(probes[p], r, 16);
			if(memcmp(w, r, 16) != 0){ s_size = 0; break; }   /* failed */
		}
	}
	s_brk = 0;
	return s_size;
}
