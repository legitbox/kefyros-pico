// port/http.h — async HTTP(S) GET/POST client (Spineko browser + DeepSeek chat).
//
// Single in-flight request over raw lwIP TCP. The response BODY is streamed straight
// into a caller-provided PSRAM region (so page size is bounded by PSRAM, not the heap).
// Headers are parsed in SRAM; chunked transfer-encoding is de-chunked on the fly;
// 301/302/303/307/308 redirects are followed automatically (bounded). Driven by the
// lwIP callbacks that fire inside cyw43_arch_poll() (which kf_net_poll() calls every
// superloop tick); the app must also call kf_http_poll() each tick for redirects +
// timeouts. https:// URLs return "HTTPS not supported".
#ifndef KF_HTTP_H
#define KF_HTTP_H
#include <stdint.h>

enum {
	KF_HTTP_IDLE = 0,
	KF_HTTP_RESOLVING,    /* DNS lookup in flight */
	KF_HTTP_CONNECTING,   /* TCP/TLS connect in flight */
	KF_HTTP_RECEIVING,    /* headers/body streaming in */
	KF_HTTP_DONE,         /* body complete in PSRAM */
	KF_HTTP_ERROR         /* failed (see kf_http_err) */
};

/* Where the response body is stored: an absolute PSRAM region [base, base+cap). */
void        kf_http_set_arena(uint32_t psram_base, uint32_t cap);

/* Begin a GET of an absolute URL ("http://..." or "https://..."). Any in-flight
   request is aborted first. Returns 0 if started, <0 on bad URL / offline. */
int         kf_http_get(const char *url);

/* Begin a POST of an absolute URL. `headers` is an optional block of EXTRA header
   lines, each already terminated with CRLF (e.g. "Authorization: Bearer x\r\n"
   "Content-Type: application/json\r\n"), or NULL. `body`/`body_len` is the request
   payload; the buffer must stay valid (caller-owned) until the request finishes
   (state DONE/ERROR), since it is streamed asynchronously. Host/Content-Length/
   Connection headers are added automatically. Returns 0 if started, <0 on error. */
int         kf_http_post(const char *url, const char *headers,
                         const char *body, uint32_t body_len);

void        kf_http_poll(void);           /* service redirects + timeouts (call each tick) */
void        kf_http_abort(void);          /* cancel in-flight request, free the pcb */

int         kf_http_state(void);          /* KF_HTTP_* */
int         kf_http_status(void);         /* HTTP status code once headers seen, else 0 */
uint32_t    kf_http_body_base(void);      /* PSRAM offset of the body */
uint32_t    kf_http_body_len(void);       /* bytes of body received */
const char *kf_http_final_url(void);      /* absolute URL after redirects */
const char *kf_http_err(void);            /* human-readable last error ("" if none) */

/* Resolve a (possibly relative) URL `ref` against absolute `base` into `out`.
   Handles scheme-relative ("//host/x"), absolute-path ("/x"), and relative
   ("x", "sub/x") refs; strips #fragments. Returns 1 on success, 0 if unusable. */
int         kf_url_resolve(const char *base, const char *ref, char *out, int outsz);

#endif /* KF_HTTP_H */
