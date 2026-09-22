// apps/spineko.c — Spineko: PSRAM-backed HTML/CSS web browser for Kefyros.
//
// Pipeline: port/http.c streams a page body into PSRAM -> port/html.c parses it into
// a flat op list (also in PSRAM) -> this file walks the ops and builds LVGL widgets
// (capped) into a scrollable document, in the OS amber theme. HTTP and HTTPS (TLS 1.2
// via BearSSL in port/tls.c, no cert verification). Links + single-field GET forms are
// followable from the keypad.
//
// Input: the app GRABS raw keys (so it can run the URL bar / link focus / history
// itself) and is pumped by browser_poll() from the main superloop:
//   F1        focus the address bar (type a URL, ENTER to go, ESC to cancel)
//   Up/Down   scroll the document
//   Left/Right move link/field focus (and scroll the focused one into view)
//   PgUp/PgDn scroll the page a screenful
//   ENTER     follow the focused link / submit a form / edit a focused field
//   Backspace history back   (while editing: delete a char)
//   ESC       quit to launcher (while editing: cancel the edit)
//
// CYW43 associates at the 250 MHz eco tier, then Spineko ramps the CPU back to the
// normal 400 MHz tier while the live PIO divider keeps gSPI at 31.25 MHz. If the link
// drops we dip to eco for the reconnect handshake and return to 400 once online.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../port/http.h"
#include "../port/html.h"
#include "../port/css.h"
#include "../port/imgdec.h"          /* JPEG/PNG/SVG -> downscaled RGB565, bounded RAM */
#include "../lib/lvgl/src/libs/tiny_ttf/lv_tiny_ttf.h"
#include "pico/stdlib.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <sys/stat.h>                /* mkdir for the SD stylesheet cache */
#include <dirent.h>                  /* bounded cache-directory cleanup */

/* heap headroom: stop building widgets before we exhaust it (the SDK's malloc PANICS
   on OOM — a huge page like Wikipedia would otherwise crash the whole OS mid-render). */
extern char __HeapLimit[], __end__[];
static uint32_t heap_free(void){
	struct mallinfo mi = mallinfo();
	uint32_t cap = (uint32_t)((uintptr_t)__HeapLimit - (uintptr_t)__end__);
	uint32_t used = (uint32_t)mi.uordblks;
	return used < cap ? cap - used : 0;
}
#define HEAP_FLOOR (48u*1024u)      /* keep this much free during render */

/* PSRAM arena: [BODY | OPS | TEXT | CSSRULES | CSSRAW | HISTORY]. Allocated once, reused
   across page loads. CSSRULES = the parsed rule table; CSSRAW = scratch that
   external stylesheets download into (the BODY region must stay intact for the
   post-CSS re-parse). */
#define ARENA_SZ    (4u*1024*1024)
#define BODY_MAX    (1536u*1024)
#define OPS_MAX     (256u*1024)
#define TEXT_MAX    (1536u*1024)
#define CSSRULE_MAX (256u*1024)   /* ~1900 expanded modern-selector rules */
#define HIST_DEPTH  16
#define HIST_REC    512u
#define HIST_BYTES  (2u*HIST_DEPTH*HIST_REC)
#define CSSRAW_OFF  (BODY_MAX+OPS_MAX+TEXT_MAX+CSSRULE_MAX)
#define CSSRAW_MAX  (ARENA_SZ-CSSRAW_OFF-HIST_BYTES)
#define HIST_OFF    (ARENA_SZ-HIST_BYTES)
static uint32_t s_arena = 0xFFFFFFFFu;

void browser_psram_invalidate(void){ s_arena = 0xFFFFFFFFu; }

#define DOC_W       (LCD_W - 16)
#define URLBAR_H    20
#define STATUS_H    16
#define MAX_WIDGETS 220
#define MAX_FOCI    200
#define MAX_FIELDS  16

/* "Default web" palette — pages render like a normal browser (white page, black text,
   blue links), NOT the OS amber theme. Scoped to Spineko's document area only. */
#define WEB_BG      lv_color_hex(0xffffff)   /* page background */
#define WEB_TEXT    lv_color_hex(0x101010)   /* body text / headings */
#define WEB_LINK    lv_color_hex(0x0000ee)   /* classic web link blue */
#define WEB_DIM     lv_color_hex(0x555555)   /* blockquote */
#define WEB_PRE     lv_color_hex(0x1a1a1a)   /* preformatted */
#define WEB_MUTED   lv_color_hex(0x888888)   /* [img] / truncation notes */
#define WEB_RULE    lv_color_hex(0xcccccc)   /* <hr> */
#define WEB_FOCUS   lv_color_hex(0xb3d1ff)   /* focused link highlight bg */
#define WEB_FOCUSB  lv_color_hex(0x1a66cc)   /* focused link border */
#define WEB_CHROME  lv_color_hex(0xe8e8e8)   /* address/status bar bg */
#define WEB_CHROMET lv_color_hex(0x222222)   /* address/status bar text */
#define WEB_FONT     (&lv_font_montserrat_14)
#define WEB_FONT_BIG (&lv_font_montserrat_20)

static lv_obj_t *scr, *lbl_url, *doc, *lbl_status, *mouse_ptr;
static int mouse_x,mouse_y;

/* Classic 14x18 pointer. RGB565A8 leaves only the exterior transparent; the
   arrow itself is solid white with a one-pixel black border. Hot spot = (0,0). */
#define PTR_W 14
#define PTR_H 18
static struct { uint16_t rgb[PTR_W*PTR_H]; uint8_t alpha[PTR_W*PTR_H]; } pointer_map;
static lv_image_dsc_t pointer_dsc = {
	.header.magic=LV_IMAGE_HEADER_MAGIC,.header.cf=LV_COLOR_FORMAT_RGB565A8,
	.header.w=PTR_W,.header.h=PTR_H,.header.stride=PTR_W*2,
	.data_size=sizeof pointer_map,.data=(const uint8_t *)&pointer_map
};

/* focusable items (links / form fields / submit buttons) in document order */
typedef struct {
	lv_obj_t *w;
	uint8_t   kind;                 /* KF_OP_LINK / KF_OP_FIELD / KF_OP_SUBMIT */
	uint32_t  href_off, href_len;   /* LINK/SUBMIT: URL/action in the TEXT arena */
	int       field;                /* FIELD: index into fld_*; SUBMIT: bound field or -1 */
	uint8_t   form_post;            /* submit control method */
} focus_t;
static focus_t foci[MAX_FOCI];
static int     foci_n, cur_focus;

static char    fld_name[MAX_FIELDS][64];
static char    fld_val[MAX_FIELDS][128];
static uint8_t fld_type[MAX_FIELDS], fld_checked[MAX_FIELDS];
static int     fld_n;

/* ---- inline images (downscaled JPEG, fetched in the background after the page) ---- */
#define IMG_MAX        24            /* images tracked per page */
#define IMG_W_MAX      DOC_W         /* target max decoded width (px) */
#define IMG_H_MAX      220           /* target max decoded height (px) */
#define IMG_OUTPUT_MAX  (96u*1024u)  /* one transient decoded bitmap; freed after SD write */
#define IMG_DECODE_FLOOR (16u*1024u) /* render is over: keep enough for LVGL/network, not 48 KB */
#define IMG_LEGACY_DIR  "/kefyros/spineko/img-v2" /* old raw-response cache, read-only migration */
#define IMG_RENDER_DIR  "/kefyros/spineko/rgb-v2"
typedef struct {
	lv_obj_t      *w;                /* the [img] placeholder label, then the lv_image */
	uint32_t       href_off, href_len;  /* src URL in the TEXT arena */
	uint8_t        state;           /* 0 pending, 1 done, 2 failed/skipped */
	uint16_t       iw, ih;
	uint32_t       cache_hash;
} img_t;
static img_t    imgs[IMG_MAX];
static int      imgs_n;
static int      img_cur;            /* index being fetched, -1 = none */
static int      img_fetching;       /* 1 downloading, 2 queued for decode */
static int      assets_stopped, assets_finished;

static char    cur_url[512];
static char    doc_base[512];      /* resolution base; Wikipedia REST HTML declares /wiki/ */
static int     hist_sp;
static int     fwd_sp;
static char    form_body[1400];    /* persists while async POST owns the request body */

static int     s_loading;
static int     s_wiki_rest;        /* current PAGE request was transparently rewritten */
static int     s_page_status;      /* HTTP status of the PAGE (css fetches clobber kf_http_status) */
static int     s_fast_clock;       /* browser is running at 400 MHz; reconnects dip to eco */
static int     s_http_last_state;
static uint32_t s_http_last_bytes, s_http_progress_ms;
static int     s_http_eco_fallback; /* this request stalled at 400; finish its network I/O at 250 */
static uint32_t s_nav_t0, s_net_ms, s_parse_ms, s_css_ms, s_render_ms;
static char    s_ready_status[160];
static int     edit_mode;          /* 0 none, 1 url bar, 2 field */
static int     edit_field;
static char    edit_buf[512];
static int     reader_mode;
static char    find_query[64];
static uint32_t find_from;

/* external-stylesheet fetch chain (runs between page load and render) */
static int     css_active;         /* a stylesheet HTTP fetch is in flight */
static int     css_i, css_n;
static char    css_cachef[80];     /* SD cache path of the sheet being fetched */
static char    css_abs[600];

#define PAGE_FONT_MAX 2
#define FONT_CACHE_DIR "/kefyros/spineko/font-v1"
static lv_font_t *page_fonts[PAGE_FONT_MAX][2]; /* [slot][normal/big] */
static int font_active,font_i,font_slot;
static char font_cachef[80],font_abs[600];

static void load_url(const char *url, int push);
static void css_next(void);
static int pointer_hit(void);
static void fonts_next(void);
static void render_page(void);

/* ---------- small helpers ---------- */
static int is_focus_kind(int k){ return k==KF_OP_LINK || k==KF_OP_FIELD || k==KF_OP_SUBMIT; }

static void set_status(const char *s){ if(lbl_status) lv_label_set_text(lbl_status, s); }

static void show_url(void){
	if(!lbl_url) return;
	if(edit_mode==1){ char b[520]; snprintf(b,sizeof b,"%s_", edit_buf); lv_label_set_text(lbl_url,b); }
	else lv_label_set_text(lbl_url, cur_url[0]?cur_url:"(address bar — F1)");
}

/* focus highlight: a card background + hot border on the active item */
static void style_focus(int i, int on){
	if(i<0 || i>=foci_n || !foci[i].w) return;
	lv_obj_set_style_bg_color(foci[i].w, WEB_FOCUS, 0);
	lv_obj_set_style_bg_opa(foci[i].w, on?LV_OPA_COVER:LV_OPA_TRANSP, 0);
	lv_obj_set_style_border_width(foci[i].w, on?1:0, 0);
	lv_obj_set_style_border_color(foci[i].w, WEB_FOCUSB, 0);
}
static void set_focus(int i){
	if(foci_n==0) return;
	if(i<0) i=0; if(i>=foci_n) i=foci_n-1;
	style_focus(cur_focus, 0);
	cur_focus = i;
	style_focus(cur_focus, 1);
	lv_obj_scroll_to_view(foci[cur_focus].w, LV_ANIM_OFF);
}

