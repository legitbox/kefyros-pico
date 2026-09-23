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
#include "pico/mutex.h"
#include "pico/time.h"
#include <stdlib.h>

#define AUDIO_R_PIN 26                             /* PWM slice 5 ch A -> right */
#define AUDIO_L_PIN 27                             /* PWM slice 5 ch B -> left  */
#define AUDIO_SLICE 5u
#define DBUF        256                            /* stereo frames per ping-pong half */
#define RING_N      4096                           /* stereo-frame ring (~93 ms @44.1k:
                                                      deep enough to ride out SD jitter, but
                                                      only 16 KB of heap while playing — 8192
                                                      (32 KB) made playback malloc-fail on a
                                                      heap already holding music's JP font +
                                                      cover thumb (stuck play button, no sound) */

/* runtime carrier resolution (chosen from clk_sys at start). bits in [11..13]. */
static uint32_t pwm_top   = 4095;                  /* (1<<bits)-1 */
static uint32_t pwm_mid   = 2048;                  /* (top+1)/2   */
static uint32_t pwm_bits  = 12;
#define SILENCE()   (pwm_mid | (pwm_mid << 16))

/* ring holds pre-packed duty frames: low 16 = ch A (GP26/R), high 16 = ch B (GP27/L).
   Ordinary callers malloc it only while playing. Music supplies storage in its
   idle KAPI arena, so its ring does not consume the shared heap. */
static uint32_t pp[2][DBUF];
static uint32_t *ring;                               /* RING_N frames; NULL when idle */
static int ring_owned;                               /* malloc-owned, not caller storage */
static uint32_t ring_n = RING_N;                     /* active power-of-two capacity */
static volatile uint32_t r_w = 0, r_r = 0;          /* free-running; count = r_w - r_r */
static int dch = -1, dtimer = -1;
static volatile int cur = 0;
static volatile int running = 0;
static volatile int bt_route = 0;
static int sample_rate = 44100;
static uint64_t bt_phase;
static mutex_t audio_writer_lock;
static volatile kf_audio_stats_t s_stats;
static volatile uint32_t s_last_dma_irq_us;
static volatile int s_had_data;

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
	uint32_t missing = 0;
	for(int i = 0; i < DBUF; i++){
		if(ring && r_r != r_w){ b[i] = ring[r_r & (ring_n-1u)]; r_r++; }
		else { b[i] = SILENCE(); missing++; }
	}
	if(s_had_data){
		if(missing){ s_stats.pwm_underrun_events++; s_stats.pwm_underrun_frames += missing; }
		uint32_t buffered = ring_count();
		if(buffered < s_stats.min_buffered_frames) s_stats.min_buffered_frames = buffered;
	}
}

static void dma_isr(void){
	if(dma_hw->ints1 & (1u << dch)){
		dma_hw->ints1 = 1u << dch;                  /* ack */
		if(!running) return;                        /* stopped: don't restart/refill */
		uint32_t now = time_us_32();
		if(s_last_dma_irq_us){
			uint32_t gap = now - s_last_dma_irq_us;
			if(gap > s_stats.max_dma_irq_gap_us) s_stats.max_dma_irq_gap_us = gap;
		}
		s_last_dma_irq_us = now;
		s_stats.dma_irqs++;
		int played = cur;
		cur ^= 1;
		dma_channel_set_read_addr(dch, pp[cur], false);
		dma_channel_set_trans_count(dch, DBUF, true);   /* restart on the prefilled half */
		fill(pp[played]);                           /* refill the half we just drained */
	}
}

void kf_audio_init(void){
	mutex_init(&audio_writer_lock);
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
	if(bt_route) return;
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_PWM);   /* slice keeps running; pin resumes at 50% */
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_PWM);
}

/* Park the speaker pins high-Z for the WHOLE duration of CPU sleep (not just the relock).
   The PWM carrier frequency scales with clk_sys: carrier = clk_sys / (TOP+1). At the normal
   400 MHz clock a 13-bit carrier sits ~48 kHz (inaudible), but the idle PWM slice keeps
   running, so at the 150 MHz sleep clock that same carrier drops to ~18 kHz — a steady
   high-pitch whine through the amp the whole time we're asleep. Audio apps are tagged
   no-sleep, so the slice is always idle here; just float the pins until wake, then restore.
   (kf_clock_wake()'s clock_apply already calls kf_audio_clock_change_end, so unpark mainly
   covers the no-clock-change paths and makes the restore explicit.) */
void kf_audio_idle_park(void){
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_SIO); gpio_set_dir(AUDIO_R_PIN, GPIO_IN);
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_SIO); gpio_set_dir(AUDIO_L_PIN, GPIO_IN);
}
void kf_audio_idle_unpark(void){
	if(bt_route) return;
	gpio_set_function(AUDIO_R_PIN, GPIO_FUNC_PWM);
	gpio_set_function(AUDIO_L_PIN, GPIO_FUNC_PWM);
}

