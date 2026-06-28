// apps/music.c — FLAC player ("iPod-style"). Recursively indexes EVERY *.flac under
// /kefyros/music (all subdirs) into one global list, decodes bit-exact FLAC through
// dr_flac into the noise-shaped PWM DAC (port/audio.c), and shows extracted metadata
// + embedded/sidecar cover art. Browse/seek/shuffle/repeat; grabs raw keys like the
// old player. Decoding is pumped from music_poll() in the main loop.
//
// Encoding: the SD card library is mostly Japanese (Splatoon OSTs). FatFs now returns
// UTF-8 (FF_LFN_UNICODE=2) so readdir/fopen/LVGL all agree; metadata labels use the
// baked full-JIS font (g_jpfont, ui/lv_font_jp_16.c).
//
// Memory: the track index lives in PSRAM (block store) — see rec_get/rec_put. The
// index + sort order persist across app opens (static), so re-opening is instant; F5
// forces a rescan. dr_flac decode buffers + the cover thumbnail are small + static.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_FLAC_NO_OGG
#include "dr_flac/dr_flac.h"

#include "src/libs/tjpgd/tjpgd.h"    /* raw scaled-stream JPEG decode for cover art */
#include "src/font/lv_binfont_loader.h"  /* JP font loaded from SD at runtime, NOT baked
                                            into firmware: a ~1 MB baked font overflowed the
                                            400 MHz flash/XIP boot envelope and bricked boot.
                                            ~35 KB on SD -> RAM only while the app is open. */

/* ------------------------------------------------------------------ config -- */
#define MUSDIR      "/kefyros/music"
#define MAX_TRACKS  1500
#define REC_PATH    224            /* relative path under MUSDIR (UTF-8) */
#define REC_TXT     96             /* title/artist/album (UTF-8, may truncate) */
#define VIS_ROWS    11             /* visible wheel rows (odd; center is the slit) */
#define ROW_CENTER  (VIS_ROWS/2)
#define LEFTW       150            /* left wheel column width */
#define THUMB       120            /* cover thumbnail max edge (px) */
#define CHUNK       512            /* PCM frames per decode pump step */

/* one library entry; stored in PSRAM as a flat array of these. */
typedef struct {
	char     path[REC_PATH];       /* relative to MUSDIR, e.g. "Album/01 x.flac" */
	char     title[REC_TXT];
	char     artist[REC_TXT];
	char     album[REC_TXT];
	uint32_t duration;             /* seconds (0 until detail parsed) */
	uint32_t rate;                 /* sample rate (0 until detail) */
	uint32_t cover_off;            /* embedded JPEG byte offset in the file (0=none) */
	uint32_t cover_len;            /* embedded JPEG byte length */
	uint16_t track;                /* track number from filename/tag */
	uint8_t  bits;                 /* bit depth */
	uint8_t  channels;
	uint8_t  detail;               /* 1 once flac_meta() filled rate/dur/cover */
	uint8_t  pad[3];
} rec_t;

/* ------------------------------------------------------------------ state --- */
static int       g_active;
static const lv_font_t *g_jpfont;         /* loaded from SD (A:/kefyros/fonts/jp.bin); KF_FONT fallback */
static int       g_built;                 /* index built this boot */
static uint32_t  g_idx_base = 0xFFFFFFFFu; /* PSRAM base of the rec array */
static int       g_n;                     /* number of tracks */
static int       g_full;                  /* hit MAX_TRACKS */
static uint16_t  g_sorted[MAX_TRACKS];    /* display order -> rec index */
static uint16_t  g_order[MAX_TRACKS];     /* play order (display positions) */
static int       g_order_pos;             /* cursor in g_order */
static int       g_sel;                   /* highlighted display position */
static int       g_play_disp = -1;        /* display position currently loaded (-1 none) */
static int       g_shuffle, g_repeat;     /* repeat: 0 off, 1 all, 2 one */

/* decode/playback */
static drflac   *g_dec;
static int       g_paused;
static uint32_t  g_native;                /* source sample rate */
static int       g_ch, g_decim, g_playhz;
static uint64_t  g_pos, g_total;          /* native PCM frames */
static int32_t   g_raw[CHUNK*2], g_st[CHUNK*2], g_dec2[CHUNK*2];

/* cover thumbnail (RGB565); heap-allocated while the app is open, freed on exit so
   the ~29 KB doesn't permanently shrink the shared heap for other apps. */
static uint16_t     *g_thumb;
static lv_image_dsc_t g_cover_dsc;
static int           g_cover_ok;
static int           g_focus_dirty;       /* selection changed -> (re)load detail+cover */
static uint32_t      g_focus_t;

