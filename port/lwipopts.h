// port/lwipopts.h — lwIP config for the Kefyros WiFi stack (Pico 2 W / CYW43).
// NO_SYS (bare-metal, poll mode: pico_cyw43_arch_lwip_poll). DHCP + DNS + TCP/UDP +
// SNTP. Single radio, single TCP connection at a time, so the pools stay modest.
// (No TLS — the Spineko browser is HTTP-only.)
#ifndef KF_LWIPOPTS_H
#define KF_LWIPOPTS_H

#include <stdint.h>
/* SNTP time sync -> our handler (port/net.c) keeps wall-clock time in software. */
void kf_sntp_set_time(uint32_t sec);
#define SNTP_SET_SYSTEM_TIME(sec)   kf_sntp_set_time(sec)
#define SNTP_SERVER_DNS             1

#define NO_SYS                      1
#define LWIP_SOCKET                 0
#define LWIP_NETCONN                0

/* let lwIP use the C-library malloc for the heap (our heap is ~338 KB). */
#define MEM_LIBC_MALLOC             0
#define MEM_ALIGNMENT               4
#define MEM_SIZE                    8000          /* lwIP heap (pbufs, etc.) */
#define MEMP_NUM_TCP_SEG            32
#define MEMP_NUM_ARP_QUEUE          10
#define PBUF_POOL_SIZE              24
#define MEMP_NUM_SYS_TIMEOUT        (LWIP_NUM_SYS_TIMEOUT_INTERNAL + 4)

#define LWIP_ARP                    1
#define LWIP_ETHERNET               1
#define LWIP_ICMP                   1
#define LWIP_RAW                    1

#define LWIP_IPV4                   1
#define LWIP_TCP                    1
#define LWIP_UDP                    1
#define LWIP_DNS                    1
#define LWIP_DHCP                   1
#define DHCP_DOES_ARP_CHECK         0
#define LWIP_DHCP_DOES_ACD_CHECK    0

#define TCP_MSS                     1460
#define TCP_WND                     (8 * TCP_MSS)
#define TCP_SND_BUF                 (8 * TCP_MSS)
#define TCP_SND_QUEUELEN            ((4 * (TCP_SND_BUF) + (TCP_MSS - 1)) / (TCP_MSS))
#define LWIP_TCP_KEEPALIVE          1
#define LWIP_NETIF_TX_SINGLE_PBUF   1

#define LWIP_NETIF_STATUS_CALLBACK  1
#define LWIP_NETIF_LINK_CALLBACK    1
#define LWIP_NETIF_HOSTNAME         1

/* checksums in software (no checksum offload on the CYW43 path). */
#define LWIP_CHKSUM_ALGORITHM       3

/* stats off (saves RAM); keep link stats for debugging. */
#define MEM_STATS                   0
#define SYS_STATS                   0
#define MEMP_STATS                  0
#define LINK_STATS                  0

#endif /* KF_LWIPOPTS_H */