static void start_pwm_dma(int hz){
	kf_audio_idle_unpark();
	pwm_set_enabled(AUDIO_SLICE, true);
	pick_resolution();
	uint32_t sysclk = clock_get_hz(clk_sys);
	uint32_t X = (uint32_t)(((uint64_t)65535 * hz) / sysclk);
	if(X < 1) X = 1; else if(X > 65535) X = 65535;
	uint32_t Y = (uint32_t)(((uint64_t)X * sysclk + hz/2) / hz);
	if(Y < 1) Y = 1; else if(Y > 65535) Y = 65535;
	dma_timer_set_fraction(dtimer, (uint16_t)X, (uint16_t)Y);
	fill(pp[0]); fill(pp[1]);
	cur = 0;
	dma_channel_set_read_addr(dch, pp[0], false);
	dma_channel_set_trans_count(dch, DBUF, true);
}

static int audio_start_buffered(int hz, int ring_frames, uint32_t *external){
	if(hz < 8000) hz = 8000; else if(hz > 48000) hz = 48000;
	if(running){ dma_channel_abort(dch); running = 0; }
	if(ring_frames < 1024) ring_frames = 1024;
	if(ring_frames > RING_N) ring_frames = RING_N;
	/* Keep masking cheap and unambiguous: round down to a supported power of two. */
	uint32_t cap = 1024;
	while((cap << 1) <= (uint32_t)ring_frames) cap <<= 1;
	if(ring && (ring_n != cap || (external && ring != external) || (!external && !ring_owned))){
		if(ring_owned) free(ring);
		ring = NULL; ring_owned = 0;
	}
	ring_n = cap;
	if(external){
		if((uintptr_t)external & 3u) return 0;
		ring = external; ring_owned = 0;
	} else if(!ring){
		ring = malloc(sizeof(uint32_t) * ring_n);
		if(!ring) return 0;
		ring_owned = 1;
	}
	r_w = r_r = 0;
	s_stats = (kf_audio_stats_t){0};
	s_stats.min_buffered_frames = ring_n;
	s_last_dma_irq_us = 0;
	s_had_data = 0;
	bt_phase = 0;
	sample_rate = hz;
	ns_reset();
	if(bt_route){
		pwm_set_enabled(AUDIO_SLICE, false);
		kf_audio_idle_park();
	} else start_pwm_dma(hz);
	running = 1;
	return 1;
}

int kf_audio_start_buffered(int hz, int ring_frames){
	return audio_start_buffered(hz, ring_frames, NULL);
}
int kf_audio_start_buffered_external(int hz, int ring_frames, uint32_t *storage){
	if(!storage) return 0;
	return audio_start_buffered(hz, ring_frames, storage);
}
void kf_audio_start(int hz){ (void)kf_audio_start_buffered(hz, RING_N); }

void kf_audio_stop(void){
	if(!running) return;
	mutex_enter_blocking(&audio_writer_lock);
	/* mask DMA_IRQ_1 at the NVIC so dma_isr can't preempt us mid-free and touch
	   `ring` after it's freed; the channel is aborted and the pending flag cleared
	   before we re-enable, so no stale completion restarts playback. */
	irq_set_enabled(DMA_IRQ_1, false);
	dma_channel_abort(dch);
	running = 0;
	r_w = r_r = 0;
	pwm_hw->slice[AUDIO_SLICE].cc = SILENCE();   /* silence */
	if(ring_owned) free(ring);
	ring = NULL; ring_owned = 0;
	dma_hw->ints1 = 1u << dch;                   /* clear any pending completion */
	irq_set_enabled(DMA_IRQ_1, true);
	mutex_exit(&audio_writer_lock);
}

int  kf_audio_running(void){ return running; }
int  kf_audio_space(void){ return ring ? (int)(ring_n - (r_w - r_r)) : 0; }
int  kf_audio_buffered(void){ return ring ? (int)(r_w - r_r) : 0; }   /* frames queued but unplayed */
void kf_audio_get_stats(kf_audio_stats_t *out){
	if(!out) return;
	out->pwm_underrun_events = s_stats.pwm_underrun_events;
	out->pwm_underrun_frames = s_stats.pwm_underrun_frames;
	out->bt_underrun_frames = s_stats.bt_underrun_frames;
	out->dma_irqs = s_stats.dma_irqs;
	out->max_dma_irq_gap_us = s_stats.max_dma_irq_gap_us;
	out->min_buffered_frames = s_had_data ? s_stats.min_buffered_frames : 0;
}

/* drop buffered audio + reset the shaper (used right after a seek so the new
   position is heard immediately; DMA keeps running and plays silence until refilled). */
void kf_audio_flush(void){
	mutex_enter_blocking(&audio_writer_lock);
	r_w = r_r = 0; bt_phase = 0; ns_reset();
	mutex_exit(&audio_writer_lock);
}

/* The public producer API remains stable. The ring changes representation only
   when the sink changes: PWM duty for local output, packed signed PCM for A2DP. */
