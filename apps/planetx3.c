// apps/planetx3.c - Planet X3 DOS Real-Time Strategy game runner for Kefyros OS
// Runs natively on RP2350 with RGB565 display streaming and AdLib OPL2 sound.

#include "../kefyros.h"
#include "../ui/theme.h"
#include "../port/disp.h"
#include "../port/board.h"
#include "../port/lcdspi/lcdspi.h"
#include "px3_cpu.h"
#include "px3_opl2.h"

#include "hardware/spi.h"
#include "hardware/xip_cache.h"
#include "hardware/sync.h"
#include "pico/time.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "../port/clock.h"
#include "../port/bt_audio.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <sys/stat.h>

#define OPL_FIFO_SIZE 256
static volatile uint16_t s_opl_fifo[OPL_FIFO_SIZE];
static volatile uint32_t s_opl_head = 0;
static volatile uint32_t s_opl_tail = 0;
static volatile bool     s_audio_core1_running = false;
static uint8_t           s_adlib_addr_latch = 0;
static uint8_t           s_adlib_reg4 = 0;

static inline void opl_fifo_push(uint8_t reg, uint8_t val){
	uint32_t next = (s_opl_head + 1) & (OPL_FIFO_SIZE - 1);
	if(next != s_opl_tail){
		s_opl_fifo[s_opl_head] = ((uint16_t)reg << 8) | val;
		__dmb();
		s_opl_head = next;
	}
}

static inline bool opl_fifo_pop(uint8_t *reg, uint8_t *val){
	if(s_opl_head == s_opl_tail) return false;
	uint16_t item = s_opl_fifo[s_opl_tail];
	*reg = (item >> 8) & 0xFF;
	*val = item & 0xFF;
	__dmb();
	s_opl_tail = (s_opl_tail + 1) & (OPL_FIFO_SIZE - 1);
	return true;
}

static void px3_audio_core1_main(void){
	flash_safe_execute_core_init();
	static int16_t s_core1_buf[256 * 2];
	while(s_audio_core1_running){
		/* Drain pending register writes from Core 0 */
		uint8_t reg, val;
		while(opl_fifo_pop(&reg, &val)){
			px3_opl2_write_raw(reg, val);
		}

		/* Top up audio DMA ring buffer */
		if(kf_audio_running()){
			int space = kf_audio_space();
			if(space >= 256){
				px3_opl2_render_stereo(s_core1_buf, 256);
				kf_audio_write(s_core1_buf, 256);
			} else {
				tight_loop_contents();
			}
		} else {
			tight_loop_contents();
		}
	}
}

#define KF_QMI_PSRAM_CACHED ((uintptr_t)0x11000000u)

#define PX3_GAME_DIR  "/kefyros/games/planetx3"
#define PX3_SAVE_DIR  "/kefyros/saves/planetx3"
#define PX3_EXE_NAME  "PX3_VGA.COM"

#define GAME_W        320
#define GAME_H        200
#define GAME_Y_OFFSET ((LCD_H - GAME_H) / 2)  /* 60 px black border top & bottom */

#define AUDIO_MAX_FRAMES 2048

static int      s_running = 0;
static int      s_audio_chosen = 0;
static uint32_t s_psram_off = 0xFFFFFFFFu;
static uint8_t *s_ram = NULL;

static uint16_t s_palette[256];
static uint8_t  s_dac_idx = 0;
static uint8_t  s_dac_phase = 0;
static uint8_t  s_dac_rgb[3];

#define CHUNK_LINES 20
static uint16_t s_chunk_buf[GAME_W * CHUNK_LINES];
static int16_t  s_audio_buf[AUDIO_MAX_FRAMES * 2];

/* Key queue for INT 16h */
#define KEY_QUEUE_SIZE 16
static uint16_t s_key_queue[KEY_QUEUE_SIZE];
static int      s_key_head = 0;
static int      s_key_tail = 0;

static void queue_key(uint16_t key){
	int next = (s_key_head + 1) % KEY_QUEUE_SIZE;
	if(next != s_key_tail){
		s_key_queue[s_key_head] = key;
		s_key_head = next;
	}
}

static int pop_key(uint16_t *out){
	if(s_key_head == s_key_tail) return 0;
	*out = s_key_queue[s_key_tail];
	s_key_tail = (s_key_tail + 1) % KEY_QUEUE_SIZE;
	return 1;
}

