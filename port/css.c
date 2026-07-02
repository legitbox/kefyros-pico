// port/css.c — streaming CSS parser + PSRAM rule store + computed styles (see css.h).
//
// Deliberately lenient and bounded: selectors with combinators/pseudo-classes we
// don't support (>, +, ~, :, [) are skipped selector-by-selector; @-rules (@media,
// @font-face, ...) are skipped whole-block (correct-ish for a fixed 320px always-
// light-mode device); unknown properties are dropped at parse time. All heavy state
// lives in PSRAM; SRAM cost is ~1 KB of static parse buffers plus a 4 KB rule index
// that exists only between kf_css_reset() and kf_css_shutdown().
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "../kefyros.h"
#include "css.h"

#define MAX_RULES 512
#define MAX_DECLS 8
#define MAX_SELS  3
#define MAX_VARS  96          /* CSS custom properties (--x), global last-wins */
#define SCREEN_W  320         /* @media evaluation viewport */

typedef struct { uint32_t id_hash, cls_hash; uint16_t tag, pad; } c_sel;   /* 12 B */
typedef struct { uint8_t prop, v8; uint16_t v16; } c_decl;                 /*  4 B */
typedef struct {
	c_sel    sel[MAX_SELS];        /* sel[nsel-1] = rightmost (the subject) */
	uint16_t spec;
	uint8_t  nsel, ndecl;
	c_decl   d[MAX_DECLS];
} c_rule;                          /* 64 B */

enum { CP_COLOR=1, CP_BG, CP_DISPLAY, CP_ALIGN, CP_DECOR, CP_FSIZE, CP_INDENT, CP_LIST,
       CP_BORDER,   /* v8 = width px, v16 = color */
       CP_RADIUS, CP_MARGV, CP_LINEH /* v16 = signed extra line space */,
       CP_LETSP /* v16 = signed px */, CP_XFORM };

static uint32_t s_base, s_cap;     /* PSRAM rule store */
static uint32_t s_nrules;

/* SRAM index: one entry per rule, keyed on its rightmost compound (id > class > tag)
   so per-element matching is a cheap SRAM scan + a few 64-byte PSRAM reads. */
typedef struct { uint32_t key; uint16_t idx; uint8_t kind; uint8_t pad; } c_idx;
enum { K_TAG=0, K_CLASS, K_ID };   /* K_TAG with key==0 = universal '*' */
static c_idx  *s_idx;              /* malloc'd MAX_RULES entries; NULL = css disabled */

/* CSS custom properties: global, last-wins (no scoping — :root wins by coming first) */
typedef struct { uint32_t h; char v[44]; } c_var;
static c_var *s_vars;              /* malloc'd MAX_VARS alongside s_idx */
static int    s_nvars;

/* ===== hashing ===== */
uint32_t kf_css_hash(const char *s, int n){
	uint32_t h = 2166136261u;
	for(int i=0; (n<0) ? (s[i]!=0) : (i<n); i++){
		uint8_t c = (uint8_t)s[i];
		if(c>='A' && c<='Z') c += 32;
		h = (h ^ c) * 16777619u;
	}
	return h ? h : 1;
}
uint16_t kf_css_tag_hash(const char *name){
	uint32_t h = kf_css_hash(name, -1);
	uint16_t t = (uint16_t)(h ^ (h>>16));
	return t ? t : 1;
}

/* ===== lifecycle ===== */
void kf_css_set_arena(uint32_t base, uint32_t cap){ s_base=base; s_cap=cap; s_nrules=0; }
uint32_t kf_css_rule_count(void){ return s_nrules; }
void kf_css_reset(void){
	s_nrules = 0; s_nvars = 0;
	if(!s_idx)  s_idx  = malloc(sizeof(c_idx)*MAX_RULES);   /* NULL -> css quietly off */
	if(!s_vars) s_vars = malloc(sizeof(c_var)*MAX_VARS);    /* NULL -> vars quietly off */
}
void kf_css_shutdown(void){
	free(s_idx);  s_idx=NULL;  s_nrules=0;
	free(s_vars); s_vars=NULL; s_nvars=0;
}

