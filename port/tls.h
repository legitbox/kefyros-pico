// port/tls.h — BearSSL TLS 1.2 client glue for the Spineko browser (HTTPS).
//
// Thin wrapper around a single BearSSL client engine. port/http.c drives the engine
// directly (sendrec/recvrec/sendapp/recvapp) over its raw-lwIP TCP connection; this
// module just owns the engine + I/O buffer, installs an accept-all X.509 handler
// (no cert verification — deliberate, see http.c), and seeds the PRNG from the RP2350
// hardware TRNG. One connection at a time, matching the HTTP client.
//
// Why BearSSL and not mbedtls: the mbedtls attempt died on a deterministic, voltage-
// independent ECDSA ServerKeyExchange verify failure (-0x4E00 / ECP_VERIFY_FAILED) that
// we could never root-cause — RSA-cert sites worked, ECDSA-cert sites (most of the web)
// did not, with provably-correct inputs. BearSSL is an independent EC/X.509 codebase
// (the engine behind Arduino-Pico's WiFiClientSecure on this silicon), so it sidesteps
// whatever that mbedtls build got wrong.
#ifndef KF_TLS_H
#define KF_TLS_H

#include "bearssl.h"

/* Reset the engine for a fresh handshake to `host` (used for SNI). Call once per
   https request, right after the TCP connection is established. Seeds entropy,
   installs the accept-all X.509 handler, pins TLS 1.2. */
void kf_tls_begin(const char *host);

/* The live BearSSL engine, so http.c can run the sendrec/recvrec/sendapp/recvapp
   pump against it. Valid after kf_tls_begin(). */
br_ssl_engine_context *kf_tls_eng(void);

#endif /* KF_TLS_H */
