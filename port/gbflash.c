// port/gbflash.c — see gbflash.h. Reads a ROM from SD straight into a malloc'd RAM buffer.
// No flash writing and no dual-core coordination: gb_rom_read() just indexes this buffer.
#include "gbflash.h"
#include <stdio.h>
#include <stdlib.h>

static uint8_t *s_rom;
static long     s_rom_sz;

const uint8_t *gbflash_rom(void){ return s_rom; }

void gbflash_free(void){ free(s_rom); s_rom = NULL; s_rom_sz = 0; }

long gbflash_load(const char *path, void (*progress)(int, int)){
	gbflash_free();

	FILE *f = fopen(path, "rb");
	if(!f) return -1;
	fseek(f, 0, SEEK_END);
	long sz = ftell(f);
	fseek(f, 0, SEEK_SET);
	if(sz <= 0){ fclose(f); return -1; }

	uint8_t *buf = malloc((size_t)sz);
	if(!buf){ fclose(f); return -2; }      // doesn't fit in the heap

	long off = 0;
	while(off < sz){
		size_t want = (sz - off) > 65536 ? 65536 : (size_t)(sz - off);
		size_t r = fread(buf + off, 1, want, f);
		if(r == 0) break;
		off += (long)r;
		if(progress) progress((int)off, (int)sz);
	}
	fclose(f);

	if(off != sz){ free(buf); return -1; }
	s_rom = buf; s_rom_sz = sz;
	return sz;
}
