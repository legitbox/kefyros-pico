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

enum {
	KF_OP_H1, KF_OP_H2, KF_OP_H3, KF_OP_H4, KF_OP_H5, KF_OP_H6,
	KF_OP_P, KF_OP_LI, KF_OP_PRE, KF_OP_QUOTE, KF_OP_HR, KF_OP_BR,
	KF_OP_LINK,    /* text=anchor text, href=absolute-or-relative URL */
	KF_OP_IMG,     /* text=alt text */
	KF_OP_FIELD,   /* text=current value, href=input name (single-line text input) */
	KF_OP_SUBMIT   /* text=button label, href=form action URL */
};

typedef struct {
	uint8_t  kind;
	uint8_t  depth;        /* list / blockquote nesting (indent) */
	uint16_t index;        /* ordered-list item number; 0 = bullet / n/a */
	uint32_t text_off, text_len;   /* into the TEXT arena */
	uint32_t href_off, href_len;   /* into the TEXT arena (LINK/FIELD/SUBMIT) */
} kf_html_op;

/* Configure the two PSRAM regions the parser writes into. */
void        kf_html_set_arena(uint32_t ops_base, uint32_t ops_cap_bytes,
                              uint32_t text_base, uint32_t text_cap);

/* Parse `len` bytes of HTML at PSRAM offset `body_base`. Returns op count. */
uint32_t    kf_html_parse(uint32_t body_base, uint32_t len);

uint32_t    kf_html_op_count(void);
void        kf_html_get_op(uint32_t i, kf_html_op *out);
/* Copy text/href bytes into a SRAM buffer, NUL-terminated, truncated to cap-1. */
void        kf_html_read_text(uint32_t off, uint32_t len, char *out, uint32_t cap);
const char *kf_html_title(void);          /* page <title> ("" if none) */

#endif /* KF_HTML_H */