/* ---------- rendering ---------- */
static lv_color_t c565(uint16_t v){
	uint8_t r=(uint8_t)((v>>11)<<3); r|=r>>5;
	uint8_t g=(uint8_t)(((v>>5)&0x3f)<<2); g|=g>>6;
	uint8_t b=(uint8_t)((v&0x1f)<<3); b|=b>>5;
	return lv_color_make(r,g,b);
}
static void apply_size(lv_obj_t *o, const kf_html_op *op){
	if(op->width_unit==KF_CSS_SIZE_PCT) lv_obj_set_width(o,lv_pct(op->width));
	else if(op->width_unit==KF_CSS_SIZE_PX) lv_obj_set_width(o,op->width);
	if(op->max_width_unit==KF_CSS_SIZE_PCT) lv_obj_set_style_max_width(o,lv_pct(op->max_width),0);
	else if(op->max_width_unit==KF_CSS_SIZE_PX) lv_obj_set_style_max_width(o,op->max_width,0);
}
static void apply_fx(lv_obj_t *o, const kf_html_op *op){
	apply_size(o,op);
	if(op->opacity<255) lv_obj_set_style_opa(o,op->opacity,0);
	if(op->shadow_w){
		lv_obj_set_style_shadow_width(o,op->shadow_w,0);
		lv_obj_set_style_shadow_color(o,c565(op->shadow_c),0);
		lv_obj_set_style_shadow_opa(o,LV_OPA_40,0);
	}
}
/* CSS extras that go on top of mk_label: background, alignment, decoration,
   borders, spacing */
static void post_style(lv_obj_t *l, const kf_html_op *op){
	if(reader_mode){
		if(op->line_sp)lv_obj_set_style_text_line_space(l,op->line_sp,0);
		if(op->let_sp)lv_obj_set_style_text_letter_space(l,op->let_sp,0);
		return;
	}
	if(op->sflags & KF_ST_BG){
		lv_obj_set_style_bg_color(l, c565(op->bg), 0);
		lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
	}
	if(op->sflags & KF_ST_CENTER)     lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
	else if(op->sflags & KF_ST_RIGHT) lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_RIGHT, 0);
	int dec = ((op->sflags&KF_ST_UNDER) ? LV_TEXT_DECOR_UNDERLINE : 0)
	        | ((op->sflags&KF_ST_STRIKE) ? LV_TEXT_DECOR_STRIKETHROUGH : 0);
	if(dec) lv_obj_set_style_text_decor(l, dec, 0);
	if(op->border_w){
		lv_obj_set_style_border_width(l, op->border_w, 0);
		lv_obj_set_style_border_color(l, c565(op->border_c), 0);
		lv_obj_set_style_pad_hor(l, 3, 0);
	}
	if(op->radius)  lv_obj_set_style_radius(l, op->radius, 0);
	if(op->pad_v)   lv_obj_set_style_pad_ver(l, 1 + op->pad_v/2, 0);  /* halved: rhythm, not chasm */
	if(op->pad_h)   lv_obj_set_style_pad_hor(l, op->pad_h, 0);
	if(op->line_sp) lv_obj_set_style_text_line_space(l, op->line_sp, 0);
	if(op->let_sp)  lv_obj_set_style_text_letter_space(l, op->let_sp, 0);
	if(op->lflags & KF_LAY_NOWRAP) lv_label_set_long_mode(l,LV_LABEL_LONG_CLIP);
	apply_fx(l,op);
}
/* text-transform, in place */
static void xform_buf(char *s, uint8_t mode){
	if(!mode) return;
	for(; *s; s++){
		if(mode==1){ if(*s>='a'&&*s<='z') *s -= 32; }
		else       { if(*s>='A'&&*s<='Z') *s += 32; }
	}
}
static int padc(int base, const kf_html_op *op){
	int p = base + op->indent;
	return p > DOC_W-60 ? DOC_W-60 : p;
}
/* container (BOX/END) nesting: labels/widgets parent to the innermost open box */
#define BOX_DEPTH 8
static lv_obj_t *bstk[BOX_DEPTH];   /* bstk[0] = doc */
static uint8_t   brow[BOX_DEPTH];   /* parent is a flex ROW (chips: content-sized labels) */
static uint8_t   bcols[BOX_DEPTH];  /* grid column count at each nesting level */
static int       bdepth, bskip;
static lv_obj_t *run_parent;        /* current implicit inline-formatting row */

/* virtualization: the widget tree is a WINDOW over the op list (which lives whole in
   PSRAM). [win_first, win_next) is materialized; scrolling near an edge re-renders
   the window anchored at the op that is currently at the top of the view. Top-level
   children carry their op index (+1) in user_data so the anchor can be found. */
static uint32_t win_first, win_next;
static int      win_more;           /* ops remain beyond win_next */

static lv_obj_t *mk_label(const char *txt, const lv_font_t *font, lv_color_t color, int pad_left){
	lv_obj_t *l = lv_label_create(run_parent?run_parent:bstk[bdepth]);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	if(run_parent){
		lv_obj_set_width(l,LV_SIZE_CONTENT);
		lv_obj_set_style_max_width(l,DOC_W-12,0);
	} else if(brow[bdepth]){                       /* chip in a row: content-sized, capped */
		if(bcols[bdepth]>0) lv_obj_set_width(l,lv_pct(96/bcols[bdepth]));
		else lv_obj_set_width(l, LV_SIZE_CONTENT);
		lv_obj_set_style_max_width(l, DOC_W-12, 0);
	} else
		lv_obj_set_width(l, lv_pct(100));
	lv_obj_set_style_pad_left(l, pad_left, 0);
	lv_obj_set_style_pad_ver(l, 1, 0);
	lv_label_set_text(l, txt);
	return l;
}

static void add_focus(lv_obj_t *w, int kind, kf_html_op *op, int field){
	if(foci_n>=MAX_FOCI) return;
	foci[foci_n].w=w; foci[foci_n].kind=(uint8_t)kind;
	foci[foci_n].href_off=op->href_off; foci[foci_n].href_len=op->href_len;
	foci[foci_n].field=field;
	foci[foci_n].form_post=(kind==KF_OP_SUBMIT && (op->index&KF_FORM_POST))?1:0;
	foci_n++;
}

/* ---------- inline images ----------
   After the page renders as text + [img] placeholders, browser_poll() fetches each
   image URL one at a time (reusing the now-free BODY arena, since the HTML is already
   parsed into ops), decodes JPEG/PNG/GIF/SVG into one transient RGB565 buffer, writes
   that once to a content-addressed .bin, frees the buffer, and lets LVGL stream scanlines
   from SD. Page RAM therefore does not grow with image count. */
static int decode_img(uint32_t base, uint32_t len, int max_w, int max_h,
                      uint16_t **pix,uint8_t **alpha,int *w,int *h){
	return kf_img_decode(base,len,max_w,max_h,IMG_OUTPUT_MAX,IMG_DECODE_FLOOR,pix,alpha,w,h);
}
static void set_img_error(int i,const char *why){
	if(i<0||i>=imgs_n)return;
	imgs[i].state=2;
	if(imgs[i].w && lv_obj_check_type(imgs[i].w,&lv_label_class)){
		static char b[96]; snprintf(b,sizeof b,"[img %s]",why);
		lv_label_set_text(imgs[i].w,b);
	}
}
static void place_image_file(int i,const char *path,int w,int h){
	img_t *im = &imgs[i];
	if(!im->w){ im->state = 1; return; }
	int idx = lv_obj_get_index(im->w);
	lv_obj_t *img = lv_image_create(lv_obj_get_parent(im->w));  /* may be nested in a box */
	static char lvsrc[96]; snprintf(lvsrc,sizeof lvsrc,"A:%s",path);
	lv_image_set_src(img, lvsrc);                /* LVGL duplicates the filename */
	lv_obj_move_to_index(img, idx);     /* keep document order */
	lv_obj_del(im->w);
	im->w = img;                        /* handle now points at the image */
	im->iw=(uint16_t)w; im->ih=(uint16_t)h;
	im->state = 1;
}
static void free_images(void){
	imgs_n = 0; img_cur = -1; img_fetching = 0;
	assets_stopped=0;assets_finished=0;
}
static uint32_t url_hash32(const char *url){
	uint32_t h=2166136261u;for(;*url;url++)h=(h^(uint8_t)*url)*16777619u;return h;
}
static void legacy_cache_path(uint32_t h,char *out,int cap){
	snprintf(out,cap,IMG_LEGACY_DIR "/%08lx.bin",(unsigned long)h);
}
static void render_cache_path(uint32_t h,char *out,int cap){
	snprintf(out,cap,IMG_RENDER_DIR "/%08lx.bin",(unsigned long)h);
}
static int render_cache_info(const char *path,int *w,int *h){
	struct stat st; if(stat(path,&st)!=0 || st.st_size<(off_t)sizeof(lv_image_header_t))return 0;
	FILE *f=fopen(path,"rb");if(!f)return 0;
	lv_image_header_t hd;int ok=fread(&hd,1,sizeof hd,f)==sizeof hd;fclose(f);
	uint64_t need=(uint64_t)sizeof hd+(uint64_t)hd.stride*hd.h;
	if(hd.cf==LV_COLOR_FORMAT_RGB565A8)need+=(uint64_t)hd.w*hd.h;
	ok=ok&&hd.magic==LV_IMAGE_HEADER_MAGIC
	   &&(hd.cf==LV_COLOR_FORMAT_RGB565||hd.cf==LV_COLOR_FORMAT_RGB565A8)&&hd.w&&hd.h
	   &&hd.stride==hd.w*2u&&need==(uint64_t)st.st_size;
	if(!ok)return 0;*w=hd.w;*h=hd.h;return 1;
}
/* Atomic, write-once rendered cache. A repeat visit only stats/reads it. */
static int render_cache_save(const char *path,const uint16_t *pix,const uint8_t *alpha,int w,int h){
	int cw,ch;if(render_cache_info(path,&cw,&ch))return 1;
	static char tmp[96];snprintf(tmp,sizeof tmp,"%s.tmp",path);remove(tmp);
	FILE *f=fopen(tmp,"wb");if(!f)return 0;
	lv_image_header_t hd;lv_memzero(&hd,sizeof hd);hd.magic=LV_IMAGE_HEADER_MAGIC;
	hd.cf=alpha?LV_COLOR_FORMAT_RGB565A8:LV_COLOR_FORMAT_RGB565;hd.w=w;hd.h=h;hd.stride=w*2;
	uint32_t bytes=(uint32_t)w*h*2u;
	uint32_t abytes=(uint32_t)w*h;
	int ok=fwrite(&hd,1,sizeof hd,f)==sizeof hd && fwrite(pix,1,bytes,f)==bytes
	       &&(!alpha||fwrite(alpha,1,abytes,f)==abytes);
	if(fclose(f)!=0)ok=0;
	if(!ok){remove(tmp);return 0;}
	remove(path);if(rename(tmp,path)!=0){remove(tmp);return 0;}return 1;
}
static uint32_t cache_load_psram(const char *path,uint32_t base,uint32_t cap){
	FILE *f=fopen(path,"rb");if(!f)return 0;
	static uint8_t b[1024];uint32_t off=0;size_t n;
	while(off<cap){
		size_t want=cap-off;if(want>sizeof b)want=sizeof b;
		n=fread(b,1,want,f);if(!n)break;
		kf_psram_write(base+off,b,n);off+=(uint32_t)n;
		if(n<want)break;
	}
	int extra=fgetc(f);fclose(f);if(extra!=EOF)return 0;return off;
}
static int decode_save_place(int i,uint32_t base,uint32_t len,const char *rp){
	uint16_t *pix=NULL;uint8_t *alpha=NULL;int w=0,h=0;
	if(!decode_img(base,len,IMG_W_MAX,IMG_H_MAX,&pix,&alpha,&w,&h))return 0;
	int ok=render_cache_save(rp,pix,alpha,w,h);free(pix);free(alpha);
	if(ok)place_image_file(i,rp,w,h);
	return ok;
}
static int ext_eq(const char *url,const char *want){
	const char *q=strchr(url,'?');const char *e=q?q:url+strlen(url),*p=e;
	while(p>url&&p[-1]!='/'&&p[-1]!='.')p--;
	if(p==url||p[-1]!='.')return 0;p--;
	int n=(int)(e-p),wn=(int)strlen(want);if(n!=wn)return 0;
	for(int i=0;i<n;i++){
		int a=p[i],b=want[i];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;
		if(a!=b)return 0;
	}
	return 1;
}
static int known_unsupported_image(const char *url){
	return ext_eq(url,".webp")||ext_eq(url,".avif")||ext_eq(url,".bmp")||
	       ext_eq(url,".tif")||ext_eq(url,".tiff")||ext_eq(url,".ico")||ext_eq(url,".svgz");
}
static void images_ready_status(void){
	if(assets_finished)return;assets_finished=1;
	int ok=0,bad=0;for(int i=0;i<imgs_n;i++){if(imgs[i].state==1)ok++;else if(imgs[i].state==2)bad++;}
	char b[192];if(bad)snprintf(b,sizeof b,"%s I%d/%d X%d",s_ready_status,ok,imgs_n,bad);
	else snprintf(b,sizeof b,"%s I%d/%d",s_ready_status,ok,imgs_n);set_status(b);
}
/* advance the one-at-a-time background image fetch. called from browser_poll(). */
static void images_pump(void){
	if(assets_stopped||s_loading||css_active||font_active)return; /* never compete with page/style/font */
	if(kf_net_state()!=KF_NET_ONLINE) return;
	if(img_fetching==2){
		img_fetching=0;int i=img_cur;img_cur=-1;
		if(i>=0&&i<imgs_n){
			uint32_t base=kf_http_body_base(),len=kf_http_body_len();
			static char rp[80];render_cache_path(imgs[i].cache_hash,rp,sizeof rp);
			if(!decode_save_place(i,base,len,rp))set_img_error(i,"decode/memory failed");
		}
		return;
	}
	if(img_fetching==1){
		int st = kf_http_state();
		if(st==KF_HTTP_DONE){
			img_fetching=2;char b[72];snprintf(b,sizeof b,"Image %d/%d decoding %luKB... [F9 stop]",
				img_cur+1,imgs_n,(unsigned long)(kf_http_body_len()/1024u));set_status(b);
		} else if(st==KF_HTTP_ERROR){
			img_fetching = 0;
			if(img_cur>=0 && img_cur<imgs_n) set_img_error(img_cur,"fetch failed");
			img_cur = -1;
		}else{char b[88];snprintf(b,sizeof b,"Image %d/%d %luKB %luMHz%s [F9 stop]",
			img_cur+1,imgs_n,(unsigned long)(kf_http_body_len()/1024u),
			(unsigned long)(kf_clock_khz()/1000u),s_http_eco_fallback?" fallback":"");set_status(b);}
		return;
	}
	for(int i=0;i<imgs_n;i++){
		if(imgs[i].state==0){
			static char src[512], abs[600], cp[80], rp[80]; /* off the 4 KB stack */
			kf_html_read_text(imgs[i].href_off, imgs[i].href_len, src, sizeof src);
			if(!kf_url_resolve(doc_base[0]?doc_base:cur_url, src, abs, sizeof abs)){ set_img_error(i,"bad URL"); return; }
			if(known_unsupported_image(abs)){set_img_error(i,"unsupported format");return;}
			imgs[i].cache_hash=url_hash32(abs);render_cache_path(imgs[i].cache_hash,rp,sizeof rp);
			int w,h;if(render_cache_info(rp,&w,&h)){char b[56];snprintf(b,sizeof b,"Image %d/%d from SD cache...",i+1,imgs_n);set_status(b);place_image_file(i,rp,w,h);return;}
			if(heap_free()<IMG_DECODE_FLOOR+48u*1024u){set_status("Images waiting for memory [F9 stop]");return;}
			legacy_cache_path(imgs[i].cache_hash,cp,sizeof cp);
			uint32_t cached=cache_load_psram(cp,s_arena,BODY_MAX);
			if(cached){
				char b[64];snprintf(b,sizeof b,"Image %d/%d migrating cache...",i+1,imgs_n);set_status(b);
				if(!decode_save_place(i,s_arena,cached,rp))set_img_error(i,"cached decode failed");
				return;
			}
			if(kf_http_get(abs)==0){img_cur=i;img_fetching=1;char b[64];snprintf(b,sizeof b,"Image %d/%d connecting... [F9 stop]",i+1,imgs_n);set_status(b);}
			else set_img_error(i,"request failed");
			return;
		}
	}
	images_ready_status();
}