/* ===== value parsing ===== */
static int lc(int c){ return (c>='A'&&c<='Z') ? c+32 : c; }
static uint16_t rgb565(int r,int g,int b){ return (uint16_t)(((r>>3)<<11)|((g>>2)<<5)|(b>>3)); }

static const struct { const char *n; uint32_t rgb; } NAMED[] = {
	{"black",0x000000},{"white",0xffffff},{"red",0xff0000},{"green",0x008000},
	{"blue",0x0000ff},{"yellow",0xffff00},{"orange",0xffa500},{"purple",0x800080},
	{"gray",0x808080},{"grey",0x808080},{"silver",0xc0c0c0},{"maroon",0x800000},
	{"navy",0x000080},{"teal",0x008080},{"aqua",0x00ffff},{"cyan",0x00ffff},
	{"fuchsia",0xff00ff},{"magenta",0xff00ff},{"lime",0x00ff00},{"olive",0x808000},
	{"brown",0xa52a2a},{"pink",0xffc0cb},{"gold",0xffd700},{"whitesmoke",0xf5f5f5},
	{"darkgray",0xa9a9a9},{"lightgray",0xd3d3d3},{"darkred",0x8b0000},
	{"darkblue",0x00008b},{"darkgreen",0x006400},{"lightblue",0xadd8e6},
};

static int hexv(int c){
	if(c>='0'&&c<='9') return c-'0';
	c = lc(c);
	if(c>='a'&&c<='f') return c-'a'+10;
	return -1;
}
/* parse one color token; 0 = not a color, 1 = *out set, 2 = transparent */
static int parse_color(const char *v, uint16_t *out){
	while(*v==' ') v++;
	if(*v=='#'){
		int h[6], n=0;
		while(n<6 && hexv(v[1+n])>=0){ h[n]=hexv(v[1+n]); n++; }
		if(n==3){ *out = rgb565(h[0]*17, h[1]*17, h[2]*17); return 1; }
		if(n>=6){ *out = rgb565(h[0]*16+h[1], h[2]*16+h[3], h[4]*16+h[5]); return 1; }
		return 0;
	}
	if(!strncmp(v,"rgb",3)){
		const char *p = strchr(v,'(');
		if(!p) return 0;
		int c[3];
		p++;
		for(int i=0;i<3;i++){
			while(*p==' '||*p==',') p++;
			char *e; long x = strtol(p,&e,10);
			if(e==p) return 0;
			if(*e=='%'){ x = x*255/100; e++; }
			c[i] = (int)(x<0?0:(x>255?255:x)); p = e;
		}
		/* rgba(...,0) -> transparent */
		while(*p==' '||*p==',') p++;
		if(v[3]=='a' && p[0]=='0' && (p[1]==')'||p[1]==' '||p[1]==0)) return 2;
		*out = rgb565(c[0],c[1],c[2]);
		return 1;
	}
	if(!strncmp(v,"transparent",11)) return 2;
	for(unsigned i=0;i<sizeof NAMED/sizeof NAMED[0];i++){
		int l = (int)strlen(NAMED[i].n);
		if(!strncmp(v,NAMED[i].n,l) && !((v[l]>='a'&&v[l]<='z'))){
			uint32_t c = NAMED[i].rgb;
			*out = rgb565((c>>16)&255,(c>>8)&255,c&255);
			return 1;
		}
	}
	return 0;
}
/* find the first color anywhere in a value (for the `background` shorthand) */
static int color_in_value(const char *v, uint16_t *out){
	while(*v){
		while(*v==' ') v++;
		int r = parse_color(v, out);
		if(r) return r;
		while(*v && *v!=' ') v++;
	}
	return 0;
}
/* length -> px (13px = 1em base). Returns <0 if not a length. */
static int parse_px(const char *v){
	char *e; double x = strtod(v,&e);
	if(e==v) return -1;
	if(!strncmp(e,"pt",2)) x = x*4.0/3.0;
	else if(!strncmp(e,"em",2) || !strncmp(e,"rem",3)) x = x*13.0;
	else if(*e=='%') x = x*13.0/100.0;
	if(x < 0) x = 0;
	if(x > 400) x = 400;
	return (int)x;
}