/* UI objects (rebuilt each open) */
static lv_obj_t *g_scr, *g_cont, *g_box, *g_rows[VIS_ROWS];
static lv_obj_t *g_cover, *g_noart, *g_l_title, *g_l_artist, *g_l_album, *g_l_badge;
static lv_obj_t *g_bar, *g_l_time, *g_l_stat, *g_l_empty;

/* =================================================================== PSRAM == */
static void rec_get(int i, rec_t *r){ kf_psram_read(g_idx_base + (uint32_t)i*sizeof(rec_t), r, sizeof *r); }
static void rec_put(int i, const rec_t *r){ kf_psram_write(g_idx_base + (uint32_t)i*sizeof(rec_t), r, sizeof *r); }

/* =================================================================== utils == */
static int has_flac(const char *n){ const char *d = strrchr(n,'.'); return d && !strcasecmp(d,".flac"); }

/* copy up to dstsz-1 bytes of UTF-8 src, never splitting a multibyte sequence. */
static void utf8_cpy(char *dst, const char *src, int dstsz){
	int n = 0; int max = dstsz - 1;
	while(src[n] && n < max) n++;
	/* if we stopped mid-sequence, back off to a boundary */
	if(src[n]){
		while(n > 0 && (src[n] & 0xC0) == 0x80) n--;     /* trailing continuation bytes */
		if(n > 0 && (src[n-1] & 0x80)) n--;              /* a lead byte whose tail was cut */
	}
	memcpy(dst, src, n); dst[n] = 0;
}

/* parse "NN Artist - Title.flac" into track/artist/title (filename fallback). */
static void parse_name(const char *fname, rec_t *r){
	char stem[REC_PATH]; utf8_cpy(stem, fname, sizeof stem);
	char *dot = strrchr(stem, '.'); if(dot) *dot = 0;
	const char *p = stem;
	r->track = 0;
	while(isdigit((unsigned char)*p)){ r->track = r->track*10 + (*p - '0'); p++; }
	while(*p==' '||*p=='.'||*p=='-'||*p=='_') p++;
	const char *sep = strstr(p, " - ");
	if(sep){
		int al = (int)(sep - p);
		if(al > REC_TXT-1) al = REC_TXT-1;
		char tmp[REC_TXT]; memcpy(tmp, p, al); tmp[al] = 0; utf8_cpy(r->artist, tmp, sizeof r->artist);
		utf8_cpy(r->title, sep+3, sizeof r->title);
	} else {
		r->artist[0] = 0;
		utf8_cpy(r->title, p, sizeof r->title);
	}
	if(!r->title[0]) utf8_cpy(r->title, fname, sizeof r->title);
}

/* album = the track's immediate parent directory name (within MUSDIR). */
static void set_album(const char *rel, rec_t *r){
	const char *slash = strrchr(rel, '/');
	if(!slash){ r->album[0] = 0; return; }     /* track sits directly in MUSDIR */
	char dir[REC_PATH]; int dl = (int)(slash - rel);
	if(dl > REC_PATH-1) dl = REC_PATH-1;
	memcpy(dir, rel, dl); dir[dl] = 0;
	const char *base = strrchr(dir, '/');
	utf8_cpy(r->album, base ? base+1 : dir, sizeof r->album);
}

/* =================================================== FLAC metadata (detail) = */
static uint32_t rd_be32(FILE *f){ uint8_t b[4]; if(fread(b,1,4,f)!=4) return 0; return ((uint32_t)b[0]<<24)|(b[1]<<16)|(b[2]<<8)|b[3]; }
static uint32_t le32(const uint8_t *p){ return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }

static void take_tag(rec_t *r, const char *key, const char *val){
	if(!strcasecmp(key,"TITLE"))       utf8_cpy(r->title, val, sizeof r->title);
	else if(!strcasecmp(key,"ARTIST")) utf8_cpy(r->artist, val, sizeof r->artist);
	else if(!strcasecmp(key,"ALBUM"))  utf8_cpy(r->album, val, sizeof r->album);
	else if(!strcasecmp(key,"ALBUMARTIST") && !r->artist[0]) utf8_cpy(r->artist, val, sizeof r->artist);
	else if(!strcasecmp(key,"TRACKNUMBER")){ int t = atoi(val); if(t>0) r->track = (uint16_t)t; }
}

/* read STREAMINFO (rate/bits/dur), VORBIS_COMMENT (tags), locate PICTURE (cover).
   Reads only the metadata blocks at the head of the file — never the audio. */
