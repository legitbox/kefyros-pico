// port/kbd_uart.c — uart1 link to the PicoCalc STM32 southbridge (UART_PICO_INTERFACE).
// Bare-metal RP2350 / Pico 2 W port of port/uart.c: same framed protocol, same key ring,
// modifier tracking and request/response register API — only the byte I/O is swapped from
// /dev/ttyS1 termios to pico-sdk uart1 (GP8=TX1, GP9=RX1, 115200 8N1, no flow control).
//
// Frame: 0xA5 | TYPE | LEN(0..16) | PAYLOAD[LEN] | CRC8(poly 0x07, init 0x00 over TYPE+LEN+PAYLOAD).
// Device->host: KEY_EVENT 0x01 [state,key], LOCK_EVENT 0x02, REG_RESPONSE 0x04 [reg,data...],
//               BOOT 0x05, NACK 0x06.  Host->device: REG_READ 0x10 [reg], REG_WRITE 0x11 [reg,data...],
//               PING 0x12.
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

/* Name-clash resolution.  <hardware/uart.h> declares the SDK initialiser as
   `uint uart_init(uart_inst_t*, uint)`, while our framework contract (kefyros.h) declares the
   public entry point as `void uart_init(void)` — an incompatible redeclaration of the same name
   in one translation unit.  While the SDK headers are parsed we rename the token `uart_init` to a
   private identifier (`kf_hw_uart_init`), so the SDK's *prototype* is emitted under that private
   name and the bare `uart_init` name is left free for kefyros.h / our definition.  The matching
   asm-aliased extern below re-binds `kf_hw_uart_init` to the SDK's real link-time symbol
   (`uart_init`), so the renamed call still links to the SDK function. */
#define uart_init kf_hw_uart_init
#include "pico/stdlib.h"
#include "hardware/uart.h"
#include "hardware/gpio.h"
#undef uart_init
extern uint kf_hw_uart_init(uart_inst_t *uart, uint baudrate) __asm__("uart_init");

#include "lvgl/lvgl.h"
#include "kefyros.h"

/* frame TYPE constants */
#define T_KEY_EVENT    0x01
#define T_LOCK_EVENT   0x02
#define T_REG_RESPONSE 0x04
#define T_BOOT         0x05
#define T_NACK         0x06
#define T_REG_READ     0x10
#define T_REG_WRITE    0x11
#define T_PING         0x12

static int g_mods = 0;
static uint32_t g_last_activity = 0;   /* lv_tick of the last key event (idle-dim) */
uint32_t uart_last_activity(void){ return g_last_activity; }

/* ---- key ring ---- */
#define RING 64
static struct { uint8_t st, key; } ring[RING];
static int rh = 0, rt = 0;
static void ring_push(uint8_t st, uint8_t key){ g_last_activity = lv_tick_get(); int n=(rt+1)%RING; if(n!=rh){ ring[rt].st=st; ring[rt].key=key; rt=n; } }
int uart_pop_key(uint8_t *st, uint8_t *key){ if(rh==rt) return 0; *st=ring[rh].st; *key=ring[rh].key; rh=(rh+1)%RING; return 1; }
int uart_mods(void){ return g_mods; }

/* ---- reg response slot ---- */
static volatile int reg_ready = 0;
static uint8_t reg_buf[18]; static int reg_len = 0;

