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

int wc_decode_region(const char *path, int scale, int sx0, int sy0, int dstW, int dstH, uint32_t dst){
	return wc_run(path, scale, sx0, sy0, dstW, dstH, dst);
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

#define WC_SCRATCH_MAX 1024   /* decode the crop into a bounded PSRAM scratch (<=2 MB) */

int wc_bake(const char *path, int cx, int cy, int side, int out_px, uint32_t dst){
	/* Decode the crop as large as a bounded scratch allows — full-res (s=0) for crops up to
	   WC_SCRATCH_MAX, a power-of-two descale for bigger ones — then BOX-AVERAGE down to
	   out_px. Area averaging (not nearest) is what keeps the result smooth instead of the
	   chunky 'pixelator' look. Worst-case ~2x-3x supersample feeds the average. */
	int s = 0;
	while(s < 3 && (side >> s) > WC_SCRATCH_MAX) s++;
	int cs = side >> s;
	if(cs < 1) cs = 1;
	int sx0 = cx >> s, sy0 = cy >> s;

	uint32_t mark = kf_psram_brk();
	uint32_t scratch = kf_psram_alloc((uint32_t)cs * cs * 2);
	if(scratch == 0xFFFFFFFFu) return 0;

	/* zero the scratch so any 1px rounding gap at the image edge stays black, not garbage */
	{
		static uint8_t z[512] = { 0 };
		uint32_t tot = (uint32_t)cs * cs * 2, off = 0;
		while(off < tot){ uint32_t n = tot - off; if(n > sizeof z) n = sizeof z; kf_psram_write(scratch + off, z, n); off += n; }
	}

	if(!wc_run(path, s, sx0, sy0, cs, cs, scratch)){ kf_psram_free_to(mark); return 0; }

	/* pass 2: streaming box-average cs x cs -> out_px x out_px. Each source row is added
	   once into the accumulator of its target output row (vertical avg); a precomputed
	   column map buckets source columns (horizontal avg). One PSRAM write per output row. */
	int *colmap = malloc((size_t)cs * sizeof(int));
	uint32_t *aR = calloc(out_px, 4), *aG = calloc(out_px, 4), *aB = calloc(out_px, 4), *cn = calloc(out_px, 4);
	uint16_t *srow = malloc((size_t)cs * 2), *orow = malloc((size_t)out_px * 2);
	int ok = (colmap && aR && aG && aB && cn && srow && orow);
	if(ok) for(int sx = 0; sx < cs; sx++) colmap[sx] = (int)((long)sx * out_px / cs);
	int cur = 0;
	for(int sy = 0; ok && sy < cs; sy++){
		int oy = (int)((long)sy * out_px / cs);
		if(oy != cur){                          /* finalize the completed output row */
			for(int ox = 0; ox < out_px; ox++){ uint32_t c = cn[ox] ? cn[ox] : 1;
				orow[ox] = (uint16_t)(((aR[ox]/c) << 11) | ((aG[ox]/c) << 5) | (aB[ox]/c)); }
			kf_psram_write(dst + (size_t)cur * out_px * 2, orow, (uint32_t)out_px * 2);
			memset(aR, 0, out_px*4); memset(aG, 0, out_px*4); memset(aB, 0, out_px*4); memset(cn, 0, out_px*4);
			cur = oy;
		}
		kf_psram_read(scratch + (size_t)sy * cs * 2, srow, (uint32_t)cs * 2);
		for(int sx = 0; sx < cs; sx++){ int ox = colmap[sx]; uint16_t v = srow[sx];
			aR[ox] += (v >> 11) & 0x1F; aG[ox] += (v >> 5) & 0x3F; aB[ox] += v & 0x1F; cn[ox]++; }
	}
	if(ok){                                     /* final output row */
		for(int ox = 0; ox < out_px; ox++){ uint32_t c = cn[ox] ? cn[ox] : 1;
			orow[ox] = (uint16_t)(((aR[ox]/c) << 11) | ((aG[ox]/c) << 5) | (aB[ox]/c)); }
		kf_psram_write(dst + (size_t)cur * out_px * 2, orow, (uint32_t)out_px * 2);
	}
	free(colmap); free(aR); free(aG); free(aB); free(cn); free(srow); free(orow);
	kf_psram_free_to(mark);
	return ok;
}
