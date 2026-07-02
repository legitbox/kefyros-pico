// port/vt100.h — pure VT100/xterm-256color terminal emulator core for the
// Kefyros "Term" SSH client. No pico-sdk / lwIP / lcdspi dependencies: this file
// compiles unchanged on the host for unit testing (tools/sshtest/vt_test.c).
//
// The emulator maintains a fixed VT_COLS x VT_ROWS character grid (plus an
// alternate screen), parses a byte stream of terminal output with a strict
// VT500-style state machine, and exposes the resolved grid + cursor to a
// renderer. Colors are resolved to RGB565 at write time so the renderer never
// touches SGR state. All I/O (answerback, scroll-off) is via caller callbacks.
#ifndef KF_VT100_H
#define KF_VT100_H
#include <stdint.h>
#include <stddef.h>

#define VT_COLS 53
#define VT_ROWS 26

/* Cell attribute bits (vt_cell_t.attr). */
#define VT_A_BOLD   0x01
#define VT_A_UNDER  0x02
#define VT_A_REVERSE 0x04
#define VT_A_DIM    0x08

/* Internal glyph indices for the baked font (ui/font_term6x12.c).
   0x20..0x7E are the ASCII byte itself. The line-draw / special block below
   (0x80..0x90) is what the DEC special-graphics charset (ESC(0) and UTF-8 box
   characters map onto — the renderer's font must provide these cells. */
enum {
	VT_GL_DIAMOND   = 0x80,  /* ◆ */
	VT_GL_CHECKER   = 0x81,  /* ▒ */
	VT_GL_DEGREE    = 0x82,  /* ° */
	VT_GL_PLUSMINUS = 0x83,  /* ± */
	VT_GL_LR        = 0x84,  /* ┘ */
	VT_GL_UR        = 0x85,  /* ┐ */
	VT_GL_UL        = 0x86,  /* ┌ */
	VT_GL_LL        = 0x87,  /* └ */
	VT_GL_CROSS     = 0x88,  /* ┼ */
	VT_GL_HLINE     = 0x89,  /* ─ */
	VT_GL_LTEE      = 0x8A,  /* ├ */
	VT_GL_RTEE      = 0x8B,  /* ┤ */
	VT_GL_BTEE      = 0x8C,  /* ┴ */
	VT_GL_TTEE      = 0x8D,  /* ┬ */
	VT_GL_VLINE     = 0x8E,  /* │ */
	VT_GL_BULLET    = 0x8F,  /* · */
	VT_GL_REPLACE   = 0x90,  /* unknown codepoint fallback */
	VT_GL_MAX       = 0x91
};

typedef struct {
	uint8_t  glyph;   /* ASCII byte, or a VT_GL_* index */
	uint8_t  attr;    /* VT_A_* bits */
	uint16_t fg, bg;  /* resolved RGB565 */
} vt_cell_t;

/* Callback: terminal wants to send bytes back to the host (DA / DSR / CPR). */
typedef void (*vt_answer_fn)(const uint8_t *bytes, int n, void *ud);
/* Callback: one row scrolled off the top of the MAIN screen (never alt). The
   row is VT_COLS cells; the emulator hands it over before overwriting it so the
   app can push it into scrollback. */
typedef void (*vt_scrolloff_fn)(const vt_cell_t *row, void *ud);

typedef struct vt vt_t;   /* opaque; defined in vt100.c */

/* Create/destroy. `alloc` allocates the ~19 KB state; on the host pass malloc,
   on device pass the app allocator. Returns NULL if alloc fails. */
vt_t *vt_create(void *(*alloc)(size_t), vt_answer_fn ans, vt_scrolloff_fn off, void *ud);
void  vt_destroy(vt_t *t, void (*dealloc)(void *));

void  vt_reset(vt_t *t);                             /* RIS: full reset */
void  vt_feed(vt_t *t, const uint8_t *data, int n);  /* parse host output */

/* Renderer access. */
const vt_cell_t *vt_row(vt_t *t, int r);             /* live grid row r (0..VT_ROWS-1) */
uint32_t vt_dirty_and_clear(vt_t *t);                /* bitmask of dirty rows, then clears */
void     vt_mark_all_dirty(vt_t *t);                 /* force a full repaint next frame */
void     vt_cursor(vt_t *t, int *row, int *col, int *visible);

int         vt_mode_appcursor(vt_t *t);              /* DECCKM: arrows send ESC O.. not ESC [.. */
int         vt_on_altscreen(vt_t *t);
int         vt_reverse_screen(vt_t *t);              /* DECSCNM: renderer should invert whole screen */
const char *vt_title(vt_t *t);                       /* last OSC 0/2 title ("" if none) */

/* RGB565 for a foreground/background reset (used by the renderer to clear). */
uint16_t vt_default_fg(void);
uint16_t vt_default_bg(void);

#endif /* KF_VT100_H */