static int peek_key(uint16_t *out){
	if(s_key_head == s_key_tail) return 0;
	*out = s_key_queue[s_key_tail];
	return 1;
}

/* File handle table for INT 21h */
#define MAX_DOS_FILES 16
static FILE *s_files[MAX_DOS_FILES];

static int alloc_file_handle(FILE *f){
	for(int i = 3; i < MAX_DOS_FILES; i++){
		if(!s_files[i]){
			s_files[i] = f;
			return i;
		}
	}
	return -1;
}

static void free_file_handle(int h){
	if(h >= 3 && h < MAX_DOS_FILES && s_files[h]){
		fclose(s_files[h]);
		s_files[h] = NULL;
	}
}

/* Map DOS filename to POSIX SD path */
static void sanitize_filename(const char *dos_name, char *out, size_t maxlen){
	const char *p = dos_name;
	/* Skip drive letter like A: or B: or C: */
	if(p[0] && p[1] == ':') p += 2;
	while(*p == '\\' || *p == '/' || *p == '.') p++;

	char clean[64];
	size_t len = 0;
	while(*p && len < sizeof(clean) - 1){
		clean[len++] = (char)toupper((unsigned char)*p);
		p++;
	}
	clean[len] = '\0';

	if(strstr(clean, "SAVE") || strstr(clean, ".DAT") && strstr(clean, "SAVE")){
		snprintf(out, maxlen, "%s/%s", PX3_SAVE_DIR, clean);
	} else {
		snprintf(out, maxlen, "%s/%s", PX3_GAME_DIR, clean);
	}
}

/* Port I/O handlers */
void px3_port_out(uint16_t port, uint8_t val){
	if(port == 0x388 || port == 0x228){
		s_adlib_addr_latch = val;
	} else if(port == 0x389 || port == 0x229){
		if(s_adlib_addr_latch == 0x04) s_adlib_reg4 = val;
		opl_fifo_push(s_adlib_addr_latch, val);
	} else if(port == 0x3C8){
		s_dac_idx = val;
		s_dac_phase = 0;
	} else if(port == 0x3C9){
		s_dac_rgb[s_dac_phase++] = val;
		if(s_dac_phase == 3){
			uint16_t r = (s_dac_rgb[0] * 31 + 31) / 63;
			uint16_t g = (s_dac_rgb[1] * 63 + 31) / 63;
			uint16_t b = (s_dac_rgb[2] * 31 + 31) / 63;
			s_palette[s_dac_idx] = (r << 11) | (g << 5) | b;
			s_dac_idx = (s_dac_idx + 1) & 0xFF;
			s_dac_phase = 0;
		}
	} else if(port == 0x20){
		/* PIC 8259 EOI command */
		return;
	}
}

uint8_t px3_port_in(uint16_t port){
	if(port == 0x388 || port == 0x228 || port == 0x389 || port == 0x229){
		uint8_t st = 0x00;
		if(s_adlib_reg4 & 1) st |= 0xC0;
		if(s_adlib_reg4 & 2) st |= 0xA0;
		return st;
	} else if(port == 0x3DA){
		static uint8_t vblank = 0;
		vblank ^= 0x08;
		return vblank;
	} else if(port == 0x61){
		static uint8_t p61 = 0;
		p61 ^= 0x10; /* Toggle refresh detection bit */
		return p61;
	} else if(port == 0x40){
		static uint8_t t0 = 0;
		t0 += 17;
		return t0;
	}
	return 0xFF;
}