/* ===== CSS custom properties ===== */
static void var_set(const char *name, const char *val){
	if(!s_vars) return;
	uint32_t h = kf_css_hash(name, -1);
	for(int i=0;i<s_nvars;i++)
		if(s_vars[i].h==h){ snprintf(s_vars[i].v, sizeof s_vars[i].v, "%s", val); return; }
	if(s_nvars < MAX_VARS){
		s_vars[s_nvars].h = h;
		snprintf(s_vars[s_nvars].v, sizeof s_vars[s_nvars].v, "%s", val);
		s_nvars++;
	}
}
static const char *var_get(const char *name, int len){
	if(!s_vars) return NULL;
	uint32_t h = kf_css_hash(name, len);
	for(int i=0;i<s_nvars;i++) if(s_vars[i].h==h) return s_vars[i].v;
	return NULL;
}
/* expand var(--x[, fallback]) references in `in` -> `out` (one level per pass) */
static int var_expand(const char *in, char *out, int cap){
	int o=0, changed=0;
	while(*in && o<cap-1){
		if(!strncmp(in,"var(",4)){
			const char *p = in+4;
			while(*p==' ') p++;
			const char *nm = p;
			while(*p && *p!=','&&*p!=')') p++;
			int nl=(int)(p-nm);
			while(nl>0 && nm[nl-1]==' ') nl--;
			const char *fb=NULL; int fbl=0;
			if(*p==','){
				p++; while(*p==' ') p++;
				fb=p; int depth=0;
				while(*p && (depth||*p!=')')){ if(*p=='(')depth++; if(*p==')')depth--; p++; }
				fbl=(int)(p-fb);
			}
			if(*p==')') p++;
			const char *v = var_get(nm, nl);
			if(v){ while(*v && o<cap-1) out[o++]=*v++; }
			else if(fb){ for(int i=0;i<fbl&&o<cap-1;i++) out[o++]=fb[i]; }
			in = p; changed=1;
		} else out[o++]=*in++;
	}
	out[o]=0;
	return changed;
}