/* create one container box and push it as the current parent */
static void make_box(const kf_html_op *op){
	int kind = op->index;
	lv_obj_t *bx = lv_obj_create(bstk[bdepth]);
	lv_obj_remove_style_all(bx);
	lv_obj_clear_flag(bx, LV_OBJ_FLAG_SCROLLABLE);  /* only doc scrolls */
	lv_obj_set_height(bx, LV_SIZE_CONTENT);
	if(brow[bdepth] || kind==KF_BOX_TD){            /* cell / box inside a row */
		if(bcols[bdepth]) lv_obj_set_width(bx,lv_pct(96/bcols[bdepth]));
		else { lv_obj_set_width(bx, LV_SIZE_CONTENT);lv_obj_set_flex_grow(bx,1); }
	} else
		lv_obj_set_width(bx, lv_pct(100));
	lv_flex_flow_t flow=LV_FLEX_FLOW_COLUMN;
	if(reader_mode) flow=LV_FLEX_FLOW_COLUMN;
	else if(kind==KF_BOX_GRID || kind==KF_BOX_TR) flow=LV_FLEX_FLOW_ROW_WRAP;
	else if(kind==KF_BOX_ROW){
		if(op->flex_dir==1) flow=op->flex_wrap?LV_FLEX_FLOW_COLUMN_WRAP:LV_FLEX_FLOW_COLUMN;
		else if(op->flex_dir==3) flow=op->flex_wrap?LV_FLEX_FLOW_COLUMN_WRAP_REVERSE:LV_FLEX_FLOW_COLUMN_REVERSE;
		else if(op->flex_dir==2) flow=op->flex_wrap?LV_FLEX_FLOW_ROW_WRAP_REVERSE:LV_FLEX_FLOW_ROW_REVERSE;
		else flow=op->flex_wrap?LV_FLEX_FLOW_ROW_WRAP:LV_FLEX_FLOW_ROW;
	}
	lv_obj_set_flex_flow(bx,flow);
	lv_flex_align_t maina=LV_FLEX_ALIGN_START, crossa=LV_FLEX_ALIGN_START;
	switch(op->justify){
	case KF_CSS_JUSTIFY_CENTER: maina=LV_FLEX_ALIGN_CENTER; break;
	case KF_CSS_JUSTIFY_END: maina=LV_FLEX_ALIGN_END; break;
	case KF_CSS_JUSTIFY_BETWEEN: maina=LV_FLEX_ALIGN_SPACE_BETWEEN; break;
	case KF_CSS_JUSTIFY_AROUND: maina=LV_FLEX_ALIGN_SPACE_AROUND; break;
	case KF_CSS_JUSTIFY_EVENLY: maina=LV_FLEX_ALIGN_SPACE_EVENLY; break;
	default: break;
	}
	if(op->align==KF_CSS_ALIGN_CENTER) crossa=LV_FLEX_ALIGN_CENTER;
	else if(op->align==KF_CSS_ALIGN_END) crossa=LV_FLEX_ALIGN_END;
	lv_obj_set_flex_align(bx,maina,crossa,crossa);
	int defpad=kind==KF_BOX_CARD?4:2;
	lv_obj_set_style_pad_ver(bx,op->pad_v?op->pad_v:defpad,0);
	lv_obj_set_style_pad_hor(bx,op->pad_h?op->pad_h:defpad,0);
	lv_obj_set_style_pad_row(bx,op->gap?op->gap:2,0);
	lv_obj_set_style_pad_column(bx,op->gap?op->gap:5,0);
	if(!reader_mode&&(op->sflags & KF_ST_BG)){
		lv_obj_set_style_bg_color(bx, c565(op->bg), 0);
		lv_obj_set_style_bg_opa(bx, LV_OPA_COVER, 0);
	}
	if(!reader_mode&&op->border_w){
		lv_obj_set_style_border_width(bx, op->border_w, 0);
		lv_obj_set_style_border_color(bx, c565(op->border_c), 0);
	} else if(kind==KF_BOX_TD){                     /* legible tables by default */
		lv_obj_set_style_border_width(bx, 1, 0);
		lv_obj_set_style_border_color(bx, WEB_RULE, 0);
	}
	if(!reader_mode&&op->radius) lv_obj_set_style_radius(bx, op->radius, 0);
	if(!reader_mode)apply_fx(bx,op);
	bdepth++;
	bstk[bdepth] = bx;
	brow[bdepth] = !reader_mode&&(kind==KF_BOX_ROW || kind==KF_BOX_TR || kind==KF_BOX_GRID) ? 1 : 0;
	bcols[bdepth] = !reader_mode&&kind==KF_BOX_GRID ? (op->grid_cols?op->grid_cols:2) : 0;
}

