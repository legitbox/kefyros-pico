// port/psram_qmi.c - Pimoroni Pico Plus 2 W onboard APS6404 QMI PSRAM.
//
// The chip shares QSPI data/clock with flash, has CS on GP47, and is exposed by
// RP2350 memory window 1 at 0x11000000 after setup. This keeps Kefyros's offset-
// based kf_psram_* API while replacing the PicoCalc mainboard's ~18 MHz PIO bus
// with memory-mapped quad transfers around 100-125 MHz.
//
// QMI bring-up is adapted from Pimoroni's MicroPython RP2350 port (MIT),
// rp2_psram.c, copyright 2025 Phil Howard, Mike Bell and Kirk D. Benell.

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "pico/stdlib.h"
#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/structs/qmi.h"
#include "hardware/structs/xip_ctrl.h"

#include "kefyros.h"

#ifndef PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN
#error "psram_qmi.c requires the Pimoroni Pico Plus 2 W board definition"
#endif

#define KF_QMI_PSRAM_NOCACHE ((uintptr_t)0x15000000u)
#define KF_QMI_PSRAM_MAX_HZ  133000000u

static uint32_t s_size;
static uint32_t s_brk;
static uint32_t s_bus_hz;
static uint8_t s_test[1024];

static uint32_t qmi_psram_timing(uint32_t clock_hz, uint32_t *bus_hz){
	uint32_t divisor = (clock_hz + KF_QMI_PSRAM_MAX_HZ - 1u) / KF_QMI_PSRAM_MAX_HZ;
	if(divisor == 1u && clock_hz > 100000000u) divisor = 2u;
	uint32_t rxdelay = divisor;
	if(clock_hz / divisor > 100000000u) rxdelay++;

	/* APS6404: CS active <=8 us; CS deselected >=18 ns. Register units are
	   64 clk_sys cycles and clk_sys cycles respectively. */
	uint64_t period_fs = 1000000000000000ull / clock_hz;
	uint32_t max_select = (uint32_t)((125ull * 1000000ull) / period_fs);
	uint32_t min_deselect = (uint32_t)((18ull * 1000000ull + period_fs - 1ull) / period_fs);
	uint32_t half_sck = (divisor + 1u) / 2u;
	min_deselect = min_deselect > half_sck ? min_deselect - half_sck : 0u;
	if(max_select > 0x3fu) max_select = 0x3fu;
	if(min_deselect > 0x1fu) min_deselect = 0x1fu;
	if(rxdelay > 0x7u) rxdelay = 0x7u;

	*bus_hz = clock_hz / divisor;
	return 1u << QMI_M1_TIMING_COOLDOWN_LSB |
	       QMI_M1_TIMING_PAGEBREAK_VALUE_1024 << QMI_M1_TIMING_PAGEBREAK_LSB |
	       max_select << QMI_M1_TIMING_MAX_SELECT_LSB |
	       min_deselect << QMI_M1_TIMING_MIN_DESELECT_LSB |
	       rxdelay << QMI_M1_TIMING_RXDELAY_LSB |
	       divisor << QMI_M1_TIMING_CLKDIV_LSB;
}

static size_t __no_inline_not_in_flash_func(qmi_psram_detect)(void){
	qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
	while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}

	/* Exit QPI in case PSRAM stayed powered across a warm RP2350 reset. */
	qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
	qmi_hw->direct_tx = QMI_DIRECT_TX_OE_BITS |
	                     (QMI_DIRECT_TX_IWIDTH_VALUE_Q << QMI_DIRECT_TX_IWIDTH_LSB) |
	                     0xf5u;
	while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}
	(void)qmi_hw->direct_rx;
	qmi_hw->direct_csr &= ~QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
	qmi_hw->direct_csr |= QMI_DIRECT_CSR_ASSERT_CS1N_BITS;
	uint8_t kgd = 0, eid = 0;
	for(size_t i = 0; i < 7; i++){
		qmi_hw->direct_tx = i == 0 ? 0x9fu : 0xffu;
		while(!(qmi_hw->direct_csr & QMI_DIRECT_CSR_TXEMPTY_BITS)){}
		while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}
		uint8_t v = (uint8_t)qmi_hw->direct_rx;
		if(i == 5) kgd = v;
		if(i == 6) eid = v;
	}
	qmi_hw->direct_csr &= ~(QMI_DIRECT_CSR_ASSERT_CS1N_BITS | QMI_DIRECT_CSR_EN_BITS);
	if(kgd != 0x5du) return 0;
	uint8_t size_id = eid >> 5;
	if(eid == 0x26u || size_id == 2u) return 8u * 1024u * 1024u;
	if(size_id == 1u) return 4u * 1024u * 1024u;
	if(size_id == 0u) return 2u * 1024u * 1024u;
	return 0;
}