/* DOS INT 21h and BIOS INT 10h/16h handlers */
int px3_handle_interrupt(uint8_t intnum){
	if(intnum == 0x21){
		uint8_t ah = regs.byteregs[regah];
		switch(ah){
		case 0x30: /* Get DOS version: return 5.00 */
			regs.byteregs[regal] = 5;
			regs.byteregs[regah] = 0;
			cf = 0;
			return 1;

		case 0x3D: { /* Open file */
			uint32_t addr = (((uint32_t)segregs[regds] << 4) + regs.wordregs[regdx]) & 0xFFFFF;
			char dos_name[64];
			for(size_t i = 0; i < sizeof(dos_name) - 1; i++){
				dos_name[i] = (char)s_ram[addr + i];
				if(!dos_name[i]) break;
			}
			dos_name[sizeof(dos_name) - 1] = '\0';

			char path[128];
			sanitize_filename(dos_name, path, sizeof(path));

			FILE *f = fopen(path, "rb");
			if(!f){
				/* Retry lowercase */
				for(size_t i = strlen(path); i > 0; i--){
					if(path[i] == '/') break;
					path[i] = (char)tolower((unsigned char)path[i]);
				}
				f = fopen(path, "rb");
			}

			if(f){
				int h = alloc_file_handle(f);
				if(h >= 0){
					regs.wordregs[regax] = (uint16_t)h;
					cf = 0;
					return 1;
				}
				fclose(f);
			}
			regs.wordregs[regax] = 2; /* File not found */
			cf = 1;
			return 1;
		}

		case 0x3C: { /* Create file */
			uint32_t addr = (((uint32_t)segregs[regds] << 4) + regs.wordregs[regdx]) & 0xFFFFF;
			char dos_name[64];
			for(size_t i = 0; i < sizeof(dos_name) - 1; i++){
				dos_name[i] = (char)s_ram[addr + i];
				if(!dos_name[i]) break;
			}
			dos_name[sizeof(dos_name) - 1] = '\0';

			char path[128];
			sanitize_filename(dos_name, path, sizeof(path));
			FILE *f = fopen(path, "wb");
			if(f){
				int h = alloc_file_handle(f);
				if(h >= 0){
					regs.wordregs[regax] = (uint16_t)h;
					cf = 0;
					return 1;
				}
				fclose(f);
			}
			regs.wordregs[regax] = 3; /* Path not found */
			cf = 1;
			return 1;
		}

		case 0x3E: { /* Close file */
			int h = regs.wordregs[regbx];
			free_file_handle(h);
			cf = 0;
			return 1;
		}

		case 0x3F: { /* Read file */
			int h = regs.wordregs[regbx];
			uint16_t count = regs.wordregs[regcx];
			uint32_t dest = (((uint32_t)segregs[regds] << 4) + regs.wordregs[regdx]) & 0xFFFFF;

			if(h >= 3 && h < MAX_DOS_FILES && s_files[h]){
				size_t read_bytes = fread(&s_ram[dest], 1, count, s_files[h]);
				regs.wordregs[regax] = (uint16_t)read_bytes;
				cf = 0;
			} else {
				regs.wordregs[regax] = 6; /* Invalid handle */
				cf = 1;
			}
			return 1;
		}

		case 0x40: { /* Write file */
			int h = regs.wordregs[regbx];
			uint16_t count = regs.wordregs[regcx];
			uint32_t src = (((uint32_t)segregs[regds] << 4) + regs.wordregs[regdx]) & 0xFFFFF;

			if(h >= 3 && h < MAX_DOS_FILES && s_files[h]){
				size_t written = fwrite(&s_ram[src], 1, count, s_files[h]);
				regs.wordregs[regax] = (uint16_t)written;
				cf = 0;
			} else {
				regs.wordregs[regax] = 6;
				cf = 1;
			}
			return 1;
		}

		case 0x42: { /* Lseek */
			int h = regs.wordregs[regbx];
			uint8_t method = regs.byteregs[regal];
			int32_t ofs = (int32_t)(((uint32_t)regs.wordregs[regcx] << 16) | regs.wordregs[regdx]);
			int origin = (method == 1) ? SEEK_CUR : (method == 2) ? SEEK_END : SEEK_SET;

			if(h >= 3 && h < MAX_DOS_FILES && s_files[h]){
				fseek(s_files[h], ofs, origin);
				long pos = ftell(s_files[h]);
				regs.wordregs[regdx] = (uint16_t)(pos >> 16);
				regs.wordregs[regax] = (uint16_t)(pos & 0xFFFF);
				cf = 0;
			} else {
				cf = 1;
			}
			return 1;
		}

		case 0x25: { /* Set Interrupt Vector */
			uint8_t vec = regs.byteregs[regal];
			uint32_t ivt_addr = vec * 4;
			s_ram[ivt_addr + 0] = (uint8_t)(regs.wordregs[regdx] & 0xFF);
			s_ram[ivt_addr + 1] = (uint8_t)(regs.wordregs[regdx] >> 8);
			s_ram[ivt_addr + 2] = (uint8_t)(segregs[regds] & 0xFF);
			s_ram[ivt_addr + 3] = (uint8_t)(segregs[regds] >> 8);
			return 1;
		}

		case 0x35: { /* Get Interrupt Vector */
			uint8_t vec = regs.byteregs[regal];
			uint32_t ivt_addr = vec * 4;
			regs.wordregs[regbx] = (uint16_t)s_ram[ivt_addr] | ((uint16_t)s_ram[ivt_addr + 1] << 8);
			segregs[reges] = (uint16_t)s_ram[ivt_addr + 2] | ((uint16_t)s_ram[ivt_addr + 3] << 8);
			return 1;
		}

		case 0x09: { /* Print string '$' terminated */
			uint32_t addr = (((uint32_t)segregs[regds] << 4) + regs.wordregs[regdx]) & 0xFFFFF;
			while(s_ram[addr] && s_ram[addr] != '$'){
				putchar(s_ram[addr++]);
			}
			return 1;
		}

		case 0x4C: /* Terminate program */
		case 0x00:
			s_running = 0;
			return 1;

		default:
			cf = 0;
			return 1;
		}
	} else if(intnum == 0x10){ /* Video BIOS */
		uint8_t ah = regs.byteregs[regah];
		if(ah == 0x00){ /* Set video mode */
			uint8_t mode = regs.byteregs[regal];
			if(mode == 0x03){
				/* Exit to text mode / DOS */
				s_running = 0;
			}
			return 1;
		} else if(ah == 0x1A){ /* Display Combination Code */
			regs.byteregs[regal] = 0x1A;
			regs.byteregs[regbl] = 0x08; /* VGA with analog color display */
			return 1;
		} else if(ah == 0x12){ /* Alternate select */
			regs.byteregs[regbh] = 0x00;
			regs.byteregs[regbl] = 0x03; /* 256k EGA/VGA memory */
			regs.byteregs[regch] = 0x00;
			regs.byteregs[regcl] = 0x09;
			return 1;
		} else if(ah == 0x0F){ /* Get video mode */
			regs.byteregs[regal] = 0x13; /* 320x200 256 colors */
			regs.byteregs[regah] = 40;   /* columns */
			regs.byteregs[regbh] = 0;    /* page */
			return 1;
		} else if(ah == 0x0E || ah == 0x09 || ah == 0x02){
			return 1; /* Ignore character / cursor output */
		}
		return 1;
	} else if(intnum == 0x16){ /* Keyboard BIOS */
		uint8_t ah = regs.byteregs[regah];
		if(ah == 0x00){ /* Get key */
			if(!s_audio_chosen){
				/* Automatically choose '3' (AdLib FM) at initial audio prompt */
				s_audio_chosen = 1;
				regs.wordregs[regax] = 0x0433; /* scan 04, ASCII '3' */
				zf = 0;
				return 1;
			}
			uint16_t k = 0;
			if(pop_key(&k)){
				regs.wordregs[regax] = k;
				zf = 0;
				return 1;
			}
			/* No key ready: rewind IP by 2 to re-execute INT 16h on next instruction */
			ip -= 2;
			return 1;
		} else if(ah == 0x01){ /* Check key */
			if(!s_audio_chosen){
				regs.wordregs[regax] = 0x0433; /* scan 04, ASCII '3' */
				zf = 0;
				return 1;
			}
			uint16_t k = 0;
			if(peek_key(&k)){
				regs.wordregs[regax] = k;
				zf = 0;
			} else {
				zf = 1;
			}
			return 1;
		}
		return 1;
	}

	return 0; /* Let standard CPU interrupt vector execute */
}

