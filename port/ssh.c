// port/ssh.c — pure SSH-2 client core (see ssh.h).
//
// Suite (modern OpenSSH only): kex curve25519-sha256, host key ssh-ed25519,
// cipher chacha20-poly1305@openssh.com both directions. Crypto = Monocypher
// (x25519 / ed25519 / chacha20_djb / poly1305) + port/sha256.c (exchange hash
// + KDF). No platform headers: all I/O and randomness go through ssh_cb_t, so
// this compiles and runs identically on host and device.
//
// cb.tx contract: accept ALL n bytes (buffer internally if the socket is full)
// and return n, or return <0 on a fatal socket error. The device's ssh_tcp.c
// glue satisfies this with a drain-on-ack tx queue; the host uses a blocking
// send loop.
#include "ssh.h"
#include "sha256.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <string.h>

/* ---- SSH message numbers ---- */
enum {
	MSG_DISCONNECT=1, MSG_IGNORE=2, MSG_UNIMPLEMENTED=3, MSG_DEBUG=4,
	MSG_SERVICE_REQUEST=5, MSG_SERVICE_ACCEPT=6, MSG_EXT_INFO=7,
	MSG_KEXINIT=20, MSG_NEWKEYS=21,
	MSG_KEX_ECDH_INIT=30, MSG_KEX_ECDH_REPLY=31,
	MSG_USERAUTH_REQUEST=50, MSG_USERAUTH_FAILURE=51, MSG_USERAUTH_SUCCESS=52, MSG_USERAUTH_BANNER=53,
	MSG_GLOBAL_REQUEST=80, MSG_REQUEST_SUCCESS=81, MSG_REQUEST_FAILURE=82,
	MSG_CHANNEL_OPEN=90, MSG_CHANNEL_OPEN_CONFIRMATION=91, MSG_CHANNEL_OPEN_FAILURE=92,
	MSG_CHANNEL_WINDOW_ADJUST=93, MSG_CHANNEL_DATA=94, MSG_CHANNEL_EXTENDED_DATA=95,
	MSG_CHANNEL_EOF=96, MSG_CHANNEL_CLOSE=97, MSG_CHANNEL_REQUEST=98,
	MSG_CHANNEL_SUCCESS=99, MSG_CHANNEL_FAILURE=100
};

#define SSH_IDENT      "SSH-2.0-Kefyros_1.0"
#define SSH_RXBUF      8192
#define SSH_TXBUF      2048
#define SSH_CHAN_MAX   4096      /* our advertised channel max_packet */
#define SSH_WINDOW     (32*1024) /* our advertised initial window */
#define SSH_SENDCAP    1024      /* max shell bytes per outgoing DATA packet */
#define KEEPALIVE_MS   60000
#define DEAD_MS        190000

struct ssh {
	ssh_cb_t cb;
	void *(*alloc)(size_t);
	void  (*dealloc)(void *);

	ssh_state_t state;
	char err[128];
	char user[64];

	/* banner exchange */
	int  got_banner;
	char v_s[256]; int v_s_len;
	char line[300]; int line_len;

	/* kex */
	int      kex_sent;            /* our KEXINIT sent for the current exchange */
	uint8_t  x_priv[32], x_pub[32];
	uint8_t  i_c[2048]; int i_c_len;
	uint8_t  i_s[2048]; int i_s_len;
	uint8_t  session_id[32]; int have_sid;
	uint8_t  host_pub[32];

	/* transport keys */
	uint8_t  key_c2s[64], key_s2c[64];
	uint32_t seq_in, seq_out;
	int      enc_in, enc_out;
	int      service_sent;

	/* auth */
	int      have_key; uint8_t seed[32];
	int      tried_none, tried_pubkey, pw_sent, pw_tries;
	int      want_password;
	char     password[128];

	/* channel */
	int      chan_opened, want_ptyreq, want_shell;
	uint32_t local_win, remote_win, remote_maxpkt, remote_chan;
	uint32_t consumed;
	int      exit_status;
	int      cols, rows;

	/* timers */
	uint32_t last_rx_ms, keepalive_at; int keepalive_pending;

	/* buffers */
	uint8_t  rx[SSH_RXBUF]; int rx_len;
	uint8_t  pkt[SSH_RXBUF];
	uint8_t  tx_out[SSH_TXBUF + 64];
	uint8_t  tx_plain[SSH_TXBUF];
	uint8_t  tx_enc[SSH_TXBUF];
	uint8_t  tx_mac[SSH_TXBUF + 8];
};

/* ---- wire buffer builder ---- */
typedef struct { uint8_t *p; int len, cap; } wbuf;
static void wb_init(wbuf *b, uint8_t *m, int cap){ b->p = m; b->len = 0; b->cap = cap; }
static void wb_byte(wbuf *b, uint8_t v){ if(b->len < b->cap) b->p[b->len] = v; b->len++; }
static void wb_raw(wbuf *b, const void *d, int n){ const uint8_t *s = d; for(int i=0;i<n;i++) wb_byte(b, s[i]); }
static void wb_u32(wbuf *b, uint32_t v){ wb_byte(b,v>>24); wb_byte(b,v>>16); wb_byte(b,v>>8); wb_byte(b,v); }
static void wb_str(wbuf *b, const void *d, int n){ wb_u32(b, n); wb_raw(b, d, n); }
static void wb_cstr(wbuf *b, const char *s){ wb_str(b, s, (int)strlen(s)); }
static void wb_mpint(wbuf *b, const uint8_t *v, int n){
	int i = 0; while(i < n && v[i] == 0) i++;
	if(i == n){ wb_u32(b, 0); return; }
	int hi = (v[i] & 0x80) ? 1 : 0;
	wb_u32(b, (uint32_t)((n - i) + hi));
	if(hi) wb_byte(b, 0);
	wb_raw(b, v + i, n - i);
}

