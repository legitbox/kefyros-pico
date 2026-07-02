// port/http.c — async HTTP(S) GET client for the Spineko browser.
//
// Raw lwIP TCP, one request at a time. The response body is streamed straight into a
// caller-provided PSRAM region; headers are parsed in SRAM; chunked transfer-encoding
// is de-chunked on the fly; 301/302/303/307/308 redirects are followed (bounded).
//
// HTTPS: when the scheme is https, a BearSSL TLS 1.2 client engine (port/tls.c) sits
// between the TCP stream and the byte parser below. Received TCP bytes are fed into the
// engine (recvrec); decrypted plaintext (recvapp) runs through the same feed() parser
// as plain HTTP; the engine's outbound records (sendrec) and the request itself
// (sendapp) are written to the socket. The handshake is driven entirely from the lwIP
// callbacks. No cert verification (see tls.c). Plain http is unchanged — no TLS overhead.
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"

#include "../kefyros.h"
#include "http.h"
#include "tls.h"
#include "miniz_tinfl.h"           /* streaming DEFLATE (transparent gzip bodies) */

#define HTTP_MAX_REDIRECTS 6
#define HTTP_TIMEOUT_MS    25000u
#define HTTP_STALL_MS      10000u

/* ---- request target ---- */
static char     s_host[128];
static char     s_path[512];
static uint16_t s_port;
static char     s_final_url[512];          /* absolute URL of the current request */
static int      s_redirects;

/* ---- connection ---- */
static struct tcp_pcb *s_pcb;
static int       s_state = KF_HTTP_IDLE;
static char      s_err[64];
static uint32_t  s_t0, s_tlast;             /* start / last-activity ms */
static char      s_req[768];
static int       s_req_len;

/* ---- TLS (https) ---- */
static int       s_secure;                  /* 1 = drive the request through BearSSL */
static int       s_req_off;                 /* bytes of the logical request already sent */
static int       s_req_sent;                /* 1 once the whole request is flushed */

/* ---- request shape (GET vs POST) ----
   The logical request is two contiguous segments: the header block (s_req[0..s_req_len),
   built by build_req) followed by an optional POST body (s_sbody[0..s_sbody_len),
   caller-owned). s_req_off is the offset into that combined stream; req_at() maps it
   back to whichever segment is live. This lets bodies far exceed the small s_req buffer. */
static int         s_post;                  /* 0 = GET, 1 = POST */
static const char *s_xhdr;                  /* extra header lines (CRLF-terminated) or NULL */
static const char *s_sbody;                 /* POST body (caller-owned) or NULL */
static int         s_sbody_len;

/* ---- response parse ---- */
static int       s_status;
static int       s_chunked;
static int       s_first_line;
static char      s_location[512];
static int       s_do_redirect;
static char      s_redirect_url[512];

static char      s_line[1024];
static int       s_line_n;

enum { PH_HEADLINE, PH_BODY_RAW, PH_CSIZE, PH_CDATA, PH_CAFTER, PH_TRAILER, PH_DONE };
static int       s_phase;
static uint32_t  s_chunk_rem;

/* PSRAM body arena + SRAM staging */
static uint32_t  s_arena_base, s_arena_cap;
static uint32_t  s_body_base, s_body_len;
static uint8_t   s_stage[1024];
static uint32_t  s_stage_n;

static uint32_t now_ms(void){ return (uint32_t)(time_us_64() / 1000u); }

void kf_http_set_arena(uint32_t base, uint32_t cap){ s_arena_base = base; s_arena_cap = cap; }
int         kf_http_state(void){ return s_state; }
int         kf_http_status(void){ return s_status; }
uint32_t    kf_http_body_base(void){ return s_body_base; }
uint32_t    kf_http_body_len(void){ return s_body_len; }
const char *kf_http_final_url(void){ return s_final_url; }
const char *kf_http_err(void){ return s_err; }

static void gz_free(void);
static void fail(const char *why){
	gz_free();
	snprintf(s_err, sizeof s_err, "%s", why);
	s_state = KF_HTTP_ERROR;
	s_phase = PH_DONE;
}

