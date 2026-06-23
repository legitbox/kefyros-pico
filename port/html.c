// port/html.c — lenient HTML tokenizer (see html.h).
//
// Single linear pass over the body (streamed from PSRAM a byte at a time through a
// 512-byte window), accumulating inline text for the "current block" in an SRAM
// buffer and flushing it as a render-op whenever a block boundary is crossed. Tags
// are read whole, attributes parsed on demand. <script>/<style> bodies are skipped
// raw; <head> text is dropped except <title>; entities are decoded to ASCII;
// whitespace is collapsed everywhere but inside <pre>. The parser is deliberately
// forgiving — malformed, truncated or adversarial markup degrades to plain text.
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../kefyros.h"
#include "html.h"

/* ===== arenas (PSRAM) ===== */
static uint32_t ops_base, ops_cap, op_count;
static uint32_t text_base, text_cap, text_brk;

void kf_html_set_arena(uint32_t ob, uint32_t oc, uint32_t tb_, uint32_t tc){
	ops_base=ob; ops_cap=oc; text_base=tb_; text_cap=tc;
}
uint32_t kf_html_op_count(void){ return op_count; }
void kf_html_get_op(uint32_t i, kf_html_op *out){
	if(i<op_count) kf_psram_read(ops_base + i*sizeof(kf_html_op), out, sizeof *out);
	else memset(out, 0, sizeof *out);
}
void kf_html_read_text(uint32_t off, uint32_t len, char *o, uint32_t cap){
	if(len > cap-1) len = cap-1;
	if(len) kf_psram_read(text_base + off, o, len);
	o[len] = 0;
}

/* ===== byte reader over the PSRAM body ===== */
static uint32_t r_base, r_len, r_pos, r_bufpos, r_buflen;
static uint8_t  r_buf[512];
static int next_byte(void){
	if(r_pos >= r_len) return -1;
	if(r_bufpos >= r_buflen){
		uint32_t want = r_len - r_pos; if(want > sizeof r_buf) want = sizeof r_buf;
		kf_psram_read(r_base + r_pos, r_buf, want);
		r_buflen = want; r_bufpos = 0;
	}
	r_pos++;
	return r_buf[r_bufpos++];
}

/* ===== parser state ===== */
static char     tb[4096];   static int tbn;        /* current block text */
static char     linkbuf[1024]; static int linkn;   /* current <a> text */
static char     curhref[512];                      /* current <a> href */
static char     s_title[128]; static int title_n;
static int      in_title, in_head, in_pre, in_link;
static int      pending_space;
static int      cur_kind;
static int      quote_depth;
static uint16_t li_index; static uint8_t li_depth;
static int      l_ordered[8], l_count[8], l_sp;

static char     form_action[512];
static int      in_form;

/* ===== op emission ===== */
static void emit(uint8_t kind, uint8_t depth, uint16_t index,
                 const char *text, int tlen, const char *href, int hlen){
	kf_html_op op; memset(&op, 0, sizeof op);
	op.kind = kind; op.depth = depth; op.index = index;
	if(text){
		if(tlen < 0) tlen = (int)strlen(text);
		if(tlen > 0 && text_brk + (uint32_t)tlen <= text_cap){
			kf_psram_write(text_base + text_brk, text, tlen);
			op.text_off = text_brk; op.text_len = tlen; text_brk += tlen;
		}
	}
	if(href){
		if(hlen < 0) hlen = (int)strlen(href);
		if(hlen > 0 && text_brk + (uint32_t)hlen <= text_cap){
			kf_psram_write(text_base + text_brk, href, hlen);
			op.href_off = text_brk; op.href_len = hlen; text_brk += hlen;
		}
	}
	if((op_count+1)*sizeof(kf_html_op) <= ops_cap){
		kf_psram_write(ops_base + op_count*sizeof(kf_html_op), &op, sizeof op);
		op_count++;
	}
}

static void flush_block(void){
	pending_space = 0;
	while(tbn > 0 && tb[tbn-1] == ' ') tbn--;
	if(tbn > 0){
		uint16_t idx = (cur_kind==KF_OP_LI) ? li_index : 0;
		uint8_t  dep = (cur_kind==KF_OP_LI) ? li_depth
		             : (cur_kind==KF_OP_QUOTE ? (uint8_t)quote_depth : 0);
		emit((uint8_t)cur_kind, dep, idx, tb, tbn, NULL, 0);
	}
	tbn = 0;
}