/* ---- wire reader ---- */
typedef struct { const uint8_t *p; int len, pos; } rbuf;
static void rb_init(rbuf *r, const uint8_t *p, int len){ r->p = p; r->len = len; r->pos = 0; }
static int rb_byte(rbuf *r, uint8_t *o){ if(r->pos + 1 > r->len) return -1; *o = r->p[r->pos++]; return 0; }
static int rb_u32(rbuf *r, uint32_t *o){
	if(r->pos + 4 > r->len) return -1;
	*o = ((uint32_t)r->p[r->pos]<<24)|((uint32_t)r->p[r->pos+1]<<16)|((uint32_t)r->p[r->pos+2]<<8)|r->p[r->pos+3];
	r->pos += 4; return 0;
}
static int rb_str(rbuf *r, const uint8_t **o, uint32_t *ol){
	uint32_t n; if(rb_u32(r, &n)) return -1;
	if((uint32_t)(r->len - r->pos) < n) return -1;
	*o = r->p + r->pos; *ol = n; r->pos += n; return 0;
}
static int namelist_has(const uint8_t *list, uint32_t len, const char *want){
	int wl = (int)strlen(want); uint32_t i = 0;
	while(i < len){
		uint32_t j = i; while(j < len && list[j] != ',') j++;
		if((int)(j - i) == wl && memcmp(list + i, want, wl) == 0) return 1;
		i = j + 1;
	}
	return 0;
}

/* ---- base64 (for the SHA256: fingerprint, no padding) ---- */
static void b64(const uint8_t *in, int n, char *out){
	static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int o = 0;
	for(int i = 0; i < n; i += 3){
		uint32_t v = in[i] << 16;
		if(i+1 < n) v |= in[i+1] << 8;
		if(i+2 < n) v |= in[i+2];
		out[o++] = T[(v>>18)&63];
		out[o++] = T[(v>>12)&63];
		if(i+1 < n) out[o++] = T[(v>>6)&63];
		if(i+2 < n) out[o++] = T[v&63];
	}
	out[o] = 0;
}

/* ---- fatal error ---- */
static void fail(ssh_t *s, const char *msg){
	if(s->state != SSH_ST_ERROR){
		strncpy(s->err, msg, sizeof s->err - 1); s->err[sizeof s->err - 1] = 0;
		s->state = SSH_ST_ERROR;
		if(s->cb.on_state) s->cb.on_state(SSH_ST_ERROR, s->err, s->cb.ud);
	}
}
static void set_state(ssh_t *s, ssh_state_t st, const char *detail){
	s->state = st;
	if(s->cb.on_state) s->cb.on_state(st, detail, s->cb.ud);
}

/* ---- send a framed packet (payload includes the message-number byte) ---- */
static void chacha_keys(const uint8_t key[64], const uint8_t **k_main, const uint8_t **k_hdr){
	*k_main = key; *k_hdr = key + 32;
}
static void seq_nonce(uint32_t seq, uint8_t nonce[8]){
	nonce[0]=nonce[1]=nonce[2]=nonce[3]=0;
	nonce[4]=seq>>24; nonce[5]=seq>>16; nonce[6]=seq>>8; nonce[7]=seq;
}

static int tx_all(ssh_t *s, const uint8_t *d, int n){
	int r = s->cb.tx(d, n, s->cb.ud);
	if(r != n){ fail(s, "socket write failed"); return -1; }
	return 0;
}

static int send_packet(ssh_t *s, const uint8_t *payload, int plen){
	if(s->state == SSH_ST_ERROR) return -1;
	wbuf b; wb_init(&b, s->tx_out, sizeof s->tx_out);
	if(!s->enc_out){
		int L = 4 + 1 + plen, pad = 8 - (L % 8); if(pad < 4) pad += 8;
		uint32_t pkt_len = 1 + plen + pad;
		wb_u32(&b, pkt_len); wb_byte(&b, (uint8_t)pad);
		wb_raw(&b, payload, plen);
		uint8_t padb[16]; s->cb.rng(padb, pad, s->cb.ud); wb_raw(&b, padb, pad);
	} else {
		int pad = 8 - ((1 + plen) % 8); if(pad < 4) pad += 8;
		uint32_t pkt_len = 1 + plen + pad;
		const uint8_t *km, *kh; chacha_keys(s->key_c2s, &km, &kh);
		uint8_t nonce[8]; seq_nonce(s->seq_out, nonce);
		/* plaintext = padlen || payload || random padding */
		wbuf pb; wb_init(&pb, s->tx_plain, sizeof s->tx_plain);
		wb_byte(&pb, (uint8_t)pad); wb_raw(&pb, payload, plen);
		uint8_t padb[16]; s->cb.rng(padb, pad, s->cb.ud); wb_raw(&pb, padb, pad);
		/* encrypted length (K_hdr, ctr0) */
		uint8_t len_be[4] = { pkt_len>>24, pkt_len>>16, pkt_len>>8, pkt_len };
		uint8_t enc_len[4];
		crypto_chacha20_djb(enc_len, len_be, 4, kh, nonce, 0);
		/* encrypted payload (K_main, ctr1) */
		crypto_chacha20_djb(s->tx_enc, s->tx_plain, pkt_len, km, nonce, 1);
		/* poly key = chacha(K_main, ctr0)[0..31] */
		uint8_t poly_key[64], zero[64]; memset(zero, 0, 64);
		crypto_chacha20_djb(poly_key, zero, 64, km, nonce, 0);
		memcpy(s->tx_mac, enc_len, 4); memcpy(s->tx_mac + 4, s->tx_enc, pkt_len);
		uint8_t tag[16]; crypto_poly1305(tag, s->tx_mac, 4 + pkt_len, poly_key);
		crypto_wipe(poly_key, 64);
		wb_raw(&b, enc_len, 4); wb_raw(&b, s->tx_enc, pkt_len); wb_raw(&b, tag, 16);
	}
	if(b.len > b.cap){ fail(s, "tx packet too large"); return -1; }
	s->seq_out++;
	return tx_all(s, s->tx_out, b.len);
}