/* ===== declaration parsing (shared by rule bodies and style="" attributes) ===== */
static int parse_decls(const char *s, c_decl *out, int cap){
	int n = 0;
	while(*s && n<cap){
		while(*s==';'||*s==' '||*s=='\t'||*s=='\n'||*s=='\r') s++;
		if(!*s) break;
		char prop[28]; int pl=0;
		while(*s && *s!=':' && *s!=';'){ if(pl<27) prop[pl++]=(char)lc(*s); s++; }
		while(pl>0 && prop[pl-1]==' ') pl--;
		prop[pl]=0;
		if(*s!=':') continue;
		s++;
		char val[160]; int vl=0;
		while(*s==' ') s++;
		while(*s && *s!=';'){ if(vl<159) val[vl++]=(char)lc(*s); s++; }
		while(vl>0 && val[vl-1]==' ') vl--;
		val[vl]=0;
		char *im = strstr(val,"!important");
		if(im){ *im=0; vl=(int)strlen(val); while(vl>0&&val[vl-1]==' ') val[--vl]=0; }

		if(prop[0]=='-' && prop[1]=='-'){ var_set(prop, val); continue; }
		if(strstr(val,"var(")){                       /* expand custom properties (2 levels) */
			char ex[160];
			if(var_expand(val, ex, sizeof ex)){
				if(strstr(ex,"var(")) var_expand(ex, val, sizeof val);
				else snprintf(val, sizeof val, "%s", ex);
			}
		}

		uint16_t col; int cr;
		c_decl *d = &out[n];
		d->prop=0; d->v8=0; d->v16=0;
		if(!strcmp(prop,"color")){
			if(parse_color(val,&col)==1){ d->prop=CP_COLOR; d->v16=col; }
		} else if(!strcmp(prop,"background")||!strcmp(prop,"background-color")){
			cr = color_in_value(val,&col);
			if(cr==1){ d->prop=CP_BG; d->v8=1; d->v16=col; }
			else if(cr==2){ d->prop=CP_BG; d->v8=0; }
		} else if(!strcmp(prop,"display")){
			d->prop=CP_DISPLAY; d->v8 = !strcmp(val,"none");
		} else if(!strcmp(prop,"visibility")){
			d->prop=CP_DISPLAY; d->v8 = !strcmp(val,"hidden");
		} else if(!strcmp(prop,"text-align")){
			d->prop=CP_ALIGN;
			d->v8 = !strncmp(val,"center",6) ? 1 : (!strncmp(val,"right",5) ? 2 : 0);
		} else if(!strcmp(prop,"text-decoration")||!strcmp(prop,"text-decoration-line")){
			d->prop=CP_DECOR;
			if(strstr(val,"underline"))    d->v8 |= 1;
			if(strstr(val,"line-through")) d->v8 |= 2;
		} else if(!strcmp(prop,"font-size")){
			int px = parse_px(val);
			if(px<0){
				if(strstr(val,"xx-large")) px=22;
				else if(strstr(val,"x-large")) px=19;
				else if(strstr(val,"large"))   px=17;   /* also "larger" */
				else px=13;
			}
			d->prop=CP_FSIZE; d->v16=(uint16_t)px;
		} else if(!strcmp(prop,"margin-left")||!strcmp(prop,"padding-left")){
			int px = parse_px(val);
			if(px>=0){ d->prop=CP_INDENT; d->v16=(uint16_t)(px>200?200:px); }
		} else if(!strcmp(prop,"list-style")||!strcmp(prop,"list-style-type")){
			d->prop=CP_LIST; d->v8 = strstr(val,"none")!=NULL;
		} else if(!strcmp(prop,"border")||!strcmp(prop,"border-top")||!strcmp(prop,"border-bottom")||
		          !strcmp(prop,"border-left")||!strcmp(prop,"border-right")){
			if(strstr(val,"none")||!strcmp(val,"0")){ d->prop=CP_BORDER; d->v8=0; }
			else {
				int w = parse_px(val); if(w<1) w=1; if(w>6) w=6;
				uint16_t bc; int has = (color_in_value(val,&bc)==1);
				d->prop=CP_BORDER; d->v8=(uint8_t)w; d->v16 = has ? bc : 0x8410;  /* gray */
			}
		} else if(!strcmp(prop,"border-radius")){
			int r = parse_px(val); if(r<0) r=0; if(r>24) r=24;
			d->prop=CP_RADIUS; d->v16=(uint16_t)r;
		} else if(!strcmp(prop,"margin-top")||!strcmp(prop,"margin-bottom")||
		          !strcmp(prop,"padding-top")||!strcmp(prop,"padding-bottom")||
		          !strcmp(prop,"margin")||!strcmp(prop,"padding")){
			int m = parse_px(val);                    /* shorthand: first value = vertical */
			if(m>=0){ d->prop=CP_MARGV; d->v16=(uint16_t)(m>24?24:m); }
		} else if(!strcmp(prop,"line-height")){
			char *e; double x = strtod(val,&e);
			if(e!=val){
				int px;
				if(*e=='p')      px=(int)x;           /* px */
				else if(*e=='%') px=(int)(x*13.0/100.0);
				else             px=(int)(x*13.0);    /* em/rem/unitless multiplier */
				int extra = px-13; if(extra<-3)extra=-3; if(extra>14)extra=14;
				d->prop=CP_LINEH; d->v16=(uint16_t)(int16_t)extra;
			}
		} else if(!strcmp(prop,"letter-spacing")){
			char *e; double x = strtod(val,&e);       /* may be negative */
			if(e!=val){
				int v=(int)x; if(v<-2)v=-2; if(v>6)v=6;
				d->prop=CP_LETSP; d->v16=(uint16_t)(int16_t)v;
			}
		} else if(!strcmp(prop,"text-transform")){
			d->prop=CP_XFORM;
			d->v8 = strstr(val,"upper") ? 1 : (strstr(val,"lower") ? 2 : 0);
		}
		if(d->prop) n++;
	}
	return n;
}

