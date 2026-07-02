// port/vt100.c — pure VT100/xterm-256color terminal emulator (see vt100.h).
//
// Design notes:
//  * Fixed VT_COLS x VT_ROWS grid, plus a full alternate screen. Colors resolve
//    to RGB565 at cell-write time; per-cell reverse and the global DECSCNM flag
//    are stored logically and applied by the renderer (so the cursor's own
//    inversion composes correctly).
//  * The parser is a strict state machine modelled on Paul Williams' VT500
//    diagram: any unrecognised final byte is swallowed without desyncing, which
//    is the exact property vim/htop/nano rely on.
//  * The only outward effects are the two caller callbacks (answerback + a
//    main-screen scroll-off hook for scrollback). No platform headers.
#include "vt100.h"
#include <string.h>

/* ---- RGB565 helpers + palette ---------------------------------------- */
static uint16_t rgb565(int r, int g, int b){
	return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
/* Classic xterm 16-color palette. */
static const uint8_t ANSI16[16][3] = {
	{  0,  0,  0},{205,  0,  0},{  0,205,  0},{205,205,  0},
	{  0,  0,238},{205,  0,205},{  0,205,205},{229,229,229},
	{127,127,127},{255,  0,  0},{  0,255,  0},{255,255,  0},
	{ 92, 92,255},{255,  0,255},{  0,255,255},{255,255,255},
};
#define VT_DEF_FG_R 229
#define VT_DEF_FG_G 229
#define VT_DEF_FG_B 229

uint16_t vt_default_fg(void){ return rgb565(VT_DEF_FG_R, VT_DEF_FG_G, VT_DEF_FG_B); }
uint16_t vt_default_bg(void){ return rgb565(0, 0, 0); }

static uint16_t ansi256(int idx){
	if(idx < 16) return rgb565(ANSI16[idx][0], ANSI16[idx][1], ANSI16[idx][2]);
	if(idx < 232){
		static const int L[6] = {0, 95, 135, 175, 215, 255};
		int n = idx - 16;
		return rgb565(L[(n / 36) % 6], L[(n / 6) % 6], L[n % 6]);
	}
	int g = 8 + 10 * (idx - 232);   /* 232..255 grayscale ramp */
	return rgb565(g, g, g);
}

/* ---- color mode used in SGR state ------------------------------------ */
enum { CM_DEFAULT = 0, CM_INDEX, CM_RGB };

/* ---- parser states --------------------------------------------------- */
enum {
	ST_GROUND = 0, ST_ESC, ST_CSI, ST_OSC, ST_STR_IGNORE,  /* DCS/PM/APC */
	ST_CHARSET_G0, ST_CHARSET_G1, ST_ESC_HASH
};

#define MAXPARAM 16
#define OSCBUF   64

typedef struct {
	int cx, cy; uint8_t attr;
	int fgm, fgi, bgm, bgi; uint16_t fgr, bgr;
	int g0, g1, gl, origin;
} vt_save_t;

struct vt {
	vt_answer_fn    ans;
	vt_scrolloff_fn off;
	void           *ud;

	vt_cell_t main[VT_ROWS * VT_COLS];
	vt_cell_t alt [VT_ROWS * VT_COLS];
	vt_cell_t *grid;          /* -> main or alt */
	int alt_active;

	int cx, cy;               /* cursor (0-based) */
	int wrap_pending;         /* deferred end-of-line wrap */
	int top, bot;             /* scroll region rows (inclusive, 0-based) */

	/* current SGR state */
	uint8_t attr;
	int fg_mode, fg_idx; uint16_t fg_rgb;
	int bg_mode, bg_idx; uint16_t bg_rgb;

	/* modes */
	int mode_appcursor;       /* DECCKM 1 */
	int mode_origin;          /* DECOM 6 */
	int mode_autowrap;        /* DECAWM 7 (default on) */
	int mode_insert;          /* IRM 4 */
	int mode_reverse;         /* DECSCNM 5 */
	int mode_cursorvis;       /* DECTCEM 25 (default on) */
	int mode_newline;         /* LNM 20 */

	/* charsets: 0='B' ASCII, 1='0' DEC graphics */
	int g0_graphics, g1_graphics, gl;   /* gl = 0 (G0) or 1 (G1) */

	/* saved cursor state (DECSC), one per screen */
	vt_save_t save_main, save_alt;

	uint8_t tabs[VT_COLS];

	/* parser */
	int state;
	int params[MAXPARAM];
	int nparam;
	int csi_cur, csi_cur_has; /* current field accumulator */
	int priv;                 /* '?' seen in CSI */
	int csi_gt;               /* '>' seen in CSI */
	char osc[OSCBUF];
	int osc_len;

	/* UTF-8 assembly (ground state) */
	int u8_need, u8_have; uint32_t u8_cp;

	/* REP support */
	vt_cell_t last_cell; int have_last;

	char title[OSCBUF];
	uint32_t dirty;           /* bitmask of dirty rows */
};

/* ---- small helpers --------------------------------------------------- */
static void mark(vt_t *t, int row){ if(row >= 0 && row < VT_ROWS) t->dirty |= (1u << row); }
static void mark_all(vt_t *t){ t->dirty = (VT_ROWS >= 32) ? 0xFFFFFFFFu : ((1u << VT_ROWS) - 1u); }

static uint16_t resolve_color(int mode, int idx, uint16_t rgb, int is_fg, int bold){
	if(mode == CM_RGB) return rgb;
	if(mode == CM_INDEX){
		if(bold && idx < 8) idx += 8;   /* bold brightens the 8 base colors */
		return ansi256(idx);
	}
	return is_fg ? vt_default_fg() : vt_default_bg();
}

static vt_cell_t cur_blank(vt_t *t){
	/* An erased cell keeps the current background but no glyph/attrs. */
	vt_cell_t c;
	c.glyph = ' ';
	c.attr = 0;
	c.fg = vt_default_fg();
	c.bg = resolve_color(t->bg_mode, t->bg_idx, t->bg_rgb, 0, 0);
	return c;
}

static void fill_row(vt_t *t, int row, int c0, int c1, vt_cell_t blank){
	vt_cell_t *r = &t->grid[row * VT_COLS];
	for(int x = c0; x <= c1 && x < VT_COLS; x++) r[x] = blank;
	mark(t, row);
}

/* ---- scrolling ------------------------------------------------------- */
static void scroll_up(vt_t *t, int n){
	if(n <= 0) return;
	int span = t->bot - t->top + 1;
	if(n > span) n = span;
	/* main-screen top-scroll feeds scrollback */
	if(!t->alt_active && t->top == 0 && t->off){
		for(int i = 0; i < n; i++) t->off(&t->grid[(t->top + i) * VT_COLS], t->ud);
	}
	vt_cell_t blank = cur_blank(t);
	for(int row = t->top; row <= t->bot - n; row++)
		memcpy(&t->grid[row * VT_COLS], &t->grid[(row + n) * VT_COLS], VT_COLS * sizeof(vt_cell_t));
	for(int row = t->bot - n + 1; row <= t->bot; row++)
		fill_row(t, row, 0, VT_COLS - 1, blank);
	for(int row = t->top; row <= t->bot; row++) mark(t, row);
}
static void scroll_down(vt_t *t, int n){
	if(n <= 0) return;
	int span = t->bot - t->top + 1;
	if(n > span) n = span;
	vt_cell_t blank = cur_blank(t);
	for(int row = t->bot; row >= t->top + n; row--)
		memcpy(&t->grid[row * VT_COLS], &t->grid[(row - n) * VT_COLS], VT_COLS * sizeof(vt_cell_t));
	for(int row = t->top; row < t->top + n; row++)
		fill_row(t, row, 0, VT_COLS - 1, blank);
	for(int row = t->top; row <= t->bot; row++) mark(t, row);
}

/* ---- cursor motion --------------------------------------------------- */
static void clamp_cursor(vt_t *t){
	if(t->cx < 0) t->cx = 0; if(t->cx >= VT_COLS) t->cx = VT_COLS - 1;
	if(t->cy < 0) t->cy = 0; if(t->cy >= VT_ROWS) t->cy = VT_ROWS - 1;
}
static void index_down(vt_t *t){       /* IND / LF */
	if(t->cy == t->bot) scroll_up(t, 1);
	else if(t->cy < VT_ROWS - 1) t->cy++;
	mark(t, t->cy);
}
static void reverse_index(vt_t *t){    /* RI */
	if(t->cy == t->top) scroll_down(t, 1);
	else if(t->cy > 0) t->cy--;
	mark(t, t->cy);
}

/* ---- glyph mapping --------------------------------------------------- */
static uint8_t dec_graphics(uint8_t b){
	switch(b){
		case '`': return VT_GL_DIAMOND;
		case 'a': return VT_GL_CHECKER;
		case 'f': return VT_GL_DEGREE;
		case 'g': return VT_GL_PLUSMINUS;
		case 'j': return VT_GL_LR;
		case 'k': return VT_GL_UR;
		case 'l': return VT_GL_UL;
		case 'm': return VT_GL_LL;
		case 'n': return VT_GL_CROSS;
		case 'q': return VT_GL_HLINE;
		case 't': return VT_GL_LTEE;
		case 'u': return VT_GL_RTEE;
		case 'v': return VT_GL_BTEE;
		case 'w': return VT_GL_TTEE;
		case 'x': return VT_GL_VLINE;
		case 'o': case 'p': case 'r': case 's': return VT_GL_HLINE; /* scan lines */
		case '~': return VT_GL_BULLET;
		default:  return b;   /* space and everything else pass through */
	}
}
static uint8_t map_codepoint(uint32_t cp){
	if(cp >= 0x20 && cp < 0x7F) return (uint8_t)cp;
	switch(cp){
		case 0x00A0: return ' ';
		case 0x00B0: return VT_GL_DEGREE;
		case 0x00B1: return VT_GL_PLUSMINUS;
		case 0x2022: return VT_GL_BULLET;
		case 0x00B7: return VT_GL_BULLET;
		case 0x2500: case 0x2501: return VT_GL_HLINE;
		case 0x2502: case 0x2503: return VT_GL_VLINE;
		case 0x250C: case 0x250D: case 0x250E: case 0x250F: return VT_GL_UL;
		case 0x2510: case 0x2511: case 0x2512: case 0x2513: return VT_GL_UR;
		case 0x2514: case 0x2515: case 0x2516: case 0x2517: return VT_GL_LL;
		case 0x2518: case 0x2519: case 0x251A: case 0x251B: return VT_GL_LR;
		case 0x251C: case 0x251D: case 0x251E: case 0x251F: return VT_GL_LTEE;
		case 0x2524: case 0x2525: case 0x2526: case 0x2527: return VT_GL_RTEE;
		case 0x252C: case 0x252D: case 0x252E: case 0x252F: return VT_GL_TTEE;
		case 0x2534: case 0x2535: case 0x2536: case 0x2537: return VT_GL_BTEE;
		case 0x253C: return VT_GL_CROSS;
		case 0x2591: case 0x2592: case 0x2593: return VT_GL_CHECKER;
		case 0x25C6: return VT_GL_DIAMOND;
		case 0x2264: case 0x2265: return VT_GL_REPLACE;
		default: return VT_GL_REPLACE;
	}
}

/* ---- put a printable glyph ------------------------------------------- */
static void put_glyph(vt_t *t, uint8_t glyph){
	if(t->wrap_pending){
		t->cx = 0;
		index_down(t);
		t->wrap_pending = 0;
	}
	int bold = (t->attr & VT_A_BOLD) != 0;
	vt_cell_t c;
	c.glyph = glyph;
	c.attr  = t->attr;
	c.fg = resolve_color(t->fg_mode, t->fg_idx, t->fg_rgb, 1, bold);
	c.bg = resolve_color(t->bg_mode, t->bg_idx, t->bg_rgb, 0, 0);

	vt_cell_t *row = &t->grid[t->cy * VT_COLS];
	if(t->mode_insert){
		for(int x = VT_COLS - 1; x > t->cx; x--) row[x] = row[x - 1];
	}
	row[t->cx] = c;
	mark(t, t->cy);
	t->last_cell = c; t->have_last = 1;

	if(t->cx >= VT_COLS - 1){
		if(t->mode_autowrap) t->wrap_pending = 1;   /* defer */
	} else {
		t->cx++;
	}
}
static void put_byte_text(vt_t *t, uint8_t b){
	int graphics = t->gl ? t->g1_graphics : t->g0_graphics;
	put_glyph(t, graphics ? dec_graphics(b) : b);
}

/* ---- erase ----------------------------------------------------------- */
static void erase_line(vt_t *t, int mode){    /* 0 c->end, 1 start->c, 2 all */
	vt_cell_t blank = cur_blank(t);
	int a = 0, b = VT_COLS - 1;
	if(mode == 0) a = t->cx;
	else if(mode == 1) b = t->cx;
	fill_row(t, t->cy, a, b, blank);
}
static void erase_display(vt_t *t, int mode){ /* 0 c->end, 1 start->c, 2 all, 3 scrollback */
	vt_cell_t blank = cur_blank(t);
	if(mode == 2 || mode == 3){
		for(int r = 0; r < VT_ROWS; r++) fill_row(t, r, 0, VT_COLS - 1, blank);
		return;
	}
	if(mode == 0){
		fill_row(t, t->cy, t->cx, VT_COLS - 1, blank);
		for(int r = t->cy + 1; r < VT_ROWS; r++) fill_row(t, r, 0, VT_COLS - 1, blank);
	} else if(mode == 1){
		for(int r = 0; r < t->cy; r++) fill_row(t, r, 0, VT_COLS - 1, blank);
		fill_row(t, t->cy, 0, t->cx, blank);
	}
}

/* ---- line/char insert-delete ----------------------------------------- */
static void insert_chars(vt_t *t, int n){
	if(n < 1) n = 1;
	vt_cell_t *row = &t->grid[t->cy * VT_COLS];
	vt_cell_t blank = cur_blank(t);
	for(int x = VT_COLS - 1; x >= t->cx + n; x--) row[x] = row[x - n];
	for(int x = t->cx; x < t->cx + n && x < VT_COLS; x++) row[x] = blank;
	mark(t, t->cy);
}
static void delete_chars(vt_t *t, int n){
	if(n < 1) n = 1;
	vt_cell_t *row = &t->grid[t->cy * VT_COLS];
	vt_cell_t blank = cur_blank(t);
	for(int x = t->cx; x < VT_COLS; x++) row[x] = (x + n < VT_COLS) ? row[x + n] : blank;
	mark(t, t->cy);
}
static void erase_chars(vt_t *t, int n){
	if(n < 1) n = 1;
	vt_cell_t *row = &t->grid[t->cy * VT_COLS];
	vt_cell_t blank = cur_blank(t);
	for(int x = t->cx; x < t->cx + n && x < VT_COLS; x++) row[x] = blank;
	mark(t, t->cy);
}
static void insert_lines(vt_t *t, int n){
	if(t->cy < t->top || t->cy > t->bot) return;
	if(n < 1) n = 1;
	int span = t->bot - t->cy + 1; if(n > span) n = span;
	vt_cell_t blank = cur_blank(t);
	for(int row = t->bot; row >= t->cy + n; row--)
		memcpy(&t->grid[row * VT_COLS], &t->grid[(row - n) * VT_COLS], VT_COLS * sizeof(vt_cell_t));
	for(int row = t->cy; row < t->cy + n; row++) fill_row(t, row, 0, VT_COLS - 1, blank);
	for(int row = t->cy; row <= t->bot; row++) mark(t, row);
}
static void delete_lines(vt_t *t, int n){
	if(t->cy < t->top || t->cy > t->bot) return;
	if(n < 1) n = 1;
	int span = t->bot - t->cy + 1; if(n > span) n = span;
	vt_cell_t blank = cur_blank(t);
	for(int row = t->cy; row <= t->bot - n; row++)
		memcpy(&t->grid[row * VT_COLS], &t->grid[(row + n) * VT_COLS], VT_COLS * sizeof(vt_cell_t));
	for(int row = t->bot - n + 1; row <= t->bot; row++) fill_row(t, row, 0, VT_COLS - 1, blank);
	for(int row = t->cy; row <= t->bot; row++) mark(t, row);
}

/* ---- SGR ------------------------------------------------------------- */
static void sgr(vt_t *t){
	int n = t->nparam ? t->nparam : 1;
	if(t->nparam == 0){ t->params[0] = 0; }
	for(int i = 0; i < n; i++){
		int p = t->params[i];
		switch(p){
			case 0:
				t->attr = 0;
				t->fg_mode = CM_DEFAULT; t->bg_mode = CM_DEFAULT;
				break;
			case 1: t->attr |= VT_A_BOLD; break;
			case 2: t->attr |= VT_A_DIM; break;
			case 4: t->attr |= VT_A_UNDER; break;
			case 7: t->attr |= VT_A_REVERSE; break;
			case 21: case 22: t->attr &= ~(VT_A_BOLD | VT_A_DIM); break;
			case 24: t->attr &= ~VT_A_UNDER; break;
			case 27: t->attr &= ~VT_A_REVERSE; break;
			case 39: t->fg_mode = CM_DEFAULT; break;
			case 49: t->bg_mode = CM_DEFAULT; break;
			case 38: case 48: {
				int is_fg = (p == 38);
				if(i + 1 < n && t->params[i + 1] == 5 && i + 2 < n){
					int idx = t->params[i + 2];
					if(is_fg){ t->fg_mode = CM_INDEX; t->fg_idx = idx; }
					else     { t->bg_mode = CM_INDEX; t->bg_idx = idx; }
					i += 2;
				} else if(i + 1 < n && t->params[i + 1] == 2 && i + 4 < n){
					uint16_t c = rgb565(t->params[i + 2], t->params[i + 3], t->params[i + 4]);
					if(is_fg){ t->fg_mode = CM_RGB; t->fg_rgb = c; }
					else     { t->bg_mode = CM_RGB; t->bg_rgb = c; }
					i += 4;
				}
				break;
			}
			default:
				if(p >= 30 && p <= 37){ t->fg_mode = CM_INDEX; t->fg_idx = p - 30; }
				else if(p >= 40 && p <= 47){ t->bg_mode = CM_INDEX; t->bg_idx = p - 40; }
				else if(p >= 90 && p <= 97){ t->fg_mode = CM_INDEX; t->fg_idx = p - 90 + 8; }
				else if(p >= 100 && p <= 107){ t->bg_mode = CM_INDEX; t->bg_idx = p - 100 + 8; }
				break;
		}
	}
}

/* ---- DEC private + ANSI mode set/reset ------------------------------- */
static void save_cursor(vt_t *t){
	vt_save_t *s = t->alt_active ? &t->save_alt : &t->save_main;
	s->cx = t->cx; s->cy = t->cy; s->attr = t->attr;
	s->fgm = t->fg_mode; s->fgi = t->fg_idx; s->fgr = t->fg_rgb;
	s->bgm = t->bg_mode; s->bgi = t->bg_idx; s->bgr = t->bg_rgb;
	s->g0 = t->g0_graphics; s->g1 = t->g1_graphics; s->gl = t->gl; s->origin = t->mode_origin;
}
static void restore_cursor(vt_t *t){
	vt_save_t *s = t->alt_active ? &t->save_alt : &t->save_main;
	t->cx = s->cx; t->cy = s->cy; t->attr = s->attr;
	t->fg_mode = s->fgm; t->fg_idx = s->fgi; t->fg_rgb = s->fgr;
	t->bg_mode = s->bgm; t->bg_idx = s->bgi; t->bg_rgb = s->bgr;
	t->g0_graphics = s->g0; t->g1_graphics = s->g1; t->gl = s->gl; t->mode_origin = s->origin;
	t->wrap_pending = 0;
	clamp_cursor(t);
	mark(t, t->cy);
}
static void switch_screen(vt_t *t, int to_alt){
	if(to_alt == t->alt_active) return;
	t->alt_active = to_alt;
	t->grid = to_alt ? t->alt : t->main;
	mark_all(t);
}
static void set_mode(vt_t *t, int set){
	for(int i = 0; i < t->nparam; i++){
		int p = t->params[i];
		if(t->priv){
			switch(p){
				case 1:  t->mode_appcursor = set; break;
				case 5:  t->mode_reverse = set; mark_all(t); break;
				case 6:  t->mode_origin = set;
				         /* DECOM moves cursor home (relative to region) */
				         t->cx = 0; t->cy = set ? t->top : 0; t->wrap_pending = 0; break;
				case 7:  t->mode_autowrap = set; break;
				case 25: t->mode_cursorvis = set; mark(t, t->cy); break;
				case 47: case 1047:
					switch_screen(t, set);
					if(set){ vt_cell_t bl = cur_blank(t);
					         for(int r = 0; r < VT_ROWS; r++) fill_row(t, r, 0, VT_COLS - 1, bl); }
					break;
				case 1048: if(set) save_cursor(t); else restore_cursor(t); break;
				case 1049:
					if(set){ save_cursor(t); switch_screen(t, 1);
					         vt_cell_t bl = cur_blank(t);
					         for(int r = 0; r < VT_ROWS; r++) fill_row(t, r, 0, VT_COLS - 1, bl);
					         t->cx = t->cy = 0; }
					else   { switch_screen(t, 0); restore_cursor(t); }
					break;
				default: break;   /* 3,12,1000-1006,1015,2004 etc. swallowed */
			}
		} else {
			switch(p){
				case 4:  t->mode_insert = set; break;
				case 20: t->mode_newline = set; break;
				default: break;
			}
		}
	}
}

/* ---- answerback ------------------------------------------------------ */
static void answer(vt_t *t, const char *s){
	if(t->ans) t->ans((const uint8_t*)s, (int)strlen(s), t->ud);
}
static void report_cursor(vt_t *t){
	int row = t->cy + 1, col = t->cx + 1;
	if(t->mode_origin) row = t->cy - t->top + 1;
	char b[24]; int n = 0;
	b[n++] = 0x1b; b[n++] = '[';
	char num[8]; int k;
	k = 0; if(row == 0) num[k++] = '0'; else { int r = row; char tmp[8]; int m = 0; while(r){ tmp[m++] = '0' + r % 10; r /= 10; } while(m) num[k++] = tmp[--m]; }
	for(int i = 0; i < k; i++) b[n++] = num[i];
	b[n++] = ';';
	k = 0; if(col == 0) num[k++] = '0'; else { int c = col; char tmp[8]; int m = 0; while(c){ tmp[m++] = '0' + c % 10; c /= 10; } while(m) num[k++] = tmp[--m]; }
	for(int i = 0; i < k; i++) b[n++] = num[i];
	b[n++] = 'R';
	if(t->ans) t->ans((const uint8_t*)b, n, t->ud);
}

/* ---- CSI dispatch ---------------------------------------------------- */
static int p0(vt_t *t, int def){ int v = (t->nparam > 0) ? t->params[0] : 0; return v ? v : def; }

static void csi_dispatch(vt_t *t, uint8_t final){
	int n = p0(t, 1);
	switch(final){
		case 'A': { int old = t->cy; t->cy -= n;
		            if(old >= t->top && t->cy < t->top) t->cy = t->top;   /* don't cross the top margin */
		            if(t->cy < 0) t->cy = 0; mark(t, old); mark(t, t->cy); break; }
		case 'B': t->cy += n; clamp_cursor(t); mark(t, t->cy); break;
		case 'C': t->cx += n; t->wrap_pending = 0; clamp_cursor(t); mark(t, t->cy); break;
		case 'D': t->cx -= n; t->wrap_pending = 0; clamp_cursor(t); mark(t, t->cy); break;
		case 'E': t->cx = 0; t->cy += n; clamp_cursor(t); mark(t, t->cy); break;
		case 'F': t->cx = 0; t->cy -= n; clamp_cursor(t); mark(t, t->cy); break;
		case 'G': case '`': t->cx = n - 1; t->wrap_pending = 0; clamp_cursor(t); mark(t, t->cy); break;
		case 'd': t->cy = n - 1; if(t->mode_origin) t->cy += t->top; clamp_cursor(t); mark(t, t->cy); break;
		case 'H': case 'f': {
			int row = (t->nparam > 0 && t->params[0]) ? t->params[0] : 1;
			int col = (t->nparam > 1 && t->params[1]) ? t->params[1] : 1;
			t->cx = col - 1;
			t->cy = row - 1 + (t->mode_origin ? t->top : 0);
			t->wrap_pending = 0;
			clamp_cursor(t);
			mark(t, t->cy);
			break;
		}
		case 'J': erase_display(t, t->nparam ? t->params[0] : 0); break;
		case 'K': erase_line(t, t->nparam ? t->params[0] : 0); break;
		case 'L': insert_lines(t, n); break;
		case 'M': delete_lines(t, n); break;
		case '@': insert_chars(t, n); break;
		case 'P': delete_chars(t, n); break;
		case 'X': erase_chars(t, n); break;
		case 'S': scroll_up(t, n); break;
		case 'T': scroll_down(t, n); break;
		case 'b': if(t->have_last){ for(int i = 0; i < n; i++) put_glyph(t, t->last_cell.glyph); } break;
		case 'r': {
			int top = (t->nparam > 0 && t->params[0]) ? t->params[0] : 1;
			int bot = (t->nparam > 1 && t->params[1]) ? t->params[1] : VT_ROWS;
			if(top < 1) top = 1; if(bot > VT_ROWS) bot = VT_ROWS;
			if(top < bot){ t->top = top - 1; t->bot = bot - 1;
			               t->cx = 0; t->cy = t->mode_origin ? t->top : 0; }
			break;
		}
		case 'g': if((t->nparam ? t->params[0] : 0) == 3){ memset(t->tabs, 0, sizeof t->tabs); }
		          else if(t->cx < VT_COLS){ t->tabs[t->cx] = 0; } break;
		case 'h': set_mode(t, 1); break;
		case 'l': set_mode(t, 0); break;
		case 'm': if(t->priv) break; sgr(t); break;
		case 's': save_cursor(t); break;
		case 'u': restore_cursor(t); break;
		case 'c':
			if(t->csi_gt) answer(t, "\033[>0;10;0c");     /* secondary DA */
			else          answer(t, "\033[?62;22c");       /* primary DA: VT220 + color */
			break;
		case 'n':
			if((t->nparam ? t->params[0] : 0) == 5) answer(t, "\033[0n");
			else if((t->nparam ? t->params[0] : 0) == 6) report_cursor(t);
			break;
		default: break;   /* swallow unknown finals */
	}
}

/* ---- ESC dispatch ---------------------------------------------------- */
static void esc_dispatch(vt_t *t, uint8_t b){
	switch(b){
		case '7': save_cursor(t); break;
		case '8': restore_cursor(t); break;
		case 'D': index_down(t); break;
		case 'E': t->cx = 0; index_down(t); break;
		case 'M': reverse_index(t); break;
		case 'H': if(t->cx < VT_COLS) t->tabs[t->cx] = 1; break;
		case 'c': vt_reset(t); break;
		case '=': case '>': break;    /* keypad application/numeric — swallow */
		default: break;
	}
}

/* DECALN test pattern (ESC # 8): fill screen with 'E'. */
static void decaln(vt_t *t){
	vt_cell_t c; c.glyph = 'E'; c.attr = 0; c.fg = vt_default_fg(); c.bg = vt_default_bg();
	for(int r = 0; r < VT_ROWS; r++)
		for(int x = 0; x < VT_COLS; x++) t->grid[r * VT_COLS + x] = c;
	mark_all(t);
	t->cx = t->cy = 0;
}

/* ---- parser feed ----------------------------------------------------- */
static void reset_csi(vt_t *t){
	t->nparam = 0; t->csi_cur = 0; t->csi_cur_has = 0; t->priv = 0; t->csi_gt = 0;
	for(int i = 0; i < MAXPARAM; i++) t->params[i] = 0;
}
/* commit the current numeric field as a parameter */
static void csi_commit(vt_t *t){
	if(t->nparam < MAXPARAM) t->params[t->nparam++] = t->csi_cur_has ? t->csi_cur : 0;
	t->csi_cur = 0; t->csi_cur_has = 0;
}
static void osc_finish(vt_t *t){
	t->osc[t->osc_len < OSCBUF ? t->osc_len : OSCBUF - 1] = 0;
	if(t->osc_len >= 2 && (t->osc[0] == '0' || t->osc[0] == '2') && t->osc[1] == ';'){
		int j = 0;
		for(int k = 2; k < t->osc_len && j < (int)sizeof(t->title) - 1; k++) t->title[j++] = t->osc[k];
		t->title[j] = 0;
	}
}

static void ground_byte(vt_t *t, uint8_t b){
	/* control chars */
	switch(b){
		case 0x07: return;                       /* BEL */
		case 0x08: if(t->wrap_pending) t->wrap_pending = 0; else if(t->cx > 0) t->cx--; mark(t, t->cy); return; /* BS */
		case 0x09: {                              /* HT */
			t->wrap_pending = 0;
			int x = t->cx + 1;
			while(x < VT_COLS - 1 && !t->tabs[x]) x++;
			t->cx = x; if(t->cx >= VT_COLS) t->cx = VT_COLS - 1; return;
		}
		case 0x0A: case 0x0B: case 0x0C:          /* LF/VT/FF */
			index_down(t); if(t->mode_newline) t->cx = 0; return;
		case 0x0D: t->cx = 0; t->wrap_pending = 0; return;   /* CR */
		case 0x0E: t->gl = 1; return;             /* SO -> G1 */
		case 0x0F: t->gl = 0; return;             /* SI -> G0 */
		case 0x1B: t->state = ST_ESC; return;     /* ESC */
		default: break;
	}
	if(b < 0x20) return;                          /* other C0: ignore */

	/* UTF-8 assembly */
	if(b < 0x80){
		t->u8_need = 0;
		put_byte_text(t, b);
		return;
	}
	if(t->u8_need == 0){
		if((b & 0xE0) == 0xC0){ t->u8_need = 1; t->u8_have = 0; t->u8_cp = b & 0x1F; }
		else if((b & 0xF0) == 0xE0){ t->u8_need = 2; t->u8_have = 0; t->u8_cp = b & 0x0F; }
		else if((b & 0xF8) == 0xF0){ t->u8_need = 3; t->u8_have = 0; t->u8_cp = b & 0x07; }
		else { put_glyph(t, VT_GL_REPLACE); }     /* stray continuation / invalid */
		return;
	}
	if((b & 0xC0) != 0x80){                        /* expected continuation, got junk */
		t->u8_need = 0; put_glyph(t, VT_GL_REPLACE);
		ground_byte(t, b);                         /* reprocess */
		return;
	}
	t->u8_cp = (t->u8_cp << 6) | (b & 0x3F);
	if(++t->u8_have == t->u8_need){
		uint32_t cp = t->u8_cp; t->u8_need = 0;
		put_glyph(t, map_codepoint(cp));
	}
}

void vt_feed(vt_t *t, const uint8_t *data, int n){
	for(int i = 0; i < n; i++){
		uint8_t b = data[i];
		switch(t->state){
			case ST_GROUND:
				ground_byte(t, b);
				break;

			case ST_ESC:
				switch(b){
					case '[': reset_csi(t); t->state = ST_CSI; break;
					case ']': t->osc_len = 0; t->state = ST_OSC; break;
					case 'P': case '^': case '_': t->state = ST_STR_IGNORE; break;
					case '(': t->state = ST_CHARSET_G0; break;
					case ')': t->state = ST_CHARSET_G1; break;
					case '#': t->state = ST_ESC_HASH; break;
					case '\\': t->state = ST_GROUND; break;  /* stray ST */
					default: esc_dispatch(t, b); t->state = ST_GROUND; break;
				}
				break;

			case ST_ESC_HASH:
				if(b == '8') decaln(t);
				t->state = ST_GROUND;
				break;

			case ST_CHARSET_G0:
				t->g0_graphics = (b == '0'); t->state = ST_GROUND; break;
			case ST_CHARSET_G1:
				t->g1_graphics = (b == '0'); t->state = ST_GROUND; break;

			case ST_CSI:
				if(b == '?'){ t->priv = 1; }
				else if(b == '>'){ t->csi_gt = 1; }
				else if(b == '<' || b == '='){ /* param prefix, ignore */ }
				else if(b >= '0' && b <= '9'){
					t->csi_cur = t->csi_cur * 10 + (b - '0');
					if(t->csi_cur > 65535) t->csi_cur = 65535;
					t->csi_cur_has = 1;
				}
				else if(b == ';'){ csi_commit(t); }
				else if(b >= 0x20 && b <= 0x2F){ /* intermediate — ignore, stay */ }
				else if(b >= 0x40 && b <= 0x7E){
					if(t->csi_cur_has || t->nparam > 0) csi_commit(t);
					csi_dispatch(t, b);
					t->state = ST_GROUND;
				}
				else if(b == 0x1B){ t->state = ST_ESC; }
				else { t->state = ST_GROUND; }
				break;

			case ST_OSC:
				if(b == 0x07 || b == 0x9C){ osc_finish(t); t->state = ST_GROUND; }   /* BEL or ST */
				else if(b == 0x1B){ osc_finish(t); t->state = ST_ESC; }              /* ESC \ */
				else { if(t->osc_len < OSCBUF - 1) t->osc[t->osc_len++] = (char)b; }
				break;

			case ST_STR_IGNORE:                       /* DCS/PM/APC: consume to ST */
				if(b == 0x1B) t->state = ST_ESC;      /* ESC \ ends it */
				else if(b == 0x9C) t->state = ST_GROUND;
				break;

			default:
				t->state = ST_GROUND;
				break;
		}
	}
}

/* ---- lifecycle + accessors ------------------------------------------- */
static void full_reset(vt_t *t){
	t->grid = t->main; t->alt_active = 0;
	t->cx = t->cy = 0; t->wrap_pending = 0;
	t->top = 0; t->bot = VT_ROWS - 1;
	t->attr = 0;
	t->fg_mode = CM_DEFAULT; t->bg_mode = CM_DEFAULT;
	t->fg_idx = 7; t->bg_idx = 0; t->fg_rgb = t->bg_rgb = 0;
	t->mode_appcursor = 0; t->mode_origin = 0; t->mode_autowrap = 1;
	t->mode_insert = 0; t->mode_reverse = 0; t->mode_cursorvis = 1; t->mode_newline = 0;
	t->g0_graphics = 0; t->g1_graphics = 0; t->gl = 0;
	t->state = ST_GROUND; reset_csi(t);
	t->u8_need = 0; t->have_last = 0; t->osc_len = 0; t->title[0] = 0;
	for(int i = 0; i < VT_COLS; i++) t->tabs[i] = (i % 8 == 0 && i != 0) ? 1 : 0;
	vt_cell_t blank; blank.glyph = ' '; blank.attr = 0; blank.fg = vt_default_fg(); blank.bg = vt_default_bg();
	for(int i = 0; i < VT_ROWS * VT_COLS; i++){ t->main[i] = blank; t->alt[i] = blank; }
	mark_all(t);
}

void vt_reset(vt_t *t){ if(t) full_reset(t); }

vt_t *vt_create(void *(*alloc)(size_t), vt_answer_fn ans, vt_scrolloff_fn off, void *ud){
	vt_t *t = (vt_t*)alloc(sizeof(vt_t));
	if(!t) return NULL;
	memset(t, 0, sizeof *t);
	t->ans = ans; t->off = off; t->ud = ud;
	full_reset(t);
	return t;
}
void vt_destroy(vt_t *t, void (*dealloc)(void *)){ if(t && dealloc) dealloc(t); }

const vt_cell_t *vt_row(vt_t *t, int r){
	if(r < 0 || r >= VT_ROWS) r = 0;
	return &t->grid[r * VT_COLS];
}
uint32_t vt_dirty_and_clear(vt_t *t){ uint32_t d = t->dirty; t->dirty = 0; return d; }
void vt_mark_all_dirty(vt_t *t){ mark_all(t); }

void vt_cursor(vt_t *t, int *row, int *col, int *visible){
	if(row) *row = t->cy;
	if(col) *col = t->cx;
	if(visible) *visible = t->mode_cursorvis;
}
int vt_mode_appcursor(vt_t *t){ return t->mode_appcursor; }
int vt_on_altscreen(vt_t *t){ return t->alt_active; }
int vt_reverse_screen(vt_t *t){ return t->mode_reverse; }
const char *vt_title(vt_t *t){ return t->title; }