/* ===== text accumulation (whitespace-collapsing) ===== */
static void push_char(int c){
	char *buf; int *n; int cap;
	if(in_title){ buf=s_title; n=&title_n; cap=(int)sizeof s_title; }
	else if(in_head) return;                 /* drop non-title <head> text */
	else if(in_link){ buf=linkbuf; n=&linkn; cap=(int)sizeof linkbuf; }
	else { buf=tb; n=&tbn; cap=(int)sizeof tb; }
	if(!in_pre){
		if(c==' '||c=='\t'||c=='\n'||c=='\r'){ pending_space = 1; return; }
		if(pending_space){ if(*n>0 && *n<cap-1) buf[(*n)++]=' '; pending_space = 0; }
	}
	if(*n < cap-1) buf[(*n)++] = (char)c;
}

/* ===== entity decode ===== */
static void push_str(const char *s){ while(*s) push_char((unsigned char)*s++); }
static void read_tag(void);
static void decode_named(const char *e){
	if(!strcmp(e,"amp")) push_char('&');
	else if(!strcmp(e,"lt")) push_char('<');
	else if(!strcmp(e,"gt")) push_char('>');
	else if(!strcmp(e,"quot")) push_char('"');
	else if(!strcmp(e,"apos")||!strcmp(e,"rsquo")||!strcmp(e,"lsquo")) push_char('\'');
	else if(!strcmp(e,"nbsp")) push_char(' ');
	else if(!strcmp(e,"ldquo")||!strcmp(e,"rdquo")) push_char('"');
	else if(!strcmp(e,"ndash")||!strcmp(e,"mdash")) push_char('-');
	else if(!strcmp(e,"hellip")) push_str("...");
	else if(!strcmp(e,"copy")) push_str("(c)");
	else if(!strcmp(e,"reg")) push_str("(R)");
	else if(!strcmp(e,"trade")) push_str("(TM)");
	else if(!strcmp(e,"deg")) push_str("deg");
	else if(!strcmp(e,"middot")||!strcmp(e,"bull")) push_char('*');
	else push_char('?');
}
static void decode_codepoint(long v){
	if(v=='\t'||v=='\n'||v==0xa0) push_char(' ');
	else if(v>=32 && v<127) push_char((int)v);
	else if(v==0x2018||v==0x2019) push_char('\'');
	else if(v==0x201c||v==0x201d) push_char('"');
	else if(v==0x2013||v==0x2014) push_char('-');
	else if(v==0x2026) push_str("...");
	else push_char('?');
}
static void decode_entity(void){               /* '&' already consumed */
	char e[12]; int n=0, c=-1;
	while(n<11){
		c = next_byte();
		if(c < 0) break;
		if(c==';') break;
		if(!((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='#')) break;
		e[n++] = (char)c;
	}
	e[n] = 0;
	if(c==';'){
		if(e[0]=='#'){ long v = (e[1]=='x'||e[1]=='X') ? strtol(e+2,NULL,16) : strtol(e+1,NULL,10);
		               decode_codepoint(v); }
		else decode_named(e);
		return;
	}
	/* not a real entity — emit literally, then re-handle the stop char */
	push_char('&'); for(int i=0;i<n;i++) push_char((unsigned char)e[i]);
	if(c=='<') read_tag();
	else if(c=='&') decode_entity();
	else if(c>=0) push_char(c);
}

/* ===== attribute parsing ===== */
static int ci_pfx(const char *s, const char *p){
	while(*p){ char a=*s,b=*p; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; if(a!=b) return 0; s++; p++; }
	return 1;
}
static int get_attr(const char *tag, const char *name, char *out, int cap){
	int nl=(int)strlen(name); const char *p=tag;
	while(*p){
		if((p==tag || p[-1]==' '||p[-1]=='\t'||p[-1]=='\n') && ci_pfx(p,name)){
			const char *q=p+nl; while(*q==' ') q++;
			if(*q=='='){ q++; while(*q==' ') q++;
				char qt=0; if(*q=='"'||*q=='\''){ qt=*q; q++; }
				int n=0;
				while(*q && ((qt && *q!=qt) || (!qt && *q!=' '&&*q!='\t'&&*q!='\n'&&*q!='>'))){
					if(n<cap-1) out[n++]=*q; q++;
				}
				out[n]=0; return 1;
			}
		}
		p++;
	}
	out[0]=0; return 0;
}

/* ===== raw skip for <script>/<style> ===== */
static void skip_until_close(const char *name){
	int c;
	for(;;){
		c = next_byte(); if(c<0) return;
		if(c!='<') continue;
		c = next_byte(); if(c<0) return;
		if(c!='/') continue;
		int i=0, ok=1;
		while(name[i]){ c=next_byte(); if(c<0) return; char ch=(char)c; if(ch>='A'&&ch<='Z')ch+=32; if(ch!=name[i]){ ok=0; break; } i++; }
		if(ok){ while((c=next_byte())>=0 && c!='>'); return; }
	}
}

/* ===== link helpers ===== */
static void close_link(void){
	while(linkn>0 && linkbuf[linkn-1]==' ') linkn--;
	if(linkn>0 && curhref[0]) emit(KF_OP_LINK, 0, 0, linkbuf, linkn, curhref, -1);
	in_link=0; linkn=0; pending_space=0;
}

/* ===== tag dispatch ===== */
#define NM(s) (!strcmp(name,(s)))
static void handle_tag(const char *tag){
	int closing = tag[0]=='/';
	const char *np = closing ? tag+1 : tag;
	char name[16]; int n=0;
	while(np[n] && np[n]!=' '&&np[n]!='\t'&&np[n]!='\n'&&np[n]!='/'&&np[n]!='>' && n<15){
		char ch=np[n]; if(ch>='A'&&ch<='Z')ch+=32; name[n]=ch; n++;
	}
	name[n]=0;
	if(!name[0]) return;

	if(NM("script")||NM("style")){ if(!closing) skip_until_close(name); return; }
	if(NM("head")){ in_head = !closing; return; }
	if(NM("body")){ in_head = 0; return; }
	if(NM("title")){ if(!closing){ in_title=1; title_n=0; pending_space=0; } else { in_title=0; while(title_n>0&&s_title[title_n-1]==' ')title_n--; s_title[title_n]=0; } return; }

	if(name[0]=='h' && name[1]>='1'&&name[1]<='6'&&name[2]==0){
		flush_block();
		cur_kind = closing ? KF_OP_P : (KF_OP_H1 + (name[1]-'1'));
		return;
	}
	if(NM("a")){
		if(!closing){ flush_block(); get_attr(tag,"href",curhref,sizeof curhref); in_link=1; linkn=0; pending_space=0; }
		else close_link();
		return;
	}
	if(NM("img")){
		if(closing) return;
		char alt[256], src[512];
		if(!get_attr(tag,"src",src,sizeof src)) src[0]=0;
		if(!get_attr(tag,"alt",alt,sizeof alt) || !alt[0]){
			snprintf(alt,sizeof alt,"%s",src);          /* no alt -> show the filename */
			char *sl=strrchr(alt,'/'); if(sl) memmove(alt,sl+1,strlen(sl+1)+1);
		}
		/* href carries the src URL so the browser can fetch + decode the image. */
		flush_block(); emit(KF_OP_IMG,0,0, alt[0]?alt:"image", -1, src[0]?src:NULL, -1);
		return;
	}
	if(NM("br")){ flush_block(); return; }
	if(NM("hr")){ flush_block(); emit(KF_OP_HR,0,0,NULL,0,NULL,0); return; }
	if(NM("ul")||NM("ol")){
		flush_block();
		if(!closing){ if(l_sp<8){ l_ordered[l_sp]=NM("ol"); l_count[l_sp]=0; l_sp++; } }
		else { if(l_sp>0) l_sp--; }
		cur_kind = KF_OP_P;
		return;
	}
	if(NM("li")){
		flush_block();
		if(!closing){
			cur_kind = KF_OP_LI;
			li_depth = (uint8_t)(l_sp>0 ? l_sp : 1);
			int top = l_sp-1;
			li_index = (top>=0 && l_ordered[top]) ? (uint16_t)(++l_count[top]) : 0;
		} else cur_kind = KF_OP_P;
		return;
	}
	if(NM("pre")){
		flush_block();
		if(!closing){ in_pre=1; cur_kind=KF_OP_PRE; } else { in_pre=0; cur_kind=KF_OP_P; }
		return;
	}
	if(NM("blockquote")){
		flush_block();
		if(!closing){ quote_depth++; cur_kind=KF_OP_QUOTE; }
		else { if(quote_depth>0) quote_depth--; cur_kind = quote_depth?KF_OP_QUOTE:KF_OP_P; }
		return;
	}
	if(NM("form")){
		flush_block();
		if(!closing){ in_form=1; if(!get_attr(tag,"action",form_action,sizeof form_action)) form_action[0]=0; }
		else in_form=0;
		return;
	}
	if(NM("input")){
		if(closing) return;
		char ty[24], val[256], nm[128];
		if(!get_attr(tag,"type",ty,sizeof ty)) snprintf(ty,sizeof ty,"text");
		for(char*q=ty;*q;q++) if(*q>='A'&&*q<='Z') *q+=32;
		get_attr(tag,"value",val,sizeof val);
		get_attr(tag,"name",nm,sizeof nm);
		flush_block();
		if(!strcmp(ty,"hidden")) return;
		if(!strcmp(ty,"submit")||!strcmp(ty,"button")||!strcmp(ty,"image"))
			emit(KF_OP_SUBMIT,0,0, val[0]?val:"Submit", -1, form_action, -1);
		else
			emit(KF_OP_FIELD,0,0, val, -1, nm, -1);
		return;
	}
	if(NM("button")){ if(!closing){ flush_block(); emit(KF_OP_SUBMIT,0,0,"Submit",-1,form_action,-1); } return; }

	/* generic block-level containers -> just break the line */
	if(NM("p")||NM("div")||NM("section")||NM("article")||NM("header")||NM("footer")||
	   NM("main")||NM("nav")||NM("table")||NM("tr")||NM("td")||NM("th")||NM("figure")||
	   NM("figcaption")||NM("aside")||NM("address")||NM("dl")||NM("dt")||NM("dd")||
	   NM("caption")||NM("fieldset")||NM("ul")||NM("ol")){
		flush_block();
		if(cur_kind!=KF_OP_PRE && !in_link) cur_kind = quote_depth?KF_OP_QUOTE:KF_OP_P;
		return;
	}
	/* everything else (span/em/strong/b/i/code/small/label/...) is inline: ignore. */
}

/* read a whole tag (already consumed '<'); dispatch. Handles comments/doctype. */
static void read_tag(void){
	int c = next_byte();
	if(c < 0) return;
	if(c=='!'){
		int d1=next_byte(), d2=next_byte();
		if(d1=='-' && d2=='-'){                       /* <!-- comment --> */
			int a=0,b=0,e;
			while((e=next_byte())>=0){ if(a=='-'&&b=='-'&&e=='>') return; a=b; b=e; }
			return;
		}
		if(d1=='>'||d2=='>'||d1<0||d2<0) return;      /* short <!...> */
		while((c=next_byte())>=0 && c!='>');          /* doctype etc. */
		return;
	}
	if(c=='?'){ while((c=next_byte())>=0 && c!='>'); return; }   /* <?...?> */
	char t[1024]; int tn=0;
	t[tn++]=(char)c;
	while((c=next_byte())>=0 && c!='>'){ if(tn<(int)sizeof t-1) t[tn++]=(char)c; }
	t[tn]=0;
	handle_tag(t);
}

uint32_t kf_html_parse(uint32_t body_base, uint32_t len){
	/* reset everything */
	r_base=body_base; r_len=len; r_pos=0; r_bufpos=0; r_buflen=0;
	op_count=0; text_brk=0;
	tbn=0; linkn=0; curhref[0]=0; title_n=0; s_title[0]=0;
	in_title=in_head=in_pre=in_link=pending_space=0;
	cur_kind=KF_OP_P; quote_depth=0; li_index=0; li_depth=0; l_sp=0;
	in_form=0; form_action[0]=0;

	int c;
	while((c = next_byte()) >= 0){
		if(c=='<') read_tag();
		else if(c=='&') decode_entity();
		else push_char(c);
	}
	if(in_link) close_link();
	flush_block();
	return op_count;
}

const char *kf_html_title(void){ return s_title; }