/* ===== URL parsing / resolution ===== */
static int parse_url(const char *url, char *scheme, int slen, char *host, int hlen,
                     uint16_t *port, char *path, int plen){
	const char *p = url;
	const char *c = strstr(url, "://");
	if(!c) return 0;
	int n = (int)(c - p); if(n >= slen) n = slen-1;
	memcpy(scheme, p, n); scheme[n] = 0;
	p = c + 3;
	*port = !strcmp(scheme, "https") ? 443 : 80;
	int hi = 0;
	while(*p && *p!='/' && *p!='?' && *p!='#'){
		if(*p == ':'){
			p++; int pv = 0;
			while(*p>='0'&&*p<='9'){ pv = pv*10 + (*p-'0'); p++; }
			if(pv>0 && pv<65536) *port = (uint16_t)pv;
			break;
		}
		if(hi < hlen-1) host[hi++] = *p;
		p++;
	}
	host[hi] = 0;
	while(*p && *p!='/' && *p!='?' && *p!='#') p++;
	int pi = 0;
	if(*p != '/'){ if(pi<plen-1) path[pi++]='/'; }
	while(*p && *p!='#'){ if(pi<plen-1) path[pi++]=*p; p++; }
	path[pi] = 0;
	if(!host[0]) return 0;
	return 1;
}

int kf_url_resolve(const char *base, const char *ref, char *out, int outsz){
	if(!ref || !ref[0]) return 0;
	while(*ref==' ') ref++;
	if(ref[0]=='#') return 0;
	if(strstr(ref, "://")){ snprintf(out, outsz, "%s", ref); }
	else {
		char bs[8], bh[128], bp[512]; uint16_t bport;
		if(!parse_url(base, bs, sizeof bs, bh, sizeof bh, &bport, bp, sizeof bp)) return 0;
		int dflt = (!strcmp(bs,"https") && bport==443) || (!strcmp(bs,"http") && bport==80);
		char hostport[160];
		if(dflt) snprintf(hostport, sizeof hostport, "%s", bh);
		else     snprintf(hostport, sizeof hostport, "%s:%u", bh, bport);
		if(ref[0]=='/' && ref[1]=='/')        snprintf(out, outsz, "%s:%s", bs, ref);
		else if(ref[0]=='/')                  snprintf(out, outsz, "%s://%s%s", bs, hostport, ref);
		else {
			char dir[512]; snprintf(dir, sizeof dir, "%s", bp);
			char *sl = strrchr(dir, '/'); if(sl) sl[1]=0; else { dir[0]='/'; dir[1]=0; }
			snprintf(out, outsz, "%s://%s%s%s", bs, hostport, dir, ref);
		}
	}
	char *frag = strchr(out, '#'); if(frag) *frag = 0;
	return out[0] ? 1 : 0;
}

/* ===== body staging into PSRAM ===== */
static void body_flush(void){
	if(s_stage_n){ kf_psram_write(s_body_base + s_body_len, s_stage, s_stage_n);
	               s_body_len += s_stage_n; s_stage_n = 0; }
}
static void body_put(uint8_t b){
	if(s_body_len + s_stage_n >= s_arena_cap) return;
	s_stage[s_stage_n++] = b;
	if(s_stage_n == sizeof s_stage) body_flush();
}

/* ===== transparent gzip decode =====
   Some servers send Content-Encoding: gzip even unsolicited (S3 static sites store
   pre-compressed objects), and we now ask for it (smaller transfers at the eco
   clock). De-chunked body bytes route through a gzip-header skipper + the miniz
   streaming inflater (same tinfl core the PNG decoder uses); plain output lands in
   the normal PSRAM staging. The ~43 KB inflater state (32 KB dict + decompressor)
   is malloc'd only while a gzip response is in flight. */
static int      s_gzip;                 /* this response is gzip-encoded */
static int      gz_phase;
static uint8_t  gz_flg;
static uint32_t gz_skip;                /* remaining FEXTRA bytes */
static uint8_t  gz_seen[4]; static int gz_seen_n;   /* magic bytes, for plain replay */
static tinfl_decompressor *gz_dec;
static uint8_t *gz_dict;                /* TINFL_LZ_DICT_SIZE ring */
static size_t   gz_dict_ofs;
static uint8_t  gz_in[512];
static size_t   gz_in_n;
static int      gz_done, gz_err;