static void render_window(uint32_t start){
	lv_obj_clean(doc);
	free_images();              /* drop the previous page's decoded bitmaps */
	foci_n=0; cur_focus=-1; fld_n=0;
	/* The exported page style merges body with the visible html/root paint. */
	kf_css_style bs; kf_html_body_style(&bs);
	lv_obj_set_style_bg_color(scr, (!reader_mode&&(bs.flags & KF_CSS_F_BG)) ? c565(bs.bg) : WEB_BG, 0);
	/* Body padding belongs to the page box, not to every child. */
	lv_obj_set_style_pad_left(doc, 4 + (!reader_mode ? bs.pad_h : 0), 0);
	lv_obj_set_style_pad_right(doc, 4 + (!reader_mode ? bs.pad_h : 0), 0);
	lv_obj_set_style_pad_top(doc, 4 + (!reader_mode ? bs.pad_v : 0), 0);
	lv_obj_set_style_pad_bottom(doc, 4 + (!reader_mode ? bs.pad_v : 0), 0);
	bstk[0] = doc; brow[0] = 0; bcols[0]=0; bdepth = 0; bskip = 0; run_parent=NULL;
	uint32_t n = kf_html_op_count();
	if(start > n) start = n;
	/* reconstruct containers still open at `start` (window may begin mid-box) */
	if(start){
		uint32_t open[BOX_DEPTH]; int nopen = 0;
		for(uint32_t i=0; i<start; i++){
			kf_html_op op; kf_html_get_op(i, &op);
			if(op.kind==KF_OP_BOX){ if(nopen < BOX_DEPTH) open[nopen] = i; nopen++; }
			else if(op.kind==KF_OP_END){ if(nopen > 0) nopen--; }
		}
		if(nopen > BOX_DEPTH) nopen = BOX_DEPTH;
		for(int k=0; k<nopen && bdepth < BOX_DEPTH-1; k++){
			kf_html_op op; kf_html_get_op(open[k], &op);
			make_box(&op);
		}
	}
	int widgets=0;
	lv_obj_t *inline_row=NULL; int inline_depth=-1;
	/* static: keep these big buffers OFF the stack — render runs inside LVGL's deep
	   layout/render call chain and the core0 stack is only 4 KB (overflow -> corruption
	   that surfaces as garbled text). Not reentrant, so static is safe. */
	static char buf[1024];
	static char line[1100];
	int low_mem=0;
	uint32_t i;
	for(i=start; i<n && widgets<MAX_WIDGETS; i++){
		if(heap_free() < HEAP_FLOOR){ low_mem=1; break; }   /* stop before OOM-panic */
		uint32_t tl_before = lv_obj_get_child_count(doc);   /* for op-index tagging */
		kf_html_op op; kf_html_get_op(i, &op);
		int isrun=(op.lflags&KF_LAY_INLINE) &&
		          (op.kind==KF_OP_P||op.kind==KF_OP_LINK||op.kind==KF_OP_IMG);
		if(isrun&&brow[bdepth]){ run_parent=NULL;inline_row=NULL;inline_depth=-1; }
		else if(isrun){
			if(!inline_row||inline_depth!=bdepth){
				inline_row=lv_obj_create(bstk[bdepth]);lv_obj_remove_style_all(inline_row);
				lv_obj_set_size(inline_row,lv_pct(100),LV_SIZE_CONTENT);
				lv_obj_set_flex_flow(inline_row,LV_FLEX_FLOW_ROW_WRAP);
				lv_obj_set_style_pad_all(inline_row,0,0);lv_obj_set_style_pad_column(inline_row,2,0);
				inline_depth=bdepth;
			}
			run_parent=inline_row;
		} else { run_parent=NULL; inline_row=NULL; inline_depth=-1; }
		kf_html_read_text(op.text_off, op.text_len, buf, sizeof buf);
		xform_buf(buf, op.xform);
		/* CSS overrides: color + font-size, on top of the per-kind defaults */
		const lv_font_t *fnt=NULL;int big=(op.sflags&KF_ST_BIG)?1:0;
		if(op.font_id>=1&&op.font_id<=PAGE_FONT_MAX)fnt=page_fonts[op.font_id-1][big];
		if(!fnt)fnt=(op.lflags&KF_LAY_MONO)?(big?KF_FONT_BIG:KF_FONT):(big?WEB_FONT_BIG:WEB_FONT);
		lv_color_t      ctx  = (!reader_mode&&(op.sflags & KF_ST_FG)) ? c565(op.fg) : WEB_TEXT;
		switch(op.kind){
		case KF_OP_H1: case KF_OP_H2: {
			const lv_font_t *hf=fnt;
			if(!op.font_id)hf=(op.lflags&KF_LAY_MONO)?KF_FONT_BIG:WEB_FONT_BIG;
			lv_obj_t *l = mk_label(buf,hf,ctx,padc(0,&op));
			post_style(l,&op); widgets++; break;
		}
		case KF_OP_H3: case KF_OP_H4: case KF_OP_H5: case KF_OP_H6:
		case KF_OP_P: {
			lv_obj_t *l = mk_label(buf, fnt, ctx, padc(0,&op));
			post_style(l,&op); widgets++; break;
		}
		case KF_OP_QUOTE: {
			lv_obj_t *l = mk_label(buf, fnt,
			    (op.sflags & KF_ST_FG) ? ctx : WEB_DIM, padc(8 + op.depth*8,&op));
			post_style(l,&op); widgets++; break;
		}
		case KF_OP_LI: {
			const char *txt;
			if(op.sflags & KF_ST_NOBULLET) txt = buf;         /* list-style: none */
			else {
				if(op.index) snprintf(line,sizeof line,"%u. %s", op.index, buf);
				else         snprintf(line,sizeof line,"- %s", buf);
				txt = line;
			}
			lv_obj_t *l = mk_label(txt, fnt, ctx, padc(6 + op.depth*10,&op));
			post_style(l,&op); widgets++; break;
		}
		case KF_OP_PRE: {
			lv_obj_t *l = mk_label(buf, KF_FONT,
			    (op.sflags & KF_ST_FG) ? ctx : WEB_PRE, padc(4,&op));
			post_style(l,&op); widgets++; break;
		}
		case KF_OP_IMG: {
			snprintf(line,sizeof line,"[img: %s]", buf);
			lv_obj_t *l = mk_label(line, WEB_FONT, WEB_MUTED, 2);
			if(op.href_len && imgs_n<IMG_MAX){     /* queue for background fetch */
				imgs[imgs_n].w=l; imgs[imgs_n].href_off=op.href_off;
				imgs[imgs_n].href_len=op.href_len; imgs[imgs_n].state=0;
				imgs[imgs_n].iw=imgs[imgs_n].ih=0; imgs[imgs_n].cache_hash=0; imgs_n++;
			}
			widgets++; break;
		}
		case KF_OP_HR: {
			lv_obj_t *h=lv_obj_create(bstk[bdepth]); lv_obj_remove_style_all(h);
			lv_obj_set_size(h, lv_pct(96), 2); lv_obj_set_style_bg_color(h, WEB_RULE, 0);
			lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0); widgets++; break;
		}
		case KF_OP_BOX:
			if(bdepth >= BOX_DEPTH-1){ bskip++; break; }
			make_box(&op);
			widgets++;
			break;
		case KF_OP_END:
			if(bskip) bskip--;
			else if(bdepth > 0) bdepth--;
			break;
		case KF_OP_LINK: {
			lv_obj_t *l=mk_label(buf[0]?buf:"(link)", fnt,
			    (op.sflags & KF_ST_FG) ? ctx : WEB_LINK, padc(2,&op));
			lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
			post_style(l,&op);
			add_focus(l, KF_OP_LINK, &op, -1); widgets++; break;
		}
		case KF_OP_FIELD: {
			int fi = fld_n<MAX_FIELDS ? fld_n++ : MAX_FIELDS-1;
			kf_html_read_text(op.href_off, op.href_len, fld_name[fi], sizeof fld_name[fi]);
			snprintf(fld_val[fi], sizeof fld_val[fi], "%s", buf);
			fld_type[fi]=(uint8_t)op.index; fld_checked[fi]=op.depth?1:0;
			if(fld_type[fi]==KF_FIELD_HIDDEN) break;
			if(fld_type[fi]==KF_FIELD_CHECKBOX||fld_type[fi]==KF_FIELD_RADIO)
				snprintf(line,sizeof line,"[%c] %s",fld_checked[fi]?'x':' ',fld_name[fi][0]?fld_name[fi]:fld_val[fi]);
			else if(fld_type[fi]==KF_FIELD_PASSWORD){
				int n=(int)strlen(fld_val[fi]); if(n>32)n=32;
				line[0]='['; line[1]=' '; for(int z=0;z<n;z++)line[2+z]='*';
				line[2+n]=' '; line[3+n]=']'; line[4+n]=0;
			} else snprintf(line,sizeof line,"[ %s ]", fld_val[fi][0]?fld_val[fi]:"...");
			lv_obj_t *l=mk_label(line, WEB_FONT, WEB_TEXT, 2);
			post_style(l,&op);
			add_focus(l, KF_OP_FIELD, &op, fi); widgets++; break;
		}
		case KF_OP_SUBMIT: {
			snprintf(line,sizeof line,"[ %s ]", buf[0]?buf:"Submit");
			lv_obj_t *l=mk_label(line, WEB_FONT, WEB_LINK, 2);
			post_style(l,&op);
			add_focus(l, KF_OP_SUBMIT, &op, -1); widgets++; break;
		}
		default: break;
		}
		if(lv_obj_get_child_count(doc) > tl_before)        /* new top-level widget: */
			lv_obj_set_user_data(lv_obj_get_child(doc, tl_before),
			                     (void*)(uintptr_t)(i+1)); /* tag with its op (+1) */
	}
	run_parent=NULL;
	win_first = start; win_next = i; win_more = (i < n);
	if(win_more || low_mem)
		mk_label("...", KF_FONT, WEB_MUTED, 2);
	lv_obj_scroll_to_y(doc, 0, LV_ANIM_OFF);
	if(mouse_ptr){lv_obj_move_foreground(mouse_ptr);pointer_hit();}
}
static void render_ops(void){ render_window(0); }

/* ---------- external stylesheets ----------
   After the page HTML parses (pass A: <style> rules + <link> URLs collected),
   each linked sheet is satisfied from the SD cache (/kefyros/spineko/css/, keyed
   by URL hash) or fetched into the CSSRAW scratch region and cached. Then the
   still-intact BODY re-parses (pass B) with the full rule set and renders. */
#define CSS_CACHE_DIR "/kefyros/spineko/css-v2"