/* Keyboard mapping: PicoCalc key -> PC scancode (AH) + ASCII (AL) */
static void translate_key(uint8_t key){
	uint8_t scan = 0, ascii = key;
	switch(key){
	case DK_UP:    scan = 0x48; ascii = 0; break;
	case DK_DOWN:  scan = 0x50; ascii = 0; break;
	case DK_LEFT:  scan = 0x4B; ascii = 0; break;
	case DK_RIGHT: scan = 0x4D; ascii = 0; break;
	case DK_ENTER: scan = 0x1C; ascii = 0x0D; break;
	case DK_ESC:   scan = 0x01; ascii = 0x1B; break;
	case DK_TAB:   scan = 0x0F; ascii = 0x09; break;
	case DK_BACKSPACE: scan = 0x0E; ascii = 0x08; break;
	case ' ':      scan = 0x39; ascii = ' '; break;
	case 'b': case 'B': scan = 0x30; ascii = 'b'; break;
	case 'm': case 'M': scan = 0x32; ascii = 'm'; break;
	case 'h': case 'H': scan = 0x23; ascii = 'h'; break;
	case 't': case 'T': scan = 0x14; ascii = 't'; break;
	case 'd': case 'D': scan = 0x20; ascii = 'd'; break;
	case 'a': case 'A': scan = 0x1E; ascii = 'a'; break;
	case 'p': case 'P': scan = 0x19; ascii = 'p'; break;
	case 'u': case 'U': scan = 0x16; ascii = 'u'; break;
	case 's': case 'S': scan = 0x1F; ascii = 's'; break;
	case 'r': case 'R': scan = 0x13; ascii = 'r'; break;
	case '1': scan = 0x02; ascii = '1'; break;
	case '2': scan = 0x03; ascii = '2'; break;
	case '3': scan = 0x04; ascii = '3'; break;
	case '4': scan = 0x05; ascii = '4'; break;
	case '5': scan = 0x06; ascii = '5'; break;
	case '6': scan = 0x07; ascii = '6'; break;
	case '7': scan = 0x08; ascii = '7'; break;
	case '8': scan = 0x09; ascii = '8'; break;
	case '9': scan = 0x0A; ascii = '9'; break;
	case '0': scan = 0x0B; ascii = '0'; break;
	case '+': scan = 0x0D; ascii = '+'; break;
	case '-': scan = 0x0C; ascii = '-'; break;
	default:
		if(key >= 'a' && key <= 'z') ascii = key;
		else if(key >= 'A' && key <= 'Z') ascii = (uint8_t)tolower(key);
		scan = 0x2C; /* Generic scan */
		break;
	}
	queue_key((uint16_t)((scan << 8) | ascii));
}

