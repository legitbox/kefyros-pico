// apps/deepseek.c — DeepSeek chat client for Kefyros.
//
// Two screens, mirroring the Editor's picker/editor split:
//   * Chat LIST  — a menu app (not grabbed): saved conversations + New chat +
//     Settings (API key) + a model toggle. Global ESC -> launcher.
//   * Chat VIEW  — GRABS the keyboard (like the editor) so it owns every key:
//     typing feeds the input textarea, ENTER sends, UP/DOWN scroll the log, and
//     ESC goes back to the chat list (not straight to the launcher).
//
// Networking rides the existing BearSSL HTTPS path (port/http.c kf_http_post):
// POST https://api.deepseek.com/chat/completions, Authorization: Bearer <key>,
// OpenAI-shaped JSON. Responses stream into a PSRAM arena and are parsed by a
// small streaming JSON value extractor (no JSON lib on the firmware). The radio
// only associates <=~270 MHz, so the app runs at the eco clock and restores the
// 360 MHz UI clock when it finally returns to the launcher.
//
// API key + model persist via deskconf (/kefyros/config.txt on the SD card);
// each conversation is a file under /kefyros/chats/<id>.txt.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include "../port/http.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>

#define DS_URL      "https://api.deepseek.com/chat/completions"
#define DS_CHATDIR  "/kefyros/chats"
#define DS_BODY_MAX (512u*1024u)          /* PSRAM response arena (thinking can be large) */

#define DS_SYSTEM \
	"You are a helpful assistant. Reply in plain text only. Do not use markdown, " \
	"formatting symbols, bullet points, headings, or emojis. Write naturally, as " \
	"in a normal conversation."

static const char *MODELS[] = { "deepseek-v4-flash", "deepseek-v4-pro" };
#define NMODELS (int)(sizeof(MODELS)/sizeof(MODELS[0]))

/* ---- persisted config (loaded from deskconf) ---- */
static char ds_key[200];
static char ds_model[40];

/* ---- PSRAM response arena (alloc once, reuse) ---- */
static uint32_t ds_arena = 0xFFFFFFFFu;

/* ---- conversation (in memory) ---- */
#define MSG_MAX 64
typedef struct { char role; char *text; } msg_t;   /* role 'U' user / 'A' assistant */
static msg_t msgs[MSG_MAX];
static int   nmsg;
static char  cur_path[600];                         /* chat file backing this conversation */

/* ---- request buffers ---- */
static char ds_req[12*1024];
static char ds_hdr[400];

/* ---- extractor outputs ---- */
static char g_content[4096];
static char g_reason[3072];
static char g_err[256];

/* ---- UI state ---- */
static lv_obj_t  *scr_list, *list_w, *key_box;
static lv_obj_t  *scr_chat, *log_w, *input_ta, *lbl_stat;
static lv_group_t *grp;
static int g_active = 0;        /* the app owns the foreground */
static int chatting = 0;        /* chat VIEW is up (grabbed) -> deepseek_poll runs */
static int busy = 0;            /* a request is in flight */

#define NAMES_MAX 96
static char *names[NAMES_MAX];
static int   nnames;
static void free_names(void){ for(int i=0;i<nnames;i++) free(names[i]); nnames = 0; }
static void free_msgs(void){ for(int i=0;i<nmsg;i++) free(msgs[i].text); nmsg = 0; }

static void open_list(void);
static void open_chat_view(void);

/* ===================== config ===================== */
static void cfg_load(void){
	snprintf(ds_key,   sizeof ds_key,   "%s", deskconf_get("ds_api_key", ""));
	snprintf(ds_model, sizeof ds_model, "%s", deskconf_get("ds_model", MODELS[0]));
}

/* ===================== message store ===================== */
static void add_msg(char role, const char *text){
	if(nmsg >= MSG_MAX){                      /* drop the oldest to make room */
		free(msgs[0].text);
		memmove(&msgs[0], &msgs[1], (MSG_MAX-1)*sizeof(msg_t));
		nmsg = MSG_MAX-1;
	}
	msgs[nmsg].role = role;
	msgs[nmsg].text = strdup(text ? text : "");
	if(msgs[nmsg].text) nmsg++;
}