/* ---- KEXINIT ---- */
static const char *KEX_ALGS  = "curve25519-sha256,curve25519-sha256@libssh.org";
static const char *HOSTKEY   = "ssh-ed25519";
static const char *CIPHER    = "chacha20-poly1305@openssh.com";
static const char *MACS      = "hmac-sha2-256";   /* listed but unused with the AEAD */

static void send_kexinit(ssh_t *s){
	wbuf b; wb_init(&b, s->i_c, sizeof s->i_c);
	wb_byte(&b, MSG_KEXINIT);
	uint8_t cookie[16]; s->cb.rng(cookie, 16, s->cb.ud); wb_raw(&b, cookie, 16);
	wb_cstr(&b, KEX_ALGS);
	wb_cstr(&b, HOSTKEY);
	wb_cstr(&b, CIPHER); wb_cstr(&b, CIPHER);
	wb_cstr(&b, MACS);   wb_cstr(&b, MACS);
	wb_cstr(&b, "none"); wb_cstr(&b, "none");
	wb_cstr(&b, "");     wb_cstr(&b, "");
	wb_byte(&b, 0);      /* first_kex_packet_follows */
	wb_u32(&b, 0);       /* reserved */
	s->i_c_len = b.len;
	s->kex_sent = 1;
	send_packet(s, s->i_c, s->i_c_len);
}

/* validate the server KEXINIT (stored in s->i_s) offers our one suite */
static int check_kexinit(ssh_t *s){
	rbuf r; rb_init(&r, s->i_s, s->i_s_len);
	uint8_t msg; if(rb_byte(&r, &msg)) return -1;
	r.pos += 16;   /* cookie */
	const uint8_t *kex,*hk,*ec,*es,*mc,*ms,*cc,*cs,*lc,*ls;
	uint32_t kexl,hkl,ecl,esl,mcl,msl,ccl,csl,lcl,lsl;
	if(rb_str(&r,&kex,&kexl)||rb_str(&r,&hk,&hkl)||rb_str(&r,&ec,&ecl)||rb_str(&r,&es,&esl)||
	   rb_str(&r,&mc,&mcl)||rb_str(&r,&ms,&msl)||rb_str(&r,&cc,&ccl)||rb_str(&r,&cs,&csl)||
	   rb_str(&r,&lc,&lcl)||rb_str(&r,&ls,&lsl)) return -1;
	if(!(namelist_has(kex,kexl,"curve25519-sha256") || namelist_has(kex,kexl,"curve25519-sha256@libssh.org")))
		{ fail(s, "server has no curve25519-sha256 kex"); return -1; }
	if(!namelist_has(hk,hkl,"ssh-ed25519")) { fail(s, "server has no ssh-ed25519 host key"); return -1; }
	if(!namelist_has(ec,ecl,CIPHER) || !namelist_has(es,esl,CIPHER))
		{ fail(s, "server lacks chacha20-poly1305 (need OpenSSH >= 6.5)"); return -1; }
	return 0;
}

static void send_ecdh_init(ssh_t *s){
	s->cb.rng(s->x_priv, 32, s->cb.ud);
	crypto_x25519_public_key(s->x_pub, s->x_priv);
	uint8_t p[64]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_KEX_ECDH_INIT);
	wb_str(&b, s->x_pub, 32);
	send_packet(s, p, b.len);
	set_state(s, SSH_ST_KEX, "key exchange");
}

/* hash helper: feed an SSH string (u32 length + bytes) into the exchange hash */
static void h_str(sha256_ctx *h, const void *d, int n){
	uint8_t l[4] = { (uint8_t)(n>>24), (uint8_t)(n>>16), (uint8_t)(n>>8), (uint8_t)n };
	sha256_update(h, l, 4); sha256_update(h, d, (size_t)n);
}
static void h_mpint(sha256_ctx *h, const uint8_t *v, int n){
	int i = 0; while(i < n && v[i] == 0) i++;
	int hi = (i < n && (v[i] & 0x80)) ? 1 : 0;
	int total = (i == n) ? 0 : (n - i) + hi;
	uint8_t l[4] = { (uint8_t)(total>>24), (uint8_t)(total>>16), (uint8_t)(total>>8), (uint8_t)total };
	sha256_update(h, l, 4);
	if(i == n) return;
	if(hi){ uint8_t z = 0; sha256_update(h, &z, 1); }
	sha256_update(h, v + i, (size_t)(n - i));
}