static uint8_t crc8(const uint8_t *d, int n){
	uint8_t c=0; for(int i=0;i<n;i++){ c^=d[i]; for(int b=0;b<8;b++) c=(c&0x80)?(uint8_t)((c<<1)^0x07):(uint8_t)(c<<1);} return c;
}
static void mod_update(uint8_t key, uint8_t st){
	int bit; switch(key){
	case DK_MOD_ALT:bit=MOD_ALT;break; case DK_MOD_SHL:bit=MOD_SHL;break; case DK_MOD_SHR:bit=MOD_SHR;break;
	case DK_MOD_SYM:bit=MOD_SYM;break; case DK_MOD_CTRL:bit=MOD_CTRL;break; default:return; }
	if(st==KS_RELEASE) g_mods&=~bit; else g_mods|=bit;
}
static void dispatch(uint8_t type, uint8_t len, const uint8_t *pl){
	switch(type){
	case T_KEY_EVENT: if(len>=2){ uint8_t st=pl[0],k=pl[1];
		if(k>=DK_MOD_ALT && k<=DK_MOD_CTRL) mod_update(k,st); else ring_push(st,k); } break;
	case T_REG_RESPONSE: reg_len=len>(int)sizeof reg_buf?(int)sizeof reg_buf:len; memcpy(reg_buf,pl,reg_len); reg_ready=1; break;
	default: break;   /* BOOT/LOCK/NACK ignored for now */
	}
}
static enum {WSOF,WTYPE,WLEN,WPAY,WCRC} pst=WSOF;
static uint8_t ptype,plen,ppay[16],pidx;
static void feed(uint8_t b){
	switch(pst){
	case WSOF: if(b==0xA5) pst=WTYPE; break;
	case WTYPE: ptype=b; pst=WLEN; break;
	case WLEN: if(b>16){pst=WSOF;break;} plen=b; pidx=0; pst=plen?WPAY:WCRC; break;
	case WPAY: ppay[pidx++]=b; if(pidx>=plen) pst=WCRC; break;
	case WCRC: { uint8_t t[18]; t[0]=ptype; t[1]=plen; memcpy(t+2,ppay,plen);
		if(crc8(t,2+plen)==b) dispatch(ptype,plen,ppay);
		pst=WSOF; } break;
	}
}
static void drain(void){ while(uart_is_readable(KF_KBD_UART)) feed((uint8_t)uart_getc(KF_KBD_UART)); }
void uart_poll(void){ drain(); }

static void tx(uint8_t type, const uint8_t *pl, int len){
	if(len<0) len=0;
	if(len>16) len=16;
	uint8_t hdr[2]; hdr[0]=type; hdr[1]=(uint8_t)len;
	uint8_t t[2+16]; t[0]=type; t[1]=(uint8_t)len; if(len) memcpy(t+2,pl,len);
	uint8_t crc=crc8(t,2+len);
	uart_putc_raw(KF_KBD_UART,0xA5);
	uart_putc_raw(KF_KBD_UART,hdr[0]);
	uart_putc_raw(KF_KBD_UART,hdr[1]);
	for(int i=0;i<len;i++) uart_putc_raw(KF_KBD_UART,pl[i]);
	uart_putc_raw(KF_KBD_UART,crc);
}

/* block (with timeout) pumping RX until the matching REG_RESPONSE arrives.
   One outstanding reg op at a time, per the spec's request/response pacing. */
#define REG_TIMEOUT_US  300000ULL   /* 300 ms — covers device EEPROM page-writes (tens of ms) */
static int reg_wait(uint8_t reg){
	uint64_t deadline = time_us_64() + REG_TIMEOUT_US;
	while(time_us_64() < deadline){
		drain();
		if(reg_ready && reg_len>=1 && reg_buf[0]==reg) return 1;
		if(reg_ready) reg_ready=0;   /* stale/mismatched response — keep waiting */
		tight_loop_contents();
	}
	return 0;
}

int reg_read(uint8_t reg, uint8_t *out, int maxlen){
	for(int tries=0; tries<3; tries++){
		reg_ready=0; tx(T_REG_READ,&reg,1);
		if(reg_wait(reg)){
			int n=reg_len-1; if(n>maxlen) n=maxlen; if(n>0 && out) memcpy(out,reg_buf+1,n); return n;
		}
	}
	return -1;
}
int reg_write(uint8_t reg, const uint8_t *data, int len){
	if(len<0) len=0;
	if(len>16) len=16;
	uint8_t pl[17]; pl[0]=reg; if(len) memcpy(pl+1,data,len);
	for(int tries=0; tries<3; tries++){
		reg_ready=0; tx(T_REG_WRITE,pl,1+len);
		if(reg_wait(reg)) return 0;
	}
	return -1;
}

void kbd_init(void){
	kf_hw_uart_init(KF_KBD_UART, KF_KBD_BAUD);   /* SDK uart_init(uart1, 115200) (see alias above) */
	gpio_set_function(KF_KBD_TX, UART_FUNCSEL_NUM(KF_KBD_UART, KF_KBD_TX));
	gpio_set_function(KF_KBD_RX, UART_FUNCSEL_NUM(KF_KBD_UART, KF_KBD_RX));
	uart_set_format(KF_KBD_UART, 8, 1, UART_PARITY_NONE);   /* 8N1 */
	uart_set_hw_flow(KF_KBD_UART, false, false);            /* no RTS/CTS */
	uart_set_fifo_enabled(KF_KBD_UART, true);
	pst=WSOF;
	tx(T_PING,0,0);   /* PING -> device replies BOOT */
}
