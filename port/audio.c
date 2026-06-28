// port/audio.c — PWM audio for the PicoCalc speaker/headphones (FLAC-grade output).
//
// The PicoCalc routes Pico GP26 (R) + GP27 (L) — both on PWM slice 5 — through an
// RC low-pass into the PAM8302 amp (the STM32 auto-enables the amp on headphone
// detect). We run the slice as a high-frequency carrier and DMA per-sample duty
// values into its compare register, paced by a DMA pacing timer at the audio sample
// rate. A single DMA channel ping-pongs between two small buffers; its completion
// IRQ (DMA_IRQ_1 — free; neither the LCD nor SD driver use DMA) restarts the
// prefilled buffer and refills the just-finished one from a mono PCM ring.
//
// MAX QUALITY on a PWM DAC (the hard hardware ceiling — there is no I2S):
//   * Carrier resolution scales with clk_sys. At the 400 MHz boost clock we use a
//     13-bit carrier (TOP=8191 -> ~48.8 kHz, above hearing) for an extra bit over
//     the old fixed 12-bit. Picked at kf_audio_start() from the live clk_sys.
//   * Sample -> duty uses a per-channel 2nd-order error-feedback NOISE SHAPER with
//     TPDF dither, fed from the decoder's FULL-resolution samples (kf_audio_write_s32
//     keeps 24-bit FLAC intact). This pushes requantization noise above the audible
//     band, recovering perceived dynamic range well past the raw bit count.
#include "../kefyros.h"
#include "board.h"
#include "hardware/pwm.h"
#include "hardware/dma.h"
#include "hardware/irq.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include <stdlib.h>

#define AUDIO_R_PIN 26                             /* PWM slice 5 ch A -> right */
#define AUDIO_L_PIN 27                             /* PWM slice 5 ch B -> left  */
#define AUDIO_SLICE 5u
#define DBUF        256                            /* stereo frames per ping-pong half */
#define RING_N      8192                           /* stereo-frame ring (~186 ms @44.1k:
                                                      deep enough to ride out a cover-art
                                                      JPEG decode without an underrun) */

/* runtime carrier resolution (chosen from clk_sys at start). bits in [11..13]. */
static uint32_t pwm_top   = 4095;                  /* (1<<bits)-1 */
static uint32_t pwm_mid   = 2048;                  /* (top+1)/2   */
static uint32_t pwm_bits  = 12;
#define SILENCE()   (pwm_mid | (pwm_mid << 16))

/* ring holds pre-packed duty frames: low 16 = ch A (GP26/R), high 16 = ch B (GP27/L).
   Lazily allocated at kf_audio_start() and freed at kf_audio_stop() — at 32 KB it's
   the single biggest idle .bss hog, and it's only touched while audio is playing.
   Keeping it off the heap floor when idle gives the rest of the OS (e.g. the calc
   plotter's full-screen canvas) the contiguous room it needs. */
static uint32_t pp[2][DBUF];
static uint32_t *ring;                               /* RING_N frames; NULL when idle */
static volatile uint32_t r_w = 0, r_r = 0;          /* free-running; count = r_w - r_r */
static int dch = -1, dtimer = -1;
static volatile int cur = 0;
static volatile int running = 0;

/* ---- per-channel 2nd-order noise shaper + TPDF dither state (ch0=L, ch1=R) ---- */
static int32_t  ns_e1[2], ns_e2[2];                 /* error feedback (full-scale domain) */
static uint32_t ns_lcg[2] = { 0x12345678u, 0x9e3779b9u };

static inline uint32_t lcg(int ch){                 /* fast per-channel PRNG for dither */
	ns_lcg[ch] = ns_lcg[ch] * 1664525u + 1013904223u;
	return ns_lcg[ch];
}
static void ns_reset(void){
	ns_e1[0]=ns_e1[1]=ns_e2[0]=ns_e2[1]=0;
	ns_lcg[0]=0x12345678u; ns_lcg[1]=0x9e3779b9u;
}

/* one channel: full-scale int32 sample -> dithered, noise-shaped PWM duty [0..TOP].
   2nd-order shaper H(z)=(1-z^-1)^2 (feedback = 2*e1 - e2). Quantization is to the
   top `pwm_bits` of the 32-bit sample; the discarded low bits become the error that
   is fed forward, so quiet detail survives as out-of-band noise instead of vanishing. */