enum { GZ_M0, GZ_M1, GZ_CM, GZ_FLG, GZ_MT0, GZ_MT1, GZ_MT2, GZ_MT3, GZ_XFL, GZ_OS,
       GZ_XL0, GZ_XL1, GZ_XDATA, GZ_NAME, GZ_CMT, GZ_HCRC0, GZ_HCRC1, GZ_DATA, GZ_TAIL };

static void gz_free(void){
	free(gz_dec); gz_dec = NULL;
	free(gz_dict); gz_dict = NULL;
}
static int gz_after(int stage){          /* next header phase after EXTRA/NAME/CMT */
	if(stage < 1 && (gz_flg & 8))  return GZ_NAME;
	if(stage < 2 && (gz_flg & 16)) return GZ_CMT;
	if(gz_flg & 2) return GZ_HCRC0;
	return GZ_DATA;
}
static void gz_inflate_run(int more_coming){
	size_t in_pos = 0;
	while(!gz_done && !gz_err && gz_dec){
		size_t in_bytes  = gz_in_n - in_pos;
		size_t out_bytes = TINFL_LZ_DICT_SIZE - gz_dict_ofs;
		tinfl_status st = tinfl_decompress(gz_dec, gz_in + in_pos, &in_bytes,
			gz_dict, gz_dict + gz_dict_ofs, &out_bytes,
			more_coming ? TINFL_FLAG_HAS_MORE_INPUT : 0);      /* raw DEFLATE, no zlib hdr */
		in_pos += in_bytes;
		for(size_t k = 0; k < out_bytes; k++) body_put(gz_dict[gz_dict_ofs + k]);
		gz_dict_ofs = (gz_dict_ofs + out_bytes) & (TINFL_LZ_DICT_SIZE - 1);
		if(st == TINFL_STATUS_DONE){ gz_done = 1; break; }
		if(st < TINFL_STATUS_DONE){ gz_err = 1; break; }       /* corrupt: keep partial */
		if(in_pos >= gz_in_n && st == TINFL_STATUS_NEEDS_MORE_INPUT) break;
		if(in_bytes == 0 && out_bytes == 0) break;             /* no progress guard */
	}
	gz_in_n = 0;
}
static void gz_put(uint8_t b){
	switch(gz_phase){
	case GZ_M0: if(b==0x1f){ gz_seen[gz_seen_n++]=b; gz_phase=GZ_M1; return; } break;
	case GZ_M1: if(b==0x8b){ gz_seen[gz_seen_n++]=b; gz_phase=GZ_CM; return; } break;
	case GZ_CM: if(b==8)   { gz_seen[gz_seen_n++]=b; gz_phase=GZ_FLG; return; } break;
	case GZ_FLG: gz_flg=b; gz_phase=GZ_MT0; return;
	case GZ_MT0: gz_phase=GZ_MT1; return;
	case GZ_MT1: gz_phase=GZ_MT2; return;
	case GZ_MT2: gz_phase=GZ_MT3; return;
	case GZ_MT3: gz_phase=GZ_XFL; return;
	case GZ_XFL: gz_phase=GZ_OS; return;
	case GZ_OS:  gz_phase = (gz_flg & 4) ? GZ_XL0 : gz_after(0); goto maybe_data;
	case GZ_XL0: gz_skip = b; gz_phase=GZ_XL1; return;
	case GZ_XL1: gz_skip |= (uint32_t)b<<8; gz_phase = gz_skip ? GZ_XDATA : gz_after(0); goto maybe_data;
	case GZ_XDATA: if(--gz_skip==0){ gz_phase=gz_after(0); goto maybe_data; } return;
	case GZ_NAME: if(b==0){ gz_phase=gz_after(1); goto maybe_data; } return;
	case GZ_CMT:  if(b==0){ gz_phase=gz_after(2); goto maybe_data; } return;
	case GZ_HCRC0: gz_phase=GZ_HCRC1; return;
	case GZ_HCRC1: gz_phase=GZ_DATA; goto maybe_data;
	case GZ_DATA:
		if(gz_err || !gz_dec) return;                          /* decode dead: drop */
		gz_in[gz_in_n++] = b;
		if(gz_in_n == sizeof gz_in) gz_inflate_run(1);
		if(gz_done) gz_phase = GZ_TAIL;
		return;
	case GZ_TAIL: return;                                      /* crc32+isize: ignore */
	}
	/* magic mismatch: server lied about gzip — replay as plain and pass through */
	s_gzip = 0;
	for(int i=0;i<gz_seen_n;i++) body_put(gz_seen[i]);
	body_put(b);
maybe_data:
	if(gz_phase == GZ_DATA && !gz_dec){                        /* deflate starts: alloc */
		gz_dec  = (tinfl_decompressor*)malloc(sizeof(tinfl_decompressor));
		gz_dict = (uint8_t*)malloc(TINFL_LZ_DICT_SIZE);
		if(!gz_dec || !gz_dict){ gz_free(); gz_err = 1; return; }
		tinfl_init(gz_dec);
		gz_dict_ofs = 0; gz_in_n = 0; gz_done = 0;
	}
}
static void sink_put(uint8_t b){ if(s_gzip) gz_put(b); else body_put(b); }
static void body_finish(void){
	if(s_gzip && gz_in_n && !gz_done && !gz_err) gz_inflate_run(0);
	body_flush();
	gz_free();
}

