// apps/help.c — Help: an on-device documentation browser.
//
// Content lives on the SD card as Markdown under /kefyros/help/<Topic>/<Section>.md
// (NOT baked into the firmware), so the docs can be edited/extended without reflashing
// and cost no flash. Three levels: topic folders -> section files -> a rendered article.
// A topic folder holding exactly one .md opens straight to that article (no 1-item submenu).
//
// The article view does light Markdown rendering tuned for the amber theme + ASCII Plex
// Mono font: # headings, fenced ``` code blocks (rendered as bordered cards — for CakeSpark
// scripts, calculator expressions, etc.), - bullet lists, --- rules, and paragraphs. LVGL 9
// here has no inline recolor, so emphasis is block-level; inline ** and ` markers are stripped.
//
// Pure LVGL (no raw-key grab, no main-loop poll): list levels use the keypad focus group
// (Up/Dn move, ENTER opens), the article view is one focusable scroll container (Up/Dn
// scroll). BACKSPACE goes back one level; ESC is the OS-wide exit to the launcher (indev.c).
#include "../kefyros.h"
#include "../ui/theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define HELP_ROOT "/kefyros/help"
#define DOCW      (LCD_W - 24)        /* article text width inside the scroll container */
#define MAXN      128                 /* max entries listed per directory level */
#define MAXFILE   (48*1024)           /* cap a single .md we'll load into the heap */

static lv_obj_t  *scr, *title, *hint, *holder;
static lv_group_t *grp;

/* navigation state */
static int  level;                    /* 0 = topics, 1 = sections, 2 = article */
static char cur_topic[300];           /* full path of the open topic dir */
static char cur_topic_raw[200];       /* its raw folder name (to re-enter after an article) */
static char cur_topic_disp[128];      /* its display name (for the section-list title) */
static int  article_direct;           /* article reached by skipping a 1-file submenu */

/* sorted name list for the current directory level */
static char *names[MAXN];
static int   nnames;
static void free_names(void){ for(int i=0;i<nnames;i++) free(names[i]); nnames=0; }
static int  cmp_names(const void *a, const void *b){ return strcmp(*(const char**)a, *(const char**)b); }

/* skip a leading "NN " ordering prefix used only to sort folders/files on disk */
static const char *strip_order(const char *s){
	const char *p = s;
	while(*p >= '0' && *p <= '9') p++;
	if(p != s && *p == ' ') return p + 1;
	return s;
}
/* display title: order-stripped, and without a trailing ".md" */
static void disp_title(char *out, int sz, const char *raw){
	const char *s = strip_order(raw);
	snprintf(out, sz, "%s", s);
	int n = (int)strlen(out);
	if(n > 3 && !strcmp(out + n - 3, ".md")) out[n-3] = 0;
}

static void enter_topics(void);
static void enter_topic(const char *topic_raw);
static void open_article(const char *topic_path, const char *file_raw, int direct);

/* ---------- shared back / key handling ---------- */
static void go_back(void){
	if(level == 2){ if(article_direct) enter_topics(); else enter_topic(cur_topic_raw); return; }
	if(level == 1){ enter_topics(); return; }
	kf_back_to_launcher();            /* BKSP at the top level == leave Help */
}
/* list-row keys: BACKSPACE goes back a level (Up/Dn/ENTER are handled by the group) */
static void row_key_cb(lv_event_t *e){
	if(lv_event_get_key(e) == LV_KEY_BACKSPACE) go_back();
}

/* ---------- Markdown article rendering ---------- */

/* copy `src` into `dst` stripping inline `**` (emphasis) and backticks; dst >= src length+1 */
static void inline_clean(char *dst, const char *src){
	const char *s = src; char *d = dst;
	while(*s){
		if(s[0]=='*' && s[1]=='*'){ s += 2; continue; }
		if(s[0]=='`'){ s++; continue; }
		*d++ = *s++;
	}
	*d = 0;
}