/* ===================== persistence ===================== */
/* one message per line: <role><text>, with '\\' and newlines backslash-escaped. */
static void save_chat(void){
	if(!cur_path[0]) return;
	mkdir(DS_CHATDIR, 0755);
	FILE *f = fopen(cur_path, "w");
	if(!f) return;
	for(int i=0;i<nmsg;i++){
		fputc(msgs[i].role, f);
		for(const char *p = msgs[i].text; *p; p++){
			if(*p=='\\'){ fputc('\\',f); fputc('\\',f); }
			else if(*p=='\n'){ fputc('\\',f); fputc('n',f); }
			else if(*p=='\r'){ /* drop */ }
			else fputc(*p, f);
		}
		fputc('\n', f);
	}
	fclose(f);
}

static void load_chat(const char *path){
	free_msgs();
	snprintf(cur_path, sizeof cur_path, "%s", path);
	FILE *f = fopen(path, "r");
	if(!f) return;
	char *line = malloc(8192);
	if(!line){ fclose(f); return; }
	while(fgets(line, 8192, f)){
		int len = (int)strlen(line);
		while(len>0 && (line[len-1]=='\n'||line[len-1]=='\r')) line[--len]=0;
		if(len < 1) continue;
		char role = line[0];
		if(role!='U' && role!='A') continue;
		/* unescape in place into a scratch */
		char *out = malloc(len+1); if(!out) continue;
		int o = 0;
		for(int i=1;i<len;i++){
			if(line[i]=='\\' && i+1<len){
				char n = line[++i];
				out[o++] = (n=='n') ? '\n' : n;
			} else out[o++] = line[i];
		}
		out[o] = 0;
		add_msg(role, out);
		free(out);
	}
	free(line);
	fclose(f);
}

/* derive a list title from a chat file's first user line */
static void chat_title(const char *path, char *out, int n){
	out[0] = 0;
	FILE *f = fopen(path, "r");
	if(!f){ snprintf(out, n, "%s", path); return; }
	char line[256];
	if(fgets(line, sizeof line, f)){
		const char *t = line;
		if(*t=='U' || *t=='A') t++;
		int o = 0;
		for(; *t && *t!='\n' && *t!='\r' && o<n-1; t++){
			if(*t=='\\'){ if(*(t+1)){ t++; out[o++]=' '; } continue; }
			out[o++] = *t;
		}
		out[o] = 0;
	}
	fclose(f);
	if(!out[0]) snprintf(out, n, "(empty)");
}

static int next_chat_id(void){
	int max = 0;
	DIR *d = opendir(DS_CHATDIR);
	if(d){ struct dirent *e;
		while((e = readdir(d))){
			int id = atoi(e->d_name);
			if(id > max) max = id;
		}
		closedir(d);
	}
	return max + 1;
}

/* ===================== JSON: request build ===================== */
static int jesc(char *buf, int n, int cap, const char *s){
	for(; *s && n < cap-7; s++){
		unsigned char c = (unsigned char)*s;
		switch(c){
		case '"':  buf[n++]='\\'; buf[n++]='"';  break;
		case '\\': buf[n++]='\\'; buf[n++]='\\'; break;
		case '\n': buf[n++]='\\'; buf[n++]='n';  break;
		case '\r': buf[n++]='\\'; buf[n++]='r';  break;
		case '\t': buf[n++]='\\'; buf[n++]='t';  break;
		case '\b': buf[n++]='\\'; buf[n++]='b';  break;
		case '\f': buf[n++]='\\'; buf[n++]='f';  break;
		default:
			if(c < 0x20){ n += snprintf(buf+n, cap-n, "\\u%04x", c); }
			else buf[n++] = (char)c;
		}
	}
	buf[n] = 0; return n;
}

