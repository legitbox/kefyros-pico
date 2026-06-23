// port/mbedtls_config.h — mbedtls config for the Spineko browser's HTTPS support.
// Based on the pico-sdk kitchen_sink TLS-client config (known to compile + run on
// RP2350). TLS 1.2 client only. NOTE: the browser sets authmode = NONE (no cert
// verification — a deliberate, accepted decision), but we keep X.509 parsing in so
// the handshake can still consume the server's certificate chain. Time/RTC-based
// validity checks are intentionally omitted (MBEDTLS_HAVE_TIME off) since we don't
// verify anyway and have no trusted wall clock at handshake time.
#ifndef KF_MBEDTLS_CONFIG_H
#define KF_MBEDTLS_CONFIG_H

/* some mbedtls sources use INT_MAX without including limits.h */
#include <limits.h>

/* entropy: no OS entropy source; use the RP2350 hardware RNG via the SDK alt. */
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_ENTROPY_HARDWARE_ALT

/* keep TLS record buffers small-ish; IN stays large so big cert chains fit. */
#define MBEDTLS_SSL_OUT_CONTENT_LEN    2048
#define MBEDTLS_SSL_IN_CONTENT_LEN     16384

#define MBEDTLS_ALLOW_PRIVATE_ACCESS

/* The pico_lwip altcp-TLS glue touches mbedtls_ssl_session.start unconditionally,
   which only exists with MBEDTLS_HAVE_TIME. We don't verify certs, so the actual
   wall-clock value is irrelevant — but the field/symbols must exist. The SDK
   (pico_mbedtls.c) supplies mbedtls_ms_time() for MBEDTLS_PLATFORM_MS_TIME_ALT. */
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_PLATFORM_MS_TIME_ALT

#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_SHA256_SMALLER
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_AES_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_CTR_DRBG_C
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_ERROR_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_OID_C
#define MBEDTLS_PKCS5_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_RSA_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA512_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_AES_FEWER_TABLES

/* TLS 1.2 + ECDHE/GCM */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_GCM_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ASN1_WRITE_C

#endif /* KF_MBEDTLS_CONFIG_H */
