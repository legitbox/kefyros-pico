/* tools/sshtest/shop_test.c — end-to-end proof that the Term pipeline (port/ssh.c +
 * port/vt100.c) can log into terminal.shop with an EPHEMERAL ed25519 key and render
 * its TUI. Mirrors apps/term.c run_ssh_session(): channel data -> vt_feed, and the
 * emulator's answerback (DSR/CPR, DA) is routed back over the SSH channel — which is
 * exactly what unblocks a bubbletea TUI's "query terminal then wait" startup.
 *
 * Usage: ./shop_test [host] [port] [user]   (defaults: terminal.shop 22 anon)
 * Host-only (POSIX sockets); the device uses port/ssh_tcp.c. */
#define _GNU_SOURCE
#include "ssh.h"
#include "vt100.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netdb.h>

static int   g_sock = -1;
static ssh_t *g_ssh = NULL;
static vt_t  *g_vt  = NULL;

static int cb_tx(const uint8_t *b, int n, void *ud){ (void)ud;
	int off = 0; while(off < n){ int w = send(g_sock, b + off, n - off, 0); if(w <= 0) return -1; off += w; } return n;
}
static void cb_rng(uint8_t *b, int n, void *ud){ (void)ud;
	FILE *f = fopen("/dev/urandom", "rb"); if(f){ if(fread(b, 1, n, f)!=(size_t)n){} fclose(f); }
}
static uint32_t cb_now(void *ud){ (void)ud;
	struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (uint32_t)(ts.tv_sec*1000 + ts.tv_nsec/1000000);
}
static int cb_hostkey(const uint8_t pub[32], const char *fp, void *ud){ (void)pub;(void)ud;
	printf("  host key: %s\n", fp); return 1;
}
static long g_bytes = 0;
static void cb_data(const uint8_t *b, int n, void *ud){ (void)ud;
	g_bytes += n; vt_feed(g_vt, b, n); ssh_consumed(g_ssh, n);
}
static void cb_state(ssh_state_t st, const char *d, void *ud){ (void)ud; printf("  [state] %d %s\n", st, d?d:""); }

/* emulator answerback -> back over the channel (the critical bit for TUIs) */
static void vt_ans(const uint8_t *b, int n, void *ud){ (void)ud;
	if(g_ssh && ssh_state(g_ssh) == SSH_ST_RUNNING) ssh_send_channel(g_ssh, b, n);
}

int main(int argc, char **argv){
	const char *host = argc > 1 ? argv[1] : "terminal.shop";
	int         port = argc > 2 ? atoi(argv[2]) : 22;
	const char *user = argc > 3 ? argv[3] : "anon";

	struct addrinfo hints, *res; memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
	char ports[16]; snprintf(ports, sizeof ports, "%d", port);
	if(getaddrinfo(host, ports, &hints, &res) != 0){ perror("getaddrinfo"); return 1; }
	g_sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if(connect(g_sock, res->ai_addr, res->ai_addrlen) != 0){ perror("connect"); return 1; }
	freeaddrinfo(res);

	ssh_cb_t cb = { cb_tx, cb_rng, cb_now, cb_hostkey, cb_data, cb_state, NULL };
	g_vt  = vt_create(malloc, vt_ans, NULL, NULL);
	g_ssh = ssh_create(&cb, malloc, free);

	/* ephemeral key — exactly like apps/term.c now does */
	uint8_t seed[32]; cb_rng(seed, 32, NULL);
	ssh_set_key(g_ssh, seed);

	ssh_start(g_ssh, user);

	uint8_t buf[4096];
	fcntl(g_sock, F_SETFL, O_NONBLOCK);
	time_t deadline = time(NULL) + 8;
	while(time(NULL) < deadline){
		int n = recv(g_sock, buf, sizeof buf, 0);
		if(n > 0) ssh_input(g_ssh, buf, n);
		else if(n == 0) break;
		ssh_tick(g_ssh);
		if(ssh_state(g_ssh) == SSH_ST_ERROR){ printf("  error: %s\n", ssh_error(g_ssh)); break; }
		usleep(2000);
	}

	printf("\n--- vt grid %dx%d after %ld bytes ---\n", VT_COLS, VT_ROWS, g_bytes);
	for(int r = 0; r < VT_ROWS; r++){
		const vt_cell_t *row = vt_row(g_vt, r);
		char line[VT_COLS + 1];
		for(int c = 0; c < VT_COLS; c++){
			uint8_t g = row[c].glyph;
			line[c] = (g >= 0x20 && g < 0x7f) ? (char)g : (g == 0 ? ' ' : '.');
		}
		line[VT_COLS] = 0;
		printf("|%s|\n", line);
	}
	printf("--- state=%d bytes=%ld ---\n", ssh_state(g_ssh), g_bytes);
	return 0;
}