/* Key derivation (RFC 4253 §7.2) with an explicit exchange hash H. H == the
   session_id at the first kex; on rekey H changes while session_id stays fixed. */
static void derive_key2(const uint8_t *K_mp, int K_mp_len, const uint8_t H[32],
                        const uint8_t session_id[32], char letter, uint8_t out[64]){
	sha256_ctx c; uint8_t k1[32];
	sha256_init(&c);
	sha256_update(&c, K_mp, (size_t)K_mp_len);
	sha256_update(&c, H, 32);
	sha256_update(&c, (uint8_t*)&letter, 1);
	sha256_update(&c, session_id, 32);
	sha256_final(&c, k1);
	sha256_init(&c);
	sha256_update(&c, K_mp, (size_t)K_mp_len);
	sha256_update(&c, H, 32);
	sha256_update(&c, k1, 32);
	sha256_final(&c, out + 32);
	memcpy(out, k1, 32);
}

static void handle_ecdh_reply(ssh_t *s, const uint8_t *pl, int len){
	rbuf r; rb_init(&r, pl, len);
	uint8_t msg; rb_byte(&r, &msg);
	const uint8_t *ks, *qs, *sig; uint32_t ksl, qsl, sigl;
	if(rb_str(&r, &ks, &ksl) || rb_str(&r, &qs, &qsl) || rb_str(&r, &sig, &sigl)){ fail(s, "bad ECDH reply"); return; }
	if(qsl != 32){ fail(s, "bad server ephemeral key"); return; }

	/* parse K_S = string "ssh-ed25519" + string pub(32) */
	rbuf kr; rb_init(&kr, ks, ksl);
	const uint8_t *kt, *kp; uint32_t ktl, kpl;
	if(rb_str(&kr, &kt, &ktl) || rb_str(&kr, &kp, &kpl) || ktl != 11 ||
	   memcmp(kt, "ssh-ed25519", 11) != 0 || kpl != 32){ fail(s, "bad host key blob"); return; }
	memcpy(s->host_pub, kp, 32);

	/* shared secret K = X25519(x_priv, Q_S) */
	uint8_t K[32]; crypto_x25519(K, s->x_priv, qs);
	uint8_t zero[32]; memset(zero, 0, 32);
	if(crypto_verify32(K, zero) == 0){ fail(s, "bad shared secret"); return; }

	/* exchange hash H */
	sha256_ctx h; sha256_init(&h);
	h_str(&h, SSH_IDENT, (int)strlen(SSH_IDENT));
	h_str(&h, s->v_s, s->v_s_len);
	h_str(&h, s->i_c, s->i_c_len);
	h_str(&h, s->i_s, s->i_s_len);
	h_str(&h, ks, (int)ksl);
	h_str(&h, s->x_pub, 32);
	h_str(&h, qs, 32);
	h_mpint(&h, K, 32);
	uint8_t H[32]; sha256_final(&h, H);

	/* TOFU host-key check (fingerprint = SHA256:base64(K_S blob)) */
	uint8_t fp_raw[32]; sha256(ks, ksl, fp_raw);
	char fp[8 + 48]; memcpy(fp, "SHA256:", 7); b64(fp_raw, 32, fp + 7);
	if(s->cb.check_hostkey && !s->cb.check_hostkey(s->host_pub, fp, s->cb.ud)){
		fail(s, "host key rejected"); return;
	}

	/* verify the server's signature over H: sig blob = string "ssh-ed25519" + string sig64 */
	rbuf sr; rb_init(&sr, sig, sigl);
	const uint8_t *st_, *sb; uint32_t stl, sbl;
	if(rb_str(&sr, &st_, &stl) || rb_str(&sr, &sb, &sbl) || stl != 11 ||
	   memcmp(st_, "ssh-ed25519", 11) != 0 || sbl != 64){ fail(s, "bad signature blob"); return; }
	if(crypto_ed25519_check(sb, s->host_pub, H, 32) != 0){ fail(s, "host signature invalid"); return; }

	if(!s->have_sid){ memcpy(s->session_id, H, 32); s->have_sid = 1; }

	/* derive c2s/s2c keys (mpint(K) is the KDF's K input). Max mpint = 4 len +
	   1 sign byte + 32 value = 37 bytes when K's high bit is set. */
	uint8_t K_mp[40]; wbuf kb; wb_init(&kb, K_mp, sizeof K_mp); wb_mpint(&kb, K, 32);
	derive_key2(K_mp, kb.len, H, s->session_id, 'C', s->key_c2s);
	derive_key2(K_mp, kb.len, H, s->session_id, 'D', s->key_s2c);
	crypto_wipe(K, 32);

	/* send NEWKEYS (last cleartext packet), then switch our direction on */
	uint8_t nk = MSG_NEWKEYS; send_packet(s, &nk, 1);
	s->enc_out = 1;

	/* request the userauth service (encrypted) once */
	if(!s->service_sent){
		uint8_t p[64]; wbuf b; wb_init(&b, p, sizeof p);
		wb_byte(&b, MSG_SERVICE_REQUEST); wb_cstr(&b, "ssh-userauth");
		send_packet(s, p, b.len);
		s->service_sent = 1;
	}
}

