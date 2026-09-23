/* modplay.c — MOD/XM/S3M player as a windowed class-1 .kx.
 *
 * Lists *.mod / mod.* / *.xm / *.s3m files from /kefyros/mods.
 * MOD plays with pocketmod (MIT, rombankzero). The whole module is copied into a PSRAM
 * block; pocketmod only keeps the 1084-byte header in SRAM and fetches pattern rows and
 * sample windows through POCKETMOD_READ, so module size is bounded by PSRAM, not by the
 * 48 KiB arena.
 * XM and S3M play with libxm (WTFPL, Artefact2). It needs the unpacked module in RAM, so
 * its context comes from the kernel heap: roughly 1-2.5x the file size.
 *
 * Keys: UP/DOWN/PGUP/PGDN pick, ENTER play, SPACE pause, LEFT/RIGHT prev/next,
 * ESC quit. A song advances to the next file when it loops. The file list is one
 * label drawn as a scrolling window, since the kernel caps an app at 96 UI objects. */
#include "kapi.h"
#include "kapi_rt.h"
#include <stdio.h>
#include <string.h>

static const kapi *K;
/* newlib libm sets errno; the app has no libc to supply it */
int *__errno(void){ static int e; return &e; }
static kf_mem s_mod;          /* PSRAM copy of the current module */

#define POCKETMOD_READ(c, off, dst, n) K->mem->psram_read(s_mod, (off), (dst), (n))
#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"
#include <stdbool.h>
#include "libxm/xm.h"

#define MOD_DIR   "/kefyros/mods"
#define NAME_LEN  64
#define VIS       12          /* list rows shown */
#define RATE      32768
#define CHUNK     512         /* stereo frames rendered per step */

static char *s_pool;           /* heap: all file names, NUL-separated */
static uint16_t *s_off;        /* heap: pool offset of each name, sorted */
static int s_pool_used, s_pool_cap, s_nfiles, s_off_cap, s_cur = -1, s_paused, s_sel, s_top;
#define NAME(i) (s_pool + s_off[i])
static unsigned char s_hdr[1084];
static pocketmod_context s_pm;
static xm_context_t *s_xm;   /* heap; set while an XM/S3M plays */
static char s_title[29];
static float s_mix[CHUNK][2];
static int16_t s_out[CHUNK * 2];
static kui_obj s_now, s_list;
static char s_listbuf[VIS * 48];
static int s_shown_pat = -1, s_shown_line = -1;

/* does `n` end in `ext` (lowercase, with the dot), ignoring case */
static int has_ext(const char *n, const char *ext){
	size_t l = strlen(n), e = strlen(ext);
	if(l <= e) return 0;
	for(n += l - e; *ext; n++, ext++) if((*n | 32) != *ext) return 0;
	return 1;
}
static int is_xm(const char *n){ return has_ext(n, ".xm") || has_ext(n, ".s3m"); }
static int is_song(const char *n){
	if(has_ext(n, ".mod") || is_xm(n)) return 1;
	return (n[0]|32) == 'm' && (n[1]|32) == 'o' && (n[2]|32) == 'd' && n[3] == '.';  /* Amiga "mod.name" */
}

static int ncmp(const char *a, const char *b){
	for(;; a++, b++){
		int x = *a | (*a >= 'A' && *a <= 'Z' ? 32 : 0), y = *b | (*b >= 'A' && *b <= 'Z' ? 32 : 0);
		if(x != y || !x) return x - y;
	}
}

static void scan(void){
	K->fs->mkdir(MOD_DIR);
	kf_dir d = K->fs->opendir(MOD_DIR);
	if(!d) return;
	char name[NAME_LEN]; int dir;
	while(K->fs->readdir(d, name, sizeof name, &dir)){
		if(dir || name[0] == '.' || !is_song(name)) continue;
		int len = (int)strlen(name) + 1;
		if(s_pool_used + len > s_pool_cap){
			if(s_pool_cap + 1024 > 65535) break;   /* s_off is 16-bit */
			char *p = K->mem->realloc(s_pool, (size_t)s_pool_cap + 1024);
			if(!p) break;
			s_pool = p; s_pool_cap += 1024;
		}
		if(s_nfiles == s_off_cap){
			uint16_t *o = K->mem->realloc(s_off, (size_t)(s_off_cap + 64) * sizeof *o);
			if(!o) break;
			s_off = o; s_off_cap += 64;
		}
		memcpy(s_pool + s_pool_used, name, (size_t)len);
		int i = s_nfiles++;
		while(i > 0 && ncmp(NAME(i-1), name) > 0){ s_off[i] = s_off[i-1]; i--; }
		s_off[i] = (uint16_t)s_pool_used;
		s_pool_used += len;
	}
	K->fs->closedir(d);
}

