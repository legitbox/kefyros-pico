// port/html.h — lenient HTML -> render-op list for the Spineko browser.
//
// The parser reads the HTTP body straight out of PSRAM and emits a flat array of
// "render ops" (one per block-level run or inline link/image/form control) into a
// second PSRAM region, with all text bytes pooled in a third PSRAM region. Nothing
// is held in SRAM except a couple of small line buffers, so even a multi-hundred-KB
// page parses in bounded RAM. The renderer (apps/spineko.c) walks the ops and builds
// LVGL widgets up to a cap. The tokenizer NEVER trusts the markup: unmatched tags,
// truncated input and garbage all degrade to text rather than crashing.
#ifndef KF_HTML_H
#define KF_HTML_H
#include <stdint.h>
#include "css.h"

enum {
	KF_OP_H1, KF_OP_H2, KF_OP_H3, KF_OP_H4, KF_OP_H5, KF_OP_H6,
	KF_OP_P, KF_OP_LI, KF_OP_PRE, KF_OP_QUOTE, KF_OP_HR, KF_OP_BR,
	KF_OP_LINK,    /* text=anchor text, href=absolute-or-relative URL */
	KF_OP_IMG,     /* text=alt text */
	KF_OP_FIELD,   /* text=current value, href=input name (single-line text input) */
	KF_OP_SUBMIT   /* text=button label, href=form action URL */
};

/* per-op CSS style bits (op.sflags) */
#define KF_ST_FG       0x01   /* op.fg valid */
#define KF_ST_BG       0x02   /* op.bg valid */
#define KF_ST_UNDER    0x04
#define KF_ST_STRIKE   0x08
#define KF_ST_CENTER   0x10
#define KF_ST_RIGHT    0x20
#define KF_ST_BIG      0x40   /* render with the big font */
#define KF_ST_NOBULLET 0x80   /* LI: no bullet/number prefix */

typedef struct {
	uint8_t  kind;
	uint8_t  depth;        /* list / blockquote nesting (indent) */
	uint16_t index;        /* ordered-list item number; 0 = bullet / n/a */
	uint32_t text_off, text_len;   /* into the TEXT arena */
	uint32_t href_off, href_len;   /* into the TEXT arena (LINK/FIELD/SUBMIT) */
	uint16_t fg, bg;       /* RGB565 (see sflags) */
	uint8_t  sflags;       /* KF_ST_* */
	uint8_t  indent;       /* extra CSS left indent, px */
	uint16_t border_c;     /* border color RGB565 (border_w > 0) */
	uint8_t  border_w;     /* border width px, 0 = none */
	uint8_t  radius;       /* border-radius px */
	uint8_t  pad_v;        /* extra vertical padding px */
	uint8_t  xform;        /* text-transform: 0 none, 1 upper, 2 lower */
	int8_t   line_sp;      /* extra line spacing px */
	int8_t   let_sp;       /* letter-spacing px */
} kf_html_op;

/* Configure the two PSRAM regions the parser writes into. */
void        kf_html_set_arena(uint32_t ops_base, uint32_t ops_cap_bytes,
                              uint32_t text_base, uint32_t text_cap);

/* Parse `len` bytes of HTML at PSRAM offset `body_base`. Returns op count.
   Pass A: resets the CSS rule table, feeds <style> blocks into it and collects
   <link rel=stylesheet> URLs. After feeding any external sheets to kf_css,
   call kf_html_reparse() (pass B: same body, keeps the loaded rules). */
uint32_t    kf_html_parse(uint32_t body_base, uint32_t len);
uint32_t    kf_html_reparse(void);

int         kf_html_css_link_count(void);
const char *kf_html_css_link(int i);
void        kf_html_body_style(kf_css_style *out);   /* computed style of <body> */

uint32_t    kf_html_op_count(void);
void        kf_html_get_op(uint32_t i, kf_html_op *out);
/* Copy text/href bytes into a SRAM buffer, NUL-terminated, truncated to cap-1. */
void        kf_html_read_text(uint32_t off, uint32_t len, char *out, uint32_t cap);
const char *kf_html_title(void);          /* page <title> ("" if none) */

#endif /* KF_HTML_H */