/* ===== header / chunk parser ===== */
static int ci_prefix(const char *s, const char *pfx){
	while(*pfx){ char a=*s, b=*pfx; if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32;
	            if(a!=b) return 0; s++; pfx++; }
	return 1;
}
static const char *hdr_val(const char *line){
	const char *c = strchr(line, ':'); if(!c) return "";
	c++; while(*c==' '||*c=='\t') c++; return c;
}
static void headers_done(void){
	int redir = (s_status==301||s_status==302||s_status==303||s_status==307||s_status==308);
	if(redir && s_location[0] && s_redirects < HTTP_MAX_REDIRECTS){
		if(kf_url_resolve(s_final_url, s_location, s_redirect_url, sizeof s_redirect_url)){
			s_do_redirect = 1; s_phase = PH_DONE; return;
		}
	}
	s_phase = s_chunked ? PH_CSIZE : PH_BODY_RAW;
	s_chunk_rem = 0; s_line_n = 0;
}
static void head_line(void){
	s_line[s_line_n] = 0;
	if(s_line_n && s_line[s_line_n-1]=='\r'){ s_line[--s_line_n]=0; }
	if(s_first_line){
		s_first_line = 0;
		const char *sp = strchr(s_line, ' ');
		if(sp) s_status = atoi(sp+1);
	} else if(s_line_n == 0){
		headers_done();
		return;
	} else {
		if(ci_prefix(s_line, "location:"))
			snprintf(s_location, sizeof s_location, "%s", hdr_val(s_line));
		else if(ci_prefix(s_line, "transfer-encoding:")){
			const char *v = hdr_val(s_line);
			if(strstr(v,"chunked")||strstr(v,"Chunked")) s_chunked = 1;
		}
		else if(ci_prefix(s_line, "content-encoding:")){
			const char *v = hdr_val(s_line);
			if(strstr(v,"gzip")||strstr(v,"Gzip")||strstr(v,"GZIP")) s_gzip = 1;
		}
	}
	s_line_n = 0;
}
static void feed(uint8_t b){
	switch(s_phase){
	case PH_HEADLINE:
		if(b=='\n'){ head_line(); }
		else if(s_line_n < (int)sizeof s_line - 1) s_line[s_line_n++] = (char)b;
		break;
	case PH_BODY_RAW:
		sink_put(b);
		break;
	case PH_CSIZE:
		if(b=='\n'){
			s_line[s_line_n]=0;
			s_chunk_rem = (uint32_t)strtoul(s_line, NULL, 16);
			s_line_n = 0;
			if(s_chunk_rem==0){ s_phase = PH_TRAILER; s_line_n=0; }
			else s_phase = PH_CDATA;
		} else if(b!='\r' && s_line_n < (int)sizeof s_line - 1) s_line[s_line_n++]=(char)b;
		break;
	case PH_CDATA:
		sink_put(b);
		if(--s_chunk_rem == 0) s_phase = PH_CAFTER;
		break;
	case PH_CAFTER:
		if(b=='\n') s_phase = PH_CSIZE;
		break;
	case PH_TRAILER:
		if(b=='\n'){ if(s_line_n==0 || (s_line_n==1&&s_line[0]=='\r')){ body_finish(); s_phase=PH_DONE; }
		             s_line_n=0; }
		else if(s_line_n < (int)sizeof s_line - 1) s_line[s_line_n++]=(char)b;
		break;
	default: break;
	}
}

