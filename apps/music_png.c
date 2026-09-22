// Embedded FLAC PNG cover -> bounded RGB565 thumbnail. The compressed image
// stays on SD; only the inflate dictionary, two scanlines, and 1 KB input live
// in SRAM. This deliberately avoids LVGL's full-image PNG allocation.
#include "music_png.h"
#include "miniz_tinfl.h"
#include <stdlib.h>
#include <string.h>

extern void *kapi_idle_scratch(size_t bytes);

#define PNG_INPUT 1024
#define PNG_MAX_DIM 8192u
#define PNG_MAX_ROW 32768u

typedef struct {
	FILE *f;
	long start, len;
	long scan, data;
	uint32_t left;
	int ended;
} png_idat;

static uint32_t be32(const uint8_t *p){
	return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3];
}

/* All offsets here are relative to the PICTURE image, never to the FLAC. */
static int chunk_at(FILE *f, long start, long len, long pos,
                    uint8_t head[8], uint32_t *size, long *next){
	if(pos < 0 || pos > len || len - pos < 12) return 0;
	if(fseek(f, start + pos, SEEK_SET) || fread(head, 1, 8, f) != 8) return 0;
	uint32_t n = be32(head);
	if((uint64_t)n > (uint64_t)(len - pos - 12)) return 0;
	*size = n;
	*next = pos + 12 + (long)n;
	return 1;
}

static int idat_next(png_idat *r){
	uint8_t h[8]; uint32_t n; long next;
	while(chunk_at(r->f, r->start, r->len, r->scan, h, &n, &next)){
		long data = r->scan + 8;
		r->scan = next;
		if(!memcmp(h+4, "IDAT", 4)){
			r->data = data; r->left = n;
			if(n) return 1;
		} else if(!memcmp(h+4, "IEND", 4)) break;
	}
	r->ended = 1;
	return 0;
}

static size_t idat_read(png_idat *r, uint8_t *buf, size_t want){
	size_t got = 0;
	while(want){
		if(!r->left && !idat_next(r)) break;
		size_t n = r->left < want ? r->left : want;
		if(fseek(r->f, r->start + r->data, SEEK_SET)) break;
		size_t k = fread(buf + got, 1, n, r->f);
		r->data += (long)k; r->left -= (uint32_t)k;
		got += k; want -= k;
		if(k != n) break;
	}
	return got;
}

static int paeth(int a, int b, int c){
	int p=a+b-c, da=abs(p-a), db=abs(p-b), dc=abs(p-c);
	return (da<=db && da<=dc) ? a : (db<=dc ? b : c);
}

static uint16_t pixel565(int r, int g, int b, int a){
	/* The Music cover box is KF_BG_DEEP (0x0a0806). Flatten alpha without
	   allocating an extra A8 plane or leaving transparent edges black. */
	if(a != 255){
		r=(r*a + 10*(255-a) + 127)/255;
		g=(g*a +  8*(255-a) + 127)/255;
		b=(b*a +  6*(255-a) + 127)/255;
	}
	return (uint16_t)(((r&0xf8)<<8)|((g&0xfc)<<3)|(b>>3));
}

