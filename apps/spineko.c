// apps/spineko.c — Spineko: an HTML-only web browser for Kefyros.
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
// Networking runs in eco (250 MHz) like the WiFi app — the radio can't associate at
// the 400 MHz boost clock. We restore boost on exit.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../port/http.h"
#include "../port/html.h"
#include "../port/imgdec.h"          /* JPEG/PNG/SVG -> downscaled RGB565, bounded RAM */
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>

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

/* PSRAM arena: [BODY | OPS | TEXT]. Allocated once, reused across page loads. */
#define ARENA_SZ   (4u*1024*1024)
#define BODY_MAX   (1536u*1024)
#define OPS_MAX    (256u*1024)
static uint32_t s_arena = 0xFFFFFFFFu;

#define DOC_W       (LCD_W - 16)
#define URLBAR_H    20
#define STATUS_H    16
#define MAX_WIDGETS 220
#define MAX_FOCI    200
#define MAX_FIELDS  6

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

static lv_obj_t *scr, *lbl_url, *doc, *lbl_status;

/* focusable items (links / form fields / submit buttons) in document order */
typedef struct {
	lv_obj_t *w;
	uint8_t   kind;                 /* KF_OP_LINK / KF_OP_FIELD / KF_OP_SUBMIT */
	uint32_t  href_off, href_len;   /* LINK/SUBMIT: URL/action in the TEXT arena */
	int       field;                /* FIELD: index into fld_*; SUBMIT: bound field or -1 */
} focus_t;
static focus_t foci[MAX_FOCI];
static int     foci_n, cur_focus;

static char    fld_name[MAX_FIELDS][64];
static char    fld_val[MAX_FIELDS][128];
static int     fld_n;

/* ---- inline images (downscaled JPEG, fetched in the background after the page) ---- */
#define IMG_MAX        24            /* images tracked per page */
#define IMG_W_MAX      DOC_W         /* target max decoded width (px) */
#define IMG_H_MAX      220           /* target max decoded height (px) */
#define IMG_RAM_BUDGET (128u*1024)   /* total decoded-bitmap SRAM cap per page */
typedef struct {
	lv_obj_t      *w;                /* the [img] placeholder label, then the lv_image */
	uint32_t       href_off, href_len;  /* src URL in the TEXT arena */
	uint8_t        state;           /* 0 pending, 1 done, 2 failed/skipped */
	uint16_t      *pix;             /* malloc'd RGB565 bitmap (freed on page change) */
	uint16_t       iw, ih;
	lv_image_dsc_t dsc;
} img_t;
static img_t    imgs[IMG_MAX];
static int      imgs_n;
static int      img_cur;            /* index being fetched, -1 = none */
static int      img_fetching;       /* 1 while an image HTTP fetch is in flight */
static uint32_t img_ram_used;

static char    cur_url[512];
static char    hist[16][512];
static int     hist_sp;

static int     s_loading;
static int     edit_mode;          /* 0 none, 1 url bar, 2 field */
static int     edit_field;
static char    edit_buf[512];

static void load_url(const char *url, int push);

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
static lv_obj_t *mk_label(const char *txt, const lv_font_t *font, lv_color_t color, int pad_left){
	lv_obj_t *l = lv_label_create(doc);
	lv_obj_set_style_text_font(l, font, 0);
	lv_obj_set_style_text_color(l, color, 0);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, DOC_W - pad_left);
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
	foci_n++;
}

/* ---------- inline images ----------
   After the page renders as text + [img] placeholders, browser_poll() fetches each
   image URL one at a time (reusing the now-free BODY arena, since the HTML is already
   parsed into ops), decodes JPEG/PNG/SVG downscaled-to-fit (port/imgdec), and swaps the
   placeholder for an lv_image. Bounded by IMG_RAM_BUDGET + the render heap floor. */