static int build_req_json(void){
	int cap = (int)sizeof ds_req;
	int n = snprintf(ds_req, cap,
		"{\"model\":\"%s\",\"thinking\":{\"type\":\"enabled\"},\"stream\":false,"
		"\"messages\":[{\"role\":\"system\",\"content\":\"", ds_model);
	if(n >= cap) n = cap-1;
	n = jesc(ds_req, n, cap, DS_SYSTEM);
	n += snprintf(ds_req+n, cap-n, "\"}"); if(n >= cap) n = cap-1;

	/* include as much recent history as fits the buffer (always the last message) */
	int budget = cap - 1024, acc = 0, start = nmsg;
	for(int i=nmsg-1;i>=0;i--){ acc += (int)strlen(msgs[i].text)+40; if(acc > budget) break; start = i; }
	if(start >= nmsg && nmsg > 0) start = nmsg-1;

	for(int i=start;i<nmsg;i++){
		n += snprintf(ds_req+n, cap-n, ",{\"role\":\"%s\",\"content\":\"",
		              msgs[i].role=='U' ? "user" : "assistant");
		if(n >= cap){ n = cap-1; break; }
		n = jesc(ds_req, n, cap, msgs[i].text);
		n += snprintf(ds_req+n, cap-n, "\"}"); if(n >= cap){ n = cap-1; break; }
	}
	n += snprintf(ds_req+n, cap-n, "]}"); if(n >= cap) n = cap-1;
	return n;
}

/* ===================== JSON: streaming response extractor ===================== */
/* Walks the PSRAM body in windows and captures the string values of the keys
   "content", "reasoning_content", and (for errors) "message", into g_content/
   g_reason/g_err with unescaping. A string is a KEY iff it sits before a ':';
   the next string after a key is that key's value. Object/array/number values
   (e.g. choices[].message -> {...}) are skipped, so the error "message":"..."
   (a string) is captured while the success message object is not. */
static int hexval(char c){
	if(c>='0'&&c<='9') return c-'0';
	if(c>='a'&&c<='f') return c-'a'+10;
	if(c>='A'&&c<='F') return c-'A'+10;
	return -1;
}
static char *cap_buf; static int cap_cap, cap_len;
static void cap_begin(char *b, int c){ cap_buf=b; cap_cap=c; cap_len=0; b[0]=0; }
static void cap_putc(int c){ if(cap_buf && cap_len < cap_cap-1){ cap_buf[cap_len++]=(char)c; cap_buf[cap_len]=0; } }
static void cap_putu(unsigned cp){
	if(cp < 0x80) cap_putc((int)cp);
	else if(cp < 0x800){ cap_putc(0xC0|(cp>>6)); cap_putc(0x80|(cp&0x3F)); }
	else { cap_putc(0xE0|(cp>>12)); cap_putc(0x80|((cp>>6)&0x3F)); cap_putc(0x80|(cp&0x3F)); }
}