static inline uint32_t duty_s32(int ch, int32_t x){
	const int shift = 32 - (int)pwm_bits;           /* e.g. 19 for 13-bit */
	const int32_t step = 1 << shift;
	/* TPDF dither: difference of two uniforms in [0,step) -> triangular +-(step-1). */
	int32_t dith = (int32_t)(lcg(ch) & (uint32_t)(step-1)) - (int32_t)(lcg(ch) & (uint32_t)(step-1));
	int64_t y = (int64_t)x + 2*(int64_t)ns_e1[ch] - (int64_t)ns_e2[ch] + dith;
	/* quantize to `pwm_bits`: signed value q in ~[-2^(bits-1), 2^(bits-1)) */
	int32_t half = 1 << (pwm_bits-1);
	int32_t q = (int32_t)((y + (1<<(shift-1))) >> shift);
	if(q >  half-1) q =  half-1;
	if(q < -half)   q = -half;
	int32_t err = (int32_t)(y - ((int64_t)q << shift)); /* requantization error */
	ns_e2[ch] = ns_e1[ch];
	ns_e1[ch] = err;
	int32_t d = q + (int32_t)pwm_mid;
	if(d < 0) d = 0; else if(d > (int)pwm_top) d = (int)pwm_top;
	return (uint32_t)d;
}

static inline uint32_t ring_count(void){ return r_w - r_r; }

/* refill a ping-pong half from the ring (IRQ context); silence on underrun */
static void fill(uint32_t *b){
	for(int i = 0; i < DBUF; i++){
		if(ring && r_r != r_w){ b[i] = ring[r_r & (RING_N-1)]; r_r++; }
		else b[i] = SILENCE();
	}
}

static void dma_isr(void){
	if(dma_hw->ints1 & (1u << dch)){
		dma_hw->ints1 = 1u << dch;                  /* ack */
		if(!running) return;                        /* stopped: don't restart/refill */
		int played = cur;
		cur ^= 1;
		dma_channel_set_read_addr(dch, pp[cur], false);
		dma_channel_set_trans_count(dch, DBUF, true);   /* restart on the prefilled half */
		fill(pp[played]);                           /* refill the half we just drained */
	}
}

void kf_audio_init(void){
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_PWM);
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_PWM);
	pwm_config c = pwm_get_default_config();
	pwm_config_set_wrap(&c, pwm_top);
	pwm_config_set_clkdiv(&c, 1.0f);
	pwm_init(AUDIO_SLICE, &c, true);
	pwm_hw->slice[AUDIO_SLICE].cc = SILENCE();   /* idle at mid-rail */

	dch    = dma_claim_unused_channel(true);
	dtimer = dma_claim_unused_timer(true);
	dma_channel_config dc = dma_channel_get_default_config(dch);
	channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
	channel_config_set_read_increment(&dc, true);
	channel_config_set_write_increment(&dc, false);
	channel_config_set_dreq(&dc, dma_get_timer_dreq(dtimer));
	dma_channel_configure(dch, &dc, &pwm_hw->slice[AUDIO_SLICE].cc, pp[0], DBUF, false);
	dma_channel_set_irq1_enabled(dch, true);
	irq_set_exclusive_handler(DMA_IRQ_1, dma_isr);
	irq_set_enabled(DMA_IRQ_1, true);
}

/* choose the carrier resolution: as many bits as keep the carrier >= ~40 kHz at the
   current clk_sys, capped at 13 (the extra resolution past 13 buys nothing audible
   and would drop the carrier into the filtered band). carrier = clk_sys / (TOP+1). */
static void pick_resolution(void){
	uint32_t sysclk = clock_get_hz(clk_sys);
	uint32_t bits = 13;
	while(bits > 11 && (sysclk >> bits) < 40000u) bits--;   /* keep carrier >= 40 kHz */
	pwm_bits = bits;
	pwm_top  = (1u << bits) - 1u;
	pwm_mid  = (pwm_top + 1u) / 2u;
	pwm_set_wrap(AUDIO_SLICE, pwm_top);
}

/* Park the speaker pins (high-Z) across a clk_sys change, then restore them. During a PLL
   relock, set_sys_clock_khz() briefly runs clk_sys off the ~12 MHz reference, which drops
   the PWM carrier from ~98 kHz into the audible band for a few ms — an audible "pop"/chirp
   through the amp. Tristating the pins for that window keeps the burst off the speaker; the
   RC reconstruction filter holds its ~mid-rail charge meanwhile. Called by clock_apply(). */