int music_png_thumb(FILE *f, long start, long len, uint16_t *dst,
                    int max_edge, int *out_w, int *out_h){
	if(!f || !dst || !out_w || !out_h || start < 0 || len < 33 || max_edge < 1) return 0;
	uint8_t sig[8], hb[8], ih[13]; uint32_t n; long next;
	if(fseek(f, start, SEEK_SET) || fread(sig, 1, 8, f) != 8 ||
	   memcmp(sig, "\x89PNG\r\n\x1a\n", 8)) return 0;
	if(!chunk_at(f, start, len, 8, hb, &n, &next) || n != 13 ||
	   memcmp(hb+4, "IHDR", 4) || fread(ih, 1, 13, f) != 13) return 0;
	uint32_t W=be32(ih), H=be32(ih+4);
	int depth=ih[8], color=ih[9], interlace=ih[12], ch;
	if(!W || !H || W>PNG_MAX_DIM || H>PNG_MAX_DIM || depth!=8 ||
	   ih[10]!=0 || ih[11]!=0 || interlace!=0) return 0;
	switch(color){ case 0: ch=1; break; case 2: ch=3; break;
	               case 3: ch=1; break; case 4: ch=2; break;
	               case 6: ch=4; break; default: return 0; }
	uint32_t stride=W*(uint32_t)ch;
	if(stride>PNG_MAX_ROW) return 0;
	uint32_t step=(W>H?W:H)+(uint32_t)max_edge-1;
	step/=(uint32_t)max_edge;
	int w=(int)((W+step-1)/step), h=(int)((H+step-1)/step);
	if(w>max_edge || h>max_edge) return 0;

	/* Palette and tRNS appear before IDAT. Only 8-bit PNGs are accepted. */
	uint8_t pal[768], trans[256]; int pal_n=0, trans_n=0, have_idat=0;
	memset(trans, 255, sizeof trans);
	long scan=next;
	for(;;){
		if(!chunk_at(f, start, len, scan, hb, &n, &next)) return 0;
		if(!memcmp(hb+4, "IDAT", 4)){ have_idat=1; break; }
		if(!memcmp(hb+4, "IEND", 4)) break;
		if(!memcmp(hb+4, "PLTE", 4) && n && n<=sizeof pal && n%3==0){
			if(fread(pal, 1, n, f)!=n) return 0;
			pal_n=(int)n/3;
		} else if(!memcmp(hb+4, "tRNS", 4) && color==3 && n<=sizeof trans){
			if(fread(trans, 1, n, f)!=n) return 0;
			trans_n=(int)n;
		}
		scan=next;
	}
	if(!have_idat || (color==3 && !pal_n)) return 0;

	/* The 48 KB KAPI arena is idle while this built-in runs. Borrow it only
	   for this synchronous decode, keeping the 32 KB inflate window and Huffman
	   state out of the shared heap needed by the audio ring. */
	uint8_t *work=kapi_idle_scratch(TINFL_LZ_DICT_SIZE+sizeof(tinfl_decompressor));
	uint8_t *dict=work, *input=malloc(PNG_INPUT);
	uint8_t *cur=malloc(stride), *prev=malloc(stride);
	tinfl_decompressor *dec=work?(tinfl_decompressor*)(work+TINFL_LZ_DICT_SIZE):NULL;
	int ok=dict && input && cur && prev && dec;
	if(ok){
		memset(prev, 0, stride);
		tinfl_init(dec);
		png_idat rd={f,start,len,scan,0,0,0};
		size_t in_have=0, in_pos=0, dict_ofs=0;
		uint32_t row=0, rowpos=0;
		uint8_t filter=0;
		int more=1;
		while(ok){
			if(in_pos==in_have && more){
				in_have=idat_read(&rd, input, PNG_INPUT);
				in_pos=0;
				if(!in_have) more=0;
			}
			size_t in_bytes=in_have-in_pos;
			size_t out_bytes=TINFL_LZ_DICT_SIZE-dict_ofs;
			tinfl_status st=tinfl_decompress(dec, input+in_pos, &in_bytes,
				dict, dict+dict_ofs, &out_bytes,
				(more?TINFL_FLAG_HAS_MORE_INPUT:0)|TINFL_FLAG_PARSE_ZLIB_HEADER);
			in_pos+=in_bytes;
			for(size_t k=0;k<out_bytes && ok;k++){
				uint8_t b=dict[(dict_ofs+k)&(TINFL_LZ_DICT_SIZE-1)];
				if(row>=H){ ok=0; break; }
				if(rowpos==0){ if(b>4){ok=0;break;} filter=b; rowpos=1; continue; }
				uint32_t i=rowpos-1;
				int a=i>=(uint32_t)ch?cur[i-ch]:0;
				int up=prev[i], ul=i>=(uint32_t)ch?prev[i-ch]:0;
				int v=b;
				switch(filter){ case 1: v+=a; break; case 2: v+=up; break;
				               case 3: v+=(a+up)>>1; break;
				               case 4: v+=paeth(a,up,ul); break; }
				cur[i]=(uint8_t)v;
				if(++rowpos==stride+1){
					if(row%step==0){
						uint32_t ty=row/step;
						for(int tx=0;tx<w;tx++){
							uint32_t x=(uint32_t)tx*step;
							int r,g,bl,alpha=255;
							switch(color){
							case 0: r=g=bl=cur[x]; break;
							case 2: r=cur[x*3];g=cur[x*3+1];bl=cur[x*3+2];break;
							case 3: { int ix=cur[x]; if(ix>=pal_n){ok=0;break;}
							          r=pal[ix*3];g=pal[ix*3+1];bl=pal[ix*3+2];
							          if(ix<trans_n) alpha=trans[ix]; } break;
							case 4: r=g=bl=cur[x*2];alpha=cur[x*2+1];break;
							default:r=cur[x*4];g=cur[x*4+1];bl=cur[x*4+2];alpha=cur[x*4+3];break;
							}
							if(!ok) break;
							dst[ty*(uint32_t)w+(uint32_t)tx]=pixel565(r,g,bl,alpha);
						}
					}
					uint8_t *tmp=prev;prev=cur;cur=tmp;
					row++;rowpos=0;
				}
			}
			dict_ofs=(dict_ofs+out_bytes)&(TINFL_LZ_DICT_SIZE-1);
			if(st==TINFL_STATUS_DONE) break;
			if(st<TINFL_STATUS_DONE || (st==TINFL_STATUS_NEEDS_MORE_INPUT && !more) ||
			   (!in_bytes && !out_bytes && !more)) ok=0;
		}
		if(row!=H || rowpos!=0) ok=0;
	}
	free(input);free(cur);free(prev);
	if(ok){ *out_w=w; *out_h=h; }
	return ok;
}