static void trigger_timer_tick(void){
	if(!s_ram) return;
	uint32_t *tick = (uint32_t *)&s_ram[0x046C];
	(*tick)++;
	if(ifl){
		px3_cpu_interrupt(0x08);
	}
}

static void stop_planetx3(void){
	s_running = 0;

	/* Stop Core 1 audio engine */
	if(s_audio_core1_running){
		s_audio_core1_running = false;
		sleep_ms(5);
		multicore_reset_core1();
	}

	kf_audio_stop();

	xip_cache_clean_all();
	xip_cache_invalidate_all();

	for(int i = 0; i < MAX_DOS_FILES; i++){
		if(s_files[i]){ fclose(s_files[i]); s_files[i] = NULL; }
	}

	if(s_psram_off != 0xFFFFFFFFu){
		kf_psram_clients_invalidate();
		kf_psram_reset_alloc();
		s_psram_off = 0xFFFFFFFFu;
	}

	/* Restore display controller to standard 16-bit RGB565 */
	spi_write_command(0x3A);
	spi_write_data(0x55);

	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);

	/* Relaunch Core 1 display flush pump for LVGL */
	disp_core1_relaunch();

	/* Restore normal clock (300 MHz) for OS */
	kf_clock_normal();

	kf_grab_input(0);
	launcher_show();
	lv_obj_invalidate(lv_screen_active());
}