/* ---- userauth ---- */
static void send_auth_none(ssh_t *s){
	uint8_t p[128]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_USERAUTH_REQUEST);
	wb_cstr(&b, s->user); wb_cstr(&b, "ssh-connection"); wb_cstr(&b, "none");
	send_packet(s, p, b.len);
	s->tried_none = 1;
}
static void send_auth_pubkey(ssh_t *s){
	uint8_t seed[32]; memcpy(seed, s->seed, 32);
	uint8_t sk[64], pub[32]; crypto_ed25519_key_pair(sk, pub, seed);   /* wipes seed copy */
	/* pubkey blob = string "ssh-ed25519" + string pub */
	uint8_t blob[64]; wbuf bl; wb_init(&bl, blob, sizeof blob);
	wb_cstr(&bl, "ssh-ed25519"); wb_str(&bl, pub, 32);
	/* data to sign = string(session_id) + request(up to and incl. pubkey blob) */
	uint8_t sd[256]; wbuf d; wb_init(&d, sd, sizeof sd);
	wb_str(&d, s->session_id, 32);
	wb_byte(&d, MSG_USERAUTH_REQUEST);
	wb_cstr(&d, s->user); wb_cstr(&d, "ssh-connection"); wb_cstr(&d, "publickey");
	wb_byte(&d, 1);   /* TRUE: with signature */
	wb_cstr(&d, "ssh-ed25519"); wb_str(&d, blob, bl.len);
	uint8_t sig[64]; crypto_ed25519_sign(sig, sk, sd, (size_t)d.len);
	crypto_wipe(sk, 64);
	/* signature blob = string "ssh-ed25519" + string sig */
	uint8_t sigblob[128]; wbuf sbb; wb_init(&sbb, sigblob, sizeof sigblob);
	wb_cstr(&sbb, "ssh-ed25519"); wb_str(&sbb, sig, 64);
	/* full request */
	uint8_t p[400]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_USERAUTH_REQUEST);
	wb_cstr(&b, s->user); wb_cstr(&b, "ssh-connection"); wb_cstr(&b, "publickey");
	wb_byte(&b, 1);
	wb_cstr(&b, "ssh-ed25519"); wb_str(&b, blob, bl.len);
	wb_str(&b, sigblob, sbb.len);
	send_packet(s, p, b.len);
	s->tried_pubkey = 1;
}
static void send_auth_password(ssh_t *s){
	uint8_t p[256]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_USERAUTH_REQUEST);
	wb_cstr(&b, s->user); wb_cstr(&b, "ssh-connection"); wb_cstr(&b, "password");
	wb_byte(&b, 0);   /* FALSE: not a change request */
	wb_cstr(&b, s->password);
	send_packet(s, p, b.len);
	s->pw_sent = 1; s->pw_tries++;
	crypto_wipe(s->password, sizeof s->password);
}

/* choose and send the next auth method based on the server's allowed list */
static void auth_next(ssh_t *s, const uint8_t *methods, uint32_t mlen){
	if(s->have_key && !s->tried_pubkey && namelist_has(methods, mlen, "publickey")){
		send_auth_pubkey(s); return;
	}
	if(namelist_has(methods, mlen, "password")){
		if(s->pw_tries >= 3){ fail(s, "authentication failed"); return; }
		if(s->password[0]){ send_auth_password(s); return; }
		s->want_password = 1;   /* ask the app; it calls ssh_auth_password() */
		set_state(s, SSH_ST_AUTH, "password required");
		return;
	}
	fail(s, "no supported auth method");
}

/* ---- channel ---- */
static void open_channel(ssh_t *s){
	uint8_t p[64]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_OPEN); wb_cstr(&b, "session");
	wb_u32(&b, 0);              /* our channel id */
	wb_u32(&b, SSH_WINDOW);
	wb_u32(&b, SSH_CHAN_MAX);
	send_packet(s, p, b.len);
	s->local_win = SSH_WINDOW;
	set_state(s, SSH_ST_CHANNEL, "opening channel");
}
static void send_ptyreq(ssh_t *s){
	uint8_t p[128]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_REQUEST); wb_u32(&b, s->remote_chan);
	wb_cstr(&b, "pty-req"); wb_byte(&b, 1);   /* want_reply */
	wb_cstr(&b, "xterm-256color");
	wb_u32(&b, (uint32_t)s->cols); wb_u32(&b, (uint32_t)s->rows);
	wb_u32(&b, (uint32_t)(s->cols * 6)); wb_u32(&b, (uint32_t)(s->rows * 12));
	uint8_t modes[6] = { 42, 0,0,0,1, 0 };    /* IUTF8=1, TTY_OP_END */
	wb_str(&b, modes, 6);
	send_packet(s, p, b.len);
	s->want_ptyreq = 1;
}
static void send_shell(ssh_t *s){
	uint8_t p[32]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_REQUEST); wb_u32(&b, s->remote_chan);
	wb_cstr(&b, "shell"); wb_byte(&b, 1);
	send_packet(s, p, b.len);
	s->want_shell = 1;
}
static void send_window_adjust(ssh_t *s, uint32_t add){
	uint8_t p[16]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_WINDOW_ADJUST); wb_u32(&b, s->remote_chan); wb_u32(&b, add);
	send_packet(s, p, b.len);
	s->local_win += add;
}
static void send_channel_close(ssh_t *s){
	uint8_t p[8]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_CLOSE); wb_u32(&b, s->remote_chan);
	send_packet(s, p, b.len);
}