static int build_req(void){
	if(s_post){
		return snprintf(s_req, sizeof s_req,
			"POST %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Kefyros/1.0\r\n"
			"%sContent-Length: %d\r\nConnection: close\r\n\r\n",
			s_path, s_host, s_xhdr ? s_xhdr : "", s_sbody_len);
	}
	return snprintf(s_req, sizeof s_req,
		"GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: Spineko/1.0 (Kefyros)\r\n"
		"Accept: text/html,*/*\r\nAccept-Encoding: gzip\r\n"
		"Connection: close\r\n\r\n", s_path, s_host);
}

/* ---- combined send stream (header block + optional POST body) ---- */
static int req_total(void){ return s_req_len + (s_sbody ? s_sbody_len : 0); }
/* Return a pointer at logical offset `off` and how many contiguous bytes follow it
   in that segment (*avail). Returns NULL/0 when the whole request has been consumed. */
static const char *req_at(int off, int *avail){
	if(off < s_req_len){ *avail = s_req_len - off; return s_req + off; }
	off -= s_req_len;
	if(s_sbody && off < s_sbody_len){ *avail = s_sbody_len - off; return s_sbody + off; }
	*avail = 0; return NULL;
}

/* ===== TLS pump (https only) =====
   Run the BearSSL engine state machine until it makes no more progress: flush outbound
   records to TCP, push the HTTP request once the handshake opens the app channel, and
   run decrypted plaintext through the same feed() parser as plain HTTP. */
static void tls_drive(void){
	br_ssl_engine_context *e = kf_tls_eng();
	for(;;){
		unsigned st = br_ssl_engine_current_state(e);
		if(st & BR_SSL_CLOSED) return;
		int did = 0;

		/* (1) outbound TLS records -> socket (bounded by the TCP send buffer) */
		if(st & BR_SSL_SENDREC){
			size_t len; unsigned char *buf = br_ssl_engine_sendrec_buf(e, &len);
			u16_t room = s_pcb ? tcp_sndbuf(s_pcb) : 0;
			size_t n = len < room ? len : room;
			if(n && s_pcb && tcp_write(s_pcb, buf, n, TCP_WRITE_FLAG_COPY) == ERR_OK){
				br_ssl_engine_sendrec_ack(e, n);
				tcp_output(s_pcb);
				did = 1;
			}
		}
		/* (2) app channel open (handshake done) -> push the HTTP request (headers +
		   optional POST body), in as many passes as the engine's app buffer allows */
		if((st & BR_SSL_SENDAPP) && !s_req_sent){
			size_t len; unsigned char *buf = br_ssl_engine_sendapp_buf(e, &len);
			int avail; const char *src = req_at(s_req_off, &avail);
			size_t n = (size_t)(avail > 0 ? avail : 0); if(n > len) n = len;
			if(n){ memcpy(buf, src, n); br_ssl_engine_sendapp_ack(e, n); s_req_off += (int)n; did = 1; }
			if(s_req_off >= req_total()){ br_ssl_engine_flush(e, 0); s_req_sent = 1; }
		}
		/* (3) decrypted plaintext -> HTTP byte parser */
		if(st & BR_SSL_RECVAPP){
			size_t len; unsigned char *buf = br_ssl_engine_recvapp_buf(e, &len);
			for(size_t i = 0; i < len; i++) feed(buf[i]);
			br_ssl_engine_recvapp_ack(e, len);
			did = 1;
		}
		if(!did) return;
	}
}

/* Feed `len` received TCP bytes into the engine, draining app/record output between
   chunks so the record buffer keeps reopening (handles records spanning many pbufs). */
