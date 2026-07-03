// ui/sfx.c — OS sound effects: short .wav clips from the SD card, played on UI events.
//
// Drop PCM WAVs in /kefyros/sfx/<name>.wav and call kf_sfx_play("name"). The clip is
// streamed a chunk at a time from sfx_poll() (the main loop), so even a ~1 s effect needs
// no big buffer. Design rules:
//   * Gated by the deskconf "sfx" toggle (default on; Settings has a switch).
//   * Never barges in on music — if another source already owns the PWM speaker, skip.
//   * A new effect preempts a still-playing effect.
//   * Missing / unsupported file -> silent no-op (so authoring is drop-in).
// Supports PCM (format 1), 8-bit unsigned or 16-bit signed, mono or stereo, up to 48 kHz.
// Mono is duplicated to both channels; stereo plays as-is.
#include "../kefyros.h"
#include "deskconf.h"
#include <stdio.h>
#include <string.h>

#define SFX_DIR   "/kefyros/sfx"
#define SFX_CHUNK 512                 /* stereo frames pumped per poll */

static FILE    *sfx_f   = NULL;       /* open clip; NULL = idle */
static uint32_t sfx_left = 0;         /* bytes of PCM data left to read */
static int      sfx_ch   = 1;
static int      sfx_bits = 16;
static int      sfx_active = 0;       /* 1 = WE own kf_audio (vs. music) */
static int16_t  sfx_out[SFX_CHUNK*2]; /* interleaved L,R scratch */
static uint8_t  sfx_raw[SFX_CHUNK*4]; /* raw file bytes (<= stereo 16-bit per frame) */

static uint32_t rd32(const uint8_t *p){ return p[0]|(p[1]<<8)|(p[2]<<16)|((uint32_t)p[3]<<24); }
static uint32_t rd16(const uint8_t *p){ return (uint32_t)(p[0]|(p[1]<<8)); }

static void sfx_close(void){
	if(sfx_f){ fclose(sfx_f); sfx_f = NULL; }
	if(sfx_active){ kf_audio_stop(); sfx_active = 0; }
	sfx_left = 0;
}

/* Parse a RIFF/WAVE header; on success sets sfx_f positioned at the PCM data and returns
   the format via the out-params. Walks the chunk list so a fmt/fact/LIST ordering is fine. */
static int wav_open(const char *path, uint32_t *rate, int *ch, int *bits, uint32_t *datalen){
	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	uint8_t h[12];
	if(fread(h,1,12,f) != 12 || memcmp(h,"RIFF",4) || memcmp(h+8,"WAVE",4)){ fclose(f); return 0; }
	uint32_t r = 0, dlen = 0; int c = 0, b = 0, have_fmt = 0;
	uint8_t chk[8];
	while(fread(chk,1,8,f) == 8){
		uint32_t sz = rd32(chk+4);
		if(!memcmp(chk,"fmt ",4)){
			uint8_t fmt[16]; uint32_t want = sz < 16 ? sz : 16;
			if(fread(fmt,1,want,f) != want){ fclose(f); return 0; }
			if(rd16(fmt) != 1){ fclose(f); return 0; }      /* PCM only */
			c = (int)rd16(fmt+2); r = rd32(fmt+4); b = (int)rd16(fmt+14);
			have_fmt = 1;
			if(sz > want) fseek(f, (long)(sz - want), SEEK_CUR);
		} else if(!memcmp(chk,"data",4)){
			if(!have_fmt || c < 1){ fclose(f); return 0; }
			*rate = r; *ch = c; *bits = b; *datalen = sz;
			sfx_f = f; return 1;                              /* positioned at PCM data */
		} else {
			fseek(f, (long)(sz + (sz & 1)), SEEK_CUR);        /* skip chunk (word-padded) */
		}
	}
	fclose(f); return 0;                                      /* no data chunk */
}

void kf_sfx_play(const char *name){
	if(!deskconf_get_int("sfx", 1)) return;            /* effects disabled */
	if(kf_audio_running() && !sfx_active) return;       /* music owns the speaker — don't barge in */
	sfx_close();                                        /* preempt any still-playing effect */
	char path[160];
	snprintf(path, sizeof path, "%s/%s.wav", SFX_DIR, name);
	uint32_t rate, dlen; int ch, bits;
	if(!wav_open(path, &rate, &ch, &bits, &dlen)) return;     /* missing/odd file -> silent */
	if(bits != 16 && bits != 8){ sfx_close(); return; }
	if(ch != 1 && ch != 2){ sfx_close(); return; }            /* mono or stereo only */
	sfx_ch = ch; sfx_bits = bits; sfx_left = dlen;
	kf_audio_start((int)rate);
	sfx_active = 1;
}

void sfx_poll(void){
	if(!sfx_f) return;
	if(sfx_left){
		int space = kf_audio_space();
		if(space > 0){
			int frames = space < SFX_CHUNK ? space : SFX_CHUNK;
			int bps = (sfx_bits/8) * sfx_ch;                 /* bytes per source frame */
			uint32_t want = (uint32_t)frames * (uint32_t)bps;
			if(want > sfx_left) want = sfx_left;
			if(want > sizeof sfx_raw) want = (sizeof sfx_raw / (uint32_t)bps) * (uint32_t)bps;  /* never overrun sfx_raw */
			size_t got = fread(sfx_raw, 1, want, sfx_f);
			if(got >= (size_t)bps){
				sfx_left -= (uint32_t)got;
				int n = (int)(got / (size_t)bps);
				for(int i = 0; i < n; i++){
					int16_t l, r;
					if(sfx_bits == 16){
						const int16_t *s = (const int16_t*)(sfx_raw + i*bps);
						l = s[0]; r = (sfx_ch >= 2) ? s[1] : s[0];
					} else {                                  /* 8-bit unsigned -> signed 16 */
						const uint8_t *s = sfx_raw + i*bps;
						l = (int16_t)(((int)s[0] - 128) << 8);
						r = (sfx_ch >= 2) ? (int16_t)(((int)s[1] - 128) << 8) : l;
					}
					sfx_out[2*i] = l; sfx_out[2*i+1] = r;
				}
				kf_audio_write(sfx_out, n);
			} else sfx_left = 0;                              /* short read / EOF */
		}
	}
	if(sfx_left == 0 && kf_audio_buffered() == 0) sfx_close();   /* drained -> release the speaker */
}