static void css_cache_path(const char *url, char *out, int cap){
	uint32_t h = 2166136261u;
	for(const char *p=url; *p; p++) h = (h ^ (uint8_t)*p) * 16777619u;
	snprintf(out, cap, CSS_CACHE_DIR "/%08lx.css", (unsigned long)h);
}
static int css_feed_file(const char *path,const char *base_url){
	FILE *f = fopen(path, "rb");
	if(!f) return 0;
	static uint8_t b[512]; size_t n;
	kf_css_sheet_set_base(base_url);
	kf_css_sheet_begin();
	while((n = fread(b,1,sizeof b,f)) > 0)
		for(size_t i=0;i<n;i++) kf_css_sheet_feed(b[i]);
	fclose(f);
	kf_css_sheet_end();
	return 1;
}
static void css_feed_psram(uint32_t base, uint32_t len,const char *base_url){
	static uint8_t b[512];
	kf_css_sheet_set_base(base_url);
	kf_css_sheet_begin();
	for(uint32_t o=0; o<len; o+=sizeof b){
		uint32_t w = len-o; if(w > sizeof b) w = sizeof b;
		kf_psram_read(base+o, b, w);
		for(uint32_t i=0;i<w;i++) kf_css_sheet_feed(b[i]);
	}
	kf_css_sheet_end();
}
static void css_save_cache(const char *path, uint32_t base, uint32_t len){
	if(!len || len > 512u*1024) return;
	FILE *f = fopen(path, "wb");
	if(!f) return;
	static uint8_t b[512];
	for(uint32_t o=0; o<len; o+=sizeof b){
		uint32_t w = len-o; if(w > sizeof b) w = sizeof b;
		kf_psram_read(base+o, b, w);
		if(fwrite(b,1,w,f) != w) break;
	}
	fclose(f);
}
static void free_page_fonts(void){
	for(int i=0;i<PAGE_FONT_MAX;i++)for(int z=0;z<2;z++)if(page_fonts[i][z]){
		lv_tiny_ttf_destroy(page_fonts[i][z]);page_fonts[i][z]=NULL;
	}
	font_active=0;font_i=0;
}
static void font_cache_path(const char *url,char *out,int cap){
	snprintf(out,cap,FONT_CACHE_DIR "/%08lx.ttf",(unsigned long)url_hash32(url));
}
static int font_file_valid(const char *path){
	struct stat st;if(stat(path,&st)!=0||st.st_size<512||st.st_size>512u*1024u)return 0;
	FILE *f=fopen(path,"rb");if(!f)return 0;uint8_t h[4];int ok=fread(h,1,4,f)==4;fclose(f);
	return ok&&((h[0]==0&&h[1]==1&&h[2]==0&&h[3]==0)||!memcmp(h,"OTTO",4)||!memcmp(h,"ttcf",4));
}
static int font_save(const char *path,uint32_t base,uint32_t len){
	if(!len||len>512u*1024u)return 0;static char tmp[96];snprintf(tmp,sizeof tmp,"%s.tmp",path);remove(tmp);
	FILE *f=fopen(tmp,"wb");if(!f)return 0;static uint8_t b[512];int ok=1;
	for(uint32_t o=0;o<len&&ok;o+=sizeof b){uint32_t n=len-o;if(n>sizeof b)n=sizeof b;
		kf_psram_read(base+o,b,n);if(fwrite(b,1,n,f)!=n)ok=0;}
	if(fclose(f)!=0)ok=0;if(!ok){remove(tmp);return 0;}
	remove(path);if(rename(tmp,path)!=0){remove(tmp);return 0;}return font_file_valid(path);
}
static int font_load(int slot,const char *path){
	if(slot<0||slot>=PAGE_FONT_MAX||!font_file_valid(path))return 0;
	static char lvpath[96];snprintf(lvpath,sizeof lvpath,"A:%s",path);
	page_fonts[slot][0]=lv_tiny_ttf_create_file_ex(lvpath,14,LV_FONT_KERNING_NORMAL,8);
	page_fonts[slot][1]=lv_tiny_ttf_create_file_ex(lvpath,20,LV_FONT_KERNING_NORMAL,8);
	if(page_fonts[slot][0]&&page_fonts[slot][1])return 1;
	if(page_fonts[slot][0])lv_tiny_ttf_destroy(page_fonts[slot][0]);
	if(page_fonts[slot][1])lv_tiny_ttf_destroy(page_fonts[slot][1]);
	page_fonts[slot][0]=page_fonts[slot][1]=NULL;return 0;
}
static void page_style_finish(void){
	kf_http_set_arena(s_arena, BODY_MAX);
	uint32_t t0=(uint32_t)(time_us_64()/1000u);
	if(css_n>0)kf_html_reparse();
	s_css_ms=(uint32_t)(time_us_64()/1000u)-t0;render_page();
}
static void fonts_next(void){
	int n=kf_css_font_count();if(n>PAGE_FONT_MAX)n=PAGE_FONT_MAX;
	while(font_i<n){
		int i=font_i++;const char *src=kf_css_font_src(i),*base=kf_css_font_base(i);
		if(!kf_url_resolve(base&&base[0]?base:(doc_base[0]?doc_base:cur_url),src,font_abs,sizeof font_abs))continue;
		font_cache_path(font_abs,font_cachef,sizeof font_cachef);
		if(font_load(i,font_cachef))continue;
		remove(font_cachef);kf_http_set_arena(s_arena+CSSRAW_OFF,CSSRAW_MAX);
		if(kf_http_get(font_abs)==0){font_slot=i;font_active=1;char b[40];snprintf(b,sizeof b,"font %d/%d...",i+1,n);set_status(b);return;}
	}
	page_style_finish();
}
static void render_page(void){
	uint32_t t0 = (uint32_t)(time_us_64()/1000u);
	uint32_t css_rules=kf_css_rule_count();
	render_ops();
	s_render_ms = (uint32_t)(time_us_64()/1000u) - t0;
	/* Matching is finished: all computed styles now live in the PSRAM op list.
	   Release the 16 KB rule index + ~6 KB variable table before image/font work. */
	kf_css_shutdown();
	const char *title = kf_html_title();
	snprintf(s_ready_status, sizeof s_ready_status, "%d %luK N%lu P%lu C%lu R%lu H%luK %luM%s",
	         s_page_status, (unsigned long)(kf_http_body_len()/1024u),
	         (unsigned long)s_net_ms, (unsigned long)s_parse_ms,
	         (unsigned long)s_css_ms, (unsigned long)s_render_ms,
	         (unsigned long)(heap_free()/1024u),
	         (unsigned long)(kf_clock_khz()/1000u),
	         css_rules ? "" : " no-css");
	set_status(s_ready_status);assets_finished=0;
	(void)title;
}
static void css_done(void){
	font_i=0;fonts_next();
}
static void css_next(void){
	while(css_i < css_n){
		/* static: off the 4 KB core0 stack */
		static char href[300], abs[600];
		snprintf(href, sizeof href, "%s", kf_html_css_link(css_i));
		css_i++;
		if(!kf_url_resolve(doc_base[0]?doc_base:cur_url, href, abs, sizeof abs)) continue;
		snprintf(css_abs,sizeof css_abs,"%s",abs);
		css_cache_path(abs, css_cachef, sizeof css_cachef);
		if(css_feed_file(css_cachef,abs)) continue;             /* SD cache hit */
		if(kf_net_state()==KF_NET_ONLINE){
			kf_http_set_arena(s_arena+CSSRAW_OFF, CSSRAW_MAX);
			if(kf_http_get(css_abs)==0){
				css_active = 1;
				char b[48]; snprintf(b,sizeof b,"style %d/%d...",css_i,css_n);
				set_status(b);
				return;                                         /* resume in browser_poll */
			}
			kf_http_set_arena(s_arena, BODY_MAX);
		}
	}
	css_done();
}

/* ---------- the built-in start page ---------- */
static const char START_HTML[] =
	"<title>Spineko</title>"
	/* dogfood the CSS engine: if the start page renders colored, CSS works */
	"<style>"
	":root{--hot:#c2185b;--sea:#00695c;--ice:#eef6ff}"
	"h1{color:var(--hot)} h2{color:var(--sea)}"
	".warn{color:#a00000}.dim{color:#777}"
	".toolbar{display:flex;flex-wrap:wrap;gap:4px;padding:4px;background:var(--ice);border-radius:6px}"
	".toolbar>a:link{background:#dbeaff;border:1px solid #79a7dd;border-radius:5px;padding:3px}"
	".grid{display:grid;grid-template-columns:repeat(2,1fr);gap:5px;margin:4px}"
	".grid>.card{padding:5px;border:1px solid #b8c4d0;border-radius:6px;box-shadow:0 2px 5px #8090a0}"
	".grid>.card:first-child{background:#fff2d8}"
	"input[type]{padding:4px;border:1px solid #789;border-radius:5px;background:#f8fbff}"
	"</style>"
	"<h1>Spineko</h1>"
	"<p>Web browser. F1 address, F2 reload, F3 forward, F4 reader, F5 find, F6 next. "
	"Arrows move the pointer; keep pressing at an edge to scroll. TAB jumps controls, ENTER clicks. "
	"F9 stops a stuck transfer; F10 clears the browser cache.</p>"
	"<h2>Bookmarks</h2><div class=\"toolbar\">"
	"<a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a>"
	"<a href=\"https://verpitek.com/\">Verpitek</a>"
	"<a href=\"https://lite.cnn.com/\">CNN Lite</a>"
	"<a href=\"http://info.cern.ch/\">CERN</a>"
	"<a href=\"http://example.com/\">example.com</a>"
	"<a href=\"http://bettermotherfuckingwebsite.com/\">CSS test</a>"
	"<a href=\"http://textfiles.com/\">textfiles</a></div>"
	"<h2>400 MHz engine</h2><div class=\"grid\">"
	"<div class=\"card\"><b>CSS</b><p>child selectors, variables, important cascade, flex and grid</p></div>"
	"<div class=\"card\"><b>Network</b><p>250 MHz join, then 400 MHz browsing with live PIO reclocking</p></div>"
	"</div>"
	"<h2>Form test</h2><form method=\"get\" action=\"https://en.wikipedia.org/w/index.php\">"
	"<input type=\"hidden\" name=\"title\" value=\"Special:Search\">"
	"<input type=\"text\" name=\"search\" value=\"RP2350\"><input type=\"submit\" value=\"Search Wikipedia\"></form>"
	"<h2>Layout test</h2>"
	"<table><tr><th>chip</th><th>sram</th><th>mhz</th></tr>"
	"<tr><td>RP2350</td><td>520K</td><td>150</td></tr>"
	"<tr><td>RP2040</td><td>264K</td><td>133</td></tr></table>"
	"<hr>"
	"<p class=\"warn\"><b>Security:</b> HTTPS uses TLS 1.2 (BearSSL) with <b>no certificate check</b>, so the "
	"connection is encrypted but not verified - don't enter passwords or anything sensitive.</p>"
	"<p class=\"dim\">Pages render with basic CSS (colors, backgrounds, alignment, display:none; "
	"from style tags, linked sheets + SD cache, and style attributes) and inline "
	"JPEG/PNG/SVG images (downscaled); scripts ignored.</p>";

static void render_start_page(void){
	if(s_arena==0xFFFFFFFFu) return;
	uint32_t len = (uint32_t)(sizeof START_HTML - 1);
	kf_psram_write(s_arena, START_HTML, len);
	kf_css_sheet_set_base("about:start");
	kf_html_parse(s_arena, len);
	render_ops();
	kf_css_shutdown();
	set_status("Spineko - start page");
}

/* ---------- navigation ---------- */
static uint32_t hist_addr(int forward,int index){
	return s_arena+HIST_OFF+(uint32_t)(forward?HIST_DEPTH:0)*HIST_REC+(uint32_t)index*HIST_REC;
}
static void hist_write(int forward,int index,const char *u){
	static char b[HIST_REC];snprintf(b,sizeof b,"%s",u?u:"");
	kf_psram_write(hist_addr(forward,index),b,sizeof b);
}
static void hist_read(int forward,int index,char *out,int cap){
	static char b[HIST_REC];kf_psram_read(hist_addr(forward,index),b,sizeof b);b[HIST_REC-1]=0;
	snprintf(out,cap,"%s",b);
}
static void hist_shift(int forward){
	static char b[HIST_REC];
	for(int i=1;i<HIST_DEPTH;i++){
		kf_psram_read(hist_addr(forward,i),b,sizeof b);
		kf_psram_write(hist_addr(forward,i-1),b,sizeof b);
	}
}
static void hist_push(const char *u){
	if(!u || !u[0]) return;
	if(hist_sp<HIST_DEPTH)hist_write(0,hist_sp++,u);
	else{hist_shift(0);hist_write(0,HIST_DEPTH-1,u);}
}
static void fwd_push(const char *u){
	if(!u||!u[0])return;
	if(fwd_sp<HIST_DEPTH)hist_write(1,fwd_sp++,u);
	else{hist_shift(1);hist_write(1,HIST_DEPTH-1,u);}
}

/* Desktop Wikipedia spends the first virtual render window on skin/navigation.
   MediaWiki's official REST HTML route returns the article body directly. Keep the
   normal URL in history/address bar, but fetch the lean representation underneath. */
static int wiki_rest_url(const char *url,char *out,int cap,char *base,int bcap){
	const char *sch=strstr(url,"://");if(!sch)return 0;
	const char *host=sch+3,*path=strchr(host,'/');if(!path)return 0;
	int hn=(int)(path-host);if(hn<15||hn>120)return 0;
	char h[128];memcpy(h,host,hn);h[hn]=0;
	int hl=(int)strlen(h);if(hl<14||strcmp(h+hl-14,".wikipedia.org"))return 0;
	if(strncmp(path,"/wiki/",6))return 0;
	const char *title=path+6;if(!*title||!strncmp(title,"Special:",8))return 0;
	char enc[420];int n=0;
	for(;*title&&*title!='?'&&*title!='#'&&n<(int)sizeof enc-4;title++){
		if(*title=='/'){enc[n++]='%';enc[n++]='2';enc[n++]='F';}
		else enc[n++]=*title;
	}
	enc[n]=0;if(!n)return 0;
	int sn=(int)(sch-url);snprintf(out,cap,"%.*s://%s/w/rest.php/v1/page/%s/html",sn,url,h,enc);
	snprintf(base,bcap,"%.*s://%s/wiki/",sn,url,h);
	return 1;
}