/* ---- dispatch one decrypted payload ---- */
static void dispatch(ssh_t *s, const uint8_t *pl, int len){
	if(len < 1) return;
	uint8_t msg = pl[0];
	switch(msg){
		case MSG_DISCONNECT: {
			rbuf r; rb_init(&r, pl+1, len-1); uint32_t code;
			const uint8_t *m; uint32_t ml;
			char buf[96] = "server disconnected";
			if(rb_u32(&r,&code)==0 && rb_str(&r,&m,&ml)==0){
				int k = 0; buf[k++]='s';buf[k++]='e';buf[k++]='r';buf[k++]='v';buf[k++]='e';buf[k++]='r';buf[k++]=':';buf[k++]=' ';
				for(uint32_t i=0;i<ml && k<(int)sizeof(buf)-1;i++) buf[k++]=(char)m[i];
				buf[k]=0;
			}
			fail(s, buf);
			break;
		}
		case MSG_IGNORE: case MSG_DEBUG: case MSG_UNIMPLEMENTED: case MSG_EXT_INFO:
			break;
		case MSG_KEXINIT:
			if(len > (int)sizeof s->i_s){ fail(s, "KEXINIT too large"); break; }
			memcpy(s->i_s, pl, len); s->i_s_len = len;
			if(check_kexinit(s) != 0) break;
			if(!s->kex_sent) send_kexinit(s);   /* rekey initiated by server */
			send_ecdh_init(s);
			break;
		case MSG_KEX_ECDH_REPLY:
			handle_ecdh_reply(s, pl, len);
			break;
		case MSG_NEWKEYS:
			s->enc_in = 1;
			s->kex_sent = 0;   /* ready for a future rekey */
			break;
		case MSG_SERVICE_ACCEPT:
			send_auth_none(s);
			set_state(s, SSH_ST_AUTH, "authenticating");
			break;
		case MSG_USERAUTH_SUCCESS:
			s->want_password = 0;
			open_channel(s);
			break;
		case MSG_USERAUTH_FAILURE: {
			rbuf r; rb_init(&r, pl+1, len-1);
			const uint8_t *m; uint32_t ml; uint8_t partial = 0;
			if(rb_str(&r, &m, &ml) == 0){ rb_byte(&r, &partial); auth_next(s, m, ml); }
			else fail(s, "auth failed");
			break;
		}
		case MSG_USERAUTH_BANNER:
			break;
		case MSG_GLOBAL_REQUEST: {
			rbuf r; rb_init(&r, pl+1, len-1);
			const uint8_t *nm; uint32_t nl; uint8_t want_reply = 0;
			if(rb_str(&r, &nm, &nl) == 0 && rb_byte(&r, &want_reply) == 0 && want_reply){
				uint8_t f = MSG_REQUEST_FAILURE; send_packet(s, &f, 1);
			}
			break;
		}
		case MSG_REQUEST_SUCCESS: case MSG_REQUEST_FAILURE:
			s->keepalive_pending = 0;   /* any reply proves the link is alive */
			break;
		case MSG_CHANNEL_OPEN_CONFIRMATION: {
			rbuf r; rb_init(&r, pl+1, len-1);
			uint32_t rc, sc, win, mp;
			if(rb_u32(&r,&rc)||rb_u32(&r,&sc)||rb_u32(&r,&win)||rb_u32(&r,&mp)){ fail(s,"bad channel open"); break; }
			s->remote_chan = sc; s->remote_win = win; s->remote_maxpkt = mp; s->chan_opened = 1;
			send_ptyreq(s);
			break;
		}
		case MSG_CHANNEL_OPEN_FAILURE:
			fail(s, "server refused channel");
			break;
		case MSG_CHANNEL_WINDOW_ADJUST: {
			rbuf r; rb_init(&r, pl+1, len-1); uint32_t ch, add;
			if(rb_u32(&r,&ch)==0 && rb_u32(&r,&add)==0) s->remote_win += add;
			break;
		}
		case MSG_CHANNEL_DATA: {
			rbuf r; rb_init(&r, pl+1, len-1); uint32_t ch;
			const uint8_t *d; uint32_t dl;
			if(rb_u32(&r,&ch)==0 && rb_str(&r,&d,&dl)==0){
				if(s->cb.on_channel_data) s->cb.on_channel_data(d, (int)dl, s->cb.ud);
				if(s->local_win >= dl) s->local_win -= dl; else s->local_win = 0;
			}
			break;
		}
		case MSG_CHANNEL_EXTENDED_DATA: {
			rbuf r; rb_init(&r, pl+1, len-1); uint32_t ch, type;
			const uint8_t *d; uint32_t dl;
			if(rb_u32(&r,&ch)==0 && rb_u32(&r,&type)==0 && rb_str(&r,&d,&dl)==0){
				if(s->cb.on_channel_data) s->cb.on_channel_data(d, (int)dl, s->cb.ud);
				if(s->local_win >= dl) s->local_win -= dl; else s->local_win = 0;
			}
			break;
		}
		case MSG_CHANNEL_EOF:
			break;
		case MSG_CHANNEL_CLOSE:
			send_channel_close(s);
			set_state(s, SSH_ST_CLOSED, "connection closed");
			break;
		case MSG_CHANNEL_REQUEST: {
			rbuf r; rb_init(&r, pl+1, len-1); uint32_t ch;
			const uint8_t *rq; uint32_t rql; uint8_t want_reply = 0;
			if(rb_u32(&r,&ch)==0 && rb_str(&r,&rq,&rql)==0){
				rb_byte(&r, &want_reply);
				if(rql == 11 && memcmp(rq, "exit-status", 11) == 0){
					uint32_t code; if(rb_u32(&r, &code) == 0) s->exit_status = (int)code;
				}
			}
			break;
		}
		case MSG_CHANNEL_SUCCESS:
			if(s->want_ptyreq){ s->want_ptyreq = 0; send_shell(s); }
			else if(s->want_shell){ s->want_shell = 0; set_state(s, SSH_ST_RUNNING, "connected"); }
			break;
		case MSG_CHANNEL_FAILURE:
			if(s->want_ptyreq) fail(s, "pty request failed");
			else if(s->want_shell) fail(s, "shell request failed");
			break;
		default:
			break;
	}
}