static int decode_img(uint32_t base, uint32_t len, int max_w, int max_h, img_t *im){
	if(img_ram_used >= IMG_RAM_BUDGET) return 0;
	uint16_t *pix; int w, h;
	if(!kf_img_decode(base, len, max_w, max_h, HEAP_FLOOR, &pix, &w, &h)) return 0;
	uint32_t bytes = (uint32_t)w * h * 2;
	if(img_ram_used + bytes > IMG_RAM_BUDGET){ free(pix); return 0; }
	im->pix = pix; im->iw = (uint16_t)w; im->ih = (uint16_t)h;
	lv_memzero(&im->dsc, sizeof im->dsc);
	im->dsc.header.magic  = LV_IMAGE_HEADER_MAGIC;
	im->dsc.header.cf     = LV_COLOR_FORMAT_RGB565;
	im->dsc.header.w      = w;
	im->dsc.header.h      = h;
	im->dsc.header.stride = w * 2;
	im->dsc.data          = (const uint8_t*)pix;
	im->dsc.data_size     = bytes;
	img_ram_used += bytes;
	return 1;
}
static void place_image(int i){
	img_t *im = &imgs[i];
	if(!im->w){ im->state = 1; return; }
	int idx = lv_obj_get_index(im->w);
	lv_obj_t *img = lv_image_create(doc);
	lv_image_set_src(img, &im->dsc);
	lv_obj_move_to_index(img, idx);     /* keep document order */
	lv_obj_del(im->w);
	im->w = img;                        /* handle now points at the image */
	im->state = 1;
}
static void free_images(void){
	for(int i=0;i<imgs_n;i++){
		if(imgs[i].pix){ lv_image_cache_drop(&imgs[i].dsc); free(imgs[i].pix); imgs[i].pix=NULL; }
	}
	imgs_n = 0; img_cur = -1; img_fetching = 0; img_ram_used = 0;
}
/* advance the one-at-a-time background image fetch. called from browser_poll(). */
static void images_pump(void){
	if(s_loading) return;                         /* never compete with a page load */
	if(kf_net_state()!=KF_NET_ONLINE) return;
	if(img_fetching){
		int st = kf_http_state();
		if(st==KF_HTTP_DONE){
			img_fetching = 0;
			int i = img_cur; img_cur = -1;
			if(i>=0 && i<imgs_n){
				uint32_t base = kf_http_body_base(), len = kf_http_body_len();
				if(decode_img(base, len, IMG_W_MAX, IMG_H_MAX, &imgs[i]))
					place_image(i);
				else
					imgs[i].state = 2;            /* unsupported / too big -> keep [img] */
			}
		} else if(st==KF_HTTP_ERROR){
			img_fetching = 0;
			if(img_cur>=0 && img_cur<imgs_n) imgs[img_cur].state = 2;
			img_cur = -1;
		}
		return;
	}
	if(img_ram_used >= IMG_RAM_BUDGET || heap_free() < HEAP_FLOOR + 8192u) return;
	for(int i=0;i<imgs_n;i++){
		if(imgs[i].state==0){
			static char src[512], abs[600];        /* off the 4 KB stack */
			kf_html_read_text(imgs[i].href_off, imgs[i].href_len, src, sizeof src);
			if(!kf_url_resolve(cur_url, src, abs, sizeof abs)){ imgs[i].state=2; return; }
			if(kf_http_get(abs)==0){ img_cur=i; img_fetching=1; }
			else imgs[i].state=2;
			return;
		}
	}
}

