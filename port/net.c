// port/net.c — WiFi / networking for Kefyros (Pico 2 W, CYW43 + lwIP poll mode).
//
// Single STA, single connection at a time. The radio is brought up once at boot
// (kf_net_init); kf_net_poll() is called every superloop tick to pump the CYW43
// async context (which also drives lwIP timeouts in NO_SYS poll mode) and to run a
// small reconnect watchdog. Credentials are remembered in deskconf (config.txt on
// the SD card) so kf_net_autoconnect() can reconnect on the next boot.
//
// State is derived from cyw43_tcpip_link_status() — which only reports LINK_UP once
// DHCP has handed us an IP, so KF_NET_ONLINE genuinely means "usable".
#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "hardware/clocks.h"
#include "lwip/netif.h"
#include "lwip/ip4_addr.h"
#include "lwip/tcp.h"
#include "lwip/dns.h"
#include "lwip/apps/sntp.h"
#include <time.h>

#include "../kefyros.h"
#include "../ui/deskconf.h"

/* runtime cyw43 PIO bus divider (CYW43_PIO_CLOCK_DIV_DYNAMIC=1) */
extern void cyw43_set_pio_clkdiv_int_frac8(uint32_t clock_div_int, uint8_t clock_div_frac8);

/* --- remembered-credential keys in deskconf (KF_CONFIG) --- */
#define K_SSID "wifi_ssid"
#define K_PASS "wifi_pass"

static int            s_present = 0;          /* radio inited OK */
static kf_net_state_t s_state   = KF_NET_OFF;
static char           s_ssid[33] = {0};       /* current/last target SSID */
static char           s_pass[65] = {0};       /* current/last target key  */
static int            s_have_target = 0;       /* creds set -> watchdog active */
static int            s_badauth = 0;           /* last attempt rejected the key */
static char           s_ip[16] = "0.0.0.0";

static uint32_t       s_last_attempt_ms = 0;   /* for the reconnect backoff */
static uint32_t       s_attempts = 0;

/* --- scan state --- */
static kf_scan_cb     s_scan_cb = NULL;
static char           s_seen[24][33];          /* dedupe ring for one scan pass */
static int            s_seen_n = 0;

static uint32_t now_ms(void){ return (uint32_t)(time_us_64() / 1000u); }

/* Set the cyw43 PIO gSPI bus divider for the CURRENT clk_sys, targeting the proven ~28 MHz
   window. bus = clk_sys / (2*div). 400 MHz -> div 7 (~28.6 MHz), 250 -> div 4 (~31), 150 -> div 3.
   Must be re-applied on EVERY clk_sys change (see kf_net_reclock), or the bus runs at the old
   ratio after a clock switch (e.g. ~50 MHz at 400 with the eco-era div) and a live link drops. */
static void set_bus_div(void){
	uint32_t div = clock_get_hz(clk_sys) / (2u * 28000000u);
	if(div < 2u) div = 2u;
	cyw43_set_pio_clkdiv_int_frac8(div, 0);
}

/* Single auth mode, patient connect. (Earlier we cycled a "ladder" of auth modes every
   4 s; that churn re-issued connect_async mid-handshake and never let a clean attempt
   finish -> BADAUTH loop even with a correct password.) The target AP is WPA3-Personal,
   so use WPA3-SAE: its Dragonfly handshake is slower/heavier than WPA2's 4-way and needs
   an uninterrupted window — do_connect() leaves cleanly before each (slow) retry. */
static int s_auth_idx = 0;        /* kept for the debug readout; unused for mode select */
static int s_link = 0;            /* DEBUG: last cyw43 link status */

static void update_ip(void){
	const ip4_addr_t *ip = netif_default ? netif_ip4_addr(netif_default) : NULL;
	if(ip) snprintf(s_ip, sizeof s_ip, "%s", ip4addr_ntoa(ip));
	else   snprintf(s_ip, sizeof s_ip, "0.0.0.0");
}

/* (re)issue the async join to the current target. Leaves any prior association first
   so each attempt starts from a clean slate (no half-finished handshake confusing the
   chip). */
static void do_connect(void){
	if(!s_present || !s_have_target) return;
	cyw43_wifi_leave(&cyw43_state, CYW43_ITF_STA);   /* clean slate before retry */
	s_last_attempt_ms = now_ms();
	s_attempts++;
	s_state = KF_NET_CONNECTING;
	/* WPA2-AES. WPA3-SAE is broken in the CYW43439 firmware (known issue — SAE assoc
	   fails), so we use the WPA2 path, which is reliable. This connects to WPA2 APs and
	   to WPA2/WPA3 *transition* APs (via their WPA2 leg). A pure-WPA3-only AP will reject
	   it (BADAUTH) — set such routers to WPA2/WPA3 transition mode. */
	uint32_t auth = s_pass[0] ? CYW43_AUTH_WPA2_AES_PSK : CYW43_AUTH_OPEN;
	cyw43_arch_wifi_connect_async(s_ssid, s_pass[0] ? s_pass : NULL, auth);
}

