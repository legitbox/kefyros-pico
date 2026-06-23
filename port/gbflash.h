// port/gbflash.h — load a Game Boy ROM from SD into a reserved flash region and run it
// execute-in-place (XIP). Peanut-GB's gb_rom_read() is called on every ROM memory access
// (dozens per instruction), so the ROM must be random-access fast; flash-XIP is the only
// store on this board that qualifies (SRAM is too small for most carts, and the 8 MB PSRAM
// is a slow PIO block-store, not memory-mapped). The chosen ROM is copied from SD into the
// region once at load time, then gb_rom_read just indexes flash directly.
#ifndef KF_GBFLASH_H
#define KF_GBFLASH_H
#include <stdint.h>
#include <stddef.h>

// Reserved ROM region: the top 2 MB of the 4 MB flash. The firmware lives at the bottom
// (~1.4 MB) and never reaches this far; gbflash_load() asserts that at runtime.
#define GB_ROM_FLASH_OFFSET  (2u * 1024u * 1024u)      // from XIP_BASE (0x10000000)
#define GB_ROM_FLASH_SIZE    (2u * 1024u * 1024u)

// Memory-mapped pointer to the loaded ROM (valid after a successful gbflash_load).
const uint8_t *gbflash_rom(void);

// Copy `path` (a .gb/.gbc on the SD card) into the flash ROM region. Returns the ROM size
// in bytes on success, or <0 on error:
//   -1 open/read failure   -2 ROM larger than the region   -3 firmware overlaps the region
// `progress`, if non-NULL, is called as each 4 KB sector is written (for a UI bar).
long gbflash_load(const char *path, void (*progress)(int done, int total));

#endif /* KF_GBFLASH_H */