static void flac_meta(const char *full, rec_t *r){
	FILE *f = fopen(full, "rb"); if(!f) return;
	char sig[4];
	if(fread(sig,1,4,f)!=4 || memcmp(sig,"fLaC",4)){ fclose(f); return; }
	int last = 0;
	while(!last){
		uint8_t h[4]; if(fread(h,1,4,f)!=4) break;
		last = h[0] & 0x80; int type = h[0] & 0x7f;
		uint32_t len = ((uint32_t)h[1]<<16)|((uint32_t)h[2]<<8)|h[3];
		long start = ftell(f);
		if(type == 0 && len >= 18){                         /* STREAMINFO */
			uint8_t s[18];
			if(fread(s,1,18,f)==18){
				r->rate = ((uint32_t)s[10]<<12)|((uint32_t)s[11]<<4)|(s[12]>>4);
				r->channels = (uint8_t)(((s[12]>>1)&7)+1);
				r->bits = (uint8_t)(((((s[12]&1)<<4)|(s[13]>>4)))+1);
				uint64_t total = ((uint64_t)(s[13]&0xF)<<32)|((uint64_t)s[14]<<24)|((uint32_t)s[15]<<16)|((uint32_t)s[16]<<8)|s[17];
				r->duration = r->rate ? (uint32_t)(total / r->rate) : 0;
			}
		} else if(type == 4 && len <= 32768){               /* VORBIS_COMMENT */
			uint8_t *buf = malloc(len);
			if(buf && fread(buf,1,len,f)==len){
				uint32_t off = 0;
				if(len >= 4){ uint32_t vl = le32(buf); off = 4 + vl; }
				if(off + 4 <= len){
					uint32_t cnt = le32(buf+off); off += 4;
					for(uint32_t i=0;i<cnt && off+4<=len;i++){
						uint32_t cl = le32(buf+off); off += 4;
						if(off + cl > len) break;
						char kv[300]; uint32_t cc = cl < sizeof kv-1 ? cl : sizeof kv-1;
						memcpy(kv, buf+off, cc); kv[cc] = 0; off += cl;
						char *eq = strchr(kv, '='); if(!eq) continue;
						*eq = 0; take_tag(r, kv, eq+1);
					}
				}
			}
			free(buf);
		} else if(type == 6){                               /* PICTURE */
			(void)rd_be32(f);                               /* picture type */
			uint32_t ml = rd_be32(f);
			char mime[32]; uint32_t mc = ml < sizeof mime-1 ? ml : sizeof mime-1;
			if(fread(mime,1,mc,f)!=mc){ fclose(f); return; }
			mime[mc] = 0; if(ml > mc) fseek(f, ml-mc, SEEK_CUR);
			uint32_t dl = rd_be32(f); fseek(f, dl, SEEK_CUR);   /* description */
			(void)rd_be32(f); (void)rd_be32(f); (void)rd_be32(f); (void)rd_be32(f); /* w,h,depth,colors */
			uint32_t datalen = rd_be32(f);
			long dataoff = ftell(f);
			if(strstr(mime,"jpeg") || strstr(mime,"jpg")){ r->cover_off = (uint32_t)dataoff; r->cover_len = datalen; }
		}
		fseek(f, start + (long)len, SEEK_SET);              /* next block */
	}
	fclose(f);
	r->detail = 1;
}

/* ===================================================== recursive indexing == */
static void make_full(char *out, int outsz, const char *rel){
	if(rel && rel[0]) snprintf(out, outsz, "%s/%s", MUSDIR, rel);
	else              snprintf(out, outsz, "%s", MUSDIR);
}

static void scan_progress(int n){
	if(!g_l_empty) return;
	lv_label_set_text_fmt(g_l_empty, "Indexing music...\n%d tracks", n);
	lv_refr_now(NULL);
}

/* path comparator over PSRAM recs -> albums grouped, zero-padded track order. */
static int cmp_disp(const void *a, const void *b){
	rec_t ra, rb;
	rec_get(*(const uint16_t*)a, &ra);
	rec_get(*(const uint16_t*)b, &rb);
	return strcmp(ra.path, rb.path);
}

