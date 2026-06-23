// port/gbflash.h — load a Game Boy ROM from SD into a RAM buffer for the emulator.
//
// Peanut-GB's gb_rom_read() is called on every ROM memory access (dozens per instruction),
// so the ROM must be fast random-access memory. We simply read it into a malloc'd SRAM
// buffer: trivially fast, and no flash-write / dual-core hazards. The catch is size — only
// ROMs that fit the free heap (~128 KB in practice, which covers nearly all classic DMG
// titles) load; bigger carts fail with -2 (a future PSRAM-paged path can lift that).
#ifndef KF_GBFLASH_H
#define KF_GBFLASH_H
#include <stdint.h>
#include <stddef.h>

// Pointer to the loaded ROM (valid after a successful gbflash_load); NULL when none.
const uint8_t *gbflash_rom(void);

// Read `path` (a .gb/.gbc on SD) into RAM. Returns the ROM size in bytes, or <0 on error:
//   -1 open/read failure   -2 ROM too big to fit in RAM
// `progress`, if non-NULL, is called as bytes are read (for a UI bar).
long gbflash_load(const char *path, void (*progress)(int done, int total));

// Release the ROM buffer (call when leaving a game).
void gbflash_free(void);

#endif /* KF_GBFLASH_H */