static void ds_extract(uint32_t base, uint32_t len){
	g_content[0] = g_reason[0] = g_err[0] = 0;
	int instr=0, esc=0, reading_key=0, reading_val=0, capturing=0;
	int valesc=0; unsigned uacc=0; int uhex=0;
	char keybuf[40]; int keylen=0, havekey=0;
	char pend[40]; int expectval=0;
	pend[0]=0;

	uint8_t win[256];
	for(uint32_t off=0; off<len; ){
		uint32_t n = len-off; if(n > sizeof win) n = sizeof win;
		kf_psram_read(base+off, win, n);
		for(uint32_t i=0;i<n;i++){
			char c = (char)win[i];
			if(instr){
				if(reading_val && capturing){
					if(valesc==0){
						if(c=='\\') valesc=1;
						else if(c=='"'){ instr=reading_val=capturing=0; expectval=havekey=0; }
						else cap_putc((unsigned char)c);
					} else if(valesc==1){
						switch(c){
						case 'n': cap_putc('\n'); valesc=0; break;
						case 'r': cap_putc('\r'); valesc=0; break;
						case 't': cap_putc('\t'); valesc=0; break;
						case 'b': cap_putc('\b'); valesc=0; break;
						case 'f': cap_putc('\f'); valesc=0; break;
						case '/': cap_putc('/');  valesc=0; break;
						case '"': cap_putc('"');  valesc=0; break;
						case '\\':cap_putc('\\'); valesc=0; break;
						case 'u': valesc=2; uacc=0; uhex=0; break;
						default:  cap_putc((unsigned char)c); valesc=0; break;
						}
					} else {
						int hv = hexval(c);
						if(hv < 0) valesc=0;
						else { uacc=(uacc<<4)|(unsigned)hv; if(++uhex==4){
							if(uacc>=0xD800 && uacc<=0xDFFF) cap_putc('?');
							else cap_putu(uacc); valesc=0; } }
					}
				} else if(reading_val){               /* value string we don't want */
					if(valesc) valesc=0;
					else if(c=='\\') valesc=1;
					else if(c=='"'){ instr=reading_val=0; expectval=havekey=0; }
				} else {                               /* key string */
					if(esc){ esc=0; if(keylen<(int)sizeof keybuf-1) keybuf[keylen++]=c; }
					else if(c=='\\') esc=1;
					else if(c=='"'){ instr=reading_key=0; keybuf[keylen]=0; havekey=1; }
					else if(keylen<(int)sizeof keybuf-1) keybuf[keylen++]=c;
				}
				continue;
			}
			if(c=='"'){
				instr=1;
				if(expectval){
					reading_val=1; valesc=0; capturing=0;
					if(!strcmp(pend,"content")){ capturing=1; cap_begin(g_content,sizeof g_content); }
					else if(!strcmp(pend,"reasoning_content")){ capturing=1; cap_begin(g_reason,sizeof g_reason); }
					else if(!strcmp(pend,"message") && g_content[0]==0){ capturing=1; cap_begin(g_err,sizeof g_err); }
					expectval=havekey=0;
				} else { reading_key=1; keylen=0; esc=0; havekey=0; }
			} else if(c==':'){
				if(havekey){ snprintf(pend,sizeof pend,"%s",keybuf); expectval=1; havekey=0; }
			} else if(c==' '||c=='\t'||c=='\n'||c=='\r'){
				/* whitespace: keep havekey (a ':' may still follow) */
			} else {
				havekey=0;
				if(expectval) expectval=0;          /* this value isn't a string */
			}
		}
		off += n;
	}
}

/* ===================== chat VIEW rendering ===================== */
static void set_status(const char *s){ if(lbl_stat) lv_label_set_text(lbl_stat, s ? s : ""); }

static void add_bubble(char role, const char *text){
	lv_obj_t *b = lv_obj_create(log_w);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LCD_W-16);
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(b, 4, 0);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
	lv_obj_set_style_bg_color(b, role=='U' ? KF_CARD_HI : KF_CARD, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, LCD_W-16-8);
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, role=='U' ? KF_AMBER_BR : KF_TEXT, 0);
	lv_label_set_text(l, text);
	lv_obj_update_layout(log_w);
	lv_obj_scroll_to_view(b, LV_ANIM_OFF);
}

static void add_reason(const char *text){
	lv_obj_t *b = lv_obj_create(log_w);
	lv_obj_remove_style_all(b);
	lv_obj_set_width(b, LCD_W-16);
	lv_obj_set_height(b, LV_SIZE_CONTENT);
	lv_obj_set_style_pad_all(b, 4, 0);
	lv_obj_set_style_radius(b, 0, 0);
	lv_obj_set_style_bg_opa(b, LV_OPA_TRANSP, 0);
	lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_t *l = lv_label_create(b);
	lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
	lv_obj_set_width(l, LCD_W-16-8);
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, KF_TEXT_MUTED, 0);
	lv_label_set_text_fmt(l, "thinking: %s", text);
}