static void build_index(void){
	g_n = 0; g_full = 0;
	if(g_idx_base == 0xFFFFFFFFu){
		g_idx_base = kf_psram_alloc((uint32_t)MAX_TRACKS * sizeof(rec_t));
		if(g_idx_base == 0xFFFFFFFFu){ g_built = 1; return; }   /* no PSRAM */
	}
	mkdir(MUSDIR, 0755);

	/* iterative DFS over a stack of relative dir paths (no deep recursion). Heap-
	   allocated for the scan only — freed before returning. */
	#define STACKN 64
	char (*stack)[REC_PATH] = malloc((size_t)STACKN * REC_PATH);
	if(!stack){ g_built = 1; return; }
	int sp = 0; stack[sp++][0] = 0;                      /* "" = MUSDIR root */
	char full[REC_PATH + 24];

	while(sp > 0 && g_n < MAX_TRACKS){
		char rel[REC_PATH]; utf8_cpy(rel, stack[--sp], sizeof rel);
		make_full(full, sizeof full, rel);
		DIR *d = opendir(full); if(!d) continue;
		struct dirent *e;
		while((e = readdir(d)) && g_n < MAX_TRACKS){
			if(e->d_name[0] == '.') continue;
			char child[REC_PATH];
			if(rel[0]) snprintf(child, sizeof child, "%s/%s", rel, e->d_name);
			else       snprintf(child, sizeof child, "%s", e->d_name);
			if(e->d_type == DT_DIR){
				if(sp < STACKN) utf8_cpy(stack[sp++], child, REC_PATH);
			} else if(has_flac(e->d_name)){
				rec_t r; memset(&r, 0, sizeof r);
				utf8_cpy(r.path, child, sizeof r.path);
				parse_name(e->d_name, &r);
				set_album(child, &r);
				rec_put(g_n, &r);
				g_sorted[g_n] = (uint16_t)g_n;
				g_n++;
				if((g_n & 31) == 0) scan_progress(g_n);
			}
		}
		closedir(d);
	}
	free(stack);
	#undef STACKN
	if(g_n >= MAX_TRACKS) g_full = 1;
	qsort(g_sorted, g_n, sizeof g_sorted[0], cmp_disp);
	g_built = 1;
}

/* ensure rate/duration/cover are loaded for a display position (lazy). */
static void ensure_detail(int disp){
	if(disp < 0 || disp >= g_n) return;
	int ri = g_sorted[disp];
	rec_t r; rec_get(ri, &r);
	if(r.detail) return;
	char full[REC_PATH + 24]; make_full(full, sizeof full, r.path);
	flac_meta(full, &r);
	rec_put(ri, &r);
}

/* ============================================================ cover art ===== */
typedef struct { FILE *f; long start; long pos; long end; } cov_src;
static int g_cstep, g_tw, g_th;

static size_t cover_in(JDEC *jd, uint8_t *buf, size_t nd){
	cov_src *s = (cov_src*)jd->device;
	long avail = s->end - s->pos;
	if((long)nd > avail) nd = (size_t)(avail < 0 ? 0 : avail);
	if(nd == 0) return 0;
	if(buf){
		fseek(s->f, s->start + s->pos, SEEK_SET);
		size_t r = fread(buf, 1, nd, s->f); s->pos += (long)r; return r;
	}
	s->pos += (long)nd; return nd;                       /* skip */
}
static int cover_out(JDEC *jd, void *bitmap, JRECT *rect){
	(void)jd;
	const uint8_t *src = (const uint8_t*)bitmap;          /* RGB888 (JD_FORMAT=0) */
	int rw = rect->right - rect->left + 1;
	for(int y = rect->top; y <= rect->bottom; y++){
		if(y % g_cstep) continue;
		int ty = y / g_cstep; if(ty >= g_th) continue;
		for(int x = rect->left; x <= rect->right; x++){
			if(x % g_cstep) continue;
			int tx = x / g_cstep; if(tx >= g_tw) continue;
			const uint8_t *px = src + (((y-rect->top)*rw) + (x-rect->left))*3;
			/* tjpgd (JD_FORMAT=0) emits B,G,R -> px[0]=B px[1]=G px[2]=R. */
			g_thumb[ty*g_tw + tx] = (uint16_t)(((px[2]&0xF8)<<8)|((px[1]&0xFC)<<3)|(px[0]>>3));
		}
	}
	return 1;
}
/* decode a JPEG (embedded region or whole sidecar file) into g_thumb; 1 on success. */
static int decode_jpeg(FILE *f, long start, long len){
	static uint8_t pool[4096];
	if(!g_thumb) return 0;
	cov_src src = { f, start, 0, start + len };
	JDEC jd;
	if(jd_prepare(&jd, cover_in, pool, sizeof pool, &src) != JDR_OK) return 0;
	int mx = jd.width > jd.height ? jd.width : jd.height;
	g_cstep = (mx + THUMB - 1) / THUMB; if(g_cstep < 1) g_cstep = 1;
	g_tw = jd.width / g_cstep; g_th = jd.height / g_cstep;
	if(g_tw < 1) g_tw = 1; if(g_th < 1) g_th = 1;
	if(g_tw > THUMB) g_tw = THUMB; if(g_th > THUMB) g_th = THUMB;
	if(jd_decomp(&jd, cover_out, 0) != JDR_OK) return 0;
	return 1;
}
/* load cover for a display position: embedded PICTURE first, else sidecar in its dir. */
static void load_cover(int disp){
	g_cover_ok = 0;
	if(disp < 0 || disp >= g_n) return;
	rec_t r; rec_get(g_sorted[disp], &r);
	char full[REC_PATH + 24]; make_full(full, sizeof full, r.path);

	if(r.cover_len){                                      /* embedded JPEG */
		FILE *f = fopen(full, "rb");
		if(f){ g_cover_ok = decode_jpeg(f, (long)r.cover_off, (long)r.cover_len); fclose(f); }
	}
	if(!g_cover_ok){                                      /* sidecar in the album dir */
		char dir[REC_PATH]; utf8_cpy(dir, full, sizeof dir);
		char *slash = strrchr(dir, '/'); if(slash) *slash = 0;
		static const char *names[] = { "Folder.jpg","folder.jpg","cover.jpg","Cover.jpg","front.jpg" };
		for(unsigned i=0;i<sizeof names/sizeof names[0] && !g_cover_ok;i++){
			char sc[REC_PATH+24]; snprintf(sc, sizeof sc, "%s/%s", dir, names[i]);
			FILE *f = fopen(sc, "rb"); if(!f) continue;
			fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
			g_cover_ok = decode_jpeg(f, 0, sz); fclose(f);
		}
	}
	if(g_cover_ok){
		lv_memzero(&g_cover_dsc, sizeof g_cover_dsc);
		g_cover_dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
		g_cover_dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
		g_cover_dsc.header.w      = g_tw;
		g_cover_dsc.header.h      = g_th;
		g_cover_dsc.header.stride = g_tw * 2;
		g_cover_dsc.data          = (const uint8_t*)g_thumb;
		g_cover_dsc.data_size     = (uint32_t)g_tw * g_th * 2;
	}
}

