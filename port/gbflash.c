// port/gbflash.c — see gbflash.h. Streams a ROM from SD into a reserved flash region,
// one 4 KB sector at a time. Writing flash requires that NO core executes from XIP while a
// sector is erased/programmed: Core 0's flash routines run from RAM with its own IRQs
// masked, but Core 1 runs the display flush pump from XIP — so we halt Core 1 entirely for
// the whole write (disp_core1_reset) and relaunch a fresh pump afterwards
// (disp_core1_relaunch). This replaced flash_safe_execute(), whose multicore lockout
// dead-locked when a ROM was loaded a second time.
#include "gbflash.h"
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "hardware/flash.h"            // FLASH_SECTOR_SIZE, flash_range_erase/program
#include "hardware/regs/addressmap.h"  // XIP_BASE
#include "disp.h"                      // disp_core1_reset / disp_core1_relaunch

extern char __flash_binary_end[];      // last byte of the firmware image, set by the linker

#define SECTOR  FLASH_SECTOR_SIZE      // 4096

const uint8_t *gbflash_rom(void){ return (const uint8_t *)(XIP_BASE + GB_ROM_FLASH_OFFSET); }

long gbflash_load(const char *path, void (*progress)(int, int)){
	// Refuse if the firmware has grown into the ROM region (keeps a corrupt build from
	// silently eating its own code instead of the ROM space).
	if((uint32_t)((uintptr_t)__flash_binary_end - XIP_BASE) > GB_ROM_FLASH_OFFSET)
		return -3;

	FILE *f = fopen(path, "rb");
	if(!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(sz <= 0){ fclose(f); return -1; }
	if((uint32_t)sz > GB_ROM_FLASH_SIZE){ fclose(f); return -2; }

	static uint8_t sbuf[SECTOR];           // one-sector staging (static: off the stack)
	int  total  = (int)((sz + SECTOR - 1) / SECTOR);
	long result = sz;

	disp_core1_reset();                    // Core 1 offline for the whole write
	for(int i = 0; i < total; i++){
		long remain = sz - (long)i * SECTOR;
		size_t want = remain < SECTOR ? (size_t)remain : (size_t)SECTOR;
		memset(sbuf, 0xFF, SECTOR);         // pad the final partial sector with erased value
		if(fread(sbuf, 1, want, f) != want){ result = -1; break; }

		uint32_t off = GB_ROM_FLASH_OFFSET + (uint32_t)i * SECTOR;
		flash_range_erase(off, SECTOR);     // Core 0 masks its own IRQs internally
		flash_range_program(off, sbuf, SECTOR);

		if(progress) progress(i + 1, total);
	}
	disp_core1_relaunch();                 // restart the display flush pump

	fclose(f);
	return result;
}