/* ===================== networking ===================== */
static int ensure_arena(void){
	if(ds_arena == 0xFFFFFFFFu && kf_psram_size())
		ds_arena = kf_psram_alloc(DS_BODY_MAX);
	return ds_arena != 0xFFFFFFFFu;
}

static void send_current(void){
	if(busy) return;
	const char *t = lv_textarea_get_text(input_ta);
	if(!t || !t[0]) return;
	if(!ds_key[0]){ set_status("no API key - go back, open Settings"); return; }
	if(kf_net_state() != KF_NET_ONLINE){ set_status("WiFi offline - connect in WiFi app"); return; }
	if(!ensure_arena()){ set_status("no PSRAM arena"); return; }

	add_msg('U', t);
	add_bubble('U', t);
	save_chat();
	lv_textarea_set_text(input_ta, "");

	int n = build_req_json();
	snprintf(ds_hdr, sizeof ds_hdr,
		"Authorization: Bearer %s\r\nContent-Type: application/json\r\nAccept: application/json\r\n",
		ds_key);
	kf_http_set_arena(ds_arena, DS_BODY_MAX);
	if(kf_http_post(DS_URL, ds_hdr, ds_req, (uint32_t)n) != 0){ set_status(kf_http_err()); return; }
	busy = 1;
	set_status("thinking...");
}

static void on_reply(void){
	ds_extract(kf_http_body_base(), kf_http_body_len());
	if(g_content[0]){
		if(g_reason[0]) add_reason(g_reason);
		add_msg('A', g_content);
		add_bubble('A', g_content);
		save_chat();
		set_status("");
	} else if(g_err[0]){
		char m[280]; snprintf(m, sizeof m, "error: %s", g_err);
		set_status(m);
	} else {
		char m[64]; snprintf(m, sizeof m, "no reply (HTTP %d)", kf_http_status());
		set_status(m);
	}
}

/* ===================== chat VIEW key pump (grabbed) ===================== */
static void back_to_list(void){
	kf_grab_input(0);
	chatting = 0;
	open_list();                              /* builds + loads a fresh list screen */
	if(scr_chat){ lv_obj_delete_async(scr_chat); scr_chat = NULL; }
}

void deepseek_poll(void){
	if(!chatting) return;
	uint8_t st, key;
	while(uart_pop_key(&st, &key)){
		if(st == KS_RELEASE) continue;
		if(busy){                              /* only ESC works while waiting */
			if(key==DK_ESC || key==DK_BREAK){ kf_http_abort(); busy=0; back_to_list(); return; }
			continue;
		}
		switch(key){
		case DK_ESC:
		case DK_BREAK:     back_to_list(); return;
		case DK_ENTER:     send_current(); break;
		case DK_BACKSPACE: lv_textarea_delete_char(input_ta); break;
		case DK_LEFT:      lv_textarea_cursor_left(input_ta);  break;
		case DK_RIGHT:     lv_textarea_cursor_right(input_ta); break;
		case DK_UP:        lv_obj_scroll_by(log_w, 0,  60, LV_ANIM_ON); break;
		case DK_DOWN:      lv_obj_scroll_by(log_w, 0, -60, LV_ANIM_ON); break;
		case DK_PGUP:      lv_obj_scroll_by(log_w, 0,  200, LV_ANIM_ON); break;
		case DK_PGDN:      lv_obj_scroll_by(log_w, 0, -200, LV_ANIM_ON); break;
		default:           if(key>=0x20 && key<0x7f) lv_textarea_add_char(input_ta, key); break;
		}
	}
	if(busy){
		kf_http_poll();
		int s = kf_http_state();
		if(s == KF_HTTP_DONE){ busy=0; on_reply(); }
		else if(s == KF_HTTP_ERROR){ busy=0; char m[80]; snprintf(m,sizeof m,"error: %s", kf_http_err()); set_status(m); }
	}
}

/* ===================== exit detection (clock restore) ===================== */
/* Fires when EITHER of our screens is deleted. If, after the delete, neither of
   our screens is the active one, we have truly left the app (ESC on the list ->
   launcher) — restore the UI clock and clean up. Intra-app navigation (list<->
   chat) leaves one of our screens active, so it does NOT trigger this. */
