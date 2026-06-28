// apps/wallconv.c — on-device JPEG ingest for wallpapers.
//
// Streams a JPEG through the bundled TJpgDec decoder (16px MCU bands, ~8 KB SRAM
// work pool) and writes cropped/scaled RGB565 straight into PSRAM. A multi-megapixel
// source therefore becomes a 320x320 panel wallpaper WITHOUT ever materialising a
// full frame in the ~180 KB SRAM heap (even one 320x320 RGB565 frame is 200 KB).
//
// TJpgDec gives us two things that make this fit:
//   * a per-MCU output callback (we keep only the pixels inside the crop window), and
//   * 1/2 . 1/4 . 1/8 prescaling during decode (JD_USE_SCALE) so huge sources are
//     cheap to walk.
// JD_FORMAT is 0 (RGB888) project-wide (lv_tjpgd depends on it), so the callback
// converts 888 -> 565 itself.
//
// Powers the wallpaper app's live crop/zoom editor (apps/wallpaper.c).
#include "../kefyros.h"
#include "wallconv.h"
#include "lvgl/src/libs/tjpgd/tjpgd.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WC_POOL 8192          /* TJpgDec work pool; 4 KB is the lib's rec, 8 KB is safe with scaling */

typedef struct {
	FILE *f;                  /* JPEG input stream */
	int sx0, sy0;             /* window top-left in the scaled image */
	int dstW, dstH;           /* window size == PSRAM dest dims */
	uint32_t dst;             /* PSRAM byte offset of the dest (RGB565, stride dstW*2) */
} wc_ctx;

/* TJpgDec input: pull bytes from the FILE (buf!=NULL) or skip ndata bytes (buf==NULL). */
static size_t wc_in(JDEC *jd, uint8_t *buf, size_t n){
	wc_ctx *c = (wc_ctx*)jd->device;
	if(buf) return fread(buf, 1, n, c->f);
	return fseek(c->f, (long)n, SEEK_CUR) == 0 ? n : 0;
}

/* TJpgDec output: `bmp` is a block covering output rect [left..right]x[top..bottom]
   (inclusive, in scaled-image coords). Copy the part overlapping our window as RGB565.
   Two non-obvious TJpgDec facts (both verified against a host decode of real photos):
     * the pixel order is B,G,R (NOT R,G,B) — Cb drives byte0, Cr drives byte2; and
     * for a DESCALED decode (scale>0) the valid pixels sit at the top-left of rows whose
       stride is the UNSCALED MCU width (msx*8), not the scaled rect width. lv_tjpgd never
       descales (it always runs scale 0) so this fork's descaler stride is otherwise untested. */
static int wc_out(JDEC *jd, void *bmp, JRECT *r){
	wc_ctx *c = (wc_ctx*)jd->device;
	const uint8_t *p = (const uint8_t*)bmp;
	int rowstride = (jd->msx * 8) * 3;          /* unscaled MCU width — see note above */
	uint16_t line[32];                          /* an MCU is at most 16 px wide; 32 is slack */
	for(int y = r->top; y <= r->bottom; y++){
		int dy = y - c->sy0;
		if(dy < 0 || dy >= c->dstH) continue;
		const uint8_t *row = p + (size_t)(y - r->top) * rowstride;
		int run = 0, first = -1;
		for(int x = r->left; x <= r->right; x++){
			int dx = x - c->sx0;
			if(dx < 0 || dx >= c->dstW) continue;   /* the in-window dx are contiguous */
			const uint8_t *px = row + (size_t)(x - r->left) * 3;   /* px = B,G,R */
			line[run] = (uint16_t)(((px[2] & 0xF8) << 8) | ((px[1] & 0xFC) << 3) | (px[0] >> 3));
			if(first < 0) first = dx;
			run++;
		}
		if(run) kf_psram_write(c->dst + ((size_t)dy * c->dstW + first) * 2, line, (uint32_t)run * 2);
	}
	return 1;   /* 1 = keep decoding */
}

static int wc_run(const char *path, int scale, int sx0, int sy0, int dstW, int dstH, uint32_t dst){
	wc_ctx c = { 0 };
	c.f = fopen(path, "rb");
	if(!c.f) return 0;
	c.sx0 = sx0; c.sy0 = sy0; c.dstW = dstW; c.dstH = dstH; c.dst = dst;
	void *pool = malloc(WC_POOL);
	if(!pool){ fclose(c.f); return 0; }
	JDEC jd;
	int ok = 0;
	if(jd_prepare(&jd, wc_in, pool, WC_POOL, &c) == JDR_OK &&
	   jd_decomp(&jd, wc_out, (uint8_t)scale) == JDR_OK)
		ok = 1;
	free(pool);
	fclose(c.f);
	return ok;
}

int wc_dims(const char *path, int *w, int *h){
	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	wc_ctx c = { 0 }; c.f = f;
	void *pool = malloc(WC_POOL);
	if(!pool){ fclose(f); return 0; }
	JDEC jd;
	int ok = 0;
	if(jd_prepare(&jd, wc_in, pool, WC_POOL, &c) == JDR_OK){ *w = jd.width; *h = jd.height; ok = 1; }
	free(pool);
	fclose(f);
	return ok;
}