static lv_obj_t *make_view(void){
	lv_obj_t *v = lv_obj_create(holder);
	lv_obj_remove_style_all(v);
	lv_obj_set_size(v, LCD_W-8, KF_CONTENT_H-46);
	lv_obj_align(v, LV_ALIGN_TOP_MID, 0, 26);
	lv_obj_set_flex_flow(v, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_all(v, 4, 0);
	lv_obj_set_style_pad_row(v, 7, 0);
	lv_obj_set_scroll_dir(v, LV_DIR_VER);
	lv_obj_set_scrollbar_mode(v, LV_SCROLLBAR_MODE_ACTIVE);
	lv_obj_set_style_bg_opa(v, LV_OPA_TRANSP, 0);
	return v;
}

/* one wrapped text block (paragraph run / bullet list); `col` sets the tone */
static void emit_text(lv_obj_t *v, const char *txt, lv_color_t col){
	if(!txt[0]) return;
	lv_obj_t *l = lv_label_create(v);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, DOCW);
	lv_obj_set_style_text_color(l, col, 0);
	lv_label_set_text(l, txt);
}
static void emit_heading(lv_obj_t *v, const char *txt, int hashes){
	lv_obj_t *l = lv_label_create(v);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, DOCW);
	if(hashes <= 1){ lv_obj_set_style_text_font(l, KF_FONT_BIG, 0); lv_obj_set_style_text_color(l, KF_AMBER_HOT, 0); }
	else if(hashes == 2){ lv_obj_set_style_text_color(l, KF_AMBER_BR, 0); }
	else { lv_obj_set_style_text_color(l, KF_AMBER, 0); }
	lv_obj_set_style_pad_top(l, hashes<=2 ? 4 : 2, 0);
	lv_label_set_text(l, txt);
}
static void emit_rule(lv_obj_t *v){
	lv_obj_t *r = lv_obj_create(v);
	lv_obj_remove_style_all(r);
	lv_obj_set_size(r, DOCW, 1);
	lv_obj_set_style_bg_color(r, KF_BORDER_HI, 0);
	lv_obj_set_style_bg_opa(r, LV_OPA_COVER, 0);
}
/* fenced code block -> a bordered card (CakeSpark scripts, calc expressions, ...) */
static void emit_code(lv_obj_t *v, const char *code){
	lv_obj_t *card = lv_obj_create(v);
	lv_obj_remove_style_all(card);
	lv_obj_set_width(card, DOCW);
	lv_obj_set_height(card, LV_SIZE_CONTENT);
	lv_obj_set_style_bg_color(card, KF_CARD, 0);
	lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
	lv_obj_set_style_border_color(card, KF_BORDER_HI, 0);
	lv_obj_set_style_border_width(card, 1, 0);
	lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
	lv_obj_set_style_pad_all(card, 5, 0);
	lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(card);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, DOCW - 12);
	lv_obj_set_style_text_color(l, KF_AMBER_GLOW, 0);
	lv_label_set_text(l, code[0] ? code : " ");
}

/* parse Markdown `text` into widgets under the scroll container `v`. `flow`/`code` are
   scratch buffers (each at least strlen(text)+1) reused so we make few allocations. */
static void render_md(lv_obj_t *v, char *text, char *flow, char *code){
	int flen = 0;                                  /* accumulated plain-flow length */
	char *p = text;
	while(*p){
		char *nl = strchr(p, '\n');
		char *line = p;
		if(nl) *nl = 0;
		int n = (int)strlen(line);
		if(n && line[n-1] == '\r') line[--n] = 0;

		if(line[0]=='`' && line[1]=='`' && line[2]=='`'){          /* fenced code block */
			if(flen){ flow[flen]=0; emit_text(v, flow, KF_TEXT); flen=0; }
			int clen = 0; code[0]=0;
			char *q = nl ? nl+1 : p+n;                             /* first body line */
			while(*q){
				char *qnl = strchr(q, '\n'); char *cl = q;
				if(qnl) *qnl = 0;
				int cn = (int)strlen(cl); if(cn && cl[cn-1]=='\r') cl[--cn]=0;
				if(cl[0]=='`' && cl[1]=='`' && cl[2]=='`'){ q = qnl ? qnl+1 : cl+cn; p = q; goto code_done; }
				if(clen){ code[clen++]='\n'; }
				memcpy(code+clen, cl, cn); clen += cn; code[clen]=0;
				q = qnl ? qnl+1 : cl+cn;
			}
			p = q;
		code_done:
			emit_code(v, code);
			continue;
		}
		if(line[0]=='#'){                                          /* heading */
			if(flen){ flow[flen]=0; emit_text(v, flow, KF_TEXT); flen=0; }
			int h=0; while(line[h]=='#') h++;
			const char *t = line+h; while(*t==' ') t++;
			char tmp[256]; inline_clean(tmp, t);
			emit_heading(v, tmp, h);
		}
		else if((line[0]=='-'||line[0]=='*'||line[0]=='_') && (line[1]==line[0]) && (line[2]==line[0]) && !line[3]){
			if(flen){ flow[flen]=0; emit_text(v, flow, KF_TEXT); flen=0; }
			emit_rule(v);                                          /* --- *** ___ */
		}
		else {                                                     /* paragraph / list / blank */
			char tmp[600]; inline_clean(tmp, line);
			const char *out = tmp;
			char bullet[600];
			if((tmp[0]=='-'||tmp[0]=='*') && tmp[1]==' '){         /* bullet -> "  - text" */
				snprintf(bullet, sizeof bullet, "  - %s", tmp+2); out = bullet;
			}
			int on = (int)strlen(out);
			if(flen){ flow[flen++]='\n'; }
			memcpy(flow+flen, out, on); flen += on; flow[flen]=0;
		}
		p = nl ? nl+1 : p+n;
	}
	if(flen){ flow[flen]=0; emit_text(v, flow, KF_TEXT); }
}