/* ================================================================== UI ====== */
static void show_cover(void){
	if(g_cover_ok){
		lv_image_cache_drop(&g_cover_dsc);
		lv_image_set_src(g_cover, &g_cover_dsc);
		int z = 120 * 256 / (g_tw > g_th ? g_tw : g_th);   /* fit to ~120 px box */
		lv_image_set_scale(g_cover, z < 256 ? z : 256);
		lv_obj_clear_flag(g_cover, LV_OBJ_FLAG_HIDDEN);
		lv_obj_add_flag(g_noart, LV_OBJ_FLAG_HIDDEN);
	} else {
		lv_image_set_src(g_cover, NULL);
		lv_obj_add_flag(g_cover, LV_OBJ_FLAG_HIDDEN);
		lv_obj_clear_flag(g_noart, LV_OBJ_FLAG_HIDDEN);
	}
}

static void refresh_box_text(void){
	if(g_n == 0) return;
	rec_t r; rec_get(g_sorted[g_sel], &r);
	lv_label_set_text(g_l_title,  r.title[0]  ? r.title  : "(untitled)");
	lv_label_set_text(g_l_artist, r.artist[0] ? r.artist : "");
	lv_label_set_text(g_l_album,  r.album[0]  ? r.album  : "");
	if(r.detail && r.rate){
		lv_label_set_text_fmt(g_l_badge, "FLAC %lu.%luk/%u",
			(unsigned long)(r.rate/1000), (unsigned long)((r.rate%1000)/100), r.bits);
	} else lv_label_set_text(g_l_badge, "FLAC");
}