/* Bound the decode scratch by BYTES (not max-dim) so most sources decode at full
   resolution (s=0) before averaging — half-res-first is what made thumbnails mushy. */
#define WC_SCRATCH_BYTES (4u*1024*1024)
#define WC_CW_MAX  1664   /* cap the decoded scratch WIDTH (raise s past it) so the static
                             per-row buffers below are bounded; a 4 MB square crop is <=1448 wide */
#define WC_OUT_MAX 320    /* max output width (the panel) */

/* Working buffers are STATIC, not malloc'd: the ~150 KB SRAM heap is often fragmented while
   the editor is open, and a width-proportional malloc was failing for LARGE crops (small
   crops malloc less, which is why only big crops failed -> "convert failed" -> user quits).
   ~17 KB .bss, always reserved. */
static int      wc_colmap[WC_CW_MAX];
static uint16_t wc_srow[WC_CW_MAX];
static uint32_t wc_aR[WC_OUT_MAX], wc_aG[WC_OUT_MAX], wc_aB[WC_OUT_MAX], wc_cn[WC_OUT_MAX];
static uint16_t wc_orow[WC_OUT_MAX];

int wc_render(const char *path, int rx0, int ry0, int rw, int rh, int outW, int outH, uint32_t dst){
	/* Box-average the native rectangle (rx0,ry0,rw,rh) of `path` down to outW x outH RGB565
	   at `dst`. Decode at the largest 1/2^s whose scratch fits WC_SCRATCH_BYTES and whose width
	   fits WC_CW_MAX (preferring full res for sharpness), then area-average — smooth, not
	   nearest-neighbour blocky. Iterates OUTPUT rows so it's robust to down- and up-scaling. */
	if(outW > WC_OUT_MAX) return 0;
	int s = 0;
	while(s < 3 && ((uint32_t)(rw >> s) * (uint32_t)(rh >> s) * 2u > WC_SCRATCH_BYTES
	             || (rw >> s) > WC_CW_MAX)) s++;

	uint32_t mark = kf_psram_brk(), scr;
	int cw, ch, sx0, sy0;
	for(;;){                                  /* shrink (raise s) until the scratch alloc fits */
		cw = rw >> s; ch = rh >> s; if(cw < 1) cw = 1; if(ch < 1) ch = 1;
		if(cw > WC_CW_MAX) cw = WC_CW_MAX;    /* only bites on extreme aspect ratios */
		sx0 = rx0 >> s; sy0 = ry0 >> s;
		scr = kf_psram_alloc((uint32_t)cw * ch * 2);
		if(scr != 0xFFFFFFFFu) break;
		if(s >= 3) return 0;                  /* won't fit even at 1/8 */
		s++;
	}

	/* zero the scratch so any 1px edge-rounding gap stays black, not garbage */
	{
		static uint8_t z[512] = { 0 };
		uint32_t tot = (uint32_t)cw * ch * 2, off = 0;
		while(off < tot){ uint32_t n = tot - off; if(n > sizeof z) n = sizeof z; kf_psram_write(scr + off, z, n); off += n; }
	}
	if(!wc_run(path, s, sx0, sy0, cw, ch, scr)){ kf_psram_free_to(mark); return 0; }

	for(int sx = 0; sx < cw; sx++) wc_colmap[sx] = (int)((long)sx * outW / cw);
	for(int oy = 0; oy < outH; oy++){
		int a = (int)((long)oy * ch / outH), b = (int)((long)(oy+1) * ch / outH);
		if(b <= a) b = a + 1; if(b > ch) b = ch;
		for(int ox = 0; ox < outW; ox++){ wc_aR[ox] = wc_aG[ox] = wc_aB[ox] = wc_cn[ox] = 0; }
		for(int sy = a; sy < b; sy++){
			kf_psram_read(scr + (size_t)sy * cw * 2, wc_srow, (uint32_t)cw * 2);
			for(int sx = 0; sx < cw; sx++){ int ox = wc_colmap[sx]; uint16_t v = wc_srow[sx];
				wc_aR[ox] += (v >> 11) & 0x1F; wc_aG[ox] += (v >> 5) & 0x3F; wc_aB[ox] += v & 0x1F; wc_cn[ox]++; }
		}
		for(int ox = 0; ox < outW; ox++){
			if(wc_cn[ox]) wc_orow[ox] = (uint16_t)(((wc_aR[ox]/wc_cn[ox]) << 11) | ((wc_aG[ox]/wc_cn[ox]) << 5) | (wc_aB[ox]/wc_cn[ox]));
			else          wc_orow[ox] = ox ? wc_orow[ox-1] : 0;   /* fill rare horizontal up-scale gaps */
		}
		kf_psram_write(dst + (size_t)oy * outW * 2, wc_orow, (uint32_t)outW * 2);
	}
	kf_psram_free_to(mark);
	return 1;
}
