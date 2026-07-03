// port/ssh_tcp.c — lwIP glue for the Term SSH client. Mirrors port/http.c's
// single-connection raw-TCP pattern (poll mode, NO_SYS=1). Owns its own pcb and
// state, entirely separate from http.c. Feeds received bytes to the pure SSH
// core (port/ssh.c) and provides its `tx` callback backed by a small drain-on-ack
// queue so ssh.c can always "accept all" outgoing bytes.
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "ssh.h"

enum { TCP_IDLE=0, TCP_RESOLVING, TCP_CONNECTING, TCP_UP, TCP_CLOSED, TCP_ERR };

static struct tcp_pcb *s_pcb;
static ssh_t          *s_ssh;
static int             s_state;
static uint16_t        s_port;
static char            s_err[64];

/* outbound queue (drained as tcp_sndbuf frees). Outbound SSH traffic is tiny
   (typing, window adjusts, keepalives) so this rarely fills. */
#define TXQ_CAP 8192
static uint8_t  s_txq[TXQ_CAP];
static int      s_txq_head, s_txq_len;

static void tcp_err_set(const char *m){
	strncpy(s_err, m, sizeof s_err - 1); s_err[sizeof s_err - 1] = 0;
	s_state = TCP_ERR;
}

static void detach(void){
	if(s_pcb){
		tcp_arg(s_pcb, NULL); tcp_recv(s_pcb, NULL); tcp_sent(s_pcb, NULL); tcp_err(s_pcb, NULL);
		if(tcp_close(s_pcb) != ERR_OK) tcp_abort(s_pcb);
		s_pcb = NULL;
	}
}

/* push into the ring (caller guarantees it fits or we drop — SSH never bursts
   more than a few hundred bytes outbound). */
static void txq_push(const uint8_t *d, int n){
	for(int i = 0; i < n && s_txq_len < TXQ_CAP; i++){
		s_txq[(s_txq_head + s_txq_len) % TXQ_CAP] = d[i];
		s_txq_len++;
	}
}
static void txq_drain(void){
	while(s_pcb && s_txq_len > 0){
		u16_t room = tcp_sndbuf(s_pcb);
		if(room == 0) break;
		int contig = TXQ_CAP - s_txq_head;
		int chunk = s_txq_len < contig ? s_txq_len : contig;
		if(chunk > room) chunk = room;
		if(tcp_write(s_pcb, &s_txq[s_txq_head], chunk, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
		s_txq_head = (s_txq_head + chunk) % TXQ_CAP;
		s_txq_len -= chunk;
	}
	if(s_pcb) tcp_output(s_pcb);
}

/* the ssh_cb_t.tx callback: accept all n bytes (queue if the socket is full). */
int ssh_tcp_tx(const uint8_t *buf, int n, void *ud){
	(void)ud;
	if(!s_pcb || s_state == TCP_ERR) return -1;
	/* fast path: write directly while the send buffer has room */
	int off = 0;
	if(s_txq_len == 0){
		while(off < n && s_pcb){
			u16_t room = tcp_sndbuf(s_pcb);
			if(room == 0) break;
			int w = (n - off) < room ? (n - off) : room;
			if(tcp_write(s_pcb, buf + off, w, TCP_WRITE_FLAG_COPY) != ERR_OK) break;
			off += w;
		}
	}
	if(off < n) txq_push(buf + off, n - off);
	if(s_pcb) tcp_output(s_pcb);
	return n;
}

static err_t on_sent(void *arg, struct tcp_pcb *pcb, u16_t l){
	(void)arg; (void)pcb; (void)l;
	txq_drain();
	return ERR_OK;
}
static void on_err(void *arg, err_t err){
	(void)arg; s_pcb = NULL;
	if(s_state != TCP_CLOSED){ char m[48]; snprintf(m, sizeof m, "connection error (%d)", (int)err); tcp_err_set(m); }
}
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err){
	(void)arg;
	if(err != ERR_OK){ if(p) pbuf_free(p); tcp_err_set("recv error"); detach(); return ERR_OK; }
	if(!p){ s_state = TCP_CLOSED; return ERR_OK; }        /* remote closed */
	for(struct pbuf *q = p; q; q = q->next)
		if(s_ssh) ssh_input(s_ssh, (const uint8_t*)q->payload, q->len);
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	txq_drain();                                          /* ssh_input may have queued replies */
	return ERR_OK;
}
static err_t on_connected(void *arg, struct tcp_pcb *pcb, err_t err){
	(void)arg; (void)pcb;
	if(err != ERR_OK){ tcp_err_set("connect failed"); detach(); return ERR_OK; }
	s_state = TCP_UP;
	return ERR_OK;
}
static void on_dns(const char *name, const ip_addr_t *ip, void *arg){
	(void)name; (void)arg;
	if(!ip){ tcp_err_set("DNS failed"); return; }
	s_pcb = tcp_new_ip_type(IP_GET_TYPE(ip));
	if(!s_pcb){ tcp_err_set("out of memory (pcb)"); return; }
	tcp_arg(s_pcb, NULL);
	tcp_recv(s_pcb, on_recv);
	tcp_sent(s_pcb, on_sent);
	tcp_err(s_pcb, on_err);
	s_state = TCP_CONNECTING;
	if(tcp_connect(s_pcb, ip, s_port, on_connected) != ERR_OK){ tcp_err_set("connect start failed"); detach(); }
}

/* ---- public API (used by apps/term.c) ---- */
void ssh_tcp_init(ssh_t *s){ s_ssh = s; }

int ssh_tcp_connect(const char *host, uint16_t port){
	detach();
	s_state = TCP_RESOLVING; s_err[0] = 0; s_port = port;
	s_txq_head = s_txq_len = 0;
	ip_addr_t ip;
	err_t e = dns_gethostbyname(host, &ip, on_dns, NULL);
	if(e == ERR_OK)            on_dns(host, &ip, NULL);   /* cached: resolve inline */
	else if(e != ERR_INPROGRESS){ tcp_err_set("DNS start failed"); return -1; }
	return 0;
}
void ssh_tcp_poll(void){ txq_drain(); }
void ssh_tcp_close(void){ detach(); s_state = TCP_CLOSED; s_ssh = NULL; s_txq_head = s_txq_len = 0; }
int  ssh_tcp_state(void){ return s_state; }
const char *ssh_tcp_err(void){ return s_err; }

/* state accessors for the app (avoid leaking the enum) */
int ssh_tcp_is_up(void){ return s_state == TCP_UP; }
int ssh_tcp_is_dead(void){ return s_state == TCP_ERR || s_state == TCP_CLOSED; }
