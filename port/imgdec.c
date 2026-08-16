// port/imgdec.c — bounded-RAM JPEG/PNG/SVG decoder. See imgdec.h.
#include <string.h>
#include <stdlib.h>
#include <malloc.h>
#include "pico/stdlib.h"
#include "../kefyros.h"
#include "imgdec.h"

#include "src/libs/tjpgd/tjpgd.h"          /* JPEG (LVGL's bundled tjpgd) */
#include "src/libs/gif/gifdec.h"           /* GIF first-frame decode */
#include "miniz_tinfl.h"                    /* streaming DEFLATE inflate */

/* nanosvg: header-only, instantiate the implementations here. No file I/O. */
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"

/* ---- heap headroom (mirrors apps/spineko.c) so we never call a decoder that can't fit ---- */
extern char __HeapLimit[], __end__[];
static uint32_t heap_free(void){
	struct mallinfo mi = mallinfo();
	uint32_t cap = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	uint32_t used = (uint32_t)mi.uordblks;
	return used < cap ? cap - used : 0;
}
static inline uint16_t rgb565(int r, int g, int b){
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

/* Small displays do not benefit from a bitmap that consumes the whole SRAM heap.
   Increase the integer subsampling step until both the geometry and caller's byte
   budget fit. This is deliberately shared by JPEG/PNG/GIF so every decoder has the
   same hard peak-output bound. */
static int fit_step(uint32_t w, uint32_t h, int max_w, int max_h, uint32_t max_out,int out_bpp){
	int sw=(int)((w+(uint32_t)max_w-1u)/(uint32_t)max_w);
	int sh=(int)((h+(uint32_t)max_h-1u)/(uint32_t)max_h);
	int step=sw>sh?sw:sh; if(step<1)step=1;
	while(max_out && ((uint64_t)(w/(uint32_t)step)*(h/(uint32_t)step)*(uint32_t)out_bpp > max_out)) step++;
	return step;
}

int kf_img_sniff(const uint8_t *s, int n){
	if(n >= 3 && s[0]==0xFF && s[1]==0xD8 && s[2]==0xFF) return 'j';
	if(n >= 8 && s[0]==0x89 && s[1]=='P' && s[2]=='N' && s[3]=='G') return 'p';
	if(n >= 6 && !memcmp(s,"GIF87a",6)) return 'g';
	if(n >= 6 && !memcmp(s,"GIF89a",6)) return 'g';
	/* SVG: leading '<' (after optional BOM/space) with "<svg" or "<?xml" nearby. */
	for(int i=0;i<n;i++){ if(s[i]==' '||s[i]=='\t'||s[i]=='\r'||s[i]=='\n'||s[i]==0xEF||s[i]==0xBB||s[i]==0xBF) continue;
		if(s[i]=='<'){
			if((n-i>=4 && !memcmp(s+i,"<svg",4)) || (n-i>=5 && !memcmp(s+i,"<?xml",5))) return 's';
		}
		break;
	}
	return 0;
}

/* ============================================================ GIF =====
   Classic sites depend heavily on tiny 88x31 GIF buttons. Decode the first
   composited frame through LVGL's gifdec, then immediately release the source
   and decoder workspace. Animation can come later; rendering frame zero is a
   much better degradation than a filename placeholder. */
static int decode_gif(uint32_t base, uint32_t len, int max_w, int max_h, uint32_t max_out, uint32_t reserve,
                      uint16_t **out, uint8_t **oa, int *ow, int *oh){
	if(len < 14 || len > 512u*1024u) return 0;
	uint8_t hd[10]; kf_psram_read(base, hd, sizeof hd);
	uint32_t W=(uint32_t)hd[6]|((uint32_t)hd[7]<<8);
	uint32_t H=(uint32_t)hd[8]|((uint32_t)hd[9]<<8);
	if(!W||!H||W>2048||H>2048) return 0;
	int step=fit_step(W,H,max_w,max_h,max_out,3);
	int w=(int)(W/step), h=(int)(H/step); if(w<1||h<1)return 0;
	uint32_t outbytes=(uint32_t)w*h*2,abytes=(uint32_t)w*h;
	uint64_t need=(uint64_t)reserve+len+(uint64_t)W*H*5u+outbytes+abytes+8192u;
	if(need > UINT32_MAX || heap_free() < (uint32_t)need) return 0;
	uint8_t *raw=(uint8_t*)malloc(len); if(!raw)return 0;
	kf_psram_read(base,raw,len);
	gd_GIF *gif=gd_open_gif_data(raw);
	if(!gif){free(raw);return 0;}
	int ok=gd_get_frame(gif)>0;
	uint16_t *pix=ok?(uint16_t*)malloc(outbytes):NULL;
	uint8_t *alpha=ok?(uint8_t*)malloc(abytes):NULL;
	if(!pix||!alpha)ok=0;
	if(ok){
		gd_render_frame(gif,gif->canvas);
		for(int y=0;y<h;y++)for(int x=0;x<w;x++){
			uint32_t si=((uint32_t)y*step*W+(uint32_t)x*step)*4u;
			int b=gif->canvas[si],g=gif->canvas[si+1],r=gif->canvas[si+2],a=gif->canvas[si+3];
			pix[y*w+x]=rgb565(r,g,b);alpha[y*w+x]=(uint8_t)a;
		}
	}
	gd_close_gif(gif); free(raw);
	if(!ok){free(pix);free(alpha);return 0;}
	*out=pix;*oa=alpha;*ow=w;*oh=h;return 1;
}

/* ============================================================ JPEG (tjpgd) ===== */
typedef struct { uint32_t base, pos, end; } psrc;
static uint16_t *j_dst; static int j_step, j_w, j_h;

static size_t jpg_in(JDEC *jd, uint8_t *buf, size_t nd){
	psrc *s = (psrc*)jd->device;
	uint32_t avail = s->end - s->pos;
	if((uint32_t)nd > avail) nd = avail;
	if(nd == 0) return 0;
	if(buf){ kf_psram_read(s->base + s->pos, buf, nd); s->pos += (uint32_t)nd; return nd; }
	s->pos += (uint32_t)nd; return nd;
}
static int jpg_out(JDEC *jd, void *bitmap, JRECT *rect){
	(void)jd; const uint8_t *src = (const uint8_t*)bitmap;   /* tjpgd RGB888 as B,G,R */
	int rw = rect->right - rect->left + 1;
	for(int y = rect->top; y <= rect->bottom; y++){
		if(y % j_step) continue; int ty = y / j_step; if(ty >= j_h) continue;
		for(int x = rect->left; x <= rect->right; x++){
			if(x % j_step) continue; int tx = x / j_step; if(tx >= j_w) continue;
			const uint8_t *px = src + (((y-rect->top)*rw) + (x-rect->left))*3;
			j_dst[ty*j_w + tx] = rgb565(px[2], px[1], px[0]);
		}
	}
	return 1;
}
static int decode_jpeg(uint32_t base, uint32_t len, int max_w, int max_h, uint32_t max_out, uint32_t reserve,
                       uint16_t **out, uint8_t **oa, int *ow, int *oh){
	static uint8_t pool[4096];
	psrc s = { base, 0, len };
	JDEC jd;
	if(jd_prepare(&jd, jpg_in, pool, sizeof pool, &s) != JDR_OK) return 0;
	int step = fit_step(jd.width,jd.height,max_w,max_h,max_out,2);
	int w = jd.width/step, h = jd.height/step; if(w<1||h<1) return 0;
	uint32_t bytes = (uint32_t)w*h*2;
	if(heap_free() < reserve + bytes) return 0;
	uint16_t *pix = (uint16_t*)malloc(bytes); if(!pix) return 0;
	j_dst = pix; j_step = step; j_w = w; j_h = h;
	if(jd_decomp(&jd, jpg_out, 0) != JDR_OK){ free(pix); return 0; }
	*out = pix; *oa=NULL; *ow = w; *oh = h; return 1;
}

/* ============================================================ PNG (streaming) ===== */
/* big-endian helpers reading straight from PSRAM */
static uint32_t be32(const uint8_t *p){ return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }

/* sequential reader over the concatenated IDAT data, across chunks, from PSRAM */
typedef struct {
	uint32_t base, flen;
	uint32_t scan;       /* PSRAM offset of next chunk header to consider */
	uint32_t cpos, crem; /* current IDAT data offset + remaining */
	int done;
} idat_rd;
static int idat_next(idat_rd *r){
	for(;;){
		if(r->scan + 8 > r->base + r->flen){ r->done = 1; return 0; }
		uint8_t hb[8]; kf_psram_read(r->scan, hb, 8);
		uint32_t clen = be32(hb);
		const uint8_t *ty = hb+4;
		uint32_t data = r->scan + 8;
		r->scan = data + clen + 4;            /* skip data + CRC */
		if(ty[0]=='I'&&ty[1]=='D'&&ty[2]=='A'&&ty[3]=='T'){ r->cpos = data; r->crem = clen; return 1; }
		if(ty[0]=='I'&&ty[1]=='E'&&ty[2]=='N'&&ty[3]=='D'){ r->done = 1; return 0; }
	}
}
static size_t idat_read(idat_rd *r, uint8_t *buf, size_t want){
	size_t got = 0;
	while(want){
		if(r->crem == 0){ if(!idat_next(r)) break; }
		uint32_t n = r->crem < want ? r->crem : (uint32_t)want;
		kf_psram_read(r->cpos, buf+got, n);
		r->cpos += n; r->crem -= n; got += n; want -= n;
	}
	return got;
}

/* Paeth predictor */
static int paeth(int a, int b, int c){
	int p = a + b - c, pa = abs(p-a), pb = abs(p-b), pc = abs(p-c);
	if(pa<=pb && pa<=pc) return a; if(pb<=pc) return b; return c;
}
static int decode_png(uint32_t base, uint32_t len, int max_w, int max_h, uint32_t max_out, uint32_t reserve,
                      uint16_t **out, uint8_t **oa, int *ow, int *oh){
	uint8_t ih[33];
	if(len < 33) return 0;
	kf_psram_read(base, ih, 33);             /* 8 sig + IHDR chunk (len+type+13 data) */
	if(!(ih[12]=='I'&&ih[13]=='H'&&ih[14]=='D'&&ih[15]=='R')) return 0;
	uint32_t W = be32(ih+16), H = be32(ih+20);
	int bitdepth = ih[24], colortype = ih[25], interlace = ih[28];
	if(bitdepth != 8 || interlace != 0) return 0;              /* v1: 8-bit, non-interlaced */
	int ch;                                                    /* channels = filter bpp */
	switch(colortype){ case 0: ch=1; break; case 2: ch=3; break;
	                   case 3: ch=1; break; case 4: ch=2; break; case 6: ch=4; break;
	                   default: return 0; }
	if(W==0||H==0||W>20000||H>20000) return 0;

	/* palette (type 3) */
	uint8_t pal[256*3]; int paln = 0;
	if(colortype == 3){
		uint32_t scan = base + 8;
		while(scan + 8 <= base + len){
			uint8_t hb[8]; kf_psram_read(scan, hb, 8);
			uint32_t clen = be32(hb);
			if(hb[4]=='P'&&hb[5]=='L'&&hb[6]=='T'&&hb[7]=='E'){
				paln = clen/3; if(paln>256) paln=256;
				kf_psram_read(scan+8, pal, paln*3); break;
			}
			if(hb[4]=='I'&&hb[5]=='D'&&hb[6]=='A'&&hb[7]=='T') break;
			scan += 8 + clen + 4;
		}
		if(paln == 0) return 0;
	}

	int has_alpha=(colortype==4||colortype==6);
	int step = fit_step(W,H,max_w,max_h,max_out,has_alpha?3:2);
	int w = (int)(W/step), h = (int)(H/step); if(w<1||h<1) return 0;

	uint32_t stride = W*ch;                  /* defiltered bytes per scanline */
	uint32_t outbytes = (uint32_t)w*h*2;
	uint32_t alphabytes = has_alpha?(uint32_t)w*h:0;
	/* RAM budget: 32 KB inflate window + input staging + 2 scanlines + output. */
	if(heap_free() < reserve + outbytes + alphabytes + (uint32_t)stride*2 + TINFL_LZ_DICT_SIZE + 8192 + 4096)
		return 0;

	uint8_t  *dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
	uint8_t  *inbuf= (uint8_t*)malloc(8192);
	uint8_t  *cur  = (uint8_t*)malloc(stride);
	uint8_t  *prev = (uint8_t*)malloc(stride);
	uint16_t *pix  = (uint16_t*)malloc(outbytes);
	uint8_t  *alpha= has_alpha?(uint8_t*)malloc(alphabytes):NULL;
	tinfl_decompressor *dec = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
	int ok = (dict && inbuf && cur && prev && pix && dec && (!has_alpha||alpha));
	if(ok){
		memset(prev, 0, stride);
		tinfl_init(dec);
		idat_rd rd = { base, len, base+8, 0, 0, 0 };
		size_t in_have = 0, in_pos = 0, dict_ofs = 0;
		uint32_t row = 0, rowpos = 0;        /* rowpos: bytes filled in the current raw scanline (incl filter byte) */
		uint8_t filt = 0;
		int more_in = 1, fail = 0;
		uint32_t raw_stride = stride + 1;    /* filter byte + pixel bytes */

		while(!fail){
			if(in_pos == in_have && more_in){
				in_have = idat_read(&rd, inbuf, 8192);
				in_pos = 0;
				if(in_have == 0) more_in = 0;
			}
			size_t in_bytes = in_have - in_pos;
			size_t out_bytes = TINFL_LZ_DICT_SIZE - dict_ofs;
			tinfl_status st = tinfl_decompress(dec, inbuf+in_pos, &in_bytes,
				dict, dict+dict_ofs, &out_bytes,
				(more_in ? TINFL_FLAG_HAS_MORE_INPUT : 0) | TINFL_FLAG_PARSE_ZLIB_HEADER);
			in_pos += in_bytes;

			/* consume out_bytes of decompressed output: assemble + de-filter scanlines */
			for(size_t k = 0; k < out_bytes && !fail; k++){
				uint8_t b = dict[(dict_ofs + k) & (TINFL_LZ_DICT_SIZE-1)];
				if(rowpos == 0){ filt = b; rowpos = 1; continue; }
				uint32_t i = rowpos - 1;               /* index into pixel bytes */
				int a = (i >= (uint32_t)ch) ? cur[i-ch] : 0;
				int up = prev[i];
				int ul = (i >= (uint32_t)ch) ? prev[i-ch] : 0;
				int v = b;
				switch(filt){
					case 1: v = b + a; break;
					case 2: v = b + up; break;
					case 3: v = b + ((a+up)>>1); break;
					case 4: v = b + paeth(a, up, ul); break;
					default: break;                    /* 0 = none */
				}
				cur[i] = (uint8_t)v;
				rowpos++;
				if(rowpos == raw_stride){              /* full scanline done */
					if((row % (uint32_t)step) == 0){
						int ty = (int)(row/step);
						if(ty < h){
							for(int tx = 0; tx < w; tx++){
								uint32_t x = (uint32_t)tx*step;
								int r8,g8,b8,a8=255;
								switch(colortype){
									case 0: { int g=cur[x]; r8=g8=b8=g; } break;
									case 4: { int g=cur[x*2];r8=g8=b8=g;a8=cur[x*2+1]; } break;
									case 2: { r8=cur[x*3]; g8=cur[x*3+1]; b8=cur[x*3+2]; } break;
									case 6: { r8=cur[x*4];g8=cur[x*4+1];b8=cur[x*4+2];a8=cur[x*4+3]; } break;
									case 3: { int idx=cur[x]; if(idx>=paln) idx=0;
									          r8=pal[idx*3]; g8=pal[idx*3+1]; b8=pal[idx*3+2]; } break;
									default: r8=g8=b8=0; break;
								}
								pix[ty*w + tx] = rgb565(r8,g8,b8);
								if(alpha)alpha[ty*w+tx]=(uint8_t)a8;
							}
						}
					}
					uint8_t *t = prev; prev = cur; cur = t;   /* swap */
					row++; rowpos = 0;
				}
			}
			dict_ofs = (dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE-1);

			if(st == TINFL_STATUS_DONE) break;
			if(st < TINFL_STATUS_DONE){ fail = 1; }            /* negative = error */
			if(st == TINFL_STATUS_NEEDS_MORE_INPUT && !more_in){ fail = 1; }
		}
		if(fail || row < H) ok = 0;
	}
	free(dict); free(inbuf); free(cur); free(prev); free(dec);
	if(!ok){ free(pix);free(alpha); return 0; }
	*out = pix;*oa=alpha; *ow = w; *oh = h; return 1;
}

/* ============================================================ SVG (nanosvg) ===== */
static int decode_svg(uint32_t base, uint32_t len, int max_w, int max_h, uint32_t max_out, uint32_t reserve,
                      uint16_t **out, uint8_t **oa, int *ow, int *oh){
	if(len == 0 || len > 512u*1024) return 0;                 /* sane text-size cap */
	if(heap_free() < reserve + len + 32768) return 0;          /* room for the text copy + parse */
	char *txt = (char*)malloc(len + 1);
	if(!txt) return 0;
	kf_psram_read(base, (uint8_t*)txt, len); txt[len] = 0;
	NSVGimage *img = nsvgParse(txt, "px", 96.0f);              /* mutates txt */
	free(txt);
	if(!img || img->width < 1 || img->height < 1){ if(img) nsvgDelete(img); return 0; }

	float sx = (float)max_w / img->width, sy = (float)max_h / img->height;
	float scale = sx < sy ? sx : sy; if(scale > 1.0f) scale = 1.0f;
	int w = (int)(img->width*scale + 0.5f), h = (int)(img->height*scale + 0.5f);
	if(w < 1) w = 1; if(h < 1) h = 1;
	if(max_out){
		int extra=1;
		while((uint64_t)(w/extra)*(h/extra)*3u > max_out) extra++;
		if(extra>1){ scale/=(float)extra; w/=extra; h/=extra; if(w<1)w=1;if(h<1)h=1; }
	}

	uint32_t rgba_bytes = (uint32_t)w*h*4, outbytes = (uint32_t)w*h*2,alphabytes=(uint32_t)w*h;
	if(heap_free() < reserve + rgba_bytes + outbytes + alphabytes + 16384){ nsvgDelete(img); return 0; }
	uint8_t  *rgba = (uint8_t*)malloc(rgba_bytes);
	uint16_t *pix  = (uint16_t*)malloc(outbytes);
	uint8_t  *alpha= (uint8_t*)malloc(alphabytes);
	NSVGrasterizer *rast = nsvgCreateRasterizer();
	int ok = (rgba && pix && alpha && rast);
	if(ok){
		memset(rgba, 0, rgba_bytes);
		nsvgRasterize(rast, img, 0, 0, scale, rgba, w, h, w*4);
		for(int i = 0; i < w*h; i++){
			pix[i] = rgb565(rgba[i*4],rgba[i*4+1],rgba[i*4+2]);
			alpha[i]=rgba[i*4+3];
		}
	}
	if(rast) nsvgDeleteRasterizer(rast);
	free(rgba); nsvgDelete(img);
	if(!ok){ free(pix);free(alpha); return 0; }
	*out = pix;*oa=alpha; *ow = w; *oh = h; return 1;
}

/* ============================================================ dispatch ===== */
int kf_img_decode(uint32_t base, uint32_t len, int max_w, int max_h,
                  uint32_t max_out, uint32_t reserve, uint16_t **out,uint8_t **alpha, int *ow, int *oh){
	uint8_t sig[8]; if(len < 8) return 0;
	*alpha=NULL;
	kf_psram_read(base, sig, 8);
	switch(kf_img_sniff(sig, 8)){
		case 'j': return decode_jpeg(base, len, max_w, max_h, max_out, reserve, out,alpha,ow,oh);
		case 'p': return decode_png (base, len, max_w, max_h, max_out, reserve, out,alpha,ow,oh);
		case 'g': return decode_gif (base, len, max_w, max_h, max_out, reserve, out,alpha,ow,oh);
		case 's': return decode_svg (base, len, max_w, max_h, max_out, reserve, out,alpha,ow,oh);
		default:  return 0;
	}
}