static size_t __no_inline_not_in_flash_func(qmi_psram_hw_init)(void){
	/* A CPU-only reset can leave the separately-powered PSRAM selected. Drive CS
	   high as plain GPIO before handing it back to QMI, so the recovery command
	   starts at a real transaction boundary. */
	gpio_init(PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN);
	gpio_put(PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN, 1);
	gpio_set_dir(PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN, GPIO_OUT);
	busy_wait_us_32(10);
	gpio_set_function(PIMORONI_PICO_PLUS2_W_PSRAM_CS_PIN, GPIO_FUNC_XIP_CS1);
	uint32_t irq = save_and_disable_interrupts();
	size_t size = qmi_psram_detect();
	if(!size){ restore_interrupts(irq); return 0; }
	uint32_t timing = qmi_psram_timing(clock_get_hz(clk_sys), &s_bus_hz);

	qmi_hw->direct_csr = 10u << QMI_DIRECT_CSR_CLKDIV_LSB |
	                     QMI_DIRECT_CSR_EN_BITS | QMI_DIRECT_CSR_AUTO_CS1N_BITS;
	while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}
	qmi_hw->direct_tx = QMI_DIRECT_TX_NOPUSH_BITS | 0x35u; /* enter QPI */
	while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}

	qmi_hw->m[1].timing = timing;
	qmi_hw->m[1].rfmt =
		QMI_M0_RFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_PREFIX_WIDTH_LSB |
		QMI_M0_RFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_RFMT_ADDR_WIDTH_LSB |
		QMI_M0_RFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_RFMT_SUFFIX_WIDTH_LSB |
		QMI_M0_RFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_RFMT_DUMMY_WIDTH_LSB |
		QMI_M0_RFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_RFMT_DATA_WIDTH_LSB |
		QMI_M0_RFMT_PREFIX_LEN_VALUE_8   << QMI_M0_RFMT_PREFIX_LEN_LSB |
		6u << QMI_M0_RFMT_DUMMY_LEN_LSB;
	qmi_hw->m[1].rcmd = 0xebu;
	qmi_hw->m[1].wfmt =
		QMI_M0_WFMT_PREFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_PREFIX_WIDTH_LSB |
		QMI_M0_WFMT_ADDR_WIDTH_VALUE_Q   << QMI_M0_WFMT_ADDR_WIDTH_LSB |
		QMI_M0_WFMT_SUFFIX_WIDTH_VALUE_Q << QMI_M0_WFMT_SUFFIX_WIDTH_LSB |
		QMI_M0_WFMT_DUMMY_WIDTH_VALUE_Q  << QMI_M0_WFMT_DUMMY_WIDTH_LSB |
		QMI_M0_WFMT_DATA_WIDTH_VALUE_Q   << QMI_M0_WFMT_DATA_WIDTH_LSB |
		QMI_M0_WFMT_PREFIX_LEN_VALUE_8   << QMI_M0_WFMT_PREFIX_LEN_LSB;
	qmi_hw->m[1].wcmd = 0x38u;
	qmi_hw->direct_csr = 0;
	__dsb();
	__isb();
	hw_set_bits(&xip_ctrl_hw->ctrl, XIP_CTRL_WRITABLE_M1_BITS);
	restore_interrupts(irq);
	return size;
}