void kf_audio_set_bt_route(int enabled){
	enabled = !!enabled;
	if(bt_route == enabled) return;
	mutex_enter_blocking(&audio_writer_lock);
	irq_set_enabled(DMA_IRQ_1, false);
	dma_channel_abort(dch);
	dma_hw->ints1 = 1u << dch;
	bt_route = enabled;
	r_w = r_r = 0; bt_phase = 0; ns_reset();
	if(enabled){
		pwm_hw->slice[AUDIO_SLICE].cc = SILENCE();
		pwm_set_enabled(AUDIO_SLICE, false);
		kf_audio_idle_park();
	} else if(running) start_pwm_dma(sample_rate);
	else kf_audio_idle_unpark();
	irq_set_enabled(DMA_IRQ_1, true);
	mutex_exit(&audio_writer_lock);
}

int kf_audio_bt_route(void){ return bt_route; }

/* Pull stereo PCM for the Bluetooth encoder. A phase accumulator resamples
   arbitrary app rates to the headset's 44.1/48 kHz rate without changing game
   timing. Silence is emitted on underrun; no stale samples are replayed. */
int kf_audio_bt_read(int16_t *out, int frames, int out_rate){
	if(!bt_route || !running || !ring || out_rate <= 0) return 0;
	uint64_t step = ((uint64_t)(uint32_t)sample_rate << 32) / (uint32_t)out_rate;
	for(int i=0; i<frames; ++i){
		if(r_w - r_r < 2){
			out[2*i] = out[2*i+1] = 0;
			if(s_had_data) s_stats.bt_underrun_frames++;
			continue;
		}
		__dmb();
		uint32_t a = ring[r_r & (ring_n-1u)];
		uint32_t b = ring[(r_r+1u) & (ring_n-1u)];
		uint32_t frac = (uint32_t)(bt_phase >> 16);
		int32_t al = (int16_t)a, ar = (int16_t)(a >> 16);
		int32_t bl = (int16_t)b, br = (int16_t)(b >> 16);
		out[2*i] = (int16_t)(al + (((int64_t)(bl-al) * frac) >> 16));
		out[2*i+1] = (int16_t)(ar + (((int64_t)(br-ar) * frac) >> 16));
		bt_phase += step;
		uint32_t advance = (uint32_t)(bt_phase >> 32);
		bt_phase &= 0xffffffffu;
		r_r += advance;
	}
	return frames;
}

/* push interleaved full-scale stereo frames (L,R,L,R; 32-bit, left-justified for the
   source bit depth as dr_flac's s32 output is). Returns frames accepted (< frames if
   the ring is full). ch A = GP26 = right = st[2i+1]; ch B = GP27 = left = st[2i]. */
int kf_audio_write_s32(const int32_t *st, int frames){
	if(!ring) return 0;
	mutex_enter_blocking(&audio_writer_lock);
	int w = 0;
	while(w < frames && ring_count() < ring_n){
		uint32_t packed;
		if(bt_route) packed = (uint16_t)(st[2*w] >> 16) | ((uint32_t)(uint16_t)(st[2*w+1] >> 16) << 16);
		else packed = duty_s32(1, st[2*w+1]) | (duty_s32(0, st[2*w]) << 16);
		ring[r_w & (ring_n-1u)] = packed;
		__dmb();
		r_w++; w++;
	}
	mutex_exit(&audio_writer_lock);
	if(w) s_had_data = 1;
	return w;
}

/* legacy 16-bit path (kept for any non-FLAC caller): widen to full-scale and reuse
   the same shaper so even 16-bit sources get dithered down cleanly. */
int kf_audio_write(const int16_t *st, int frames){
	if(!ring) return 0;
	mutex_enter_blocking(&audio_writer_lock);
	int w = 0;
	while(w < frames && ring_count() < ring_n){
		uint32_t packed;
		if(bt_route) packed = (uint16_t)st[2*w] | ((uint32_t)(uint16_t)st[2*w+1] << 16);
		else packed = duty_s32(1, (int32_t)st[2*w+1] << 16) | (duty_s32(0, (int32_t)st[2*w] << 16) << 16);
		ring[r_w & (ring_n-1u)] = packed;
		__dmb();
		r_w++; w++;
	}
	mutex_exit(&audio_writer_lock);
	if(w) s_had_data = 1;
	return w;
}

/* FM synths produce mono. Convert once and drive both PWM channels from the
   same duty value instead of running two identical noise shapers per frame. */
int kf_audio_write_mono(const int16_t *mono, int frames){
	if(!ring || !mono) return 0;
	mutex_enter_blocking(&audio_writer_lock);
	int w = 0;
	while(w < frames && ring_count() < ring_n){
		uint32_t packed;
		if(bt_route){
			uint32_t sample = (uint16_t)mono[w];
			packed = sample | (sample << 16);
		} else {
			uint32_t duty = duty_s32(0, (int32_t)mono[w] * 65536);
			packed = duty | (duty << 16);
		}
		ring[r_w & (ring_n-1u)] = packed;
		__dmb();
		r_w++; w++;
	}
	mutex_exit(&audio_writer_lock);
	if(w) s_had_data = 1;
	return w;
}