/* article scroll-container keys: Up/Dn scroll, BACKSPACE backs out */
static void view_key_cb(lv_event_t *e){
	lv_obj_t *v = lv_event_get_target(e);
	uint32_t k = lv_event_get_key(e);
	if(k==LV_KEY_UP)        lv_obj_scroll_by(v, 0,  60, LV_ANIM_ON);
	else if(k==LV_KEY_DOWN) lv_obj_scroll_by(v, 0, -60, LV_ANIM_ON);
	else if(k==LV_KEY_BACKSPACE) go_back();
}

static void open_article(const char *topic_path, const char *file_raw, int direct){
	char path[420];
	snprintf(path, sizeof path, "%s/%s", topic_path, file_raw);
	char dt[128]; disp_title(dt, sizeof dt, file_raw);

	level = 2; article_direct = direct;
	lv_label_set_text(title, dt);
	lv_label_set_text(hint, "Up/Dn scroll   BKSP back");
	lv_obj_clean(holder);
	grp = kf_use_group();

	lv_obj_t *v = make_view();

	FILE *f = fopen(path, "rb");
	if(!f){ emit_text(v, "Could not open this page.", KF_TEXT_DIM); }
	else {
		fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
		if(sz < 0) sz = 0; if(sz > MAXFILE) sz = MAXFILE;
		char *buf = malloc((size_t)sz + 1);
		char *flow = malloc((size_t)sz + 2);
		char *code = malloc((size_t)sz + 2);
		if(buf && flow && code){
			size_t rd = fread(buf, 1, (size_t)sz, f); buf[rd] = 0;
			render_md(v, buf, flow, code);
		} else {
			emit_text(v, "Out of memory loading this page.", KF_TEXT_DIM);
		}
		free(buf); free(flow); free(code);
		fclose(f);
	}
	lv_group_add_obj(grp, v);
	lv_obj_add_event_cb(v, view_key_cb, LV_EVENT_KEY, NULL);
	lv_group_focus_obj(v);
	lv_obj_scroll_to_y(v, 0, LV_ANIM_OFF);
}

/* list a directory level: collect names (dirs if want_dirs, else .md files), sort, build
   an lv_list. Returns the number of entries listed. */
static lv_obj_t *cur_list;

static void topic_row_cb(lv_event_t *e){ enter_topic((const char*)lv_event_get_user_data(e)); }
static void section_row_cb(lv_event_t *e){ open_article(cur_topic, (const char*)lv_event_get_user_data(e), 0); }

static int list_dir(const char *dir, int want_dirs){
	free_names();
	DIR *d = opendir(dir);
	if(d){ struct dirent *e;
		while((e=readdir(d)) && nnames<MAXN){
			if(!e->d_name[0] || e->d_name[0]=='.') continue;
			int isdir = (e->d_type==DT_DIR);
			if(want_dirs){ if(!isdir) continue; }
			else {
				if(isdir) continue;
				int ln=(int)strlen(e->d_name);
				if(ln<3 || strcmp(e->d_name+ln-3, ".md")) continue;
			}
			char *nm = strdup(e->d_name);
			if(!nm) break;
			names[nnames++] = nm;
		}
		closedir(d);
	}
	qsort(names, nnames, sizeof names[0], cmp_names);
	return nnames;
}