static void render_ops(void){
	lv_obj_clean(doc);
	free_images();              /* drop the previous page's decoded bitmaps */
	foci_n=0; cur_focus=0; fld_n=0;
	uint32_t n = kf_html_op_count();
	int widgets=0, last_field=-1;
	/* static: keep these big buffers OFF the stack — render runs inside LVGL's deep
	   layout/render call chain and the core0 stack is only 4 KB (overflow -> corruption
	   that surfaces as garbled text). Not reentrant, so static is safe. */
	static char buf[1024];
	static char line[1100];
	int low_mem=0;
	for(uint32_t i=0; i<n && widgets<MAX_WIDGETS; i++){
		if(heap_free() < HEAP_FLOOR){ low_mem=1; break; }   /* stop before OOM-panic */
		kf_html_op op; kf_html_get_op(i, &op);
		kf_html_read_text(op.text_off, op.text_len, buf, sizeof buf);
		switch(op.kind){
		case KF_OP_H1: case KF_OP_H2:
			mk_label(buf, KF_FONT_BIG, WEB_TEXT, 0); widgets++; break;
		case KF_OP_H3: case KF_OP_H4: case KF_OP_H5: case KF_OP_H6:
			mk_label(buf, KF_FONT, WEB_TEXT, 0); widgets++; break;
		case KF_OP_P:
			mk_label(buf, KF_FONT, WEB_TEXT, 0); widgets++; break;
		case KF_OP_QUOTE:
			mk_label(buf, KF_FONT, WEB_DIM, 8 + op.depth*8); widgets++; break;
		case KF_OP_LI: {
			if(op.index) snprintf(line,sizeof line,"%u. %s", op.index, buf);
			else         snprintf(line,sizeof line,"- %s", buf);
			mk_label(line, KF_FONT, WEB_TEXT, 6 + op.depth*10); widgets++; break;
		}
		case KF_OP_PRE:
			mk_label(buf, KF_FONT, WEB_PRE, 4); widgets++; break;
		case KF_OP_IMG: {
			snprintf(line,sizeof line,"[img: %s]", buf);
			lv_obj_t *l = mk_label(line, KF_FONT, WEB_MUTED, 2);
			if(op.href_len && imgs_n<IMG_MAX){     /* queue for background fetch */
				imgs[imgs_n].w=l; imgs[imgs_n].href_off=op.href_off;
				imgs[imgs_n].href_len=op.href_len; imgs[imgs_n].state=0;
				imgs[imgs_n].pix=NULL; imgs_n++;
			}
			widgets++; break;
		}
		case KF_OP_HR: {
			lv_obj_t *h=lv_obj_create(doc); lv_obj_remove_style_all(h);
			lv_obj_set_size(h, DOC_W-4, 2); lv_obj_set_style_bg_color(h, WEB_RULE, 0);
			lv_obj_set_style_bg_opa(h, LV_OPA_COVER, 0); widgets++; break;
		}
		case KF_OP_LINK: {
			lv_obj_t *l=mk_label(buf[0]?buf:"(link)", KF_FONT, WEB_LINK, 2);
			lv_obj_set_style_text_decor(l, LV_TEXT_DECOR_UNDERLINE, 0);
			add_focus(l, KF_OP_LINK, &op, -1); widgets++; break;
		}
		case KF_OP_FIELD: {
			int fi = fld_n<MAX_FIELDS ? fld_n++ : MAX_FIELDS-1;
			kf_html_read_text(op.href_off, op.href_len, fld_name[fi], sizeof fld_name[fi]);
			snprintf(fld_val[fi], sizeof fld_val[fi], "%s", buf);
			snprintf(line,sizeof line,"[ %s ]", fld_val[fi][0]?fld_val[fi]:"...");
			lv_obj_t *l=mk_label(line, KF_FONT, WEB_TEXT, 2);
			add_focus(l, KF_OP_FIELD, &op, fi); last_field=fi; widgets++; break;
		}
		case KF_OP_SUBMIT: {
			snprintf(line,sizeof line,"[ %s ]", buf[0]?buf:"Submit");
			lv_obj_t *l=mk_label(line, KF_FONT, WEB_LINK, 2);
			add_focus(l, KF_OP_SUBMIT, &op, last_field); widgets++; break;
		}
		default: break;
		}
	}
	if(low_mem)
		mk_label("[truncated - low memory]", KF_FONT, WEB_MUTED, 2);
	else if(kf_html_op_count() > (uint32_t)widgets)
		mk_label("[page truncated]", KF_FONT, WEB_MUTED, 2);
	lv_obj_scroll_to_y(doc, 0, LV_ANIM_OFF);
	if(foci_n>0){ cur_focus=0; style_focus(0,1); }
}

/* ---------- the built-in start page ---------- */
static const char START_HTML[] =
	"<title>Spineko</title>"
	"<h1>Spineko</h1>"
	"<p>Web browser. F1 = address bar, Up/Down = scroll, Left/Right = pick a link, "
	"ENTER = follow, Backspace = back, ESC = quit.</p>"
	"<h2>Bookmarks</h2>"
	"<ul>"
	"<li><a href=\"https://en.wikipedia.org/wiki/Main_Page\">Wikipedia</a></li>"
	"<li><a href=\"https://lite.cnn.com/\">CNN Lite</a></li>"
	"<li><a href=\"http://info.cern.ch/\">CERN - the first web page</a></li>"
	"<li><a href=\"http://example.com/\">example.com</a></li>"
	"<li><a href=\"http://motherfuckingwebsite.com/\">motherfuckingwebsite</a></li>"
	"<li><a href=\"http://textfiles.com/\">textfiles.com</a></li>"
	"</ul>"
	"<hr>"
	"<p>HTTP and HTTPS both work (TLS 1.2 via BearSSL, no cert check). Pages render as a "
	"plain web page (white background, black text, blue links) with inline JPEG/PNG/SVG "
	"images (downscaled); scripts/styling ignored.</p>";