void kf_psram_write(uint32_t addr, const void *buf, uint32_t n){
	if(!s_size || !buf || addr > s_size || n > s_size - addr) return;
	memcpy((void *)(KF_QMI_PSRAM_NOCACHE + addr), buf, n);
}

void kf_psram_read(uint32_t addr, void *buf, uint32_t n){
	if(!buf) return;
	if(!s_size || addr > s_size || n > s_size - addr){ memset(buf, 0, n); return; }
	memcpy(buf, (const void *)(KF_QMI_PSRAM_NOCACHE + addr), n);
}

void *kf_psram_map(uint32_t addr, uint32_t n){
	/* Deliberately keep the public Kefyros contract block-oriented. Direct cached
	   pointers made emulator reads fast, but a reset during a mapped cart write can
	   leave cache/QMI state entangled with the next boot. The GB frontend uses two
	   16 KiB SRAM banks over this uncached backing store instead. */
	(void)addr; (void)n;
	return NULL;
}

uint32_t kf_psram_size(void){ return s_size; }
uint32_t kf_psram_brk(void){ return s_brk; }
uint32_t kf_psram_bus_hz(void){ return s_size ? s_bus_hz : 0; }

uint32_t kf_psram_alloc(uint32_t n){
	n = (n + 3u) & ~3u;
	/* The destructive boot test is over before the allocator is exposed, so QMI
	   boards may use the whole chip. This matters for large GBC ROMs. */
	uint32_t ceiling = s_size;
	if(!s_size || s_brk > ceiling || n > ceiling - s_brk) return 0xffffffffu;
	uint32_t a = s_brk;
	s_brk += n;
	return a;
}

void kf_psram_reset_alloc(void){
	s_brk = 0;
}
void kf_psram_free_to(uint32_t addr){ if(addr <= s_brk) s_brk = addr; }

void __no_inline_not_in_flash_func(kf_psram_reclock)(void){
	if(!s_size) return;
	/* Compute everything which might call ordinary SDK code before direct mode
	   parks XIP. Only raw register writes remain inside the parked window. */
	uint32_t timing = qmi_psram_timing(clock_get_hz(clk_sys), &s_bus_hz);
	uint32_t irq = save_and_disable_interrupts();
	/* Direct mode parks both XIP windows so all non-divider M1 timing fields are
	   changed only while QMI is idle. This function itself runs from SRAM. */
	qmi_hw->direct_csr = 30u << QMI_DIRECT_CSR_CLKDIV_LSB | QMI_DIRECT_CSR_EN_BITS;
	while(qmi_hw->direct_csr & QMI_DIRECT_CSR_BUSY_BITS){}
	qmi_hw->m[1].timing = timing;
	qmi_hw->direct_csr = 0;
	__dsb();
	__isb();
	restore_interrupts(irq);
}

uint32_t kf_psram_init(void){
	for(int attempt = 0; attempt < 3; attempt++){
		s_size = (uint32_t)qmi_psram_hw_init();
		if(!s_size) continue;
		uint32_t base = s_size - sizeof s_test;
		uint32_t seed = 0x9e3779b9u;
		for(size_t i = 0; i < sizeof s_test; i++){
			seed = seed * 1664525u + 1013904223u;
			s_test[i] = (uint8_t)(seed >> 24);
		}
		kf_psram_write(base, s_test, sizeof s_test);
		memset(s_test, 0xa5, sizeof s_test);
		kf_psram_read(base, s_test, sizeof s_test);
		seed = 0x9e3779b9u;
		int ok = 1;
		for(size_t i = 0; i < sizeof s_test; i++){
			seed = seed * 1664525u + 1013904223u;
			if(s_test[i] != (uint8_t)(seed >> 24)){ ok = 0; break; }
		}
		if(ok){ s_brk = 0; return s_size; }
		s_size = 0;
	}
	return 0;
}