static void load_url(const char *url, int push){
	if(!url || !url[0]) return;
	if(!strncmp(url,"about:start",11)){
		if(push && cur_url[0]){ hist_push(cur_url); fwd_sp=0; }
		if(doc)lv_obj_clean(doc);free_images();free_page_fonts();css_active=font_active=0;
		snprintf(cur_url, sizeof cur_url, "about:start");snprintf(doc_base,sizeof doc_base,"about:start");s_wiki_rest=0;
		show_url(); render_start_page(); s_loading=0; return;
	}
	/* require an absolute http(s) URL (URLs-only address bar) */
	char full[600];
	if(strstr(url,"://")) snprintf(full,sizeof full,"%s",url);
	else                  snprintf(full,sizeof full,"http://%s",url);   /* bare host -> http */
	if(push && cur_url[0]){ hist_push(cur_url); fwd_sp=0; }
	snprintf(cur_url, sizeof cur_url, "%s", full);
	static char request_url[700];
	s_wiki_rest=wiki_rest_url(full,request_url,sizeof request_url,doc_base,sizeof doc_base);
	if(!s_wiki_rest)snprintf(doc_base,sizeof doc_base,"%s",full);
	show_url();
	lv_obj_clean(doc); foci_n=0; free_images();free_page_fonts(); /* cancel page assets */
	css_active = 0; css_n = 0;                    /* cancel a pending stylesheet chain */
	if(s_arena != 0xFFFFFFFFu) kf_http_set_arena(s_arena, BODY_MAX);
	set_status("Loading...");
	s_nav_t0 = (uint32_t)(time_us_64()/1000u);
	s_net_ms = s_parse_ms = s_css_ms = s_render_ms = 0;
	if(kf_net_state()!=KF_NET_ONLINE){ set_status("offline - open WiFi first"); s_loading=0; return; }
	int rc = kf_http_get(s_wiki_rest?request_url:full);
	if(rc<0){ set_status(kf_http_err()); s_loading=0; }
	else s_loading=1;
}

static void load_post(const char *full){
	if(!full||!full[0])return;
	if(cur_url[0]) hist_push(cur_url);
	fwd_sp=0;
	snprintf(cur_url,sizeof cur_url,"%s",full);snprintf(doc_base,sizeof doc_base,"%s",full);s_wiki_rest=0;show_url();
	lv_obj_clean(doc); foci_n=0; free_images();free_page_fonts();css_active=0;css_n=0;
	if(s_arena!=0xFFFFFFFFu) kf_http_set_arena(s_arena,BODY_MAX);
	set_status("Posting...");
	s_nav_t0=(uint32_t)(time_us_64()/1000u); s_net_ms=s_parse_ms=s_css_ms=s_render_ms=0;
	if(kf_net_state()!=KF_NET_ONLINE){ set_status("offline - open WiFi first"); s_loading=0; return; }
	static const char hdr[]="Content-Type: application/x-www-form-urlencoded\r\n";
	int rc=kf_http_post(full,hdr,form_body,(uint32_t)strlen(form_body));
	if(rc<0){ set_status(kf_http_err()); s_loading=0; } else s_loading=1;
}

static int form_enc(char *out,int cap,int pos,const char *s){
	static const char hx[]="0123456789ABCDEF";
	for(;*s&&pos<cap-1;s++){
		unsigned char c=(unsigned char)*s;
		if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') out[pos++]=(char)c;
		else if(c==' ')out[pos++]='+';
		else if(pos<cap-3){out[pos++]='%';out[pos++]=hx[c>>4];out[pos++]=hx[c&15];}
	}
	out[pos]=0; return pos;
}
static void form_build(void){
	int p=0; form_body[0]=0;
	for(int i=0;i<fld_n;i++){
		if(!fld_name[i][0])continue;
		if((fld_type[i]==KF_FIELD_CHECKBOX||fld_type[i]==KF_FIELD_RADIO)&&!fld_checked[i])continue;
		if(p&&p<(int)sizeof form_body-1)form_body[p++]='&';
		p=form_enc(form_body,sizeof form_body,p,fld_name[i]);
		if(p<(int)sizeof form_body-1)form_body[p++]='=';
		form_body[p]=0; p=form_enc(form_body,sizeof form_body,p,fld_val[i]);
	}
}
static void field_redraw(int fi,lv_obj_t *w){
	if(fi<0||fi>=fld_n||!w)return;
	if(fld_type[fi]==KF_FIELD_CHECKBOX||fld_type[fi]==KF_FIELD_RADIO)
		lv_label_set_text_fmt(w,"[%c] %s",fld_checked[fi]?'x':' ',fld_name[fi][0]?fld_name[fi]:fld_val[fi]);
	else if(fld_type[fi]==KF_FIELD_PASSWORD){
		static char m[40]; int n=(int)strlen(fld_val[fi]);if(n>32)n=32;
		m[0]='[';m[1]=' ';for(int i=0;i<n;i++)m[2+i]='*';m[2+n]=' ';m[3+n]=']';m[4+n]=0;
		lv_label_set_text(w,m);
	} else lv_label_set_text_fmt(w,"[ %s ]",fld_val[fi][0]?fld_val[fi]:"...");
}

static void follow_focus(void){
	if(cur_focus<0 || cur_focus>=foci_n) return;
	/* static scratch: keep these off the 4 KB core0 stack (see render_ops note). */
	static char href[512], abs[600], full[1700];
	focus_t *f=&foci[cur_focus];
	if(f->kind==KF_OP_LINK){
		kf_html_read_text(f->href_off, f->href_len, href, sizeof href);
		if(kf_url_resolve(doc_base[0]?doc_base:cur_url, href, abs, sizeof abs)) load_url(abs, 1);
	} else if(f->kind==KF_OP_FIELD){
		if(f->field>=0&&(fld_type[f->field]==KF_FIELD_CHECKBOX||fld_type[f->field]==KF_FIELD_RADIO)){
			if(fld_type[f->field]==KF_FIELD_RADIO){
				for(int i=0;i<fld_n;i++)if(i!=f->field&&fld_type[i]==KF_FIELD_RADIO&&!strcmp(fld_name[i],fld_name[f->field]))fld_checked[i]=0;
				fld_checked[f->field]=1;
			} else fld_checked[f->field]^=1;
			for(int i=0;i<foci_n;i++)if(foci[i].kind==KF_OP_FIELD&&foci[i].field>=0)
				field_redraw(foci[i].field,foci[i].w);
			return;
		}
		edit_mode=2; edit_field=f->field;
		snprintf(edit_buf,sizeof edit_buf,"%s", f->field>=0?fld_val[f->field]:"");
		set_status("edit field - type, ENTER ok, ESC cancel");
		lv_label_set_text_fmt(f->w, "[ %s_ ]", edit_buf);
	} else if(f->kind==KF_OP_SUBMIT){
		kf_html_read_text(f->href_off, f->href_len, href, sizeof href);
		if(!href[0]) snprintf(href,sizeof href,"%s",cur_url);
		if(!kf_url_resolve(doc_base[0]?doc_base:cur_url, href, abs, sizeof abs)) snprintf(abs,sizeof abs,"%s",cur_url);
		form_build();
		if(f->form_post) load_post(abs);
		else { snprintf(full,sizeof full,"%s%s%s",abs,form_body[0]?(strchr(abs,'?')?"&":"?"):"",form_body); load_url(full,1); }
	}
}

/* If the response body IS an image (direct image URL like https://cataas.com/cat),
   render it as a single full-page picture instead of parsing it as HTML. Handles
   JPEG/PNG/SVG; returns 1 if the body was an image (decodable or not). */
static int show_image_page(uint32_t base, uint32_t len){
	uint8_t sig[8]; if(len < 8) return 0;
	kf_psram_read(base, sig, 8);
	if(!kf_img_sniff(sig, 8)) return 0;          /* not an image response */
	lv_obj_clean(doc); foci_n=0; free_images();
	imgs_n=1;lv_memzero(&imgs[0],sizeof imgs[0]);
	imgs[0].w=mk_label("[decoding image...]",KF_FONT,KF_TEXT_MUTED,4);
	imgs[0].cache_hash=url_hash32(cur_url);static char rp[80];
	render_cache_path(imgs[0].cache_hash,rp,sizeof rp);
	int w,h;
	if(render_cache_info(rp,&w,&h))place_image_file(0,rp,w,h);
	else if(!decode_save_place(0,base,len,rp))set_img_error(0,"decode/memory failed");
	lv_obj_scroll_to_y(doc, 0, LV_ANIM_OFF);
	return 1;
}

static int contains_ci(const char *s,const char *q){
	if(!q[0])return 0;
	for(;*s;s++){
		int i=0;while(q[i]&&s[i]){char a=s[i],b=q[i];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)break;i++;}
		if(!q[i])return 1;
	}
	return 0;
}
static void find_next(void){
	if(!find_query[0])return;
	uint32_t n=kf_html_op_count();static char b[1024];
	for(int pass=0;pass<2;pass++){
		uint32_t a=pass?0:find_from,z=pass?find_from:n;
		for(uint32_t i=a;i<z;i++){
			kf_html_op op;kf_html_get_op(i,&op);if(!op.text_len)continue;
			kf_html_read_text(op.text_off,op.text_len,b,sizeof b);
			if(contains_ci(b,find_query)){
				find_from=i+1;render_window(i>8?i-8:0);
				char st[96];snprintf(st,sizeof st,"found '%s' at %lu/%lu",find_query,(unsigned long)(i+1),(unsigned long)n);set_status(st);return;
			}
		}
	}
	set_status("find: no match");find_from=0;
}

static void on_loaded(void){
	s_net_ms = (uint32_t)(time_us_64()/1000u) - s_nav_t0;
	if(!s_wiki_rest){
		snprintf(cur_url, sizeof cur_url, "%s", kf_http_final_url());
		snprintf(doc_base,sizeof doc_base,"%s",cur_url);
	}
	show_url();
	uint32_t base = kf_http_body_base(), len = kf_http_body_len();
	if(show_image_page(base, len)){
		char st[96]; snprintf(st, sizeof st, "%d  image %ux%u",
		    kf_http_status(), imgs_n?imgs[0].iw:0, imgs_n?imgs[0].ih:0);
		set_status(st);
		return;
	}
	s_page_status = kf_http_status();
	uint32_t pt0 = (uint32_t)(time_us_64()/1000u);
	kf_css_sheet_set_base(doc_base[0]?doc_base:cur_url);
	kf_html_parse(base, len);                    /* pass A: <style> rules + <link> URLs */
	s_parse_ms = (uint32_t)(time_us_64()/1000u) - pt0;
	css_i = 0; css_n = kf_html_css_link_count();
	if(css_n > 0) css_next();                    /* may finish synchronously off the SD cache */
	else css_done();
}

/* ---------- key handling (raw, grabbed) ---------- */
static void edit_commit(void){
	if(edit_mode==1){ int m=edit_mode; edit_mode=0; (void)m; load_url(edit_buf, 1); }
	else if(edit_mode==2){
		if(edit_field>=0) snprintf(fld_val[edit_field],sizeof fld_val[edit_field],"%s",edit_buf);
		if(cur_focus>=0 && cur_focus<foci_n)
			field_redraw(edit_field,foci[cur_focus].w);
		edit_mode=0; set_status("");
	}
	else if(edit_mode==3){snprintf(find_query,sizeof find_query,"%s",edit_buf);find_from=0;edit_mode=0;find_next();}
}
static void edit_cancel(void){
	if(edit_mode==2 && cur_focus>=0 && cur_focus<foci_n && edit_field>=0)
		lv_label_set_text_fmt(foci[cur_focus].w, "[ %s ]", fld_val[edit_field][0]?fld_val[edit_field]:"...");
	edit_mode=0; show_url(); set_status("");
}
static void edit_key(uint8_t key){
	int n=(int)strlen(edit_buf);
	if(key==DK_BACKSPACE){ if(n>0) edit_buf[n-1]=0; }
	else if(key>=0x20 && key<0x7f){ if(n<(int)sizeof edit_buf-1){ edit_buf[n]=(char)key; edit_buf[n+1]=0; } }
	if(edit_mode==1) show_url();
	else if(edit_mode==2 && cur_focus>=0 && cur_focus<foci_n){
		if(edit_field>=0&&fld_type[edit_field]==KF_FIELD_PASSWORD){
			static char m[40];int z=(int)strlen(edit_buf);if(z>31)z=31;
			m[0]='[';m[1]=' ';for(int i=0;i<z;i++)m[2+i]='*';m[2+z]='_';m[3+z]=' ';m[4+z]=']';m[5+z]=0;
			lv_label_set_text(foci[cur_focus].w,m);
		} else lv_label_set_text_fmt(foci[cur_focus].w,"[ %s_ ]",edit_buf);
	} else if(edit_mode==3){char b[96];snprintf(b,sizeof b,"find: %.72s_",edit_buf);set_status(b);}
}

