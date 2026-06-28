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

/* --- remembered networks in deskconf (KF_CONFIG) ---
   Up to KF_WIFI_MAX networks, stored as wifi_ssid0..N / wifi_pass0..N in MRU order (slot 0 =
   most recently connected). The legacy single-network keys wifi_ssid/wifi_pass are migrated into
   slot 0 on first run. On boot, kf_net_autoconnect() scans, ranks the visible saved networks by
   signal, and tries them strongest-first until one joins or all fail. */
#define KF_WIFI_MAX 8
#define K_SSID "wifi_ssid"          /* legacy single-network keys (migrated then cleared) */
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
static int            s_seen_rssi[24];         /* RSSI per s_seen entry (for autoconnect ranking) */
static int            s_seen_n = 0;

/* --- boot autoconnect campaign --- */
enum { CAMP_NONE=0, CAMP_SCAN, CAMP_TRY, CAMP_DONE };
static int      s_camp = CAMP_NONE;
static char     s_try_ssid[KF_WIFI_MAX][33];   /* visible saved nets, strongest first */
static char     s_try_pass[KF_WIFI_MAX][65];
static int      s_try_rssi[KF_WIFI_MAX];
static int      s_try_n = 0, s_try_idx = 0;
static uint32_t s_camp_ms = 0;                 /* current attempt's start time */

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

