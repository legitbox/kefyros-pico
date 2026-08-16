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
#define KF_CSS_F_FG       0x00000001u   /* fg color set */
#define KF_CSS_F_BG       0x00000002u   /* bg color set */
#define KF_CSS_F_HIDE     0x00000004u   /* display:none / visibility:hidden */
#define KF_CSS_F_UNDER    0x00000008u
#define KF_CSS_F_STRIKE   0x00000010u
#define KF_CSS_F_CENTER   0x00000020u
#define KF_CSS_F_RIGHT    0x00000040u
#define KF_CSS_F_BIG      0x00000080u   /* font-size >= ~20px -> the 20px font */
#define KF_CSS_F_NOBULLET 0x00000100u   /* list-style: none */
#define KF_CSS_F_FLEX     0x00000200u   /* display:flex */
#define KF_CSS_F_GRID     0x00000400u   /* practical grid -> bounded row tracks */
#define KF_CSS_F_NOWRAP   0x00000800u   /* white-space:nowrap */
#define KF_CSS_F_CLIP     0x00001000u   /* overflow:hidden/clip */
#define KF_CSS_F_MONO     0x00002000u   /* font-family:monospace */

enum { KF_CSS_SIZE_AUTO=0, KF_CSS_SIZE_PX, KF_CSS_SIZE_PCT };
enum { KF_CSS_JUSTIFY_START=0, KF_CSS_JUSTIFY_CENTER, KF_CSS_JUSTIFY_END,
       KF_CSS_JUSTIFY_BETWEEN, KF_CSS_JUSTIFY_AROUND, KF_CSS_JUSTIFY_EVENLY };
enum { KF_CSS_ALIGN_START=0, KF_CSS_ALIGN_CENTER, KF_CSS_ALIGN_END, KF_CSS_ALIGN_STRETCH };

typedef struct {
	uint32_t flags;                /* KF_CSS_F_* */
	uint16_t fg, bg;               /* RGB565 */
	uint8_t  indent;               /* extra left indent, px (margin/padding-left) */
	uint8_t  border_w;             /* border width px (0 = none) */
	uint16_t border_c;             /* border color RGB565 */
	uint8_t  radius;               /* border-radius px */
	uint8_t  pad_v;                /* extra vertical padding px (margin/padding-top/bottom) */
	int8_t   line_sp;              /* extra line spacing px (line-height - 1em) */
	int8_t   let_sp;               /* letter-spacing px */
	uint8_t  xform;                /* text-transform: 0 none, 1 upper, 2 lower */
	uint8_t  pad_h;                /* horizontal padding px */
	uint8_t  gap;                  /* flex/grid row+column gap px */
	uint8_t  opacity;              /* 0..255; defaults to 255 */
	uint8_t  flex_dir;             /* 0 row, 1 column, 2 row-reverse, 3 column-reverse */
	uint8_t  flex_wrap;            /* 0 nowrap, 1 wrap, 2 wrap-reverse */
	uint8_t  justify;              /* KF_CSS_JUSTIFY_* */
	uint8_t  align;                /* KF_CSS_ALIGN_* */
	uint8_t  grid_cols;            /* bounded grid track count, 1..4 */
	uint16_t width, max_width;     /* value interpreted through *_unit */
	uint8_t  width_unit, max_width_unit;
	uint8_t  shadow_w;             /* simple box-shadow blur width */
	uint16_t shadow_c;             /* RGB565 */
	uint8_t  font_id;              /* 0 built-in; 1..N downloaded @font-face */
} kf_css_style;

/* one open element, as seen by selector matching */
typedef struct {
	uint16_t tag;                  /* kf_css_tag_hash() of the tag name */
	uint8_t  ncls;
	uint8_t  nattr;
	uint16_t child_index;          /* element index within parent (for :first-child) */
	uint32_t id_hash;              /* 0 = none */
	uint32_t cls[4];               /* class-name hashes */
	uint32_t attr[4];              /* present attribute-name hashes */
} kf_css_elem;

void     kf_css_set_arena(uint32_t psram_base, uint32_t cap_bytes);
void     kf_css_reset(void);              /* drop all rules (new page) */
void     kf_css_shutdown(void);           /* free the SRAM index (browser closed) */
uint32_t kf_css_rule_count(void);

/* stream one stylesheet; sheets accumulate until kf_css_reset() */
void     kf_css_sheet_begin(void);
void     kf_css_sheet_set_base(const char *absolute_url);
void     kf_css_sheet_feed(int c);
void     kf_css_sheet_end(void);

/* Bounded @font-face registry accumulated with the stylesheets. */
int         kf_css_font_count(void);
const char *kf_css_font_src(int i);
const char *kf_css_font_base(int i);

uint32_t kf_css_hash(const char *s, int n);      /* FNV-1a, ASCII-lowercased; n<0 = strlen */
uint16_t kf_css_tag_hash(const char *name);

/* Compute the style of stack[depth-1]. `parent` = computed style of stack[depth-2]
   (NULL at the root); `inline_style` = the element's style="" text (NULL if none). */
void     kf_css_apply(const kf_css_elem *stack, int depth, const char *inline_style,
                      const kf_css_style *parent, kf_css_style *out);

#endif /* KF_CSS_H */