static void on_scr_del(lv_event_t *e){
	lv_obj_t *self = lv_event_get_target(e);
	if(self == scr_chat) scr_chat = NULL;
	if(self == scr_list) scr_list = NULL;
	lv_obj_t *act = lv_screen_active();
	if(act != scr_chat && act != scr_list){
		if(g_active){
			g_active = 0; chatting = 0; busy = 0;
			kf_http_abort();
			kf_grab_input(0);
			free_msgs(); free_names();
			kf_clock_normal();
		}
	}
}

/* ===================== chat VIEW screen ===================== */
static void open_chat_view(void){
	scr_chat = lv_obj_create(NULL);
	lv_obj_set_style_bg_color(scr_chat, KF_BG_DEEP, 0);
	lv_obj_set_style_pad_all(scr_chat, 0, 0);
	kf_inset_top(scr_chat);
	lv_obj_clear_flag(scr_chat, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr_chat, on_scr_del, LV_EVENT_DELETE, NULL);

	/* title bar */
	lv_obj_t *title = lv_label_create(scr_chat);
	lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
	lv_obj_set_width(title, LCD_W-8);
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 1);
	lv_label_set_text_fmt(title, "DeepSeek  -  %s", ds_model);

	/* message log (scrollable column) */
	int log_h = KF_CONTENT_H - 18 - 24 - 14;
	log_w = lv_obj_create(scr_chat);
	lv_obj_remove_style_all(log_w);
	lv_obj_set_size(log_w, LCD_W, log_h);
	lv_obj_align(log_w, LV_ALIGN_TOP_MID, 0, 18);
	lv_obj_set_flex_flow(log_w, LV_FLEX_FLOW_COLUMN);
	lv_obj_set_style_pad_row(log_w, 4, 0);
	lv_obj_set_style_pad_all(log_w, 4, 0);
	lv_obj_set_style_bg_color(log_w, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(log_w, LV_OPA_COVER, 0);
	lv_obj_set_scroll_dir(log_w, LV_DIR_VER);

	/* status line */
	lbl_stat = lv_label_create(scr_chat);
	lv_label_set_long_mode(lbl_stat, LV_LABEL_LONG_DOT);
	lv_obj_set_width(lbl_stat, LCD_W-8);
	lv_obj_set_style_text_font(lbl_stat, KF_FONT, 0);
	lv_obj_set_style_text_color(lbl_stat, KF_TEXT_DIM, 0);
	lv_obj_align(lbl_stat, LV_ALIGN_BOTTOM_LEFT, 4, -24);
	lv_label_set_text(lbl_stat, "");

	/* input (manually fed; grabbed mode) */
	input_ta = lv_textarea_create(scr_chat);
	lv_textarea_set_one_line(input_ta, true);
	lv_textarea_set_placeholder_text(input_ta, "message (ENTER send, ESC back)");
	lv_obj_set_size(input_ta, LCD_W, 24);
	lv_obj_align(input_ta, LV_ALIGN_BOTTOM_MID, 0, 0);
	lv_obj_set_style_text_font(input_ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(input_ta, KF_BG, 0);
	lv_obj_set_style_text_color(input_ta, KF_TEXT, 0);
	lv_obj_set_style_radius(input_ta, 0, 0);
	lv_obj_set_style_border_width(input_ta, 0, 0);
	lv_obj_set_style_outline_width(input_ta, 0, 0);
	lv_obj_set_style_outline_width(input_ta, 0, LV_STATE_FOCUSED);

	grp = kf_use_group();
	lv_group_add_obj(grp, input_ta);
	lv_group_focus_obj(input_ta);

	/* render existing conversation */
	for(int i=0;i<nmsg;i++) add_bubble(msgs[i].role, msgs[i].text);

	kf_grab_input(1);
	chatting = 1;
	busy = 0;
	lv_screen_load(scr_chat);

	lv_obj_t *prev_list = scr_list; scr_list = NULL;   /* leaving the list behind */
	if(prev_list) lv_obj_delete_async(prev_list);
}

/* open an existing chat by filename, or start a new one (NULL) */
static void open_chat(const char *filename){
	if(filename){
		char full[600]; snprintf(full, sizeof full, "%s/%s", DS_CHATDIR, filename);
		load_chat(full);
	} else {
		free_msgs();
		snprintf(cur_path, sizeof cur_path, "%s/%d.txt", DS_CHATDIR, next_chat_id());
	}
	open_chat_view();
}

/* ===================== API-key entry overlay (list screen) ===================== */
static void key_ready(lv_event_t *e){
	lv_obj_t *ta = lv_event_get_target(e);
	const char *v = lv_textarea_get_text(ta);
	snprintf(ds_key, sizeof ds_key, "%s", v ? v : "");
	deskconf_set("ds_api_key", ds_key);
	if(key_box){ lv_obj_delete(key_box); key_box = NULL; }
	open_list();                              /* rebuild (refreshes the key status) */
}

static void open_key_modal(void){
	if(key_box) lv_obj_delete(key_box);
	key_box = lv_obj_create(scr_list);
	lv_obj_set_size(key_box, LCD_W-16, 72);
	lv_obj_align(key_box, LV_ALIGN_TOP_MID, 0, 30);
	lv_obj_set_style_bg_color(key_box, KF_CARD, 0);
	lv_obj_set_style_border_color(key_box, KF_AMBER, 0);
	lv_obj_set_style_border_width(key_box, 2, 0);
	lv_obj_set_style_radius(key_box, 0, 0);
	lv_obj_clear_flag(key_box, LV_OBJ_FLAG_SCROLLABLE);

	lv_obj_t *l = lv_label_create(key_box);
	lv_label_set_text(l, "API key (ENTER save):");
	lv_obj_set_style_text_font(l, KF_FONT, 0);
	lv_obj_set_style_text_color(l, KF_AMBER_BR, 0);
	lv_obj_align(l, LV_ALIGN_TOP_LEFT, 0, 0);

	lv_obj_t *ta = lv_textarea_create(key_box);
	lv_textarea_set_one_line(ta, true);
	lv_obj_set_width(ta, LCD_W-16-14);
	lv_obj_align(ta, LV_ALIGN_BOTTOM_LEFT, 0, 0);
	lv_obj_set_style_text_font(ta, KF_FONT, 0);
	lv_obj_set_style_bg_color(ta, KF_BG, 0);
	lv_obj_set_style_text_color(ta, KF_TEXT, 0);
	lv_obj_set_style_radius(ta, 0, 0);
	lv_obj_set_style_outline_width(ta, 0, 0);
	lv_obj_set_style_outline_width(ta, 0, LV_STATE_FOCUSED);
	lv_textarea_set_text(ta, ds_key);
	lv_obj_add_event_cb(ta, key_ready, LV_EVENT_READY, NULL);
	lv_group_add_obj(grp, ta);
	lv_group_focus_obj(ta);
}

/* ===================== chat LIST screen (menu app) ===================== */
static void list_click(lv_event_t *e){
	const char *tag = lv_event_get_user_data(e);
	if(!strcmp(tag, "\x01")){ open_chat(NULL); return; }            /* New chat   */
	if(!strcmp(tag, "\x02")){ open_key_modal(); return; }           /* Set key    */
	if(!strcmp(tag, "\x03")){                                       /* cycle model*/
		int idx = 0;
		for(int i=0;i<NMODELS;i++) if(!strcmp(ds_model, MODELS[i])) idx = i;
		idx = (idx+1) % NMODELS;
		snprintf(ds_model, sizeof ds_model, "%s", MODELS[idx]);
		deskconf_set("ds_model", ds_model);
		open_list();
		return;
	}
	open_chat(tag);                                                 /* a saved chat */
}

static void build_list_body(void){
	free_names();
	lv_obj_clean(list_w);

	lv_obj_t *b;
	b = lv_list_add_button(list_w, NULL, "[ + New chat ]");
	names[nnames] = strdup("\x01");
	lv_obj_add_event_cb(b, list_click, LV_EVENT_CLICKED, names[nnames]);
	lv_group_add_obj(grp, b); nnames++;

	b = lv_list_add_button(list_w, NULL, ds_key[0] ? "[ API key: set ]" : "[ API key: NOT SET ]");
	names[nnames] = strdup("\x02");
	lv_obj_add_event_cb(b, list_click, LV_EVENT_CLICKED, names[nnames]);
	lv_group_add_obj(grp, b); nnames++;

	{ char row[64]; snprintf(row, sizeof row, "[ Model: %s ]", ds_model);
	  b = lv_list_add_button(list_w, NULL, row);
	  names[nnames] = strdup("\x03");
	  lv_obj_add_event_cb(b, list_click, LV_EVENT_CLICKED, names[nnames]);
	  lv_group_add_obj(grp, b); nnames++; }

	DIR *d = opendir(DS_CHATDIR);
	if(d){ struct dirent *e;
		while((e = readdir(d)) && nnames < NAMES_MAX){
			if(e->d_name[0]=='.') continue;
			char full[600]; snprintf(full, sizeof full, "%s/%s", DS_CHATDIR, e->d_name);
			struct stat sb; if(stat(full,&sb)!=0 || !S_ISREG(sb.st_mode)) continue;
			char title[40]; chat_title(full, title, sizeof title);
			b = lv_list_add_button(list_w, NULL, title);
			names[nnames] = strdup(e->d_name);
			lv_obj_add_event_cb(b, list_click, LV_EVENT_CLICKED, names[nnames]);
			lv_group_add_obj(grp, b); nnames++;
		}
		closedir(d);
	}
	lv_group_focus_obj(lv_obj_get_child(list_w, 0));
}

static void open_list(void){
	/* A list may already be showing when we rebuild it in place (model toggle / key
	   save). Capture it and async-delete after the new one loads — async because this
	   often runs from inside a list button's CLICKED event, whose screen is `old`. */
	lv_obj_t *old_list = scr_list;

	scr_list = lv_obj_create(NULL);
	lv_obj_set_style_pad_all(scr_list, 0, 0);
	lv_obj_set_style_bg_color(scr_list, KF_BG_DEEP, 0);
	lv_obj_set_style_bg_opa(scr_list, LV_OPA_COVER, 0);
	kf_inset_top(scr_list);
	lv_obj_clear_flag(scr_list, LV_OBJ_FLAG_SCROLLABLE);
	lv_obj_add_event_cb(scr_list, on_scr_del, LV_EVENT_DELETE, NULL);

	lv_obj_t *title = lv_label_create(scr_list);
	lv_label_set_text(title, "DeepSeek");
	lv_obj_set_style_text_font(title, KF_FONT, 0);
	lv_obj_set_style_text_color(title, KF_AMBER_BR, 0);
	lv_obj_align(title, LV_ALIGN_TOP_LEFT, 4, 5);

	list_w = lv_list_create(scr_list);
	lv_obj_set_size(list_w, LCD_W-8, KF_CONTENT_H-30);
	lv_obj_align(list_w, LV_ALIGN_TOP_MID, 0, 26);

	grp = kf_use_group();
	key_box = NULL;
	build_list_body();
	lv_screen_load(scr_list);

	if(old_list) lv_obj_delete_async(old_list);
}

/* ===================== entry ===================== */
void app_deepseek_open(void){
	g_active = 1;
	chatting = 0;
	busy = 0;
	scr_chat = NULL; scr_list = NULL; key_box = NULL;
	cur_path[0] = 0;
	cfg_load();
	mkdir("/kefyros", 0755);
	mkdir(DS_CHATDIR, 0755);

	/* radio only associates <=~270 MHz; run eco the whole session, restore UI on exit */
	kf_clock_eco();
	kf_net_init();

	open_list();
}