/* Virtualization: when scrolling near a window edge, re-render the window anchored
   at the op of the top-visible widget so long pages scroll without a widget cap.
   lv_obj_get_y() is view-relative in LVGL 9 (children shift as the parent scrolls). */
static void virt_check(void){
	if(!doc || (!win_more && win_first==0)) return;
	lv_obj_update_layout(doc);
	int vh = lv_obj_get_height(doc);
	if(win_more && lv_obj_get_scroll_bottom(doc) < vh){        /* <1 screen left below */
		uint32_t cnt = lv_obj_get_child_count(doc);
		uint32_t target = win_first; int off = 0;
		for(uint32_t k=0; k<cnt; k++){
			lv_obj_t *w = lv_obj_get_child(doc, k);
			if(lv_obj_get_y(w) + lv_obj_get_height(w) > 0){    /* first visible */
				uintptr_t ud = (uintptr_t)lv_obj_get_user_data(w);
				if(ud){ target = (uint32_t)ud - 1; off = -lv_obj_get_y(w); }
				break;
			}
		}
		if(target > win_first){                                /* window can advance */
			render_window(target);
			lv_obj_update_layout(doc);
			lv_obj_scroll_to_y(doc, off > 0 ? off : 0, LV_ANIM_OFF);
		}
	} else if(win_first > 0 && lv_obj_get_scroll_y(doc) <= 8){ /* back at the top */
		uint32_t old = win_first;
		render_window(old > 150 ? old - 150 : 0);
		lv_obj_update_layout(doc);
		uint32_t cnt = lv_obj_get_child_count(doc);
		for(uint32_t k=0; k<cnt; k++){
			lv_obj_t *w = lv_obj_get_child(doc, k);
			uintptr_t ud = (uintptr_t)lv_obj_get_user_data(w);
			if(ud && (uint32_t)ud - 1 >= old){                 /* the old window top */
				int y = lv_obj_get_y(w) - 24;
				lv_obj_scroll_to_y(doc, y > 0 ? y : 0, LV_ANIM_OFF);
				break;
			}
		}
	}
}
static void doc_scroll(int dy){
	if(!doc) return;
	lv_obj_scroll_by(doc, 0, dy, LV_ANIM_OFF);
	virt_check();
}
static int pointer_hit(void){
	if(!mouse_ptr)return -1;
	lv_area_t ma;lv_obj_get_coords(mouse_ptr,&ma);
	/* Like a desktop cursor, the click hot spot is the upper-left arrow tip. */
	int x=ma.x1,y=ma.y1,best=-1,best_area=0x7fffffff;
	for(int i=0;i<foci_n;i++)if(foci[i].w&&!lv_obj_has_flag(foci[i].w,LV_OBJ_FLAG_HIDDEN)){
		lv_area_t a;lv_obj_get_coords(foci[i].w,&a);
		if(x>=a.x1&&x<=a.x2&&y>=a.y1&&y<=a.y2){
			int ar=lv_area_get_width(&a)*lv_area_get_height(&a);
			if(ar<best_area){best=i;best_area=ar;}
		}
	}
	if(best!=cur_focus){style_focus(cur_focus,0);cur_focus=best;style_focus(cur_focus,1);}
	return best;
}
static void pointer_place(int x,int y){
	if(!mouse_ptr)return;
	/* x/y are absolute display coordinates (the same space used by hit testing).
	   The pointer is a child of scr, whose content origin is pushed down by
	   kf_inset_top(), so convert back to parent-content coordinates here. */
	lv_area_t content;lv_obj_get_content_coords(scr,&content);
	mouse_x=x;mouse_y=y;lv_obj_set_pos(mouse_ptr,x-content.x1,y-content.y1);
	lv_obj_move_foreground(mouse_ptr);pointer_hit();
}
static void pointer_move(int dx,int dy){
	if(!mouse_ptr||!doc)return;
	lv_area_t d;lv_obj_get_coords(doc,&d);
	int nx=mouse_x+dx,ny=mouse_y+dy;
	int minx=d.x1,maxx=d.x2-13,miny=d.y1,maxy=d.y2-18;
	int sx=0,sy=0;
	if(nx<minx){nx=minx;sx=24;}else if(nx>maxx){nx=maxx;sx=-24;}
	if(ny<miny){ny=miny;sy=28;}else if(ny>maxy){ny=maxy;sy=-28;}
	if(sx||sy){lv_obj_scroll_by(doc,sx,sy,LV_ANIM_OFF);virt_check();}
	pointer_place(nx,ny);
}
static void pointer_warp_focus(int i){
	if(i<0||i>=foci_n||!foci[i].w)return;
	set_focus(i);lv_obj_update_layout(doc);
	lv_area_t a,d;lv_obj_get_coords(foci[i].w,&a);lv_obj_get_coords(doc,&d);
	/* Centre the TARGET beneath the pointer's upper-left hot spot, not beneath
	   the visual centre of the 14x18 cursor bitmap. */
	int x=a.x1+lv_area_get_width(&a)/2,y=a.y1+lv_area_get_height(&a)/2;
	if(x<d.x1)x=d.x1;if(x>d.x2-13)x=d.x2-13;
	if(y<d.y1)y=d.y1;if(y>d.y2-18)y=d.y2-18;
	pointer_place(x,y);
}
static int pointer_inside(int x,int y){
	/* Even/odd test at pixel centres against the classic arrow polygon. Coordinates
	   are doubled so this stays integer-only and deterministic on the RP2350. */
	static const int8_t px[]={0,0,4,8,11,7,13};
	static const int8_t py[]={0,13,9,17,15,8,8};
	int qx=x*2+1,qy=y*2+1,in=0;
	for(int i=0,j=6;i<7;j=i++){
		int yi=py[i]*2,yj=py[j]*2;
		if((yi>qy)!=(yj>qy)){
			int64_t cross=(int64_t)(px[j]-px[i])*2*(qy-yi);
			int64_t edge=(int64_t)(qx-px[i]*2)*(yj-yi);
			if((yj>yi)?(edge<cross):(edge>cross))in=!in;
		}
	}
	return in;
}
static void pointer_bitmap_init(void){
	static const int8_t nx[]={-1,1,0,0},ny[]={0,0,-1,1};
	for(int y=0;y<PTR_H;y++)for(int x=0;x<PTR_W;x++){
		int p=y*PTR_W+x,inside=pointer_inside(x,y),edge=0;
		if(inside)for(int n=0;n<4;n++)if(!pointer_inside(x+nx[n],y+ny[n])){edge=1;break;}
		pointer_map.rgb[p]=edge?0x0000u:0xffffu;
		pointer_map.alpha[p]=inside?255u:0u;
	}
}
static void pointer_create(void){
	pointer_bitmap_init();
	mouse_ptr=lv_image_create(scr);lv_image_set_src(mouse_ptr,&pointer_dsc);
	lv_obj_clear_flag(mouse_ptr,LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_update_layout(doc);lv_area_t d;lv_obj_get_coords(doc,&d);
	pointer_place(d.x1+lv_area_get_width(&d)/2,d.y1+lv_area_get_height(&d)/2);
}

static void browser_abort_active(void){
	kf_http_abort();s_loading=css_active=font_active=0;css_n=0;
	img_fetching=0;img_cur=-1;assets_stopped=1;
	if(s_arena!=0xFFFFFFFFu)kf_http_set_arena(s_arena,BODY_MAX);
	s_http_last_state=KF_HTTP_IDLE;s_http_last_bytes=0;
	s_http_progress_ms=(uint32_t)(time_us_64()/1000u);s_http_eco_fallback=0;
	kf_clock_normal();s_fast_clock=1;
}
static void browser_stop(void){
	int page=s_loading||css_active||font_active;browser_abort_active();
	if(page&&doc){lv_obj_clean(doc);foci_n=0;run_parent=NULL;
		mk_label("Load stopped. F2 retries; F10 clears browser cache.",KF_FONT,WEB_MUTED,4);}
	set_status("Stopped [F2 retry] [F10 clear cache]");
}
static int clear_cache_dir(const char *dir){
	DIR *d=opendir(dir);if(!d)return 0;struct dirent *e;int n=0;static char p[180];
	while((e=readdir(d))){if(!strcmp(e->d_name,".")||!strcmp(e->d_name,".."))continue;
		snprintf(p,sizeof p,"%s/%s",dir,e->d_name);if(remove(p)==0)n++;}
	closedir(d);return n;
}
static void browser_clear_cache(void){
	browser_abort_active();
	if(doc)lv_obj_clean(doc);foci_n=0;run_parent=NULL;free_images();assets_stopped=1;
	free_page_fonts();kf_css_shutdown();
	int n=clear_cache_dir(CSS_CACHE_DIR)+clear_cache_dir(IMG_RENDER_DIR)+
	      clear_cache_dir(FONT_CACHE_DIR)+clear_cache_dir(IMG_LEGACY_DIR);
	char b[100];snprintf(b,sizeof b,"Browser cache cleared (%d files). F2 reloads this URL.",n);
	mk_label(b,KF_FONT,WEB_TEXT,4);set_status("Cache cleared [F2 reload]");s_ready_status[0]=0;
}

void browser_poll(void){
	if(!scr || lv_screen_active()!=scr) return;
	/* DNS always starts at 250. Give TCP/TLS and the body transfer a chance to run
	   at 400, but if neither the HTTP phase nor byte count advances for 1.5 s,
	   fall back to 250 without aborting the socket. TCP retransmission then recovers
	   the same request. Keep the safe tier for the rest of that request; rendering
	   returns to 400 after DONE/ERROR. */
	int hs = kf_http_state();
	uint32_t hb = kf_http_body_len();
	uint32_t now = (uint32_t)(time_us_64()/1000u);
	if(hs != s_http_last_state || hb != s_http_last_bytes){
		s_http_last_state = hs; s_http_last_bytes = hb; s_http_progress_ms = now;
		if(hs==KF_HTTP_RESOLVING || hs==KF_HTTP_IDLE || hs==KF_HTTP_DONE || hs==KF_HTTP_ERROR)
			s_http_eco_fallback = 0;
	}
	int active = hs==KF_HTTP_RESOLVING || hs==KF_HTTP_CONNECTING || hs==KF_HTTP_RECEIVING;
	if(active && hs!=KF_HTTP_RESOLVING && !s_http_eco_fallback && now-s_http_progress_ms > 1500u)
		s_http_eco_fallback = 1;
	int need_eco = kf_net_state()!=KF_NET_ONLINE || hs==KF_HTTP_RESOLVING || (active && s_http_eco_fallback);
	if(!need_eco){
		if(!s_fast_clock || kf_clock_khz()!=300000u){ kf_clock_normal(); s_fast_clock=1; }
	} else if(s_fast_clock || kf_clock_khz()!=250000u){
		kf_clock_eco(); s_fast_clock=0;
	}
	kf_http_poll();

	if(s_loading){
		int st=kf_http_state();
		if(st==KF_HTTP_DONE){ s_loading=0; on_loaded(); }
		else if(st==KF_HTTP_ERROR){ s_loading=0; lv_obj_clean(doc); foci_n=0;
			mk_label(kf_http_err(), KF_FONT, lv_color_hex(0xe03c32), 4);
			set_status("error"); }
		else { char b[72]; snprintf(b,sizeof b,"Page %luKB %luMHz%s [F9 stop]",
			(unsigned long)(kf_http_body_len()/1024u), (unsigned long)(kf_clock_khz()/1000u),
			s_http_eco_fallback?" fallback":""); set_status(b); }
	}

	if(css_active){             /* a linked stylesheet is downloading */
		int st = kf_http_state();
		if(st==KF_HTTP_DONE){
			css_active = 0;
			css_feed_psram(kf_http_body_base(), kf_http_body_len(),css_abs);
			css_save_cache(css_cachef, kf_http_body_base(), kf_http_body_len());
			css_next();
		} else if(st==KF_HTTP_ERROR){
			css_active = 0;
			css_next();         /* skip this sheet, keep going */
		}else{char b[80];snprintf(b,sizeof b,"Style %d/%d %luKB %luMHz%s [F9 stop]",css_i,css_n,
			(unsigned long)(kf_http_body_len()/1024u),(unsigned long)(kf_clock_khz()/1000u),
			s_http_eco_fallback?" fallback":"");set_status(b);}
	}
	if(font_active){
		int st=kf_http_state();
		if(st==KF_HTTP_DONE){
			font_active=0;
			if(font_save(font_cachef,kf_http_body_base(),kf_http_body_len()))font_load(font_slot,font_cachef);
			fonts_next();
		}else if(st==KF_HTTP_ERROR){font_active=0;fonts_next();}
		else{char b[80];snprintf(b,sizeof b,"Font %d/%d %luKB %luMHz%s [F9 stop]",font_slot+1,kf_css_font_count(),
			(unsigned long)(kf_http_body_len()/1024u),(unsigned long)(kf_clock_khz()/1000u),
			s_http_eco_fallback?" fallback":"");set_status(b);}
	}

	images_pump();              /* background: fetch + decode inline images one at a time */

	uint8_t stt, key;
	int doc_h = KF_CONTENT_H - URLBAR_H - STATUS_H;
	while(uart_pop_key(&stt, &key)){
		if(stt==KS_RELEASE) continue;
		int press = (stt==KS_PRESS);
		if(edit_mode){
			if(key==DK_ENTER){ if(press) edit_commit(); }
			else if(key==DK_ESC){ edit_cancel(); }
			else edit_key(key);
			continue;
		}
		switch(key){
		case DK_ESC:   kf_back_to_launcher(); return;       /* scr is being torn down */
		case DK_F1:    edit_mode=1; snprintf(edit_buf,sizeof edit_buf,"%s",
		                  strncmp(cur_url,"about:",6)?cur_url:""); show_url();
		               set_status("address - type URL, ENTER go, ESC cancel"); break;
		case DK_F1+1:  if(cur_url[0]){ char u[512];snprintf(u,sizeof u,"%s",cur_url);load_url(u,0); } break;
		case DK_F1+2:  if(fwd_sp>0){ char u[512];if(cur_url[0])hist_push(cur_url);hist_read(1,--fwd_sp,u,sizeof u);load_url(u,0);} break;
		case DK_F1+3:  reader_mode^=1;render_window(win_first);set_status(reader_mode?"reader mode":"page style mode");break;
		case DK_F1+4:  edit_mode=3;snprintf(edit_buf,sizeof edit_buf,"%s",find_query);set_status("find - type, ENTER search, ESC cancel");break;
		case DK_F1+5:  find_next();break;
		case DK_F1+8:  if(press)browser_stop();break;
		case DK_F1+9:  if(press)browser_clear_cache();break;
		case DK_BACKSPACE: if(hist_sp>0){ char u[512];if(cur_url[0])fwd_push(cur_url);hist_read(0,--hist_sp,u,sizeof u);load_url(u,0);} break;
		case DK_ENTER: if(press&&pointer_hit()>=0) follow_focus(); break;
		case DK_TAB:   if(foci_n>0)pointer_warp_focus(cur_focus<0?0:(cur_focus+1)%foci_n);break;
		case DK_UP:    pointer_move(0,-9); break;
		case DK_DOWN:  pointer_move(0,9); break;
		case DK_LEFT:  pointer_move(-9,0); break;
		case DK_RIGHT: pointer_move(9,0); break;
		case DK_PGUP:  doc_scroll(doc_h-24); break;
		case DK_PGDN:  doc_scroll(-(doc_h-24)); break;
		default: break;
		}
	}
}

/* ---------- lifecycle ---------- */
static void on_del(lv_event_t *e){ (void)e;
	free_images();             /* free decoded bitmaps before the screen tears down */
	free_page_fonts();
	scr=NULL; doc=NULL; lbl_url=NULL; lbl_status=NULL;mouse_ptr=NULL;
	kf_http_abort();
	css_active=0; css_n=0;
	kf_css_shutdown();         /* give the 4 KB rule index back to the heap */
	kf_grab_input(0);
	kf_clock_normal();         /* back to the normal 400 MHz clock off the network */
	s_fast_clock=0;
	s_http_eco_fallback=0;
}

/* PSRAM round-trip check at the CURRENT clock. Spineko is the first code to drive
   PSRAM at the 250 MHz eco clock; if the eco reclock leaves the bus marginal, the page
   arena reads back garbage (-> garbled text + corrupt hrefs, e.g. "unsupported scheme"
   on a hardcoded http:// link). Returns 0 if clean, else (failing offset in KB)+1. */
static uint32_t psram_check(void){
	static uint8_t w[512], r[512];
	const uint32_t step = 0x40000;                 /* probe every 256 KB of the arena */
	for(uint32_t off=0; off<ARENA_SZ; off+=step){
		uint8_t seed=(uint8_t)(off>>16);
		for(int i=0;i<512;i++) w[i]=(uint8_t)(seed + i*31 + 7);
		kf_psram_write(s_arena+off, w, 512);
	}
	for(uint32_t off=0; off<ARENA_SZ; off+=step){
		uint8_t seed=(uint8_t)(off>>16);
		for(int i=0;i<512;i++) w[i]=(uint8_t)(seed + i*31 + 7);
		kf_psram_read(s_arena+off, r, 512);
		if(memcmp(w,r,512)) return (off>>10)+1;
	}
	return 0;
}

void app_spineko_open(void){
	kf_clock_eco();               /* radio only associates <=~270 MHz */
	kf_net_init();
	s_fast_clock = 0;             /* browser_poll ramps only after LINK_UP/DHCP */
	s_http_last_state = KF_HTTP_IDLE; s_http_last_bytes = 0;
	s_http_progress_ms = (uint32_t)(time_us_64()/1000u); s_http_eco_fallback = 0;
	/* Only kick off a connect if nothing is already targeted. Calling autoconnect
	   unconditionally re-issues do_connect()'s cyw43_wifi_leave(), which would DROP a
	   live connection (e.g. one made in the WiFi app) and force a multi-second
	   re-association — during which DNS fails. */
	if(!kf_net_ssid()[0]) kf_net_autoconnect();

	if(s_arena==0xFFFFFFFFu && kf_psram_size()){
		s_arena = kf_psram_alloc(ARENA_SZ);
		if(s_arena!=0xFFFFFFFFu){
			kf_http_set_arena(s_arena, BODY_MAX);
			kf_html_set_arena(s_arena+BODY_MAX, OPS_MAX,
			                  s_arena+BODY_MAX+OPS_MAX, TEXT_MAX);
			kf_css_set_arena(s_arena+BODY_MAX+OPS_MAX+TEXT_MAX, CSSRULE_MAX);
		}
	}
	mkdir("/kefyros/spineko", 0777);              /* SD stylesheet cache (EEXIST is fine) */
	mkdir(CSS_CACHE_DIR, 0777);
	mkdir(IMG_RENDER_DIR, 0777);
	mkdir(FONT_CACHE_DIR,0777);
	css_active = 0; css_n = 0;font_active=0;

	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_set_style_bg_color(scr, WEB_BG, 0);
	lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
	kf_inset_top(scr);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr, on_del, LV_EVENT_DELETE, NULL);

	/* address bar */
	lbl_url = lv_label_create(scr);
	lv_label_set_long_mode(lbl_url, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_url, LCD_W-8);
	lv_obj_set_style_text_font(lbl_url, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_url, WEB_CHROMET, 0);
	lv_obj_set_style_bg_color(lbl_url, WEB_CHROME, 0);
	lv_obj_set_style_bg_opa(lbl_url, LV_OPA_COVER, 0);
	lv_obj_set_style_pad_left(lbl_url, 4, 0);
	lv_obj_align(lbl_url, LV_ALIGN_TOP_LEFT, 0, 0);
	lv_obj_set_size(lbl_url, LCD_W, URLBAR_H);

	/* document scroll container */
	doc = lv_obj_create(scr);
	lv_obj_remove_style_all(doc);
	lv_obj_set_size(doc, LCD_W, KF_CONTENT_H - URLBAR_H - STATUS_H);
	lv_obj_align(doc, LV_ALIGN_TOP_LEFT, 0, URLBAR_H);
	lv_obj_set_flex_flow(doc, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(doc, 4, 0);
	lv_obj_set_style_pad_row(doc, 2, 0);
	lv_obj_set_style_bg_opa(doc, LV_OPA_TRANSP, 0);
	lv_obj_set_scroll_dir(doc, LV_DIR_ALL);

	/* status line */
	lbl_status = lv_label_create(scr);
	lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_status, LCD_W-8);
	lv_obj_set_style_text_font(lbl_status, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_status, WEB_CHROMET, 0);
	lv_obj_set_style_bg_color(lbl_status, WEB_CHROME, 0);
	lv_obj_set_style_bg_opa(lbl_status, LV_OPA_COVER, 0);
	lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 4, 0);
	pointer_create();

	cur_url[0]=0; hist_sp=0; fwd_sp=0; s_loading=0; edit_mode=0; foci_n=0; cur_focus=-1;
	reader_mode=0;find_query[0]=0;find_from=0;
	kf_grab_input(1);          /* we drive all keys ourselves via browser_poll */

	if(s_arena==0xFFFFFFFFu){
		mk_label("PSRAM unavailable - browser needs it for page storage.",
		         KF_FONT, lv_color_hex(0xe03c32), 4);
		set_status("error");
	} else {
		uint32_t bad = psram_check();              /* verify the arena at the eco clock */
		if(bad){
			kf_clock_normal();                     /* re-test at the normal 400 MHz to localise the fault */
			uint32_t bad2 = psram_check();
			kf_clock_eco();
			char m[120];
			snprintf(m,sizeof m,"PSRAM corrupts @250MHz (+%luKB). @400MHz: %s",
			         (unsigned long)(bad-1), bad2?"also bad":"CLEAN -> eco-clock bug");
			mk_label(m, KF_FONT, lv_color_hex(0xe03c32), 4);
			set_status("psram self-test FAILED");
		} else {
			load_url("about:start", 0);
		}
	}
	lv_screen_load(scr);
}