static void tls_feed_net(const uint8_t *data, int len){
	while(len > 0){
		tls_drive();
		br_ssl_engine_context *e = kf_tls_eng();
		unsigned st = br_ssl_engine_current_state(e);
		if(st & BR_SSL_CLOSED) return;
		if(!(st & BR_SSL_RECVREC)) return;        /* can't accept records right now */
		size_t blen; unsigned char *buf = br_ssl_engine_recvrec_buf(e, &blen);
		size_t n = (size_t)len < blen ? (size_t)len : blen;
		memcpy(buf, data, n);
		br_ssl_engine_recvrec_ack(e, n);
		data += n; len -= (int)n;
	}
	tls_drive();
}

/* ===== connection ===== */
static void http_close(void){
	if(s_pcb){
		tcp_arg(s_pcb, NULL); tcp_recv(s_pcb, NULL); tcp_sent(s_pcb, NULL); tcp_err(s_pcb, NULL);
		if(tcp_close(s_pcb) != ERR_OK) tcp_abort(s_pcb);
		s_pcb = NULL;
	}
}
static void finish_ok(void){
	body_finish();             /* flush the inflater tail, then the PSRAM stage */
	if(s_state == KF_HTTP_RECEIVING) s_state = KF_HTTP_DONE;
	http_close();
}

static void http_err_cb(void *arg, err_t err){ (void)arg; s_pcb = NULL;
	if(s_body_len>0 && s_phase!=PH_HEADLINE){ if(s_state==KF_HTTP_RECEIVING) s_state=KF_HTTP_DONE; }
	else if(s_state!=KF_HTTP_DONE){ char m[40]; snprintf(m,sizeof m,"conn err e=%d",(int)err); fail(m); }
}
static err_t http_recv_cb(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err){
	(void)arg;
	if(err != ERR_OK){ if(p) pbuf_free(p); fail("recv error"); http_close(); return ERR_OK; }
	if(!p){ finish_ok(); return ERR_OK; }            /* remote closed -> done */
	s_tlast = now_ms();
	if(s_secure){
		for(struct pbuf *q=p; q; q=q->next) tls_feed_net((const uint8_t*)q->payload, q->len);
		tcp_recved(pcb, p->tot_len);
		pbuf_free(p);
		if(s_req_sent && s_state==KF_HTTP_CONNECTING) s_state = KF_HTTP_RECEIVING;
		br_ssl_engine_context *e = kf_tls_eng();
		unsigned st = br_ssl_engine_current_state(e);
		int terr = br_ssl_engine_last_error(e);
		if(st & BR_SSL_CLOSED){
			if(terr == BR_ERR_OK || s_body_len > 0) finish_ok();
			else { char m[40]; snprintf(m,sizeof m,"tls err %d", terr); fail(m); http_close(); }
			return ERR_OK;
		}
		if(s_phase==PH_DONE && !s_do_redirect) finish_ok();
		return ERR_OK;
	}
	for(struct pbuf *q=p; q; q=q->next){
		const uint8_t *d = (const uint8_t*)q->payload;
		for(uint16_t i=0;i<q->len;i++) feed(d[i]);
	}
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	if(s_phase==PH_DONE && !s_do_redirect) finish_ok();
	return ERR_OK;
}
/* Plain-HTTP send: push as much of the combined request (headers + body) as the TCP
   send buffer holds; the rest goes out from http_sent_cb as ACKs free the buffer. */
