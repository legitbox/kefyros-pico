/* tools/sshtest/crypto_test.c — verify the crypto foundation for the SSH core:
 * SHA-256 KAT, Monocypher linkage, x25519 ECDH agreement, ed25519 sign/verify,
 * and the chacha20-poly1305@openssh.com packet AEAD round-trip. */
#include "sha256.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <string.h>

static int checks = 0, fails = 0;
#define CHECK(c, m) do{ checks++; if(!(c)){ fails++; printf("  FAIL: %s\n", m);} }while(0)

static void hex(const uint8_t *b, int n, char *o){ static const char*h="0123456789abcdef";
	for(int i=0;i<n;i++){ o[i*2]=h[b[i]>>4]; o[i*2+1]=h[b[i]&15]; } o[n*2]=0; }

/* --- chacha20-poly1305@openssh.com, per OpenSSH PROTOCOL.chacha20poly1305 --- */
/* keys: 64 bytes; K_main = key[0..31], K_hdr = key[32..63]. seq = 32-bit. */
static void ssh_chachapoly_seal(const uint8_t key[64], uint32_t seq,
                                const uint8_t *plain, int plen,   /* padlen||payload||padding, len prefix separate */
                                const uint8_t enc_len[4],         /* the 4 length bytes, already big-endian */
                                uint8_t *out_len4, uint8_t *out_payload, uint8_t tag[16]){
	uint8_t nonce[8] = {0,0,0,0, (uint8_t)(seq>>24),(uint8_t)(seq>>16),(uint8_t)(seq>>8),(uint8_t)seq};
	const uint8_t *K_main = key;
	const uint8_t *K_hdr  = key + 32;
	/* poly key = first 32 bytes of chacha(K_main, nonce, ctr=0) */
	uint8_t poly_key[64]; uint8_t zero[64] = {0};
	crypto_chacha20_djb(poly_key, zero, 64, K_main, nonce, 0);
	/* length field with K_hdr, ctr 0 */
	crypto_chacha20_djb(out_len4, enc_len, 4, K_hdr, nonce, 0);
	/* payload with K_main, ctr 1 */
	crypto_chacha20_djb(out_payload, plain, plen, K_main, nonce, 1);
	/* tag over enc_len4 || enc_payload */
	uint8_t macbuf[4 + 4096];
	memcpy(macbuf, out_len4, 4);
	memcpy(macbuf + 4, out_payload, plen);
	crypto_poly1305(tag, macbuf, 4 + plen, poly_key);
}
static int ssh_chachapoly_open(const uint8_t key[64], uint32_t seq,
                               const uint8_t *enc_len4, const uint8_t *enc_payload, int plen,
                               const uint8_t tag[16], uint8_t *out_plain, uint32_t *out_len){
	uint8_t nonce[8] = {0,0,0,0, (uint8_t)(seq>>24),(uint8_t)(seq>>16),(uint8_t)(seq>>8),(uint8_t)seq};
	const uint8_t *K_main = key; const uint8_t *K_hdr = key + 32;
	uint8_t poly_key[64]; uint8_t zero[64] = {0};
	crypto_chacha20_djb(poly_key, zero, 64, K_main, nonce, 0);
	/* verify tag first */
	uint8_t macbuf[4 + 4096]; memcpy(macbuf, enc_len4, 4); memcpy(macbuf + 4, enc_payload, plen);
	uint8_t want[16]; crypto_poly1305(want, macbuf, 4 + plen, poly_key);
	if(crypto_verify16(want, tag) != 0) return -1;
	/* decrypt length (K_hdr, ctr0) then payload (K_main, ctr1) */
	uint8_t len4[4]; crypto_chacha20_djb(len4, enc_len4, 4, K_hdr, nonce, 0);
	*out_len = ((uint32_t)len4[0]<<24)|((uint32_t)len4[1]<<16)|((uint32_t)len4[2]<<8)|len4[3];
	crypto_chacha20_djb(out_plain, enc_payload, plen, K_main, nonce, 1);
	return 0;
}

int main(void){
	char hb[128];

	/* --- SHA-256 KAT: "abc" --- */
	{
		uint8_t d[32]; sha256("abc", 3, d); hex(d, 32, hb);
		CHECK(strcmp(hb, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")==0, "sha256(abc) KAT");
	}
	/* --- SHA-256 KAT: empty --- */
	{
		uint8_t d[32]; sha256("", 0, d); hex(d, 32, hb);
		CHECK(strcmp(hb, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")==0, "sha256() KAT");
	}
	/* --- SHA-256 multi-block (1000 'a') --- */
	{
		sha256_ctx c; sha256_init(&c);
		for(int i=0;i<1000;i++) sha256_update(&c,"a",1);
		uint8_t d[32]; sha256_final(&c,d); hex(d,32,hb);
		CHECK(strcmp(hb, "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3")==0, "sha256(1000*a) streaming KAT");
	}

	/* --- x25519 ECDH agreement --- */
	{
		uint8_t a_sk[32], b_sk[32], a_pk[32], b_pk[32], ss1[32], ss2[32];
		for(int i=0;i<32;i++){ a_sk[i]=(uint8_t)(i+1); b_sk[i]=(uint8_t)(200-i); }
		crypto_x25519_public_key(a_pk, a_sk);
		crypto_x25519_public_key(b_pk, b_sk);
		crypto_x25519(ss1, a_sk, b_pk);
		crypto_x25519(ss2, b_sk, a_pk);
		CHECK(memcmp(ss1, ss2, 32)==0, "x25519 shared secret agrees");
	}

	/* --- ed25519 sign/verify --- */
	{
		uint8_t seed[32], sk[64], pk[32], sig[64];
		for(int i=0;i<32;i++) seed[i]=(uint8_t)(0x42+i);
		crypto_ed25519_key_pair(sk, pk, seed);
		const char *msg = "kefyros exchange hash";
		crypto_ed25519_sign(sig, sk, (const uint8_t*)msg, strlen(msg));
		CHECK(crypto_ed25519_check(sig, pk, (const uint8_t*)msg, strlen(msg))==0, "ed25519 verify good sig");
		sig[0] ^= 1;
		CHECK(crypto_ed25519_check(sig, pk, (const uint8_t*)msg, strlen(msg))!=0, "ed25519 rejects bad sig");
	}

	/* --- chacha20-poly1305@openssh round trip --- */
	{
		uint8_t key[64]; for(int i=0;i<64;i++) key[i]=(uint8_t)(i*7+1);
		uint8_t payload[40]; for(int i=0;i<40;i++) payload[i]=(uint8_t)(i);
		uint8_t enc_len[4] = {0,0,0,40};
		uint8_t clen[4], cpay[40], tag[16];
		ssh_chachapoly_seal(key, 3, payload, 40, enc_len, clen, cpay, tag);
		uint8_t dpay[40]; uint32_t dlen;
		int ok = ssh_chachapoly_open(key, 3, clen, cpay, 40, tag, dpay, &dlen);
		CHECK(ok==0, "openssh AEAD tag verifies");
		CHECK(dlen==40, "openssh AEAD length decrypts");
		CHECK(memcmp(dpay, payload, 40)==0, "openssh AEAD payload round-trips");
		/* tamper -> reject */
		cpay[5] ^= 0x80;
		CHECK(ssh_chachapoly_open(key, 3, clen, cpay, 40, tag, dpay, &dlen)!=0, "openssh AEAD rejects tampered ciphertext");
	}

	printf("\n%d/%d checks passed (%d failed)\n", checks-fails, checks, fails);
	return fails ? 1 : 0;
}