/* ===== selector parsing ===== */
static int is_ident(int c){
	return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||(uint8_t)c>=0x80;
}
/* parse one compound ("tag.cls#id" / "*" / ".cls" ...). 1 ok, 0 unsupported. */
static int parse_compound(const char *s, int len, c_sel *o, int *spec){
	memset(o, 0, sizeof *o);
	int i = 0;
	if(i<len && s[i]=='*') i++;
	else if(i<len && is_ident(s[i]) && s[i]!='-'){
		char t[16]; int tl=0;
		while(i<len && is_ident(s[i])){ if(tl<15) t[tl++]=(char)lc(s[i]); i++; }
		t[tl]=0;
		o->tag = kf_css_tag_hash(t);
		*spec += 1;
	}
	while(i<len){
		int kind = s[i];
		if(kind!='.' && kind!='#') return 0;
		i++;
		int st=i;
		while(i<len && is_ident(s[i])) i++;
		if(i==st) return 0;
		uint32_t h = kf_css_hash(s+st, i-st);
		if(kind=='#'){ o->id_hash=h; *spec+=100; }
		else { if(!o->cls_hash) o->cls_hash=h; *spec+=10; }   /* .a.b: match first, count both */
	}
	return 1;
}
/* parse one full selector (descendant chain); 1 ok. */
static int parse_selector(const char *s, int len, c_sel *sels, int *nsel, uint16_t *spec){
	for(int i=0;i<len;i++){
		int c=s[i];
		if(c==':'||c=='['||c=='>'||c=='+'||c=='~'||c=='('||c==')') return 0;
	}
	int sp = 0;
	c_sel tmp[8]; int nt=0;
	int i=0;
	while(i<len){
		while(i<len && (s[i]==' '||s[i]=='\t'||s[i]=='\n'||s[i]=='\r')) i++;
		int st=i;
		while(i<len && s[i]!=' '&&s[i]!='\t'&&s[i]!='\n'&&s[i]!='\r') i++;
		if(i==st) break;
		if(nt<8){ if(!parse_compound(s+st, i-st, &tmp[nt], &sp)) return 0; nt++; }
	}
	if(!nt) return 0;
	/* keep the rightmost MAX_SELS compounds (deep chains degrade gracefully) */
	int keep = nt>MAX_SELS ? MAX_SELS : nt;
	for(int j=0;j<keep;j++) sels[j] = tmp[nt-keep+j];
	*nsel = keep;
	*spec = (uint16_t)(sp>0xffff?0xffff:sp);
	return 1;
}

static void emit_rule(const c_sel *sels, int nsel, uint16_t spec, const c_decl *ds, int nds){
	if(!s_idx || s_nrules>=MAX_RULES) return;
	if((s_nrules+1)*sizeof(c_rule) > s_cap) return;
	c_rule r; memset(&r,0,sizeof r);
	memcpy(r.sel, sels, sizeof(c_sel)*nsel);
	r.nsel=(uint8_t)nsel; r.spec=spec; r.ndecl=(uint8_t)nds;
	memcpy(r.d, ds, sizeof(c_decl)*nds);
	kf_psram_write(s_base + s_nrules*sizeof(c_rule), &r, sizeof r);
	const c_sel *rm = &sels[nsel-1];
	c_idx *ix = &s_idx[s_nrules];
	if(rm->id_hash){ ix->kind=K_ID; ix->key=rm->id_hash; }
	else if(rm->cls_hash){ ix->kind=K_CLASS; ix->key=rm->cls_hash; }
	else { ix->kind=K_TAG; ix->key=rm->tag; }   /* tag 0 = universal */
	ix->idx=(uint16_t)s_nrules;
	s_nrules++;
}

