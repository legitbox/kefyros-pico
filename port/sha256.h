// port/sha256.h — compact SHA-256 (FIPS 180-4) for the Term SSH client.
// Self-contained (no platform deps) so port/ssh.c stays host-testable and the
// exact same code runs on device. Monocypher ships blake2b/sha512 but not
// sha256, which SSH's exchange hash + KDF require.
#ifndef KF_SHA256_H
#define KF_SHA256_H
#include <stdint.h>
#include <stddef.h>

typedef struct {
	uint32_t state[8];
	uint64_t bitlen;
	uint8_t  buf[64];
	size_t   buflen;
} sha256_ctx;

void sha256_init(sha256_ctx *c);
void sha256_update(sha256_ctx *c, const void *data, size_t len);
void sha256_final(sha256_ctx *c, uint8_t out[32]);
void sha256(const void *data, size_t len, uint8_t out[32]);

#endif /* KF_SHA256_H */