/* Regulatory domain. WORLDWIDE ('XX') only permits 2.4 GHz channels 1-11, so APs on
   ch 12/13 (common in the EU) are scannable but un-joinable -> JOIN then generic FAIL.
   Lithuania ('LT') opens channels 1-13. Change this if the device moves countries. */
#define KF_WIFI_COUNTRY  CYW43_COUNTRY('L', 'T', 0)

/* Bring STA up with power-save DISABLED. The default (PM2) lets the chip micro-sleep,
   which makes it miss the time-critical WPA 4-way handshake frames -> the join reaches
   ASSOC then fails (LINK_FAIL) on every AP regardless of auth. cyw43_arch_enable_sta_mode
   resets PM to the default each time itf comes up, so re-assert NONE right after. */
static void sta_up(void){
	cyw43_arch_enable_sta_mode();
	cyw43_wifi_pm(&cyw43_state, CYW43_NONE_PM);
}

void kf_net_init(void){
	if(s_present) return;            /* idempotent — safe to call lazily/repeatedly */
	/* Set the cyw43 PIO bus divider for the CURRENT clk_sys, targeting ~28 MHz (the
	   proven window for wifi_on's handshake). Dynamic so WiFi works at any clock we
	   bring it up at; kf_net_reclock() re-applies it whenever the OS changes clk_sys. */
	set_bus_div();
	/* IMPORTANT: bring the radio up only at a WiFi-safe clock (<=~270 MHz). cyw43's
	   STA bring-up (wifi_on's ioctl handshake) fails above the ceiling, and once it
	   fails the chip stays stuck (ensure_up won't re-run on an already-inited chip).
	   So this is called from the WiFi app AFTER kf_clock_eco(), not at 400 MHz boot. */
	if(cyw43_arch_init_with_country(KF_WIFI_COUNTRY)){
		s_present = 0; s_state = KF_NET_OFF; return;
	}
	sta_up();
	s_present = 1;
	s_state = KF_NET_OFF;
}

int kf_net_present(void){ return s_present; }

/* Re-tune the cyw43 gSPI bus for the new clk_sys. Called from the clock-change path
   (reclock_peripherals) so a link associated at eco keeps its bus in spec — and thus the
   link alive — after the OS bumps to 400. No-op until the radio is up. */
void kf_net_reclock(void){
	if(s_present) set_bus_div();
}

void kf_net_connect(const char *ssid, const char *pass){
	if(!ssid || !ssid[0]) return;
	snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
	snprintf(s_pass, sizeof s_pass, "%s", pass ? pass : "");
	s_have_target = 1;
	s_badauth = 0;
	s_attempts = 0;
	s_auth_idx = 0;
	/* persist so we auto-connect next boot. plaintext on SD — accepted tradeoff. */
	deskconf_set(K_SSID, s_ssid);
	deskconf_set(K_PASS, s_pass);
	if(s_present) do_connect();
}

void kf_net_forget(void){
	s_have_target = 0;
	s_badauth = 0;
	s_ssid[0] = 0; s_pass[0] = 0;
	deskconf_set(K_SSID, "");
	deskconf_set(K_PASS, "");
	if(s_present){
		cyw43_arch_disable_sta_mode();
		sta_up();
	}
	s_state = KF_NET_OFF;
	snprintf(s_ip, sizeof s_ip, "0.0.0.0");
}

void kf_net_autoconnect(void){
	const char *ssid = deskconf_get(K_SSID, "");
	if(ssid && ssid[0]) kf_net_connect(ssid, deskconf_get(K_PASS, ""));
}

/* ===== time sync (SNTP) + locality =====
   We keep wall-clock time in software: SNTP gives UTC, we apply the locale offset and
   anchor it to time_us_64(). Avoids the STM32 RTC's ambiguous BCD/binary format. The
   locale is EET (Vilnius/Tallinn, UTC+2) with automatic EU summer time (UTC+3). */
static volatile int s_time_ok = 0;
static uint64_t     s_time_base_us;       /* time_us_64() at last sync */
static time_t       s_time_base_local;    /* local epoch at last sync  */

/* EU DST: +3h between last-Sunday-March 01:00 UTC and last-Sunday-October 01:00 UTC. */
static int eu_is_dst(int y, int mon, int day, int hour_utc){
	if(mon < 3 || mon > 10) return 0;
	if(mon > 3 && mon < 10) return 1;
	int lsm = 31 - ((5*y/4 + 4) % 7);     /* last Sunday of March */
	int lso = 31 - ((5*y/4 + 1) % 7);     /* last Sunday of October */
	if(mon == 3)  return (day > lsm) || (day == lsm && hour_utc >= 1);
	return (day < lso) || (day == lso && hour_utc < 1);
}