/* ===== @media evaluation (viewport = 320x320, light scheme, no pointer) ===== */
static int media_feature_true(const char *f, int len){
	char b[64]; int n=0;
	for(int i=0;i<len && n<63;i++) b[n++]=(char)lc(f[i]);
	b[n]=0;
	char *cl = strchr(b, ':');
	if(!cl) return 0;                        /* boolean feature test: not us */
	*cl=0;
	const char *v = cl+1; while(*v==' ') v++;
	char *name=b; while(*name==' ') name++;
	{ int e=(int)strlen(name); while(e>0&&name[e-1]==' ') name[--e]=0; }
	if(strstr(name,"width")||strstr(name,"height")){
		char *end; double x = strtod(v,&end);
		if(end==v) return 0;
		int px = (int)((!strncmp(end,"em",2)||!strncmp(end,"rem",3)) ? x*16.0 : x);
		if(!strncmp(name,"min",3)) return SCREEN_W >= px;
		if(!strncmp(name,"max",3)) return SCREEN_W <= px;
		return SCREEN_W == px;
	}
	if(strstr(name,"prefers-color-scheme")) return strstr(v,"light")!=NULL;
	if(strstr(name,"prefers-reduced"))      return 1;      /* reduced motion/data: yes */
	if(!strcmp(name,"orientation"))         return 1;      /* square screen: either */
	if(strstr(name,"hover"))                return strstr(v,"none")!=NULL;
	if(strstr(name,"pointer"))              return strstr(v,"coarse")!=NULL;
	return 0;                                /* unknown feature: clause fails */
}
static int media_clause_true(const char *q, int len){
	int i=0;
	while(i<len){
		while(i<len && (q[i]==' '||q[i]=='\t'||q[i]=='\n'||q[i]=='\r')) i++;
		if(i>=len) break;
		if(q[i]=='('){
			int st=++i, depth=1;
			while(i<len && depth){ if(q[i]=='(')depth++; else if(q[i]==')')depth--; i++; }
			if(!media_feature_true(q+st, i-st-1)) return 0;
		} else {
			int st=i;
			while(i<len && q[i]!=' '&&q[i]!='\t'&&q[i]!='(') i++;
			int wl=i-st;
			if(wl==3 && !strncmp(q+st,"not",3)) return 0;   /* conservative */
			if((wl==5 && !strncmp(q+st,"print",5)) ||
			   (wl==6 && !strncmp(q+st,"speech",6))) return 0;
			/* "and", "only", "screen", "all": accept / ignore */
		}
	}
	return 1;
}
static int media_true(const char *q){                     /* q = text after '@' */
	while(*q==' ') q++;
	if(strncmp(q,"media",5)) return 0;                     /* other @-blocks: skip */
	q += 5;
	for(;;){                                               /* comma list = OR */
		const char *end = strchr(q, ',');
		int ql = end ? (int)(end-q) : (int)strlen(q);
		if(media_clause_true(q, ql)) return 1;
		if(!end) return 0;
		q = end+1;
	}
}

/* ===== stylesheet tokenizer (streaming) ===== */
enum { TS_SEL, TS_DECL, TS_ATHEAD, TS_ATBLK };
static int  ts_state, ts_atdepth, ts_cmt, ts_star, ts_slash, ts_selovf;
static int  ts_mdepth;             /* inside N accepted @media blocks */
static char selb[256]; static int seln;
static char dclb[512]; static int dcln;
static char atb[160];  static int atbn;