/* ===== remembered-network store (deskconf slots, MRU order) ===== */
static void known_get(int i, char *ssid, char *pass){
	char k[16];
	snprintf(k, sizeof k, "wifi_ssid%d", i); snprintf(ssid, 33, "%s", deskconf_get(k, ""));
	snprintf(k, sizeof k, "wifi_pass%d", i); snprintf(pass, 65, "%s", deskconf_get(k, ""));
}
static void known_set(int i, const char *ssid, const char *pass){
	char k[16];
	snprintf(k, sizeof k, "wifi_ssid%d", i); deskconf_set(k, ssid);
	snprintf(k, sizeof k, "wifi_pass%d", i); deskconf_set(k, pass);
}
/* one-time: fold the old single-network keys into slot 0 if the new store is empty. */
static void known_migrate(void){
	char s[33], p[65]; known_get(0, s, p);
	if(s[0]) return;
	const char *os = deskconf_get(K_SSID, "");
	if(os[0]){ known_set(0, os, deskconf_get(K_PASS, "")); deskconf_set(K_SSID, ""); deskconf_set(K_PASS, ""); }
}
static int known_count(void){
	known_migrate();
	char s[33], p[65]; int n = 0;
	for(int i=0;i<KF_WIFI_MAX;i++){ known_get(i, s, p); if(!s[0]) break; n++; }
	return n;
}
/* Promote (ssid,pass) to MRU slot 0: drop any existing copy of ssid, shift the rest down. */
static void known_remember(const char *ssid, const char *pass){
	char s[KF_WIFI_MAX][33], p[KF_WIFI_MAX][65]; int n = 0;
	for(int i=0;i<KF_WIFI_MAX;i++){
		char es[33], ep[65]; known_get(i, es, ep);
		if(!es[0]) break;
		if(!strcmp(es, ssid)) continue;                 /* old copy of this ssid -> drop */
		if(n < KF_WIFI_MAX-1){ snprintf(s[n],33,"%s",es); snprintf(p[n],65,"%s",ep); n++; }
	}
	known_set(0, ssid, pass);
	for(int i=0;i<n;i++) known_set(i+1, s[i], p[i]);
	for(int i=n+1;i<KF_WIFI_MAX;i++) known_set(i, "", "");   /* clear stale tail */
}
/* Remove ssid from the store (keeps the rest in order). */
static void known_forget_one(const char *ssid){
	char s[KF_WIFI_MAX][33], p[KF_WIFI_MAX][65]; int n = 0;
	for(int i=0;i<KF_WIFI_MAX;i++){
		char es[33], ep[65]; known_get(i, es, ep);
		if(!es[0]) break;
		if(!strcmp(es, ssid)) continue;
		snprintf(s[n],33,"%s",es); snprintf(p[n],65,"%s",ep); n++;
	}
	for(int i=0;i<n;i++) known_set(i, s[i], p[i]);
	for(int i=n;i<KF_WIFI_MAX;i++) known_set(i, "", "");
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
	   STA bring-up (wifi_on's ioctl handshake) STALLS ~60s then fails above the ceiling
	   (confirmed at 400). The OS default (kf_clock_normal) is now 200 MHz — under the
	   ceiling — so init runs at the default; no special dip needed. */
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

/* aim the radio at a target without touching the saved store (used by the campaign too). */
static void set_target(const char *ssid, const char *pass){
	snprintf(s_ssid, sizeof s_ssid, "%s", ssid);
	snprintf(s_pass, sizeof s_pass, "%s", pass ? pass : "");
	s_have_target = 1;
	s_badauth = 0;
	s_attempts = 0;
	s_auth_idx = 0;
}

void kf_net_connect(const char *ssid, const char *pass){
	if(!ssid || !ssid[0]) return;
	s_camp = CAMP_NONE;                 /* a manual connect cancels any boot campaign */
	set_target(ssid, pass);
	/* remember at MRU slot 0 so it auto-connects (strongest-first) next boot.
	   plaintext on SD — accepted tradeoff. */
	known_remember(s_ssid, s_pass);
	if(s_present) do_connect();
}

void kf_net_forget(void){
	if(s_ssid[0]) known_forget_one(s_ssid);   /* drop it from the saved store */
	s_camp = CAMP_NONE;
	s_have_target = 0;
	s_badauth = 0;
	s_ssid[0] = 0; s_pass[0] = 0;
	if(s_present){
		cyw43_arch_disable_sta_mode();
		sta_up();
	}
	s_state = KF_NET_OFF;
	snprintf(s_ip, sizeof s_ip, "0.0.0.0");
}

int kf_net_has_saved(void){ return known_count() > 0; }
int kf_net_autoconnect_active(void){ return s_camp == CAMP_SCAN || s_camp == CAMP_TRY; }

/* Boot auto-connect: scan, then try the visible saved networks strongest-first (campaign_tick
   in kf_net_poll drives it). No-op if nothing saved or the radio isn't up. Caller must already
   be at a WiFi-safe clock (<=270 MHz). */
void kf_net_autoconnect(void){
	if(!s_present || known_count() == 0) return;
	s_try_n = 0; s_try_idx = 0;
	s_seen_n = 0;
	s_scan_cb = NULL;                  /* internal scan — results captured into s_seen[] */
	s_camp_ms = now_ms();
	if(kf_net_scan_start(NULL) == 0) s_camp = CAMP_SCAN;
	else                             s_camp = CAMP_NONE;   /* couldn't scan -> give up quietly */
}

/* ===== time sync (SNTP) + locality =====
   We keep wall-clock time in software: SNTP gives UTC, we apply the configured locale offset and
   anchor it to time_us_64(). Avoids the STM32 RTC's ambiguous BCD/binary format. The locale is set
   in Settings (deskconf tz_offset minutes + tz_dst rule); default UTC+2 Vilnius with EU DST. */
static volatile int s_time_ok = 0;
static uint64_t     s_time_base_us;       /* time_us_64() at last sync */
static time_t       s_time_base_local;    /* local epoch at last sync  */
static time_t       s_time_base_utc;      /* UTC epoch at last sync (re-anchored on locale change) */

/* day of week, 0=Sun..6=Sat (Sakamoto). */
static int dow(int y, int m, int d){
	static const int t[] = {0,3,2,5,0,3,5,1,4,6,2,4};
	if(m < 3) y -= 1;
	return (y + y/4 - y/100 + y/400 + t[m-1] + d) % 7;
}
/* EU DST: +1h from last-Sunday-March to last-Sunday-October (transition ~01:00 UTC). */
static int eu_is_dst(int y, int mon, int day){
	if(mon < 3 || mon > 10) return 0;
	if(mon > 3 && mon < 10) return 1;
	int last = 31 - dow(y, mon, 31);           /* last Sunday of this 31-day month */
	if(mon == 3)  return day >= last;
	return day < last;                         /* October */
}
/* US DST: +1h from 2nd-Sunday-March to 1st-Sunday-November (transition ~02:00 local). */
static int us_is_dst(int y, int mon, int day){
	if(mon < 3 || mon > 11) return 0;
	if(mon > 3 && mon < 11) return 1;
	if(mon == 3){ int first = 1 + ((7 - dow(y,3,1)) % 7); return day >= first + 7; }
	int first = 1 + ((7 - dow(y,11,1)) % 7);   /* 1st Sunday of November */
	return day < first;
}
/* DST hours for the configured rule on the given (UTC) date. rule: 0 none, 1 EU, 2 US. */
static int dst_hours(int rule, int y, int mon, int day){
	if(rule == 1) return eu_is_dst(y, mon, day);
	if(rule == 2) return us_is_dst(y, mon, day);
	return 0;
}
/* (re)derive the local-epoch anchor from a UTC epoch + the current deskconf locale. */
static void anchor_local(time_t utc){
	struct tm *g = gmtime(&utc);
	int off  = deskconf_get_int("tz_offset", 120);   /* minutes east of UTC */
	int rule = deskconf_get_int("tz_dst", 1);
	int dst  = dst_hours(rule, g->tm_year+1900, g->tm_mon+1, g->tm_mday);
	s_time_base_local = utc + (time_t)off*60 + (time_t)dst*3600;
	s_time_base_utc   = utc;
	s_time_base_us    = time_us_64();
	s_time_ok = 1;
}

/* SNTP callback (wired via SNTP_SET_SYSTEM_TIME in lwipopts.h). sec = UTC epoch. */
void kf_sntp_set_time(uint32_t sec){ anchor_local((time_t)sec); }

/* Re-apply the locale immediately (called from Settings when the zone changes) so the clock
   updates without waiting for the next SNTP poll. No-op until the first sync. */
void kf_time_apply_locale(void){
	if(!s_time_ok) return;
	time_t utc = s_time_base_utc + (time_t)((time_us_64() - s_time_base_us) / 1000000ull);
	anchor_local(utc);
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

/* Boot auto-connect: after the scan settles, rank the visible saved networks by signal and try
   them strongest-first. Each gets one clean ~15 s join window (re-issuing connect_async mid-
   handshake is what broke joins before); a bad password fails fast. When the list is exhausted we
   give up but leave the strongest as the target, so the normal watchdog keeps slow-retrying it. */
static void campaign_tick(void){
	if(s_camp == CAMP_SCAN){
		if(kf_net_scan_active()) return;                  /* still scanning */
		s_try_n = 0;
		for(int k=0;k<KF_WIFI_MAX;k++){
			char ks[33], kp[65]; known_get(k, ks, kp);
			if(!ks[0]) break;
			for(int i=0;i<s_seen_n;i++) if(!strcmp(s_seen[i], ks)){
				snprintf(s_try_ssid[s_try_n],33,"%s",ks);
				snprintf(s_try_pass[s_try_n],65,"%s",kp);
				s_try_rssi[s_try_n] = s_seen_rssi[i];
				s_try_n++;
				break;
			}
		}
		for(int a=0;a<s_try_n;a++) for(int b=a+1;b<s_try_n;b++) if(s_try_rssi[b] > s_try_rssi[a]){
			int ri=s_try_rssi[a]; s_try_rssi[a]=s_try_rssi[b]; s_try_rssi[b]=ri;
			char ts[33]; snprintf(ts,33,"%s",s_try_ssid[a]);
			snprintf(s_try_ssid[a],33,"%s",s_try_ssid[b]); snprintf(s_try_ssid[b],33,"%s",ts);
			char tp[65]; snprintf(tp,65,"%s",s_try_pass[a]);
			snprintf(s_try_pass[a],65,"%s",s_try_pass[b]); snprintf(s_try_pass[b],65,"%s",tp);
		}
		if(s_try_n == 0){ s_camp = CAMP_DONE; return; }   /* none of ours in range */
		s_try_idx = 0; s_camp = CAMP_TRY; s_camp_ms = now_ms();
		set_target(s_try_ssid[0], s_try_pass[0]); do_connect();
		return;
	}
	if(s_camp == CAMP_TRY){
		if(s_badauth || now_ms() - s_camp_ms > 15000u){
			s_try_idx++;
			if(s_try_idx >= s_try_n){ s_camp = CAMP_DONE; return; }   /* exhausted -> give up */
			s_camp_ms = now_ms();
			set_target(s_try_ssid[s_try_idx], s_try_pass[s_try_idx]); do_connect();
		}
	}
}

void kf_net_poll(void){
	if(!s_present) return;
	cyw43_arch_poll();                 /* services CYW43 + lwIP timeouts (poll mode) */

	int link = cyw43_tcpip_link_status(&cyw43_state, CYW43_ITF_STA);
	s_link = link;

	if(link == CYW43_LINK_UP){
		if(s_state != KF_NET_ONLINE){ s_state = KF_NET_ONLINE; s_attempts = 0; s_badauth = 0; }
		if(s_camp == CAMP_SCAN || s_camp == CAMP_TRY) s_camp = CAMP_DONE;   /* campaign won */
		update_ip();
		sntp_begin();                  /* start time sync once we're online */
		bench_tick();
		return;
	}

	/* while a boot campaign runs, it owns the connection — skip the single-target watchdog */
	if(s_camp == CAMP_SCAN || s_camp == CAMP_TRY){
		s_badauth = (link == CYW43_LINK_BADAUTH);
		s_state   = KF_NET_CONNECTING;
		campaign_tick();
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
	if(!r) return 0;
	char ssid[33];
	int n = r->ssid_len; if(n > 32) n = 32;
	memcpy(ssid, r->ssid, n); ssid[n] = 0;
	if(!ssid[0]) return 0;                    /* skip hidden / blank */
	for(int i=0;i<s_seen_n;i++) if(!strcmp(s_seen[i], ssid)) return 0;  /* dedupe */
	if(s_seen_n < (int)(sizeof s_seen / sizeof s_seen[0])){
		snprintf(s_seen[s_seen_n], 33, "%s", ssid);
		s_seen_rssi[s_seen_n] = (int)r->rssi;     /* kept for autoconnect ranking */
		s_seen_n++;
	}
	/* auth_mode 0 = open; anything else = secured. (cb is NULL during the internal
	   autoconnect scan — results are read back from s_seen[] instead.) */
	if(s_scan_cb) s_scan_cb(ssid, (int)r->rssi, r->auth_mode != 0);
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
