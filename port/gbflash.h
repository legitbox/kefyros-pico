// port/gbflash.h — load a Game Boy ROM into PSRAM and serve it through a small SRAM
// page cache, so big carts (GBC / MBC5, up to 8 MB) play without hogging the ~227 KB
// heap. Peanut-GB's gb_rom_read() is called on every ROM access, so the cache keeps the
// fixed bank-0 page pinned plus a few LRU 16 KB pages resident; misses stream one page
// from PSRAM (~1 ms at quad speed). The whole ROM lives in PSRAM (NOT memory-mapped,
// NOT flash), freed LIFO on exit.
#ifndef KF_GBFLASH_H
#define KF_GBFLASH_H
#include <stdint.h>
#include <stddef.h>

// Read `path` (a .gb/.gbc on SD) into PSRAM. Returns the ROM size in bytes, or <0:
//   -1 open/read failure   -2 doesn't fit in PSRAM (or PSRAM absent / OOM)
// `progress`, if non-NULL, is called as bytes are staged (for a UI bar).
long gbflash_load(const char *path, void (*progress)(int done, int total));

// Release the ROM: free the SRAM page cache and roll the PSRAM region back.
void gbflash_free(void);

// Pinned SRAM copy of bank 0 (the first 16 KB) — valid after a successful load.
const uint8_t *gbflash_page0(void);

// SRAM pointer to the 16 KB ROM page `pg` (= addr >> 14), streamed from PSRAM on a miss.
const uint8_t *gbflash_page(uint32_t pg);

#endif /* KF_GBFLASH_H */