void kf_css_sheet_begin(void){
	ts_state=TS_SEL; ts_atdepth=0; ts_cmt=0; ts_star=0; ts_slash=0; ts_selovf=0;
	ts_mdepth=0; seln=0; dcln=0; atbn=0;
}
static int sel_blank(void){
	for(int i=0;i<seln;i++)
		if(selb[i]!=' '&&selb[i]!='\t'&&selb[i]!='\n'&&selb[i]!='\r') return 0;
	return 1;
}
static void finish_rule(void){
	selb[seln]=0; dclb[dcln]=0;
	if(ts_selovf) return;
	static c_decl ds[MAX_DECLS];
	int nds = parse_decls(dclb, ds, MAX_DECLS);
	if(!nds) return;
	/* comma-separated selector list -> one rule each */
	int i=0;
	while(i<seln){
		int st=i;
		while(i<seln && selb[i]!=',') i++;
		c_sel sels[MAX_SELS]; int nsel; uint16_t spec;
		if(parse_selector(selb+st, i-st, sels, &nsel, &spec))
			emit_rule(sels, nsel, spec, ds, nds);
		i++;
	}
}
static void feed2(int c){
	switch(ts_state){
	case TS_SEL:
		if(c=='{'){ ts_state=TS_DECL; dcln=0; }
		else if(c=='@' && sel_blank()){ ts_state=TS_ATHEAD; atbn=0; }
		else if(c=='}'){ if(ts_mdepth>0) ts_mdepth--; seln=0; ts_selovf=0; }
		else { if(seln<(int)sizeof selb-1) selb[seln++]=(char)c; else ts_selovf=1; }
		break;
	case TS_ATHEAD:
		if(c==';'){ ts_state=TS_SEL; seln=0; ts_selovf=0; }
		else if(c=='{'){
			atb[atbn]=0;
			if(media_true(atb)){ ts_mdepth++; ts_state=TS_SEL; seln=0; }  /* parse inside */
			else { ts_atdepth=1; ts_state=TS_ATBLK; }                     /* skip block */
		}
		else { if(atbn<(int)sizeof atb-1) atb[atbn++]=(char)lc(c); }
		break;
	case TS_ATBLK:
		if(c=='{') ts_atdepth++;
		else if(c=='}' && --ts_atdepth==0){ ts_state=TS_SEL; seln=0; ts_selovf=0; }
		break;
	case TS_DECL:
		if(c=='}'){ finish_rule(); seln=0; dcln=0; ts_selovf=0; ts_state=TS_SEL; }
		else { if(dcln<(int)sizeof dclb-1) dclb[dcln++]=(char)c; }
		break;
	}
}
void kf_css_sheet_feed(int c){
	if(ts_cmt){ if(ts_star && c=='/') ts_cmt=0; ts_star=(c=='*'); return; }
	if(ts_slash){
		ts_slash=0;
		if(c=='*'){ ts_cmt=1; ts_star=0; return; }
		feed2('/');
	}
	if(c=='/'){ ts_slash=1; return; }
	feed2(c);
}
void kf_css_sheet_end(void){
	if(ts_slash){ feed2('/'); ts_slash=0; }
}

/* ===== matching + computed style ===== */
static int sel_match(const c_sel *s, const kf_css_elem *e){
	if(s->tag && s->tag != e->tag) return 0;
	if(s->id_hash && s->id_hash != e->id_hash) return 0;
	if(s->cls_hash){
		int ok=0;
		for(int i=0;i<e->ncls;i++) if(e->cls[i]==s->cls_hash){ ok=1; break; }
		if(!ok) return 0;
	}
	return 1;
}
static c_rule r_scratch;           /* static: keep 64 B off the 4 KB core0 stack */
static int rule_match(const c_rule *r, const kf_css_elem *stk, int depth){
	if(!sel_match(&r->sel[r->nsel-1], &stk[depth-1])) return 0;
	int k = depth-2;
	for(int j=r->nsel-2; j>=0; j--){
		int found=0;
		while(k>=0){
			if(sel_match(&r->sel[j], &stk[k])){ found=1; k--; break; }
			k--;
		}
		if(!found) return 0;
	}
	return 1;
}
static void apply_decl(kf_css_style *st, const c_decl *d){
	switch(d->prop){
	case CP_COLOR: st->fg=d->v16; st->flags|=KF_CSS_F_FG; break;
	case CP_BG:
		if(d->v8){ st->bg=d->v16; st->flags|=KF_CSS_F_BG; }
		else st->flags &= (uint16_t)~KF_CSS_F_BG;
		break;
	case CP_DISPLAY:
		if(d->v8) st->flags|=KF_CSS_F_HIDE; else st->flags&=(uint16_t)~KF_CSS_F_HIDE;
		break;
	case CP_ALIGN:
		st->flags &= (uint16_t)~(KF_CSS_F_CENTER|KF_CSS_F_RIGHT);
		if(d->v8==1) st->flags|=KF_CSS_F_CENTER;
		else if(d->v8==2) st->flags|=KF_CSS_F_RIGHT;
		break;
	case CP_DECOR:
		st->flags &= (uint16_t)~(KF_CSS_F_UNDER|KF_CSS_F_STRIKE);
		if(d->v8 & 1) st->flags|=KF_CSS_F_UNDER;
		if(d->v8 & 2) st->flags|=KF_CSS_F_STRIKE;
		break;
	case CP_FSIZE:
		/* >=20px: modern sites set 17-19px BODY text; only genuinely-large text
		   (headings) should get the 20px font */
		if(d->v16>=20) st->flags|=KF_CSS_F_BIG; else st->flags&=(uint16_t)~KF_CSS_F_BIG;
		break;
	case CP_INDENT:
		if(d->v16 > st->indent) st->indent = (uint8_t)(d->v16>200?200:d->v16);
		break;
	case CP_LIST:
		if(d->v8) st->flags|=KF_CSS_F_NOBULLET; else st->flags&=(uint16_t)~KF_CSS_F_NOBULLET;
		break;
	case CP_BORDER: st->border_w=d->v8; st->border_c=d->v16; break;
	case CP_RADIUS: st->radius=(uint8_t)d->v16; break;
	case CP_MARGV:  if((uint8_t)d->v16 > st->pad_v) st->pad_v=(uint8_t)d->v16; break;
	case CP_LINEH:  st->line_sp=(int8_t)(int16_t)d->v16; break;
	case CP_LETSP:  st->let_sp=(int8_t)(int16_t)d->v16; break;
	case CP_XFORM:  st->xform=d->v8; break;
	}
}