/* fill the wheel rows centred on g_sel; row ROW_CENTER is the amber "slit". */
static void refresh_wheel(void){
	for(int i=0;i<VIS_ROWS;i++){
		int disp = g_sel - ROW_CENTER + i;
		lv_obj_t *row = g_rows[i];
		int center = (i == ROW_CENTER);
		if(disp >= 0 && disp < g_n){
			rec_t r; rec_get(g_sorted[disp], &r);
			lv_label_set_text(row, r.title[0] ? r.title : r.path);
		} else lv_label_set_text(row, "");
		lv_obj_set_style_bg_opa(row, center ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
		lv_obj_set_style_bg_color(row, KF_AMBER, 0);
		lv_obj_set_style_text_color(row, center ? KF_BG_DEEP : KF_AMBER, 0);
		lv_obj_set_style_border_width(row, center ? 2 : 0, 0);
		if(center){
			lv_obj_set_style_border_color(row, KF_AMBER, 0);
			lv_obj_set_style_border_side(row, LV_BORDER_SIDE_TOP|LV_BORDER_SIDE_BOTTOM|LV_BORDER_SIDE_LEFT, 0);
			lv_obj_move_foreground(row);                 /* over the data box -> the "T" join */
		}
	}
}

static const char *repeat_str(void){ return g_repeat==2 ? "REP1" : g_repeat==1 ? "REP" : ""; }

static void refresh_stat(void){
	lv_label_set_text_fmt(g_l_stat, "%s%s", g_shuffle?"SHUF ":"", repeat_str());
}

/* progress/time, throttled to whole-second + state changes so we don't churn LVGL
   every 2 ms superloop tick. */
static void refresh_now(void){
	static int last_el = -1, last_pct = -1, last_pp = -1;
	int playing = (g_dec != NULL);
	int el = (playing && g_native) ? (int)(g_pos / g_native) : 0;
	int du = playing ? (int)(g_total / (g_native?g_native:1)) : 0;
	int pct = du ? (int)((long long)el * 100 / du) : 0;
	int pp = (playing<<1) | g_paused;
	if(el == last_el && pct == last_pct && pp == last_pp) return;
	last_el = el; last_pct = pct; last_pp = pp;
	lv_bar_set_value(g_bar, pct, LV_ANIM_OFF);
	lv_label_set_text_fmt(g_l_time, "%s %d:%02d/%d:%02d",
		playing ? (g_paused ? "II" : ">") : "-", el/60, el%60, du/60, du%60);
}

/* ============================================================ playback ====== */
static void close_dec(void){
	kf_audio_stop();
	if(g_dec){ drflac_close(g_dec); g_dec = NULL; }
	g_paused = 0; g_pos = g_total = 0; g_native = 0;
}

static void open_disp(int disp){
	close_dec();
	if(disp < 0 || disp >= g_n) return;
	rec_t r; rec_get(g_sorted[disp], &r);
	char full[REC_PATH + 24]; make_full(full, sizeof full, r.path);
	g_dec = drflac_open_file(full, NULL);
	if(!g_dec){ lv_label_set_text(g_l_title, "! cannot open"); return; }
	g_ch     = g_dec->channels;
	g_native = g_dec->sampleRate;
	g_total  = g_dec->totalPCMFrameCount;
	g_decim  = 1; while(g_native / (uint32_t)g_decim > 48000u) g_decim *= 2;
	g_playhz = (int)(g_native / (uint32_t)g_decim);
	g_pos = 0; g_paused = 0;
	if(!r.detail){ flac_meta(full, &r); rec_put(g_sorted[disp], &r); }
	g_play_disp = disp;
	kf_audio_start(g_playhz);
}

static int find_order_pos(int disp){
	for(int i=0;i<g_n;i++) if(g_order[i] == disp) return i;
	return 0;
}

static void rebuild_order(void){
	for(int i=0;i<g_n;i++) g_order[i] = (uint16_t)i;
	if(g_shuffle){
		uint32_t s = time_us_32() ^ 0xC0FFEEu;
		for(int i=g_n-1;i>0;i--){
			s = s*1664525u + 1013904223u;
			int j = (int)(s % (uint32_t)(i+1));
			uint16_t t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
		}
	}
	g_order_pos = (g_play_disp >= 0) ? find_order_pos(g_play_disp) : 0;
}

static void focus(int disp){           /* move highlight, queue detail+cover load */
	g_sel = disp;
	refresh_wheel();
	refresh_box_text();
	g_focus_dirty = 1; g_focus_t = lv_tick_get();
}

static void play_disp(int disp){
	g_order_pos = find_order_pos(disp);
	open_disp(disp);
	focus(disp);
}

static void on_eof(void){
	if(g_repeat == 2){ open_disp(g_play_disp); return; }   /* repeat one */
	int pos = g_order_pos + 1;
	if(pos >= g_n){ if(g_repeat==1) pos = 0; else { close_dec(); return; } }
	g_order_pos = pos; open_disp(g_order[pos]); focus(g_order[pos]);
}

static void pump(void){
	if(!g_dec || g_paused) return;
	int guard = 0;
	while(kf_audio_space() > CHUNK && guard++ < 4){
		int want = CHUNK;
		if(g_ch > 2) want = (CHUNK*2) / g_ch;              /* keep raw[] within bounds */
		drflac_uint64 got = drflac_read_pcm_frames_s32(g_dec, (drflac_uint64)want, (drflac_int32*)g_raw);
		if(got == 0){ on_eof(); return; }
		g_pos += got;
		for(drflac_uint64 i=0;i<got;i++){
			int32_t L, R;
			if(g_ch == 1){ L = R = g_raw[i]; }
			else { L = g_raw[i*g_ch + 0]; R = g_raw[i*g_ch + 1]; }
			g_st[2*i] = L; g_st[2*i+1] = R;
		}
		int outn = (int)got;
		const int32_t *outp = g_st;
		if(g_decim > 1){
			outn = (int)got / g_decim;
			for(int i=0;i<outn;i++){
				int64_t sl=0, sr=0;
				for(int k=0;k<g_decim;k++){ sl += g_st[(i*g_decim+k)*2]; sr += g_st[(i*g_decim+k)*2+1]; }
				g_dec2[2*i] = (int32_t)(sl/g_decim); g_dec2[2*i+1] = (int32_t)(sr/g_decim);
			}
			outp = g_dec2;
		}
		if(outn > 0) kf_audio_write_s32(outp, outn);
	}
}

static void do_seek(int dsec){
	if(!g_dec) return;
	int64_t tgt = (int64_t)g_pos + (int64_t)dsec * (int64_t)g_native;
	if(tgt < 0) tgt = 0;
	if((uint64_t)tgt >= g_total) tgt = (int64_t)g_total - 1;
	if(tgt < 0) tgt = 0;
	if(drflac_seek_to_pcm_frame(g_dec, (drflac_uint64)tgt)){ g_pos = (uint64_t)tgt; kf_audio_flush(); }
}

/* ================================================================= keys ===== */
static void m_exit(void){
	close_dec();               /* stop the decoder + audio BEFORE dropping the clock */
	kf_clock_normal();         /* hand the panel clock back to the normal 400 MHz UI */
	g_active = 0;
	g_cover_ok = 0;
	free(g_thumb); g_thumb = NULL;       /* return the ~29 KB to the shared heap */
	if(g_jpfont && g_jpfont != KF_FONT) lv_binfont_destroy((lv_font_t*)g_jpfont);
	g_jpfont = NULL;
	kf_grab_input(0);
	kf_back_to_launcher();
}

static void m_key(uint8_t k){
	switch(k){
	case DK_ESC: case DK_BREAK: m_exit(); return;
	/* scrolling the list stops playback (frees the decoder + keeps the now-playing
	   in sync with what you're browsing). ENTER starts the highlighted track. */
	case DK_UP:   if(g_sel > 0)     { close_dec(); focus(g_sel-1); } return;
	case DK_DOWN: if(g_sel < g_n-1) { close_dec(); focus(g_sel+1); } return;
	case DK_LEFT:  do_seek(-5); return;
	case DK_RIGHT: do_seek(+5); return;
	case DK_ENTER:
		if(g_n == 0) return;
		if(g_dec && g_sel == g_play_disp) g_paused = !g_paused;
		else play_disp(g_sel);
		return;
	case 's': case 'S': g_shuffle = !g_shuffle; rebuild_order(); refresh_stat(); return;
	case 'r': case 'R': g_repeat = (g_repeat+1)%3; refresh_stat(); return;
	case 0x85: /* F5 */ close_dec(); g_built = 0; g_play_disp = -1; build_index();
		g_sel = 0; rebuild_order(); refresh_wheel(); refresh_box_text(); focus(0); return;
	default: return;
	}
}

void music_poll(void){
	if(!g_active) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		m_key(key);
	}
	if(g_focus_dirty && (lv_tick_get() - g_focus_t) > 120){
		g_focus_dirty = 0;
		ensure_detail(g_sel);
		refresh_box_text();
		load_cover(g_sel);
		show_cover();
	}
	pump();
	refresh_now();
}