static void pump_plain_send(void){
	while(s_pcb && s_req_off < req_total()){
		int avail; const char *src = req_at(s_req_off, &avail);
		if(!src || avail <= 0) break;
		u16_t room = tcp_sndbuf(s_pcb);
		if(!room) break;
		size_t n = (size_t)avail; if(n > room) n = room;
		if(tcp_write(s_pcb, src, n, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
		s_req_off += (int)n;
	}
	if(s_pcb) tcp_output(s_pcb);
	if(s_req_off >= req_total()) s_req_sent = 1;
}
static err_t http_sent_cb(void *arg, struct tcp_pcb *pcb, u16_t l){
	(void)arg; (void)pcb; (void)l;
	if(!s_req_sent) pump_plain_send();
	return ERR_OK;
}
static err_t http_connected_cb(void *arg, struct tcp_pcb *pcb, err_t err){
	(void)arg;
	if(err != ERR_OK){ fail("connect failed"); http_close(); return ERR_OK; }
	s_tlast = now_ms();
	s_req_len = build_req();
	s_req_off = 0; s_req_sent = 0;
	if(s_secure){
		kf_tls_begin(s_host);          /* host = SNI */
		s_state = KF_HTTP_CONNECTING;  /* TLS handshake in flight */
		tls_drive();                   /* emit the ClientHello */
	} else {
		tcp_sent(pcb, http_sent_cb);   /* continue the body as the buffer drains */
		pump_plain_send();
		s_state = KF_HTTP_RECEIVING;
	}
	return ERR_OK;
}
static void http_dns(const char *name, const ip_addr_t *ip, void *arg){
	(void)name; (void)arg;
	if(!ip){ fail("DNS failed"); return; }
	s_pcb = tcp_new_ip_type(IP_GET_TYPE(ip));
	if(!s_pcb){ fail("no pcb (out of memory)"); return; }
	tcp_arg(s_pcb, NULL);
	tcp_recv(s_pcb, http_recv_cb);
	tcp_err(s_pcb, http_err_cb);
	s_state = KF_HTTP_CONNECTING; s_tlast = now_ms();
	if(tcp_connect(s_pcb, ip, s_port, http_connected_cb) != ERR_OK){ fail("connect start"); http_close(); }
}

/* ===== start / poll / abort ===== */
static int start_request(const char *url){
	char scheme[8];
	if(!parse_url(url, scheme, sizeof scheme, s_host, sizeof s_host, &s_port, s_path, sizeof s_path)){
		fail("bad URL"); return -1;
	}
	if(!strcmp(scheme, "https"))      s_secure = 1;
	else if(!strcmp(scheme, "http"))  s_secure = 0;
	else { fail("unsupported scheme"); return -1; }
	snprintf(s_final_url, sizeof s_final_url, "%s", url);

	http_close();
	s_state = KF_HTTP_RESOLVING;
	s_status = 0; s_chunked = 0; s_first_line = 1; s_location[0] = 0;
	s_do_redirect = 0; s_phase = PH_HEADLINE; s_line_n = 0; s_chunk_rem = 0;
	s_req_off = 0; s_req_sent = 0;
	s_body_base = s_arena_base; s_body_len = 0; s_stage_n = 0;
	gz_free(); s_gzip = 0; gz_phase = GZ_M0; gz_seen_n = 0; gz_flg = 0;
	gz_in_n = 0; gz_done = 0; gz_err = 0; gz_skip = 0;
	s_err[0] = 0; s_t0 = s_tlast = now_ms();

	ip_addr_t ip;
	err_t e = dns_gethostbyname(s_host, &ip, http_dns, NULL);
	if(e == ERR_OK)            http_dns(s_host, &ip, NULL);
	else if(e != ERR_INPROGRESS){ fail("DNS start"); return -1; }
	return 0;
}

int kf_http_get(const char *url){
	if(!s_arena_cap){ fail("no PSRAM arena"); return -1; }
	s_redirects = 0;
	s_post = 0; s_xhdr = NULL; s_sbody = NULL; s_sbody_len = 0;
	return start_request(url);
}

int kf_http_post(const char *url, const char *headers, const char *body, uint32_t body_len){
	if(!s_arena_cap){ fail("no PSRAM arena"); return -1; }
	s_redirects = 0;
	s_post = 1; s_xhdr = headers; s_sbody = body; s_sbody_len = (int)body_len;
	return start_request(url);
}

void kf_http_abort(void){
	http_close();
	gz_free();
	s_state = KF_HTTP_IDLE; s_phase = PH_DONE; s_do_redirect = 0;
}

void kf_http_poll(void){
	if(s_do_redirect){
		s_do_redirect = 0;
		s_redirects++;
		start_request(s_redirect_url);
		return;
	}
	if(s_state==KF_HTTP_RESOLVING || s_state==KF_HTTP_CONNECTING || s_state==KF_HTTP_RECEIVING){
		uint32_t t = now_ms();
		if(t - s_t0 > HTTP_TIMEOUT_MS || t - s_tlast > HTTP_STALL_MS){
			if(s_body_len>0){ finish_ok(); }
			else { fail("timeout"); http_close(); }
		}
	}
}