void kf_audio_clock_change_begin(void){
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_SIO); gpio_set_dir(AUDIO_R_PIN, GPIO_IN);
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_SIO); gpio_set_dir(AUDIO_L_PIN, GPIO_IN);
}
void kf_audio_clock_change_end(void){
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_PWM);   /* slice keeps running; pin resumes at 50% */
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_PWM);
}

void kf_audio_start(int hz){
	if(hz < 8000) hz = 8000; else if(hz > 48000) hz = 48000;
	if(running){ dma_channel_abort(dch); running = 0; }
	if(!ring){ ring = malloc(sizeof(uint32_t) * RING_N); if(!ring) return; }  /* no RAM -> stay silent */
	r_w = r_r = 0;
	ns_reset();
	pick_resolution();

	/* DMA pacing timer DREQ = clk_sys * X/Y; pick X/Y (16-bit) closest to hz/clk. */
	uint32_t sysclk = clock_get_hz(clk_sys);
	uint32_t X = (uint32_t)(((uint64_t)65535 * hz) / sysclk);
	if(X < 1) X = 1; else if(X > 65535) X = 65535;
	uint32_t Y = (uint32_t)(((uint64_t)X * sysclk + hz/2) / hz);
	if(Y < 1) Y = 1; else if(Y > 65535) Y = 65535;
	dma_timer_set_fraction(dtimer, (uint16_t)X, (uint16_t)Y);

	fill(pp[0]); fill(pp[1]);                        /* prime (silence; ring empty) */
	cur = 0;
	dma_channel_set_read_addr(dch, pp[0], false);
	dma_channel_set_trans_count(dch, DBUF, true);   /* go */
	running = 1;
}

void kf_audio_stop(void){
	if(!running) return;
	/* mask DMA_IRQ_1 at the NVIC so dma_isr can't preempt us mid-free and touch
	   `ring` after it's freed; the channel is aborted and the pending flag cleared
	   before we re-enable, so no stale completion restarts playback. */
	irq_set_enabled(DMA_IRQ_1, false);
	dma_channel_abort(dch);
	running = 0;
	r_w = r_r = 0;
	pwm_hw->slice[AUDIO_SLICE].cc = SILENCE();   /* silence */
	free(ring); ring = NULL;
	dma_hw->ints1 = 1u << dch;                   /* clear any pending completion */
	irq_set_enabled(DMA_IRQ_1, true);
}

int  kf_audio_running(void){ return running; }
int  kf_audio_space(void){ return ring ? (int)(RING_N - (r_w - r_r)) : 0; }
int  kf_audio_buffered(void){ return ring ? (int)(r_w - r_r) : 0; }   /* frames queued but unplayed */

/* drop buffered audio + reset the shaper (used right after a seek so the new
   position is heard immediately; DMA keeps running and plays silence until refilled). */
void kf_audio_flush(void){ r_w = r_r = 0; ns_reset(); }

/* push interleaved full-scale stereo frames (L,R,L,R; 32-bit, left-justified for the
   source bit depth as dr_flac's s32 output is). Returns frames accepted (< frames if
   the ring is full). ch A = GP26 = right = st[2i+1]; ch B = GP27 = left = st[2i]. */
int kf_audio_write_s32(const int32_t *st, int frames){
	if(!ring) return 0;
	int w = 0;
	while(w < frames && ring_count() < RING_N){
		uint32_t a = duty_s32(1, st[2*w+1]);        /* right */
		uint32_t b = duty_s32(0, st[2*w]);          /* left  */
		ring[r_w & (RING_N-1)] = a | (b << 16);
		r_w++; w++;
	}
	return w;
}

/* legacy 16-bit path (kept for any non-FLAC caller): widen to full-scale and reuse
   the same shaper so even 16-bit sources get dithered down cleanly. */
int kf_audio_write(const int16_t *st, int frames){
	if(!ring) return 0;
	int w = 0;
	while(w < frames && ring_count() < RING_N){
		uint32_t a = duty_s32(1, (int32_t)st[2*w+1] << 16);
		uint32_t b = duty_s32(0, (int32_t)st[2*w]   << 16);
		ring[r_w & (RING_N-1)] = a | (b << 16);
		r_w++; w++;
	}
	return w;
}