/* ---------- levels ---------- */
static void enter_topics(void){
	level = 0;
	lv_label_set_text(title, "Help");
	lv_label_set_text(hint, "ENTER open   BKSP/ESC exit");
	lv_obj_clean(holder);
	grp = kf_use_group();

	cur_list = lv_list_create(holder);
	lv_obj_set_size(cur_list, LCD_W-8, KF_CONTENT_H-46);
	lv_obj_align(cur_list, LV_ALIGN_TOP_MID, 0, 26);

	int n = list_dir(HELP_ROOT, 1);
	if(n == 0){
		lv_obj_t *l = lv_label_create(holder);
		lv_obj_align(l, LV_ALIGN_CENTER, 0, 0);
		lv_obj_set_width(l, DOCW);
		lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_color(l, KF_TEXT_DIM, 0);
		lv_label_set_text(l, "No help content found.\n\nExpected Markdown under\n/kefyros/help/<Topic>/<Section>.md\non the SD card.");
		return;
	}
	for(int i=0;i<nnames;i++){
		char dt[128]; disp_title(dt, sizeof dt, names[i]);
		lv_obj_t *b = lv_list_add_button(cur_list, NULL, dt);
		lv_obj_add_event_cb(b, topic_row_cb, LV_EVENT_CLICKED, names[i]);
		lv_obj_add_event_cb(b, row_key_cb,   LV_EVENT_KEY,     NULL);
		lv_group_add_obj(grp, b);
	}
}

static void enter_topic(const char *topic_raw){
	/* topic_raw is one of names[] from the topics level OR cur_topic_disp on re-entry;
	   rebuild the full path + display name from the raw folder name. */
	/* copy the raw name FIRST: topic_raw may point into names[], which list_dir frees */
	snprintf(cur_topic_raw, sizeof cur_topic_raw, "%s", topic_raw);
	snprintf(cur_topic, sizeof cur_topic, "%s/%s", HELP_ROOT, cur_topic_raw);
	disp_title(cur_topic_disp, sizeof cur_topic_disp, cur_topic_raw);

	int n = list_dir(cur_topic, 0);
	if(n == 1){ open_article(cur_topic, names[0], 1); return; }   /* skip 1-item submenu */
	if(n == 0){ enter_topics(); return; }

	level = 1;
	lv_label_set_text(title, cur_topic_disp);
	lv_label_set_text(hint, "ENTER open   BKSP back");
	lv_obj_clean(holder);
	grp = kf_use_group();

	cur_list = lv_list_create(holder);
	lv_obj_set_size(cur_list, LCD_W-8, KF_CONTENT_H-46);
	lv_obj_align(cur_list, LV_ALIGN_TOP_MID, 0, 26);
	for(int i=0;i<nnames;i++){
		char dt[128]; disp_title(dt, sizeof dt, names[i]);
		lv_obj_t *b = lv_list_add_button(cur_list, NULL, dt);
		lv_obj_add_event_cb(b, section_row_cb, LV_EVENT_CLICKED, names[i]);
		lv_obj_add_event_cb(b, row_key_cb,     LV_EVENT_KEY,     NULL);
		lv_group_add_obj(grp, b);
	}
}

void app_help_open(void){
	scr = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr, 0, 0);
	lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
	kf_inset_top(scr);

	title = lv_label_create(scr);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 5);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, LCD_W-8);

	hint = lv_label_create(scr);
	lv_obj_set_style_text_color(hint, KF_TEXT_MUTED, 0);
	lv_obj_align(hint, LV_ALIGN_BOTTOM_LEFT, 4, -3);

	holder = lv_obj_create(scr);
	lv_obj_remove_style_all(holder);
	lv_obj_set_size(holder, LCD_W, KF_CONTENT_H-20);
	lv_obj_align(holder, LV_ALIGN_TOP_MID, 0, 0);
	lv_obj_clear_flag(holder, LV_OBJ_FLAG_SCROLLABLE);

	nnames = 0; grp = NULL;
	enter_topics();
	lv_screen_load(scr);
}
