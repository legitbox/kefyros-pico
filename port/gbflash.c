// port/gbflash.c — see gbflash.h. The ROM lives in PSRAM (quad block store); reads go
// through a tiny SRAM page cache: slot 0 pins bank 0 (read constantly), the rest are LRU
// 16 KB pages. A miss streams one page from PSRAM. This keeps big carts off the heap and
// makes gb_rom_read cheap for the common case (the caller also caches the active-bank
// pointer, so the cache is only consulted on a bank change).
#include "gbflash.h"
#include "../kefyros.h"          // kf_psram_*
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PG_SZ   (16u * 1024u)
#define NSLOT   4                // [0] = pinned page 0; [1..NSLOT-1] = LRU

static uint32_t s_base = 0xFFFFFFFFu;   // PSRAM byte address of the ROM (none = 0xFFFFFFFF)
static long     s_sz;
static uint8_t *s_slot[NSLOT];
static uint32_t s_pg[NSLOT];            // ROM page held in each slot (0xFFFFFFFF = empty)
static uint32_t s_used[NSLOT], s_tick;  // LRU clock

void gbflash_free(void){
	for(int i = 0; i < NSLOT; i++){ free(s_slot[i]); s_slot[i] = NULL; s_pg[i] = 0xFFFFFFFFu; }
	if(s_base != 0xFFFFFFFFu){ kf_psram_free_to(s_base); s_base = 0xFFFFFFFFu; }
	s_sz = 0; s_tick = 0;
}

const uint8_t *gbflash_page0(void){ return s_slot[0]; }

/* Stream 16 KB page `pg` from PSRAM into slot `slot` (0xFF-fill past the ROM end). */
static void load_page(int slot, uint32_t pg){
	uint32_t off = pg * PG_SZ;
	if(off >= (uint32_t)s_sz){
		memset(s_slot[slot], 0xFF, PG_SZ);                 // past end -> open bus
	} else {
		uint32_t n = PG_SZ;
		if(off + n > (uint32_t)s_sz) n = (uint32_t)s_sz - off;
		kf_psram_read(s_base + off, s_slot[slot], n);
		if(n < PG_SZ) memset(s_slot[slot] + n, 0xFF, PG_SZ - n);
	}
	s_pg[slot] = pg; s_used[slot] = ++s_tick;
}

const uint8_t *gbflash_page(uint32_t pg){
	if(pg == 0){ s_used[0] = ++s_tick; return s_slot[0]; }
	for(int i = 1; i < NSLOT; i++)
		if(s_pg[i] == pg){ s_used[i] = ++s_tick; return s_slot[i]; }   /* hit */
	int lru = 1;                                                       /* miss: evict LRU */
	for(int i = 2; i < NSLOT; i++) if(s_used[i] < s_used[lru]) lru = i;
	load_page(lru, pg);
	return s_slot[lru];
}

long gbflash_load(const char *path, void (*progress)(int, int)){
	gbflash_free();
	if(!kf_psram_size()) return -2;

	FILE *f = fopen(path, "rb");
	if(!f) return -1;
	fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
	if(sz <= 0){ fclose(f); return -1; }

	uint32_t base = kf_psram_alloc((uint32_t)sz);
	if(base == 0xFFFFFFFFu){ fclose(f); return -2; }    // doesn't fit in PSRAM

	uint8_t *tmp = malloc(4096);                         // SD -> PSRAM staging
	if(!tmp){ fclose(f); kf_psram_free_to(base); return -2; }
	long off = 0;
	while(off < sz){
		size_t want = (sz - off) > 4096 ? 4096 : (size_t)(sz - off);
		size_t r = fread(tmp, 1, want, f);
		if(r == 0) break;
		kf_psram_write(base + (uint32_t)off, tmp, (uint32_t)r);
		off += (long)r;
		if(progress) progress((int)off, (int)sz);
	}
	free(tmp); fclose(f);
	if(off != sz){ kf_psram_free_to(base); return -1; }

	s_base = base; s_sz = sz; s_tick = 0;
	for(int i = 0; i < NSLOT; i++){
		s_slot[i] = malloc(PG_SZ); s_pg[i] = 0xFFFFFFFFu; s_used[i] = 0;
		if(!s_slot[i]){ gbflash_free(); return -2; }
	}
	load_page(0, 0);                                     // pin bank 0 in slot 0
	return sz;
}