/* ---- decrypt / frame incoming packets from the rx accumulator ---- */
static int one_packet(ssh_t *s){    /* returns 1 if a packet was processed, 0 if need more, -1 error */
	if(!s->enc_in){
		if(s->rx_len < 4) return 0;
		uint32_t plen = ((uint32_t)s->rx[0]<<24)|((uint32_t)s->rx[1]<<16)|((uint32_t)s->rx[2]<<8)|s->rx[3];
		if(plen < 2 || plen > SSH_RXBUF - 4){ fail(s, "bad packet length"); return -1; }
		int total = 4 + (int)plen;
		if(s->rx_len < total) return 0;
		uint8_t pad = s->rx[4];
		int paylen = (int)plen - pad - 1;
		if(paylen < 0){ fail(s, "bad padding"); return -1; }
		dispatch(s, s->rx + 5, paylen);
		s->seq_in++;
		memmove(s->rx, s->rx + total, s->rx_len - total); s->rx_len -= total;
		return 1;
	} else {
		if(s->rx_len < 4) return 0;
		const uint8_t *km, *kh; chacha_keys(s->key_s2c, &km, &kh);
		uint8_t nonce[8]; seq_nonce(s->seq_in, nonce);
		uint8_t len_be[4];
		crypto_chacha20_djb(len_be, s->rx, 4, kh, nonce, 0);
		uint32_t plen = ((uint32_t)len_be[0]<<24)|((uint32_t)len_be[1]<<16)|((uint32_t)len_be[2]<<8)|len_be[3];
		if(plen < 2 || plen > SSH_RXBUF - 20){ fail(s, "bad encrypted length"); return -1; }
		int total = 4 + (int)plen + 16;
		if(s->rx_len < total) return 0;
		/* verify tag over enc_len(4) || enc_payload(plen) */
		uint8_t poly_key[64], zero[64]; memset(zero, 0, 64);
		crypto_chacha20_djb(poly_key, zero, 64, km, nonce, 0);
		uint8_t want[16];
		crypto_poly1305(want, s->rx, 4 + plen, poly_key);
		crypto_wipe(poly_key, 64);
		if(crypto_verify16(want, s->rx + 4 + plen) != 0){ fail(s, "MAC verification failed"); return -1; }
		/* decrypt payload */
		crypto_chacha20_djb(s->pkt, s->rx + 4, plen, km, nonce, 1);
		uint8_t pad = s->pkt[0];
		int paylen = (int)plen - pad - 1;
		if(paylen < 0){ fail(s, "bad padding"); return -1; }
		dispatch(s, s->pkt + 1, paylen);
		s->seq_in++;
		memmove(s->rx, s->rx + total, s->rx_len - total); s->rx_len -= total;
		return 1;
	}
}

/* ---- banner exchange ---- */
static int consume_banner(ssh_t *s){   /* returns 1 once V_S captured */
	while(s->rx_len > 0){
		uint8_t c = s->rx[0];
		memmove(s->rx, s->rx + 1, s->rx_len - 1); s->rx_len--;
		if(c == '\n'){
			/* end of a line: strip trailing \r */
			int L = s->line_len; if(L > 0 && s->line[L-1] == '\r') L--;
			s->line[L] = 0;
			if(L >= 4 && memcmp(s->line, "SSH-", 4) == 0){
				if(L > (int)sizeof s->v_s - 1) L = sizeof s->v_s - 1;
				memcpy(s->v_s, s->line, L); s->v_s_len = L; s->v_s[L] = 0;
				s->got_banner = 1; s->line_len = 0;
				return 1;
			}
			s->line_len = 0;   /* preamble line, discard */
		} else if(s->line_len < (int)sizeof s->line - 1){
			s->line[s->line_len++] = (char)c;
		}
	}
	return 0;
}

/* ---- public API ---- */
ssh_t *ssh_create(const ssh_cb_t *cb, void *(*alloc)(size_t), void (*dealloc)(void *)){
	ssh_t *s = (ssh_t*)alloc(sizeof(ssh_t));
	if(!s) return NULL;
	memset(s, 0, sizeof *s);
	s->cb = *cb; s->alloc = alloc; s->dealloc = dealloc;
	s->state = SSH_ST_BANNER;
	s->exit_status = -1;
	s->cols = 53; s->rows = 26;
	return s;
}
void ssh_destroy(ssh_t *s){
	if(!s) return;
	void (*df)(void*) = s->dealloc;
	crypto_wipe(s->key_c2s, 64); crypto_wipe(s->key_s2c, 64);
	crypto_wipe(s->seed, 32); crypto_wipe(s->x_priv, 32);
	crypto_wipe(s, sizeof *s);
	df(s);
}
void ssh_set_key(ssh_t *s, const uint8_t seed[32]){ memcpy(s->seed, seed, 32); s->have_key = 1; }

