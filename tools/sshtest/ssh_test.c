/* tools/sshtest/ssh_test.c — drive port/ssh.c through a REAL handshake against a
 * live sshd (or any OpenSSH server) over a blocking TCP socket, then run a
 * command and check the echoed output.
 *
 * Usage:  ./ssh_test <host> <port> <user> [password]
 *   - password given  -> password auth
 *   - no password     -> publickey auth using ./sshkey.seed (32 raw bytes);
 *                        generate one + print the .pub with:  ./ssh_test --keygen
 *
 * This is host-only (POSIX sockets); the device uses port/ssh_tcp.c instead. */
#define _GNU_SOURCE
#include "ssh.h"
#include "monocypher.h"
#include "monocypher-ed25519.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>

/* ---- callbacks ---- */
static int g_sock = -1;
static int cb_tx(const uint8_t *b, int n, void *ud){ (void)ud;
	int off = 0;
	while(off < n){ int w = send(g_sock, b + off, n - off, 0); if(w <= 0) return -1; off += w; }
	return n;
}
static void cb_rng(uint8_t *b, int n, void *ud){ (void)ud;
	FILE *f = fopen("/dev/urandom", "rb"); if(f){ fread(b, 1, n, f); fclose(f); }
}
static uint32_t cb_now(void *ud){ (void)ud;
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}
static int g_hostkey_seen = 0;
static int cb_hostkey(const uint8_t pub[32], const char *fp, void *ud){ (void)pub;(void)ud;
	printf("  host key: %s\n", fp); g_hostkey_seen = 1; return 1;   /* TOFU: accept */
}
static char g_out[1<<20]; static int g_outlen = 0;
static void cb_data(const uint8_t *b, int n, void *ud){ (void)ud;
	for(int i = 0; i < n && g_outlen < (int)sizeof g_out - 1; i++) g_out[g_outlen++] = (char)b[i];
	g_out[g_outlen] = 0;
}
static ssh_state_t g_state = SSH_ST_BANNER;
static void cb_state(ssh_state_t st, const char *d, void *ud){ (void)ud;
	g_state = st; printf("  [state] %d %s\n", st, d ? d : "");
}

static int checks = 0, fails = 0;
#define CHECK(c,m) do{ checks++; if(!(c)){ fails++; printf("  FAIL: %s\n", m);} else printf("  ok: %s\n", m);}while(0)

/* generate a key seed + print an OpenSSH authorized_keys line */
static void b64(const uint8_t *in, int n, char *out){
	static const char T[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	int o=0; for(int i=0;i<n;i+=3){ uint32_t v=in[i]<<16; if(i+1<n)v|=in[i+1]<<8; if(i+2<n)v|=in[i+2];
		out[o++]=T[(v>>18)&63]; out[o++]=T[(v>>12)&63];
		out[o++]=(i+1<n)?T[(v>>6)&63]:'='; out[o++]=(i+2<n)?T[v&63]:'='; } out[o]=0;
}
static void keygen(void){
	uint8_t seed[32]; cb_rng(seed, 32, NULL);
	FILE *f = fopen("sshkey.seed", "wb"); fwrite(seed, 1, 32, f); fclose(f);
	uint8_t sk[64], pub[32], s2[32]; memcpy(s2, seed, 32);
	crypto_ed25519_key_pair(sk, pub, s2);
	uint8_t blob[64]; int bl = 0;
	/* string "ssh-ed25519" + string pub */
	const char *t = "ssh-ed25519"; int tl = 11;
	blob[bl++]=0;blob[bl++]=0;blob[bl++]=0;blob[bl++]=tl; memcpy(blob+bl,t,tl); bl+=tl;
	blob[bl++]=0;blob[bl++]=0;blob[bl++]=0;blob[bl++]=32; memcpy(blob+bl,pub,32); bl+=32;
	char b[256]; b64(blob, bl, b);
	printf("wrote sshkey.seed\nssh-ed25519 %s kefyros@picocalc\n", b);
}

int main(int argc, char **argv){
	if(argc >= 2 && strcmp(argv[1], "--keygen") == 0){ keygen(); return 0; }
	if(argc < 4){ fprintf(stderr, "usage: %s <host> <port> <user> [password]\n", argv[0]); return 2; }
	const char *host = argv[1]; int port = atoi(argv[2]); const char *user = argv[3];
	const char *pw = (argc >= 5) ? argv[4] : NULL;

	/* connect */
	struct addrinfo hints, *res; memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	char ports[16]; snprintf(ports, sizeof ports, "%d", port);
	if(getaddrinfo(host, ports, &hints, &res) != 0){ perror("getaddrinfo"); return 1; }
	g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(connect(g_sock, res->ai_addr, res->ai_addrlen) != 0){ perror("connect"); return 1; }
	freeaddrinfo(res);

	ssh_cb_t cb = { cb_tx, cb_rng, cb_now, cb_hostkey, cb_data, cb_state, NULL };
	ssh_t *s = ssh_create(&cb, malloc, free);

	/* key auth if no password given */
	if(!pw){
		FILE *f = fopen("sshkey.seed", "rb");
		if(!f){ fprintf(stderr, "no password and no sshkey.seed (run --keygen)\n"); return 2; }
		uint8_t seed[32]; if(fread(seed, 1, 32, f) != 32){ fprintf(stderr,"bad seed\n"); return 2; } fclose(f);
		ssh_set_key(s, seed);
	}

	ssh_start(s, user);

	/* pump */
	uint8_t buf[4096];
	int sent_cmd = 0;
	time_t deadline = time(NULL) + 20;
	fcntl(g_sock, F_SETFL, O_NONBLOCK);
	while(time(NULL) < deadline){
		int n = recv(g_sock, buf, sizeof buf, 0);
		if(n > 0){ ssh_input(s, buf, n); }
		else if(n == 0){ break; }
		ssh_tick(s);

		if(ssh_wants_password(s) && pw) ssh_auth_password(s, pw);

		if(ssh_state(s) == SSH_ST_RUNNING && !sent_cmd){
			const char *cmd = "echo KEFYROS_MARKER_$((6*7))\nexit\n";
			ssh_send_channel(s, (const uint8_t*)cmd, (int)strlen(cmd));
			sent_cmd = 1;
		}
		if(ssh_state(s) == SSH_ST_ERROR || ssh_state(s) == SSH_ST_CLOSED) break;
		ssh_consumed(s, 0);
		usleep(2000);
	}

	printf("\n--- captured shell output (%d bytes) ---\n%.*s\n---\n", g_outlen,
	       g_outlen > 400 ? 400 : g_outlen, g_out);

	CHECK(g_hostkey_seen, "host key presented + fingerprinted");
	CHECK(ssh_state(s) == SSH_ST_RUNNING || ssh_state(s) == SSH_ST_CLOSED, "reached shell (RUNNING/CLOSED)");
	CHECK(strstr(g_out, "KEFYROS_MARKER_42") != NULL, "shell executed command (echo 6*7=42)");
	if(ssh_state(s) == SSH_ST_ERROR) printf("  error: %s\n", ssh_error(s));

	ssh_destroy(s);
	close(g_sock);
	printf("\n%d/%d checks passed (%d failed)\n", checks - fails, checks, fails);
	return fails ? 1 : 0;
}