/* ================================================================= build UI = */
static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, lv_color_t col, int w){
	lv_obj_t *l = lv_label_create(par);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, col, 0);
	if(w > 0){ lv_obj_set_width(l, w); lv_label_set_long_mode(l, LV_LABEL_LONG_DOT); }
	return l;
}

void app_music_open(void){
	/* JP font from the SD card (kept out of firmware). Fallback = Latin Plex font. */
	g_jpfont = lv_binfont_create("A:/kefyros/fonts/jp.bin");
	if(!g_jpfont) g_jpfont = KF_FONT;

	g_scr = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(g_scr, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_all(g_scr, 0, 0);
	lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

	g_cont = lv_obj_create(g_scr);
	lv_obj_remove_style_all(g_cont);
	lv_obj_set_pos(g_cont, 0, KF_CONTENT_Y);
	lv_obj_set_size(g_cont, LCD_W, KF_CONTENT_H);
	lv_obj_set_style_bg_color(g_cont, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(g_cont, LV_OPA_COVER, 0);
	lv_obj_remove_flag(g_cont, LV_OBJ_FLAG_SCROLLABLE);

	/* right data box: all borders; the centre slit overlaps its left edge -> "T". */
	g_box = lv_obj_create(g_cont);
	lv_obj_remove_style_all(g_box);
	lv_obj_set_pos(g_box, LEFTW, 2);
	lv_obj_set_size(g_box, LCD_W - LEFTW - 2, KF_CONTENT_H - 4);
	lv_obj_set_style_bg_color(g_box, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(g_box, LV_OPA_COVER, 0);
	lv_obj_set_style_border_width(g_box, 2, 0);
	lv_obj_set_style_border_color(g_box, KF_AMBER, 0);
	lv_obj_set_style_pad_all(g_box, 6, 0);
	lv_obj_remove_flag(g_box, LV_OBJ_FLAG_SCROLLABLE);

	/* cover (centred near the top of the box) + "no art" glyph fallback */
	g_cover = lv_image_create(g_box);
	lv_obj_align(g_cover, LV_ALIGN_TOP_MID, 0, 0);
	g_noart = lv_label_create(g_box);
	lv_obj_set_style_text_font(g_noart, g_jpfont, 0);
	lv_obj_set_style_text_color(g_noart, KF_TEXT_MUTED, 0);
	lv_label_set_text(g_noart, "\xE2\x99\xAA");          /* U+266A music note */
	lv_obj_align(g_noart, LV_ALIGN_TOP_MID, 0, 40);

	int bw = LCD_W - LEFTW - 2 - 12;                     /* box inner width */
	g_l_title  = mk_label(g_box, g_jpfont, KF_AMBER_BR, bw);
	lv_obj_align(g_l_title, LV_ALIGN_TOP_LEFT, 0, 126);
	g_l_artist = mk_label(g_box, g_jpfont, KF_AMBER, bw);
	lv_obj_align(g_l_artist, LV_ALIGN_TOP_LEFT, 0, 146);
	g_l_album  = mk_label(g_box, g_jpfont, KF_TEXT_DIM, bw);
	lv_obj_align(g_l_album, LV_ALIGN_TOP_LEFT, 0, 166);
	g_l_badge  = mk_label(g_box, KF_FONT, KF_GREEN, bw);
	lv_obj_align(g_l_badge, LV_ALIGN_TOP_LEFT, 0, 190);

	g_bar = lv_bar_create(g_box);
	lv_obj_set_size(g_bar, bw, 5);
	lv_obj_align(g_bar, LV_ALIGN_BOTTOM_LEFT, 0, -22);
	lv_bar_set_range(g_bar, 0, 100);
	lv_obj_set_style_bg_color(g_bar, KF_CARD, 0);
	lv_obj_set_style_bg_color(g_bar, KF_AMBER, LV_PART_INDICATOR);
	g_l_time = mk_label(g_box, KF_FONT, KF_TEXT_DIM, bw);
	lv_obj_align(g_l_time, LV_ALIGN_BOTTOM_LEFT, 0, -10);
	g_l_stat = mk_label(g_box, KF_FONT, KF_ACTIVE, bw);
	lv_obj_align(g_l_stat, LV_ALIGN_BOTTOM_RIGHT, 0, -10);

	/* left wheel rows */
	int rowh = KF_CONTENT_H / VIS_ROWS;
	int y0 = (KF_CONTENT_H - rowh*VIS_ROWS) / 2;
	for(int i=0;i<VIS_ROWS;i++){
		int center = (i == ROW_CENTER);
		lv_obj_t *row = lv_label_create(g_cont);
		lv_obj_set_style_text_font(row, g_jpfont, 0);
		lv_obj_set_pos(row, 2, y0 + i*rowh);
		lv_obj_set_size(row, center ? LEFTW : LEFTW-10, rowh-1);
		lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
		lv_obj_set_style_text_color(row, KF_AMBER, 0);
		lv_obj_set_style_pad_left(row, 4, 0);
		lv_obj_set_style_pad_top(row, 2, 0);
		lv_obj_set_style_radius(row, 0, 0);
		g_rows[i] = row;
	}

	/* "empty / indexing" overlay label (also used for the scan progress) */
	g_l_empty = lv_label_create(g_cont);
	lv_obj_set_style_text_color(g_l_empty, KF_TEXT_MUTED, 0);
	lv_obj_align(g_l_empty, LV_ALIGN_CENTER, 0, 0);
	lv_label_set_text(g_l_empty, "");

	if(!g_thumb) g_thumb = malloc((size_t)THUMB*THUMB*2);   /* cover scratch (freed on exit) */
	kf_clock_normal();                /* the normal 400 MHz clock: 13-bit carrier + decode headroom */
	g_active = 1;
	lv_screen_load(g_scr);

	if(!g_built){ lv_label_set_text(g_l_empty, "Indexing music..."); lv_refr_now(NULL); build_index(); }

	if(g_n == 0){
		lv_label_set_text(g_l_empty, "no .flac under\n" MUSDIR);
	} else {
		lv_obj_add_flag(g_l_empty, LV_OBJ_FLAG_HIDDEN);
		if(g_sel >= g_n) g_sel = 0;
		rebuild_order();
		refresh_wheel();
		refresh_box_text();
		refresh_stat();
		focus(g_sel);
	}
	kf_grab_input(1);
}