static void play_planetx3(void){
	/* Ensure save directory exists */
	mkdir(PX3_SAVE_DIR, 0777);

	/* Allocate 1 MB in PSRAM: on Pimoroni QMI, base address is 0x11000000 (cached) + off */
	s_psram_off = kf_psram_alloc(1024 * 1024);
	if(s_psram_off == 0xFFFFFFFFu){
		printf("PX3: Out of PSRAM\n");
		return;
	}
	s_ram = (uint8_t *)(KF_QMI_PSRAM_CACHED + s_psram_off);
	memset(s_ram, 0, 1024 * 1024);

	/* Setup initial VGA palette grayscale/default */
	for(int i = 0; i < 256; i++){
		uint16_t c = (i * 31) / 255;
		s_palette[i] = (c << 11) | ((c * 2) << 5) | c;
	}

	/* Setup DOS PSP at segment 0x1000 */
	uint32_t psp = 0x10000;
	s_ram[psp + 0] = 0xCD; s_ram[psp + 1] = 0x20; /* INT 20h */
	s_ram[psp + 2] = 0x00; s_ram[psp + 3] = 0xA0; /* Top of memory 0xA000 */
	s_ram[psp + 0x80] = 4;                        /* Command tail len */
	memcpy(&s_ram[psp + 0x81], " /ae\r", 5);      /* Request AdLib sound */

	/* Load PX3_VGA.COM from SD card into 0x1000:0x0100 (0x10100) */
	char exe_path[128];
	snprintf(exe_path, sizeof(exe_path), "%s/%s", PX3_GAME_DIR, PX3_EXE_NAME);
	FILE *f = fopen(exe_path, "rb");
	if(!f){
		snprintf(exe_path, sizeof(exe_path), "%s/px3_vga.com", PX3_GAME_DIR);
		f = fopen(exe_path, "rb");
	}
	if(!f){
		printf("PX3: Cannot open executable %s\n", exe_path);
		stop_planetx3();
		return;
	}
	size_t read_bytes = fread(&s_ram[0x10100], 1, 65536 - 256, f);
	fclose(f);
	printf("PX3: Loaded %u bytes\n", (unsigned)read_bytes);

	/* Setup BIOS ROM with IRET at F000:0000 (0xF0000) */
	s_ram[0xF0000] = 0xCF; /* IRET */

	/* Setup initial IVT for all 256 interrupts to point to F000:0000 */
	for(int i = 0; i < 256; i++){
		s_ram[i * 4 + 0] = 0x00;
		s_ram[i * 4 + 1] = 0x00;
		s_ram[i * 4 + 2] = 0x00;
		s_ram[i * 4 + 3] = 0xF0; /* F000:0000 */
	}

	/* Initialize BIOS Data Area (BDA at 0040:0000) */
	s_ram[0x0410] = 0x21; /* Equipment word: color 80x25, 1 floppy */
	s_ram[0x0449] = 0x13; /* Video mode 13h */
	s_ram[0x044A] = 40;   /* Columns */
	s_ram[0x0462] = 0;    /* Page */

	/* Initialize CPU and sound */
	px3_cpu_init(s_ram);
	px3_cpu_reset(0x1000, 0x0100, 0x1000, 0xFFFE);
	px3_opl2_init(44100);

	/* Boost clock to 350 MHz @ 1.20 V */
	kf_clock_boost();

	/* Initialize OPL2 synth and audio output */
	px3_opl2_init(44100);
	kf_audio_idle_unpark();
	if(!kf_audio_start_buffered(44100, 4096)){
		kf_audio_start_buffered(44100, 2048);
	}

	/* Park & reset Core 1 from LVGL display pump, then launch dedicated audio synth on Core 1 */
	disp_core1_reset();
	s_opl_head = 0;
	s_opl_tail = 0;
	s_audio_core1_running = true;
	multicore_launch_core1(px3_audio_core1_main);

	spi_set_baudrate(Pico_LCD_SPI_MOD, LCD_SPI_SPEED);
	draw_rect_spi(0, 0, LCD_W - 1, LCD_H - 1, 0); /* Black out panel */

	kf_grab_input(1);

	s_key_head = 0;
	s_key_tail = 0;
	s_audio_chosen = 0;
	s_running = 1;

	const uint64_t TICK_PERIOD_US = 13731u; /* 72.826 Hz PIT timer tick */
	const uint64_t FRAME_PERIOD_US = 33333u; /* 30.0 FPS */
	uint64_t next_tick_us = time_us_64();
	uint64_t next_frame_us = time_us_64() + FRAME_PERIOD_US;
	uint32_t radio_ms=0;

	while(s_running){
		uint32_t ms=to_ms_since_boot(get_absolute_time());
		if(ms-radio_ms>=2){ kf_net_poll(); kf_bt_poll(); radio_ms=ms; }
		/* 1. Poll UART keyboard */
		uart_poll();
		uint8_t st, key;
		while(uart_pop_key(&st, &key)){
			if((key == DK_BREAK || key == DK_ESC) && st != KS_RELEASE){
				stop_planetx3();
				return;
			}
			if(st == KS_PRESS){
				translate_key(key);
			}
		}

		/* 2. Execute CPU instructions paced to real-time timer ticks */
		uint64_t now = time_us_64();
		int ticks_run = 0;
		while(now >= next_tick_us && ticks_run < 8){
			trigger_timer_tick();
			px3_cpu_exec(12000); /* ~12k instructions per 13.7ms tick = ~870k IPS (~10 MHz XT) */
			next_tick_us += TICK_PERIOD_US;
			ticks_run++;
			now = time_us_64();
		}
		if(next_tick_us + TICK_PERIOD_US * 8 < now){
			next_tick_us = now;
		}

		/* 3. Display blit at 30 FPS */
		now = time_us_64();
		if(now >= next_frame_us){
			uint8_t *vga_ram = &s_ram[0xA0000];
			for(int cy = 0; cy < GAME_H; cy += CHUNK_LINES){
				int lines = (cy + CHUNK_LINES <= GAME_H) ? CHUNK_LINES : (GAME_H - cy);
				uint16_t *dst = s_chunk_buf;
				for(int y = 0; y < lines; y++){
					uint8_t *src_row = vga_ram + (cy + y) * GAME_W;
					for(int x = 0; x < GAME_W; x++){
						*dst++ = s_palette[src_row[x]];
					}
				}
				draw_buffer_spi(0, GAME_Y_OFFSET + cy, GAME_W - 1, GAME_Y_OFFSET + cy + lines - 1, (unsigned char *)s_chunk_buf);
			}
			next_frame_us += FRAME_PERIOD_US;
			if(next_frame_us + FRAME_PERIOD_US < now){
				next_frame_us = now + FRAME_PERIOD_US;
			}
		} else {
			tight_loop_contents();
		}
	}

	stop_planetx3();
}