void ssh_start(ssh_t *s, const char *user){
	strncpy(s->user, user, sizeof s->user - 1);
	s->last_rx_ms = s->cb.now_ms ? s->cb.now_ms(s->cb.ud) : 0;
	s->keepalive_at = s->last_rx_ms + KEEPALIVE_MS;
	/* send our identification string */
	const char *id = SSH_IDENT "\r\n";
	if(tx_all(s, (const uint8_t*)id, (int)strlen(id)) < 0) return;
	send_kexinit(s);
	set_state(s, SSH_ST_BANNER, "connecting");
}

void ssh_input(ssh_t *s, const uint8_t *data, int n){
	if(s->state == SSH_ST_ERROR || s->state == SSH_ST_CLOSED) return;
	if(s->cb.now_ms) s->last_rx_ms = s->cb.now_ms(s->cb.ud);
	int off = 0;
	while(off < n){
		int space = SSH_RXBUF - s->rx_len;
		if(space <= 0){
			/* try to drain before giving up */
			int prog = 0, r;
			while((r = one_packet(s)) == 1) prog = 1;
			if(r < 0) return;
			if(!prog){ fail(s, "rx overflow"); return; }
			continue;
		}
		int take = n - off; if(take > space) take = space;
		memcpy(s->rx + s->rx_len, data + off, take);
		s->rx_len += take; off += take;

		if(!s->got_banner){ if(!consume_banner(s)) continue; }
		int r; while((r = one_packet(s)) == 1){}
		if(r < 0) return;
	}
}

void ssh_auth_password(ssh_t *s, const char *pw){
	strncpy(s->password, pw, sizeof s->password - 1);
	s->password[sizeof s->password - 1] = 0;
	s->want_password = 0;
	if(s->state == SSH_ST_AUTH) send_auth_password(s);
}
int ssh_wants_password(ssh_t *s){ return s->want_password; }

int ssh_send_channel(ssh_t *s, const uint8_t *buf, int n){
	if(s->state != SSH_ST_RUNNING) return 0;
	int sent = 0;
	while(sent < n){
		int chunk = n - sent;
		if(chunk > SSH_SENDCAP) chunk = SSH_SENDCAP;
		if((uint32_t)chunk > s->remote_win) chunk = (int)s->remote_win;
		if((uint32_t)chunk > s->remote_maxpkt) chunk = (int)s->remote_maxpkt;
		if(chunk <= 0) break;   /* remote window full — caller retries later */
		uint8_t p[SSH_SENDCAP + 16]; wbuf b; wb_init(&b, p, sizeof p);
		wb_byte(&b, MSG_CHANNEL_DATA); wb_u32(&b, s->remote_chan);
		wb_str(&b, buf + sent, chunk);
		if(send_packet(s, p, b.len) < 0) break;
		s->remote_win -= chunk; sent += chunk;
	}
	return sent;
}
void ssh_consumed(ssh_t *s, int n){
	s->consumed += (uint32_t)n;
	if(s->consumed >= SSH_WINDOW / 2 && s->chan_opened && s->state == SSH_ST_RUNNING){
		send_window_adjust(s, s->consumed);
		s->consumed = 0;
	}
}
void ssh_tick(ssh_t *s){
	if(s->state != SSH_ST_RUNNING || !s->cb.now_ms) return;
	uint32_t now = s->cb.now_ms(s->cb.ud);
	if((int32_t)(now - s->keepalive_at) >= 0){
		if(s->keepalive_pending && (int32_t)(now - s->last_rx_ms) > DEAD_MS){
			fail(s, "connection lost"); return;
		}
		uint8_t p[64]; wbuf b; wb_init(&b, p, sizeof p);
		wb_byte(&b, MSG_GLOBAL_REQUEST); wb_cstr(&b, "keepalive@openssh.com"); wb_byte(&b, 1);
		send_packet(s, p, b.len);
		s->keepalive_pending = 1;
		s->keepalive_at = now + KEEPALIVE_MS;
	}
}
void ssh_window_change(ssh_t *s, int cols, int rows){
	s->cols = cols; s->rows = rows;
	if(s->state != SSH_ST_RUNNING) return;
	uint8_t p[64]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_CHANNEL_REQUEST); wb_u32(&b, s->remote_chan);
	wb_cstr(&b, "window-change"); wb_byte(&b, 0);
	wb_u32(&b, cols); wb_u32(&b, rows); wb_u32(&b, cols*6); wb_u32(&b, rows*12);
	send_packet(s, p, b.len);
}
void ssh_disconnect(ssh_t *s, const char *msg){
	if(s->state == SSH_ST_ERROR || s->state == SSH_ST_CLOSED) return;
	uint8_t p[128]; wbuf b; wb_init(&b, p, sizeof p);
	wb_byte(&b, MSG_DISCONNECT); wb_u32(&b, 11 /* by application */);
	wb_cstr(&b, msg ? msg : ""); wb_cstr(&b, "");
	send_packet(s, p, b.len);
	set_state(s, SSH_ST_CLOSED, "disconnected");
}

ssh_state_t ssh_state(ssh_t *s){ return s->state; }
const char *ssh_error(ssh_t *s){ return s->err; }
int ssh_exit_status(ssh_t *s){ return s->exit_status; }
