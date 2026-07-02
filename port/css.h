// port/css.h — a small CSS engine for the Spineko browser.
//
// Streams stylesheet text (from <style> blocks or fetched .css files) into a flat
// rule table in PSRAM: each rule is a fixed 64-byte record holding up to 3 compound
// selectors (tag / .class / #id, descendant combinator) and up to 6 *supported*
// declarations, pre-resolved at parse time (colors -> RGB565, lengths -> px).
// Rules whose declarations are all unsupported (layout, fonts we don't have...)
// are dropped at parse time, so the table cap goes a long way on real-world CSS.
//
// Matching: the HTML parser maintains a stack of open elements (kf_css_elem) and
// asks for a computed style (kf_css_apply) each time an element opens; inherited
// properties (color, align, font-size class, ...) flow down via the parent style.
// A per-rule SRAM index keyed on the rightmost compound makes matching cheap; the
// index is malloc'd lazily and freed by kf_css_shutdown() so the SRAM heap only
// pays while the browser is actually open.
#ifndef KF_CSS_H
#define KF_CSS_H
#include <stdint.h>

/* computed-style flags */
#define KF_CSS_F_FG       0x0001   /* fg color set */
#define KF_CSS_F_BG       0x0002   /* bg color set */
#define KF_CSS_F_HIDE     0x0004   /* display:none / visibility:hidden */
#define KF_CSS_F_UNDER    0x0008
#define KF_CSS_F_STRIKE   0x0010
#define KF_CSS_F_CENTER   0x0020
#define KF_CSS_F_RIGHT    0x0040
#define KF_CSS_F_BIG      0x0080   /* font-size >= ~17px -> the 20px font */
#define KF_CSS_F_NOBULLET 0x0100   /* list-style: none */

typedef struct {
	uint16_t flags;                /* KF_CSS_F_* */
	uint16_t fg, bg;               /* RGB565 */
	uint8_t  indent;               /* extra left indent, px (margin/padding-left) */
	uint8_t  border_w;             /* border width px (0 = none) */
	uint16_t border_c;             /* border color RGB565 */
	uint8_t  radius;               /* border-radius px */
	uint8_t  pad_v;                /* extra vertical padding px (margin/padding-top/bottom) */
	int8_t   line_sp;              /* extra line spacing px (line-height - 1em) */
	int8_t   let_sp;               /* letter-spacing px */
	uint8_t  xform;                /* text-transform: 0 none, 1 upper, 2 lower */
} kf_css_style;

/* one open element, as seen by selector matching */
typedef struct {
	uint16_t tag;                  /* kf_css_tag_hash() of the tag name */
	uint8_t  ncls;
	uint32_t id_hash;              /* 0 = none */
	uint32_t cls[4];               /* class-name hashes */
} kf_css_elem;

void     kf_css_set_arena(uint32_t psram_base, uint32_t cap_bytes);
void     kf_css_reset(void);              /* drop all rules (new page) */
void     kf_css_shutdown(void);           /* free the SRAM index (browser closed) */
uint32_t kf_css_rule_count(void);

/* stream one stylesheet; sheets accumulate until kf_css_reset() */
void     kf_css_sheet_begin(void);
void     kf_css_sheet_feed(int c);
void     kf_css_sheet_end(void);

uint32_t kf_css_hash(const char *s, int n);      /* FNV-1a, ASCII-lowercased; n<0 = strlen */
uint16_t kf_css_tag_hash(const char *name);

/* Compute the style of stack[depth-1]. `parent` = computed style of stack[depth-2]
   (NULL at the root); `inline_style` = the element's style="" text (NULL if none). */
void     kf_css_apply(const kf_css_elem *stack, int depth, const char *inline_style,
                      const kf_css_style *parent, kf_css_style *out);

#endif /* KF_CSS_H */