static void render_start_page(void){
	if(s_arena==0xFFFFFFFFu) return;
	uint32_t len = (uint32_t)(sizeof START_HTML - 1);
	kf_psram_write(s_arena, START_HTML, len);
	kf_html_parse(s_arena, len);
	render_ops();
	set_status("Spineko - start page");
}

/* ---------- navigation ---------- */
static void hist_push(const char *u){
	if(!u || !u[0]) return;
	if(hist_sp < (int)(sizeof hist/sizeof hist[0]))
		snprintf(hist[hist_sp++], sizeof hist[0], "%s", u);
	else { memmove(hist[0], hist[1], sizeof hist[0]*(hist_sp-1));
	       snprintf(hist[hist_sp-1], sizeof hist[0], "%s", u); }
}

static void load_url(const char *url, int push){
	if(!url || !url[0]) return;
	if(!strncmp(url,"about:start",11)){
		if(push && cur_url[0]) hist_push(cur_url);
		snprintf(cur_url, sizeof cur_url, "about:start");
		show_url(); render_start_page(); s_loading=0; return;
	}
	/* require an absolute http(s) URL (URLs-only address bar) */
	char full[600];
	if(strstr(url,"://")) snprintf(full,sizeof full,"%s",url);
	else                  snprintf(full,sizeof full,"http://%s",url);   /* bare host -> http */
	if(push && cur_url[0]) hist_push(cur_url);
	snprintf(cur_url, sizeof cur_url, "%s", full);
	show_url();
	lv_obj_clean(doc); foci_n=0; free_images();   /* cancel any in-flight image fetches */
	set_status("Loading...");
	if(kf_net_state()!=KF_NET_ONLINE){ set_status("offline - open WiFi first"); s_loading=0; return; }
	int rc = kf_http_get(full);
	if(rc<0){ set_status(kf_http_err()); s_loading=0; }
	else s_loading=1;
}

