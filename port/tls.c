// port/tls.c — BearSSL TLS 1.2 client engine for Spineko HTTPS. See tls.h for rationale.
#include <string.h>
#include "pico/rand.h"
#include "tls.h"

/* ---- engine + I/O buffer ----
   Half-duplex (bidi=0): the buffer holds one direction at a time, which is exactly the
   HTTP request-then-response pattern, and halves the SRAM cost vs full-duplex. Sized to
   hold a full 16 KB TLS record (BR_SSL_BUFSIZE_MONO ~= 16.7 KB) since most servers
   ignore max-fragment-length negotiation. Static BSS — not on the 4 KB core-0 stack. */
static br_ssl_client_context s_sc;
static unsigned char         s_iobuf[BR_SSL_BUFSIZE_MONO];

/* ---- accept-all X.509 handler ----
   We do NOT verify the certificate chain (no trusted root store, no trusted wall clock
   at handshake time — the original, accepted design decision). But BearSSL still needs
   the end-entity public key to verify the ServerKeyExchange signature for ECDHE suites,
   so we decode only the leaf certificate with br_x509_decoder, hand back its public key,
   and accept the chain unconditionally. This is the standard BearSSL "insecure" pattern. */
typedef struct {
	const br_x509_class *vtable;
	br_x509_decoder_context dc;
	int leaf;                 /* 1 while the first (end-entity) cert streams in */
} noanchor_x509;

static void xwc_start_chain(const br_x509_class **ctx, const char *server_name){
	noanchor_x509 *xc = (noanchor_x509 *)ctx;
	(void)server_name;
	br_x509_decoder_init(&xc->dc, 0, 0);
	xc->leaf = 1;
}
static void xwc_start_cert(const br_x509_class **ctx, uint32_t length){
	(void)ctx; (void)length;
}
static void xwc_append(const br_x509_class **ctx, const unsigned char *buf, size_t len){
	noanchor_x509 *xc = (noanchor_x509 *)ctx;
	if(xc->leaf) br_x509_decoder_push(&xc->dc, buf, len);   /* decode leaf only */
}
static void xwc_end_cert(const br_x509_class **ctx){
	noanchor_x509 *xc = (noanchor_x509 *)ctx;
	xc->leaf = 0;             /* ignore intermediates/root — we don't build a chain */
}
static unsigned xwc_end_chain(const br_x509_class **ctx){
	(void)ctx;
	return 0;                 /* 0 = accept (BR_ERR_OK), no validation */
}
static const br_x509_pkey *xwc_get_pkey(const br_x509_class *const *ctx, unsigned *usages){
	noanchor_x509 *xc = (noanchor_x509 *)ctx;
	if(usages) *usages = BR_KEYTYPE_KEYX | BR_KEYTYPE_SIGN;  /* allow KEYX (RSA) + SIGN (ECDHE) */
	return br_x509_decoder_get_pkey(&xc->dc);
}
static const br_x509_class noanchor_vtable = {
	sizeof(noanchor_x509),
	xwc_start_chain, xwc_start_cert, xwc_append, xwc_end_cert, xwc_end_chain, xwc_get_pkey
};
static noanchor_x509 s_xc;

br_ssl_engine_context *kf_tls_eng(void){ return &s_sc.eng; }

void kf_tls_begin(const char *host){
	/* Full cipher-suite/algorithm wiring (ECDHE-ECDSA, ECDHE-RSA, RSA; AES-GCM/CBC;
	   P-256/384/521; SHA-2). init_full installs the minimal X.509 validator, which we
	   immediately override with our accept-all handler. */
	br_ssl_client_init_full(&s_sc, 0, 0, 0);
	s_xc.vtable = &noanchor_vtable;
	br_ssl_engine_set_x509(&s_sc.eng, &s_xc.vtable);

	br_ssl_engine_set_buffer(&s_sc.eng, s_iobuf, sizeof s_iobuf, 0 /* half-duplex */);

	/* Pin TLS 1.2 (BearSSL has no 1.3; this also rejects ancient downgrades). */
	br_ssl_engine_set_versions(&s_sc.eng, BR_TLS12, BR_TLS12);

	/* Seed the PRNG from the RP2350 hardware TRNG (pico_rand). Without a system seeder
	   on bare metal, BearSSL would fail the handshake with BR_ERR_NO_RANDOM. */
	unsigned char seed[32];
	for(int i = 0; i < 4; i++){
		uint64_t r = get_rand_64();
		memcpy(seed + i * 8, &r, 8);
	}
	br_ssl_engine_inject_entropy(&s_sc.eng, seed, sizeof seed);

	br_ssl_client_reset(&s_sc, host, 0 /* no session resumption */);
}