static void set_now(const char *s){ K->ui->set_text(s_now, s); }

static void draw_list(void){
	if(s_sel < s_top) s_top = s_sel;
	if(s_sel >= s_top + VIS) s_top = s_sel - VIS + 1;
	char *o = s_listbuf, *end = s_listbuf + sizeof s_listbuf;
	for(int i = s_top; i < s_nfiles && i < s_top + VIS; i++)
		o += snprintf(o, (size_t)(end - o), "%c%c%.44s\n", i == s_sel ? '>' : ' ',
		              i == s_cur ? '*' : ' ', NAME(i));
	*o = 0;
	K->ui->set_text(s_list, s_listbuf);
}

static void stop(void){
	K->aud->out_stop();
	K->sys->idle_policy(KF_IDLE_NORMAL);
	if(s_mod){ K->mem->psram_free(s_mod); s_mod = 0; }
	if(s_xm){ K->mem->free(s_xm); s_xm = 0; }
	s_cur = -1;
}

/* Copy the file into PSRAM through s_mix as a bounce buffer. */
static int load(const char *path, long *size){
	kf_file f = K->fs->open(path, "rb");
	if(!f) return 0;
	K->fs->seek(f, 0, 2); *size = K->fs->tell(f); K->fs->seek(f, 0, 0);
	if(*size < 600 || !(s_mod = K->mem->psram_alloc((size_t)*size))){ K->fs->close(f); return 0; }
	for(long at = 0; at < *size;){
		int n = K->fs->read(f, s_mix, sizeof s_mix);
		if(n <= 0){ K->fs->close(f); return 0; }
		K->mem->psram_write(s_mod, (size_t)at, s_mix, (size_t)n);
		at += n;
	}
	K->fs->close(f);
	return 1;
}

/* Read the whole file into the heap and unpack it into a libxm context. */
static const char *load_xm(const char *path){
	const char *err = "Not enough RAM";
	kf_file f = K->fs->open(path, "rb");
	if(!f) return "Can't load";
	K->fs->seek(f, 0, 2); long size = K->fs->tell(f); K->fs->seek(f, 0, 0);
	char *data = size > 0 ? K->mem->alloc((size_t)size) : 0;
	xm_prescan_data_t *pre = K->mem->alloc(XM_PRESCAN_DATA_SIZE);
	if(data && pre){
		if(K->fs->read(f, data, (int)size) != size) err = "Can't load";
		else if(!xm_prescan_module(data, (uint32_t)size, pre)) err = "Not a module";
		else {
			char *pool = K->mem->alloc(xm_size_for_context(pre));
			if(pool) s_xm = xm_create_context(pool, pre, data, (uint32_t)size);
		}
		if(s_xm){
			/* the song name: XM at offset 17 (20 chars), S3M at 0 (28 chars) */
			int xm = size > 37 && !memcmp(data, "Extended Module: ", 17);
			memcpy(s_title, data + (xm ? 17 : 0), xm ? 20 : 28);
			s_title[xm ? 20 : 28] = 0;
			err = 0;
		}
	}
	K->mem->free(pre); K->mem->free(data);
	K->fs->close(f);
	return err;
}

static void play(int i){
	char path[128], msg[96]; long size = 0;
	stop();
	if(i < 0 || i >= s_nfiles) return;
	snprintf(path, sizeof path, MOD_DIR "/%s", NAME(i));
	if(is_xm(path)){
		const char *err = load_xm(path);
		if(err){ stop(); snprintf(msg, sizeof msg, "%s: %s", err, NAME(i)); set_now(msg); return; }
	} else {
		if(!load(path, &size)){ stop(); snprintf(msg, sizeof msg, "Can't load %s", NAME(i)); set_now(msg); return; }
		K->mem->psram_read(s_mod, 0, s_hdr, size < (long)sizeof s_hdr ? (size_t)size : sizeof s_hdr);
		if(!pocketmod_init(&s_pm, s_hdr, (int)size, RATE)){ stop(); snprintf(msg, sizeof msg, "Not a MOD: %s", NAME(i)); set_now(msg); return; }
		memcpy(s_title, s_hdr, 20); s_title[20] = 0;
	}
	if(K->aud->out_start(RATE) != KF_OK){ stop(); set_now("Audio busy"); return; }
	K->sys->idle_policy(KF_IDLE_KEEP_CLOCK);
	s_cur = s_sel = i; s_paused = 0; s_shown_pat = -1;
	draw_list();
}