static void follow_focus(void){
	if(cur_focus<0 || cur_focus>=foci_n) return;
	/* static scratch: keep these off the 4 KB core0 stack (see render_ops note). */
	static char href[512], abs[600], enc[256], full[760];
	focus_t *f=&foci[cur_focus];
	if(f->kind==KF_OP_LINK){
		kf_html_read_text(f->href_off, f->href_len, href, sizeof href);
		if(kf_url_resolve(cur_url, href, abs, sizeof abs)) load_url(abs, 1);
	} else if(f->kind==KF_OP_FIELD){
		edit_mode=2; edit_field=f->field;
		snprintf(edit_buf,sizeof edit_buf,"%s", f->field>=0?fld_val[f->field]:"");
		set_status("edit field - type, ENTER ok, ESC cancel");
		lv_label_set_text_fmt(f->w, "[ %s_ ]", edit_buf);
	} else if(f->kind==KF_OP_SUBMIT){
		kf_html_read_text(f->href_off, f->href_len, href, sizeof href);
		if(!href[0]) snprintf(href,sizeof href,"%s",cur_url);
		if(!kf_url_resolve(cur_url, href, abs, sizeof abs)) snprintf(abs,sizeof abs,"%s",cur_url);
		if(f->field>=0){
			const char *s=fld_val[f->field]; int k=0;
			for(; *s && k<(int)sizeof enc-4; s++){
				unsigned char c=*s;
				if((c>='A'&&c<='Z')||(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.'||c=='~') enc[k++]=c;
				else if(c==' ') enc[k++]='+';
				else { static const char *hx="0123456789ABCDEF"; enc[k++]='%'; enc[k++]=hx[c>>4]; enc[k++]=hx[c&15]; }
			}
			enc[k]=0;
			snprintf(full,sizeof full,"%s%c%s=%s", abs, strchr(abs,'?')?'&':'?', fld_name[f->field], enc);
			load_url(full, 1);
		} else load_url(abs, 1);
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
	if(decode_img(base, len, DOC_W, 200, &imgs[0])){   /* 200 keeps it within IMG_RAM_BUDGET */
		imgs_n = 1; imgs[0].state = 1; imgs[0].w = NULL;
		lv_obj_t *img = lv_image_create(doc);
		lv_image_set_src(img, &imgs[0].dsc);
		imgs[0].w = img;
	} else {
		mk_label("[could not decode image]", KF_FONT, KF_TEXT_MUTED, 4);
	}
	lv_obj_scroll_to_y(doc, 0, LV_ANIM_OFF);
	return 1;
}

static void on_loaded(void){
	snprintf(cur_url, sizeof cur_url, "%s", kf_http_final_url());
	show_url();
	uint32_t base = kf_http_body_base(), len = kf_http_body_len();
	if(show_image_page(base, len)){
		char st[96]; snprintf(st, sizeof st, "%d  image %ux%u",
		    kf_http_status(), imgs_n?imgs[0].iw:0, imgs_n?imgs[0].ih:0);
		set_status(st);
		return;
	}
	kf_html_parse(base, len);
	render_ops();
	const char *title = kf_html_title();
	char st[160];
	snprintf(st, sizeof st, "%d  %s", kf_http_status(), title[0]?title:cur_url);
	set_status(st);
}

/* ---------- key handling (raw, grabbed) ---------- */
static void edit_commit(void){
	if(edit_mode==1){ int m=edit_mode; edit_mode=0; (void)m; load_url(edit_buf, 1); }
	else if(edit_mode==2){
		if(edit_field>=0) snprintf(fld_val[edit_field],sizeof fld_val[edit_field],"%s",edit_buf);
		if(cur_focus>=0 && cur_focus<foci_n)
			lv_label_set_text_fmt(foci[cur_focus].w, "[ %s ]", edit_field>=0?fld_val[edit_field]:"");
		edit_mode=0; set_status("");
	}
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
	else if(edit_mode==2 && cur_focus>=0 && cur_focus<foci_n)
		lv_label_set_text_fmt(foci[cur_focus].w, "[ %s_ ]", edit_buf);
}

static void doc_scroll(int dy){ if(doc) lv_obj_scroll_by(doc, 0, dy, LV_ANIM_OFF); }

void browser_poll(void){
	if(!scr || lv_screen_active()!=scr) return;
	kf_http_poll();

	if(s_loading){
		int st=kf_http_state();
		if(st==KF_HTTP_DONE){ s_loading=0; on_loaded(); }
		else if(st==KF_HTTP_ERROR){ s_loading=0; lv_obj_clean(doc); foci_n=0;
			mk_label(kf_http_err(), KF_FONT, lv_color_hex(0xe03c32), 4);
			set_status("error"); }
		else { char b[48]; snprintf(b,sizeof b,"Loading... %lu KB",(unsigned long)(kf_http_body_len()/1024u)); set_status(b); }
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
		case DK_BACKSPACE: if(hist_sp>0){ char u[512]; snprintf(u,sizeof u,"%s",hist[--hist_sp]); load_url(u,0);} break;
		case DK_ENTER: if(press) follow_focus(); break;
		/* Up/Down always scroll the page (so you can read text); Left/Right step
		   between links and scroll the focused one into view. */
		case DK_UP:    doc_scroll(28); break;
		case DK_DOWN:  doc_scroll(-28); break;
		case DK_LEFT:  if(foci_n>0) set_focus(cur_focus-1); break;
		case DK_RIGHT: if(foci_n>0) set_focus(cur_focus+1); break;
		case DK_PGUP:  doc_scroll(doc_h-24); break;
		case DK_PGDN:  doc_scroll(-(doc_h-24)); break;
		default: break;
		}
	}
}

/* ---------- lifecycle ---------- */
static void on_del(lv_event_t *e){ (void)e;
	free_images();             /* free decoded bitmaps before the screen tears down */
	scr=NULL; doc=NULL; lbl_url=NULL; lbl_status=NULL;
	kf_http_abort();
	kf_grab_input(0);
	kf_clock_normal();         /* back to the normal 400 MHz clock off the network */
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
	kf_clock_normal();            /* radio only associates <=~270 MHz */
	kf_net_init();
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
			                  s_arena+BODY_MAX+OPS_MAX, ARENA_SZ-BODY_MAX-OPS_MAX);
		}
	}

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
	lv_obj_set_scroll_dir(doc, LV_DIR_VER);

	/* status line */
	lbl_status = lv_label_create(scr);
	lv_label_set_long_mode(lbl_status, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_status, LCD_W-8);
	lv_obj_set_style_text_font(lbl_status, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_status, WEB_CHROMET, 0);
	lv_obj_set_style_bg_color(lbl_status, WEB_CHROME, 0);
	lv_obj_set_style_bg_opa(lbl_status, LV_OPA_COVER, 0);
	lv_obj_align(lbl_status, LV_ALIGN_BOTTOM_LEFT, 4, 0);

	cur_url[0]=0; hist_sp=0; s_loading=0; edit_mode=0; foci_n=0; cur_focus=0;
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
			kf_clock_normal();
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
