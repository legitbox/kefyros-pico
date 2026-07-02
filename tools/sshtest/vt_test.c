/* tools/sshtest/vt_test.c — host unit tests for port/vt100.c.
 *
 *   cc -Wall -Wextra -fsanitize=address,undefined -I../../port \
 *      vt_test.c ../../port/vt100.c -o vt_test && ./vt_test
 *
 * Feeds escape-sequence byte streams into the emulator and asserts the
 * resulting grid / cursor state. No device, no network. */
#include "vt100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(cond, msg) do { checks++; if(!(cond)){ fails++; \
	printf("  FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__); } } while(0)

/* answerback capture */
static char ansbuf[256]; static int anslen;
static void on_answer(const uint8_t *b, int n, void *ud){ (void)ud;
	for(int i = 0; i < n && anslen < (int)sizeof ansbuf - 1; i++) ansbuf[anslen++] = (char)b[i];
	ansbuf[anslen] = 0;
}
/* scroll-off capture */
static int scrolloff_count; static char last_off_first;
static void on_scrolloff(const vt_cell_t *row, void *ud){ (void)ud;
	scrolloff_count++; last_off_first = (char)row[0].glyph;
}

static vt_t *fresh(void){
	anslen = 0; ansbuf[0] = 0; scrolloff_count = 0; last_off_first = 0;
	vt_t *t = vt_create(malloc, on_answer, on_scrolloff, NULL);
	return t;
}
static void feed(vt_t *t, const char *s){ vt_feed(t, (const uint8_t*)s, (int)strlen(s)); }
static char at(vt_t *t, int r, int c){ return (char)vt_row(t, r)[c].glyph; }

/* read the glyphs of a row into a NUL-terminated string (trailing spaces kept) */
static void rowstr(vt_t *t, int r, char *out){
	const vt_cell_t *row = vt_row(t, r);
	for(int c = 0; c < VT_COLS; c++) out[c] = (char)row[c].glyph;
	out[VT_COLS] = 0;
}

int main(void){
	char buf[VT_COLS + 1];

	/* --- basic text + cursor advance --- */
	{
		vt_t *t = fresh();
		feed(t, "hello");
		CHECK(at(t,0,0)=='h' && at(t,0,4)=='o', "plain text");
		int r,c,v; vt_cursor(t,&r,&c,&v);
		CHECK(r==0 && c==5, "cursor advanced");
		vt_destroy(t, free);
	}

	/* --- CR/LF --- */
	{
		vt_t *t = fresh();
		feed(t, "ab\r\ncd");
		CHECK(at(t,0,0)=='a' && at(t,1,0)=='c', "CR LF newline");
		vt_destroy(t, free);
	}

	/* --- CUP (cursor position) --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[3;5HX");
		CHECK(at(t,2,4)=='X', "CUP 3;5 places at row2 col4");
		vt_destroy(t, free);
	}

	/* --- SGR color: red fg (31) --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[31mR");
		const vt_cell_t *cell = &vt_row(t,0)[0];
		CHECK(cell->glyph=='R', "sgr glyph written");
		CHECK(cell->fg != vt_default_fg(), "sgr fg changed from default");
		vt_destroy(t, free);
	}

	/* --- SGR 256-color and truecolor parse without desync --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[38;5;196mA\033[38;2;10;20;30mB");
		CHECK(at(t,0,0)=='A' && at(t,0,1)=='B', "256/truecolor did not desync");
		vt_destroy(t, free);
	}

	/* --- bold brightens base color --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[34mn");          /* blue */
		uint16_t plain = vt_row(t,0)[0].fg;
		vt_t *t2 = fresh();
		feed(t2, "\033[1;34mb");       /* bold blue -> bright blue */
		uint16_t bold = vt_row(t2,0)[0].fg;
		CHECK(plain != bold, "bold brightens base color");
		vt_destroy(t, free); vt_destroy(t2, free);
	}

	/* --- ED 2 clears screen --- */
	{
		vt_t *t = fresh();
		feed(t, "junk\033[2J");
		CHECK(at(t,0,0)==' ', "ED2 clears");
		vt_destroy(t, free);
	}

	/* --- EL 0 clears to end of line --- */
	{
		vt_t *t = fresh();
		feed(t, "abcdef\033[1;4H\033[0K");   /* cursor col4 (0-based 3), erase to EOL */
		CHECK(at(t,0,0)=='a' && at(t,0,2)=='c' && at(t,0,3)==' ', "EL0 to end");
		vt_destroy(t, free);
	}

	/* --- deferred wrap: writing past last column --- */
	{
		vt_t *t = fresh();
		for(int i = 0; i < VT_COLS; i++) vt_feed(t, (const uint8_t*)"x", 1);
		int r,c,v; vt_cursor(t,&r,&c,&v);
		CHECK(r==0, "cursor still on row0 after filling last col (deferred wrap)");
		feed(t, "Y");   /* now it should wrap */
		CHECK(at(t,1,0)=='Y', "next glyph wraps to row1");
		vt_destroy(t, free);
	}

	/* --- autowrap off: overwrites last column --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[?7l");                 /* DECAWM off */
		for(int i = 0; i < VT_COLS + 5; i++) vt_feed(t, (const uint8_t*)"z", 1);
		CHECK(at(t,1,0)==' ', "no wrap to row1 when autowrap off");
		CHECK(at(t,0,VT_COLS-1)=='z', "last col holds latest glyph");
		vt_destroy(t, free);
	}

	/* --- scroll region + scroll-off callback --- */
	{
		vt_t *t = fresh();
		/* fill all rows with a leading digit, then force scrolling past bottom */
		for(int r = 0; r < VT_ROWS; r++){
			char s[8]; snprintf(s, sizeof s, "\033[%d;1H%c", r+1, (char)('0' + (r%10)));
			feed(t, s);
		}
		/* cursor to last row, then several linefeeds -> top rows scroll off */
		char s[16]; snprintf(s, sizeof s, "\033[%d;1H", VT_ROWS);
		feed(t, s);
		feed(t, "\n\n\n");
		CHECK(scrolloff_count == 3, "3 rows scrolled off main screen");
		CHECK(last_off_first == '2', "third scrolled-off row was the one starting '2'");
		vt_destroy(t, free);
	}

	/* --- alt screen 1049 does NOT scroll into scrollback --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[?1049h");               /* enter alt screen */
		CHECK(vt_on_altscreen(t)==1, "on alt screen");
		char s[16]; snprintf(s, sizeof s, "\033[%d;1H", VT_ROWS);
		feed(t, s);
		feed(t, "\n\n\n");
		CHECK(scrolloff_count == 0, "alt-screen scrolling does not feed scrollback");
		feed(t, "\033[?1049l");               /* leave alt screen */
		CHECK(vt_on_altscreen(t)==0, "back to main screen");
		vt_destroy(t, free);
	}

	/* --- DECCKM (app cursor keys) toggles --- */
	{
		vt_t *t = fresh();
		CHECK(vt_mode_appcursor(t)==0, "appcursor off by default");
		feed(t, "\033[?1h");
		CHECK(vt_mode_appcursor(t)==1, "appcursor on after DECSET 1");
		vt_destroy(t, free);
	}

	/* --- DA response --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[c");
		CHECK(strcmp(ansbuf, "\033[?62;22c")==0, "primary DA answer");
		vt_destroy(t, free);
	}

	/* --- DSR cursor position report --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[5;10H\033[6n");
		CHECK(strcmp(ansbuf, "\033[5;10R")==0, "CPR reports 5;10");
		vt_destroy(t, free);
	}

	/* --- DECALN fills with E --- */
	{
		vt_t *t = fresh();
		feed(t, "\033#8");
		CHECK(at(t,0,0)=='E' && at(t,VT_ROWS-1,VT_COLS-1)=='E', "DECALN fills E");
		vt_destroy(t, free);
	}

	/* --- IL / DL (insert/delete line) --- */
	{
		vt_t *t = fresh();
		feed(t, "L0\r\nL1\r\nL2");
		feed(t, "\033[1;1H\033[1L");          /* insert a blank line at top */
		CHECK(at(t,0,0)==' ' && at(t,1,0)=='L', "IL pushed L0 down");
		vt_destroy(t, free);
	}

	/* --- ICH / DCH (insert/delete char) --- */
	{
		vt_t *t = fresh();
		feed(t, "abcdef\033[1;1H\033[2P");    /* delete 2 chars at col1 */
		CHECK(at(t,0,0)=='c' && at(t,0,1)=='d', "DCH shifted left");
		vt_destroy(t, free);
	}

	/* --- DEC line-drawing charset ESC(0 --- */
	{
		vt_t *t = fresh();
		feed(t, "\033(0lqk\033(B");            /* ┌─┐ then back to ASCII */
		CHECK(vt_row(t,0)[0].glyph==VT_GL_UL, "l -> upper-left corner glyph");
		CHECK(vt_row(t,0)[1].glyph==VT_GL_HLINE, "q -> horizontal line glyph");
		CHECK(vt_row(t,0)[2].glyph==VT_GL_UR, "k -> upper-right corner glyph");
		vt_destroy(t, free);
	}

	/* --- UTF-8 box char maps to glyph --- */
	{
		vt_t *t = fresh();
		feed(t, "\xe2\x94\x80");               /* U+2500 ─ */
		CHECK(vt_row(t,0)[0].glyph==VT_GL_HLINE, "UTF-8 U+2500 -> HLINE glyph");
		vt_destroy(t, free);
	}

	/* --- title via OSC 2 --- */
	{
		vt_t *t = fresh();
		feed(t, "\033]0;my title\007");
		CHECK(strcmp(vt_title(t), "my title")==0, "OSC 0 sets title");
		vt_destroy(t, free);
	}

	/* --- OSC terminated by ST (ESC backslash) --- */
	{
		vt_t *t = fresh();
		feed(t, "\033]2;xyz\033\\A");
		CHECK(strcmp(vt_title(t), "xyz")==0, "OSC ST-terminated title");
		CHECK(at(t,0,0)=='A', "text after OSC/ST renders");
		vt_destroy(t, free);
	}

	/* --- unknown/garbage CSI does not desync --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[99999;;;;zAB");          /* bogus final 'z' */
		CHECK(at(t,0,0)=='A' && at(t,0,1)=='B', "garbage CSI swallowed, text continues");
		vt_destroy(t, free);
	}

	/* --- REP (repeat last char) --- */
	{
		vt_t *t = fresh();
		feed(t, "*\033[4b");
		CHECK(at(t,0,0)=='*' && at(t,0,4)=='*', "REP repeated '*' 4 times");
		CHECK(at(t,0,5)==' ', "REP stopped after 4");
		vt_destroy(t, free);
	}

	/* --- scroll region DECSTBM confines scrolling --- */
	{
		vt_t *t = fresh();
		feed(t, "\033[2;4r");                  /* region rows 2..4 */
		feed(t, "\033[1;1HTOP");               /* row1 outside region */
		feed(t, "\033[4;1HbotX");              /* row4 = bottom of region */
		feed(t, "\n");                          /* scroll within region only */
		char b[VT_COLS+1]; rowstr(t, 0, b);
		CHECK(b[0]=='T' && b[1]=='O' && b[2]=='P', "row outside region untouched by scroll");
		vt_destroy(t, free);
	}

	(void)buf; (void)rowstr;
	printf("\n%d/%d checks passed (%d failed)\n", checks - fails, checks, fails);
	return fails ? 1 : 0;
}