/* SNTP callback (wired via SNTP_SET_SYSTEM_TIME in lwipopts.h). sec = UTC epoch. */
void kf_sntp_set_time(uint32_t sec){
	time_t utc = (time_t)sec;
	struct tm *g = gmtime(&utc);
	int off = (2 + eu_is_dst(g->tm_year+1900, g->tm_mon+1, g->tm_mday, g->tm_hour)) * 3600;
	s_time_base_local = utc + off;
	s_time_base_us = time_us_64();
	s_time_ok = 1;
}

int kf_time_synced(void){ return s_time_ok; }

int kf_time_local(struct tm *out){
	if(!s_time_ok) return 0;
	time_t now = s_time_base_local + (time_t)((time_us_64() - s_time_base_us) / 1000000ull);
	struct tm *t = gmtime(&now);           /* `now` is already local epoch */
	if(t){ *out = *t; return 1; }
	return 0;
}

static void sntp_begin(void){
	static int started = 0;
	if(started) return;
	started = 1;
	sntp_setoperatingmode(SNTP_OPMODE_POLL);
	sntp_setservername(0, "pool.ntp.org");
	sntp_init();
}

/* ===== throughput benchmark (raw lwIP TCP HTTP GET of a fixed file) =====
   States: 0 idle, 1 connecting/dns, 2 downloading, 3 done(kbps valid), 4 failed.
   Driven by the lwIP callbacks that fire inside cyw43_arch_poll() (poll mode). */
#define BENCH_HOST "speedtest.tele2.net"
#define BENCH_PATH "/1MB.zip"
#define BENCH_PORT 80

static struct tcp_pcb *s_bpcb;
static volatile int    s_bstate;       /* 0..4 as above */
static uint32_t        s_bbytes, s_bt0, s_bkbps, s_blast;

static void bench_finish(int ok){
	if(s_bpcb){
		tcp_arg(s_bpcb, NULL); tcp_recv(s_bpcb, NULL); tcp_err(s_bpcb, NULL);
		tcp_close(s_bpcb); s_bpcb = NULL;
	}
	if(ok && s_bbytes){
		uint32_t dt = now_ms() - s_bt0; if(!dt) dt = 1;
		s_bkbps = (uint32_t)(((uint64_t)s_bbytes * 1000u) / ((uint64_t)dt * 1024u));
		s_bstate = 3;
	} else s_bstate = 4;
}

static err_t bench_recv(void *a, struct tcp_pcb *pcb, struct pbuf *p, err_t err){
	(void)a;
	if(err != ERR_OK){ bench_finish(0); return ERR_OK; }
	if(!p){ bench_finish(1); return ERR_OK; }       /* remote closed -> done */
	s_bbytes += p->tot_len; s_blast = now_ms();
	tcp_recved(pcb, p->tot_len);
	pbuf_free(p);
	return ERR_OK;
}
static void bench_err_cb(void *a, err_t err){ (void)a; (void)err; s_bpcb = NULL; bench_finish(s_bbytes > 0); }

static err_t bench_connected(void *a, struct tcp_pcb *pcb, err_t err){
	(void)a;
	if(err != ERR_OK){ bench_finish(0); return ERR_OK; }
	static const char req[] = "GET " BENCH_PATH " HTTP/1.1\r\nHost: " BENCH_HOST
	                          "\r\nConnection: close\r\n\r\n";
	s_bt0 = now_ms(); s_blast = s_bt0; s_bbytes = 0; s_bstate = 2;
	tcp_write(pcb, req, sizeof req - 1, TCP_WRITE_FLAG_COPY);
	tcp_output(pcb);
	return ERR_OK;
}
static void bench_dns(const char *name, const ip_addr_t *ip, void *a){
	(void)name; (void)a;
	if(!ip){ bench_finish(0); return; }
	s_bpcb = tcp_new_ip_type(IP_GET_TYPE(ip));
	if(!s_bpcb){ bench_finish(0); return; }
	tcp_recv(s_bpcb, bench_recv);
	tcp_err(s_bpcb, bench_err_cb);
	if(tcp_connect(s_bpcb, ip, BENCH_PORT, bench_connected) != ERR_OK) bench_finish(0);
}

int kf_net_bench_start(void){
	if(s_state != KF_NET_ONLINE) return -1;
	if(s_bstate == 1 || s_bstate == 2) return -2;   /* already running */
	s_bbytes = 0; s_bkbps = 0; s_bstate = 1; s_bt0 = s_blast = now_ms();
	ip_addr_t ip;
	err_t e = dns_gethostbyname(BENCH_HOST, &ip, bench_dns, NULL);
	if(e == ERR_OK) bench_dns(BENCH_HOST, &ip, NULL);   /* cached */
	else if(e != ERR_INPROGRESS){ bench_finish(0); return -3; }
	return 0;
}
int      kf_net_bench_state(void){ return s_bstate; }
uint32_t kf_net_bench_kbps(void){ return s_bkbps; }
uint32_t kf_net_bench_bytes(void){ return s_bbytes; }