static void on_missing_ok(lv_event_t *e){
	(void)e;
	kf_back_to_launcher();
}

void app_planetx3_open(void){
	/* Verify that game files exist on the SD card */
	char check_path[128];
	snprintf(check_path, sizeof(check_path), "%s/%s", PX3_GAME_DIR, PX3_EXE_NAME);
	struct stat st;
	if(stat(check_path, &st) != 0){
		snprintf(check_path, sizeof(check_path), "%s/px3_vga.com", PX3_GAME_DIR);
		if(stat(check_path, &st) != 0){
			/* Missing files dialog */
			lv_obj_t *scr = lv_obj_create(NULL);
			lv_obj_set_style_bg_color(scr, KF_BG, 0);
			kf_inset_top(scr);

			lv_obj_t *card = lv_obj_create(scr);
			lv_obj_set_size(card, 280, 200);
			lv_obj_center(card);
			lv_obj_set_style_bg_color(card, KF_CARD, 0);
			lv_obj_set_style_border_color(card, KF_BORDER_HI, 0);
			lv_obj_set_style_border_width(card, 2, 0);

			lv_obj_t *title = lv_label_create(card);
			lv_label_set_text(title, "Planet X3 Files Missing");
			lv_obj_set_style_text_font(title, KF_FONT_BIG, 0);
			lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 10);

			lv_obj_t *msg = lv_label_create(card);
			lv_label_set_text(msg, "Please copy the game files\nfrom DISTRO.720 to:\n\n/kefyros/games/planetx3/\n\non your SD card.");
			lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
			lv_obj_align(msg, LV_ALIGN_CENTER, 0, 5);

			lv_obj_t *btn = lv_button_create(card);
			lv_obj_set_size(btn, 80, 32);
			lv_obj_align(btn, LV_ALIGN_BOTTOM_MID, 0, -5);
			lv_obj_add_event_cb(btn, on_missing_ok, LV_EVENT_CLICKED, NULL);

			lv_obj_t *btn_lbl = lv_label_create(btn);
			lv_label_set_text(btn_lbl, "OK");
			lv_obj_center(btn_lbl);

			lv_screen_load(scr);
			return;
		}
	}

	/* Files present: launch game immediately */
	play_planetx3();
}