static void pause_toggle(void){
	if(s_cur < 0) return;
	s_paused = !s_paused;
	if(s_paused){ K->aud->out_stop(); K->sys->idle_policy(KF_IDLE_NORMAL); }
	else { K->aud->out_start(RATE); K->sys->idle_policy(KF_IDLE_KEEP_CLOCK); }
	s_shown_pat = -1;
}

static void feed(void){
	while(K->aud->out_space() >= CHUNK){
		int n = CHUNK;
		if(s_xm) xm_generate_samples(s_xm, &s_mix[0][0], CHUNK);
		else n = pocketmod_render(&s_pm, s_mix, sizeof s_mix) / (int)sizeof s_mix[0];
		const float *m = &s_mix[0][0];
		for(int i = 0; i < n * 2; i++){
			float v = m[i] * 32767.0f;
			s_out[i] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)v;
		}
		K->aud->out_write(s_out, n);
		if(s_xm ? xm_get_loop_count(s_xm) > 0 : pocketmod_loop_count(&s_pm) > 0){ play((s_cur + 1) % s_nfiles); return; }
	}
}

static void show(void){
	if(s_cur < 0) return;
	int pos, len, row, ch;
	if(s_xm){
		uint8_t pi, pat, r; uint32_t smp;
		xm_get_position(s_xm, &pi, &pat, &r, &smp);
		pos = pi; row = r; len = xm_get_module_length(s_xm); ch = xm_get_number_of_channels(s_xm);
	} else {
		pos = s_pm.pattern; row = s_pm.line < 0 ? 0 : s_pm.line; len = s_pm.length; ch = s_pm.num_channels;
	}
	if(pos == s_shown_pat && row == s_shown_line) return;
	s_shown_pat = pos; s_shown_line = row;
	char msg[128];
	snprintf(msg, sizeof msg, "%s %s\n%s  pos %02d/%02d  row %02d  %dch",
	         s_paused ? "||" : ">", NAME(s_cur), s_title[0] ? s_title : "(untitled)", pos, len, row, ch);
	set_now(msg);
}

static void on_frame(void *ud){
	(void)ud;
	if(s_cur >= 0 && !s_paused) feed();
	show();
}

/* The kernel does not reclaim an app's heap, so give everything back. */
static void quit(void){
	stop();
	K->mem->free(s_pool); K->mem->free(s_off);
	s_pool = 0; s_off = 0; s_nfiles = 0;
}

static void on_key(void *ud, int key, int down){
	(void)ud;
	if(!down) return;
	if(key == KF_KEY_ESC){ quit(); K->sys->exit(0); }
	else if(key == ' ') pause_toggle();
	else if(key == KF_KEY_ENTER && s_nfiles) play(s_sel);
	else if((key == KF_KEY_UP || key == KF_KEY_DOWN || key == KF_KEY_PGUP || key == KF_KEY_PGDN) && s_nfiles){
		int step = key == KF_KEY_UP ? -1 : key == KF_KEY_DOWN ? 1 : key == KF_KEY_PGUP ? -VIS : VIS;
		if(step == 1 || step == -1) s_sel = (s_sel + step + s_nfiles) % s_nfiles;
		else { s_sel += step; if(s_sel < 0) s_sel = 0; if(s_sel >= s_nfiles) s_sel = s_nfiles - 1; }
		draw_list();
	}
	else if(key == KF_KEY_RIGHT && s_nfiles) play(s_cur < 0 ? 0 : (s_cur + 1) % s_nfiles);
	else if(key == KF_KEY_LEFT && s_nfiles) play(s_cur <= 0 ? s_nfiles - 1 : s_cur - 1);
}

static void on_close(void *ud){ (void)ud; quit(); }

int app_main(const kapi *k){
	K = k;
	kapi_rt_init(k);
	if(!(k->sys->caps() & KF_CAP_PSRAM) || !(k->sys->caps() & KF_CAP_AUDIO_OUT) || !k->ui){
		k->sys->log("modplay.kx: needs PSRAM, audio and the UI layer");
		return -1;
	}
	scan();

	kui_obj scr = k->ui->screen();
	k->ui->flex(scr, KUI_FLEX_COLUMN, 4);
	s_now = k->ui->label(scr, "");
	s_list = k->ui->label(scr, "");
	k->ui->grow(s_list, 1);
	k->ui->label(scr, "ENTER play  SPACE pause  </> prev/next  ESC quit");
	draw_list();
	set_now(s_nfiles ? "Pick a module" : "Put .mod/.xm/.s3m files in " MOD_DIR);

	k->sys->on_key(on_key, 0);
	k->sys->on_frame(on_frame, 0);
	k->sys->on_close(on_close, 0);
	return 0;
}