/* bg inherits too (unlike real CSS): styling is per-BLOCK, and pages hang their
   backgrounds on <div>/<body> containers whose text lives in child <p>s — without
   propagation a container background would never be visible at all. */
#define INHERIT_MASK (KF_CSS_F_FG|KF_CSS_F_BG|KF_CSS_F_CENTER|KF_CSS_F_RIGHT| \
                      KF_CSS_F_BIG|KF_CSS_F_NOBULLET|KF_CSS_F_HIDE)

void kf_css_apply(const kf_css_elem *stk, int depth, const char *inl,
                  const kf_css_style *parent, kf_css_style *out){
	memset(out, 0, sizeof *out);
	if(parent){
		out->flags   = parent->flags & INHERIT_MASK;
		out->fg      = parent->fg;
		out->bg      = parent->bg;
		out->line_sp = parent->line_sp;    /* typography inherits (per CSS) */
		out->let_sp  = parent->let_sp;
		out->xform   = parent->xform;
	}
	if(depth<=0) return;
	const kf_css_elem *E = &stk[depth-1];

	if(s_idx && s_nrules){
		/* gather matches, sorted by (specificity, rule order) */
		struct { uint16_t spec, idx; } m[16]; int nm=0;
		for(uint32_t i=0;i<s_nrules;i++){
			const c_idx *ix=&s_idx[i]; int hit=0;
			if(ix->kind==K_TAG) hit = (ix->key==0 || ix->key==E->tag);
			else if(ix->kind==K_CLASS){
				for(int j=0;j<E->ncls;j++) if(E->cls[j]==ix->key){ hit=1; break; }
			} else hit = (ix->key==E->id_hash);
			if(!hit) continue;
			kf_psram_read(s_base + ix->idx*sizeof(c_rule), &r_scratch, sizeof r_scratch);
			if(!rule_match(&r_scratch, stk, depth)) continue;
			if(nm < 16){
				int j=nm++;                     /* insertion sort, stable in rule order */
				while(j>0 && m[j-1].spec > r_scratch.spec){ m[j]=m[j-1]; j--; }
				m[j].spec=r_scratch.spec; m[j].idx=ix->idx;
			}
		}
		for(int i=0;i<nm;i++){
			kf_psram_read(s_base + m[i].idx*sizeof(c_rule), &r_scratch, sizeof r_scratch);
			for(int j=0;j<r_scratch.ndecl;j++) apply_decl(out, &r_scratch.d[j]);
		}
	}
	if(inl && inl[0]){                          /* style="" beats everything */
		static c_decl ds[MAX_DECLS];
		int nds = parse_decls(inl, ds, MAX_DECLS);
		for(int i=0;i<nds;i++) apply_decl(out, &ds[i]);
	}
}