static void bench_tick(void){
	if(s_bstate == 1 || s_bstate == 2){
		/* overall + stall timeout so a hung transfer can't wedge the benchmark */
		if(now_ms() - s_bt0 > 20000u || now_ms() - s_blast > 4000u) bench_finish(s_bbytes > 0);
	}
}

void kf_net_poll(void){
	if(!s_present) return;
	cyw43_arch_poll();                 /* services CYW43 + lwIP timeouts (poll mode) */

	int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
	s_link = link;

	if(link == CYW43_LINK_UP){
		if(s_state != KF_NET_ONLINE){ s_state = KF_NET_ONLINE; s_attempts = 0; s_badauth = 0; }
		update_ip();
		sntp_begin();                  /* start time sync once we're online */
		bench_tick();
		return;
	}

	if(!s_have_target){ s_state = KF_NET_OFF; return; }

	/* Patient single-mode connect: a negative link code means this attempt failed, but
	   we do NOT immediately re-issue — we give the handshake a long, uninterrupted window
	   and only retry (cleanly, via do_connect's leave) every 15 s. Re-hammering connect_async
	   was what broke the handshake before. BADAUTH after a clean patient attempt is a real
	   bad-password/auth signal (surfaced for the UI), but we keep slow-retrying. */
	if(link < 0){
		s_badauth = (link == CYW43_LINK_BADAUTH);
		s_state   = (s_attempts > 3) ? KF_NET_FAILED : KF_NET_CONNECTING;
	} else {
		s_state = KF_NET_CONNECTING;           /* JOIN / NOIP — still working, leave it alone */
	}

	if(now_ms() - s_last_attempt_ms > 15000u) do_connect();   /* slow, clean retry */

	bench_tick();   /* advance the throughput benchmark, if running */
}

kf_net_state_t kf_net_state(void){ return s_state; }

const char *kf_net_state_str(void){
	switch(s_state){
	case KF_NET_ONLINE:     return "online";
	case KF_NET_CONNECTING: return "connecting";
	case KF_NET_FAILED:     return s_badauth ? "bad password" : "failed";
	default:                return "off";
	}
}

const char *kf_net_ip(void){ return s_ip; }
const char *kf_net_ssid(void){ return s_ssid; }

/* --- scan --- */
static int scan_result(void *env, const cyw43_ev_scan_result_t *r){
	(void)env;
	if(!r || !s_scan_cb) return 0;
	char ssid[33];
	int n = r->ssid_len; if(n > 32) n = 32;
	memcpy(ssid, r->ssid, n); ssid[n] = 0;
	if(!ssid[0]) return 0;                    /* skip hidden / blank */
	for(int i=0;i<s_seen_n;i++) if(!strcmp(s_seen[i], ssid)) return 0;  /* dedupe */
	if(s_seen_n < (int)(sizeof s_seen / sizeof s_seen[0]))
		snprintf(s_seen[s_seen_n++], 33, "%s", ssid);
	/* auth_mode 0 = open; anything else = secured. */
	s_scan_cb(ssid, (int)r->rssi, r->auth_mode != 0);
	return 0;
}

int kf_net_scan_start(kf_scan_cb cb){
	if(!s_present) return -1;
	if(cyw43_wifi_scan_active(&cyw43_state)) return -2;
	/* STA must be "up" (itf_state bit set) or cyw43_wifi_scan returns -EPERM. If the
	   bring-up at init didn't stick, re-arm it here before scanning. */
	if((cyw43_state.itf_state & (1u << CYW43_ITF_STA)) == 0)
		sta_up();
	s_scan_cb = cb;
	s_seen_n = 0;
	cyw43_wifi_scan_options_t opt = {0};
	return cyw43_wifi_scan(&cyw43_state, &opt, NULL, scan_result);  /* real cyw43 code */
}

/* DEBUG: raw cyw43 interface-up bitmask (bit0=STA). -1 if no radio. */
int kf_net_dbg_itf(void){ return s_present ? (int)cyw43_state.itf_state : -1; }
/* DEBUG: last cyw43 link status (3=UP,2=NOIP,1=JOIN,0=DOWN,-1=FAIL,-2=NONET,-3=BADAUTH)
   in the low bits, and the current auth-ladder index in the high bits. */
int kf_net_dbg_link(void){ return (s_auth_idx << 8) | (s_link & 0xff); }

int kf_net_scan_active(void){
	if(!s_present) return 0;
	return cyw43_wifi_scan_active(&cyw43_state) ? 1 : 0;
}
