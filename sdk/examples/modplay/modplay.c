/* modplay.c — ProTracker MOD player as a windowed class-1 .kx.
 *
 * Lists *.mod / mod.* files from /kefyros/mods and plays them with pocketmod (MIT,
 * rombankzero). The whole module is copied into a PSRAM block; pocketmod only keeps
 * the 1084-byte header in SRAM and fetches pattern rows and sample windows through
 * POCKETMOD_READ, so module size is bounded by PSRAM, not by the 48 KiB arena.
 *
 * Keys: UP/DOWN/PGUP/PGDN pick, ENTER play, SPACE pause, LEFT/RIGHT prev/next,
 * ESC quit. A song advances to the next file when it loops. The file list is one
 * label drawn as a scrolling window, since the kernel caps an app at 96 UI objects. */
#include "kapi.h"
#include "kapi_rt.h"
#include <stdio.h>
#include <string.h>

static const kapi *K;
static kf_mem s_mod;          /* PSRAM copy of the current module */

#define POCKETMOD_READ(c, off, dst, n) K->mem->psram_read(s_mod, (off), (dst), (n))
#define POCKETMOD_IMPLEMENTATION
#include "pocketmod.h"

#define MOD_DIR   "/kefyros/mods"
#define MAX_FILES 400
#define POOL      12288       /* all file names, NUL-separated */
#define NAME_LEN  64
#define VIS       12          /* list rows shown */
#define RATE      32768
#define CHUNK     512         /* stereo frames rendered per step */

static char s_pool[POOL];
static uint16_t s_off[MAX_FILES];
static int s_pool_used, s_nfiles, s_cur = -1, s_paused, s_sel, s_top;
#define NAME(i) (s_pool + s_off[i])
static unsigned char s_hdr[1084];
static pocketmod_context s_pm;
static float s_mix[CHUNK][2];
static int16_t s_out[CHUNK * 2];
static kui_obj s_now, s_list;
static char s_listbuf[VIS * 48];
static int s_shown_pat = -1, s_shown_line = -1;

static int is_mod(const char *n){
	size_t l = strlen(n);
	if(l > 4 && n[l-4] == '.' && (n[l-3]|32) == 'm' && (n[l-2]|32) == 'o' && (n[l-1]|32) == 'd') return 1;
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
	while(s_nfiles < MAX_FILES && K->fs->readdir(d, name, sizeof name, &dir)){
		if(dir || name[0] == '.' || !is_mod(name)) continue;
		int len = (int)strlen(name) + 1;
		if(s_pool_used + len > POOL) break;
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

static void play(int i){
	char path[128], msg[96]; long size = 0;
	stop();
	if(i < 0 || i >= s_nfiles) return;
	snprintf(path, sizeof path, MOD_DIR "/%s", NAME(i));
	if(!load(path, &size)){ stop(); snprintf(msg, sizeof msg, "Can't load %s", NAME(i)); set_now(msg); return; }
	K->mem->psram_read(s_mod, 0, s_hdr, size < (long)sizeof s_hdr ? (size_t)size : sizeof s_hdr);
	if(!pocketmod_init(&s_pm, s_hdr, (int)size, RATE)){ stop(); snprintf(msg, sizeof msg, "Not a MOD: %s", NAME(i)); set_now(msg); return; }
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
		int n = pocketmod_render(&s_pm, s_mix, sizeof s_mix) / (int)sizeof s_mix[0];
		const float *m = &s_mix[0][0];
		for(int i = 0; i < n * 2; i++){
			float v = m[i] * 32767.0f;
			s_out[i] = v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : (int16_t)v;
		}
		K->aud->out_write(s_out, n);
		if(pocketmod_loop_count(&s_pm) > 0){ play((s_cur + 1) % s_nfiles); return; }
	}
}

static void show(void){
	if(s_cur < 0 || (s_pm.pattern == s_shown_pat && s_pm.line == s_shown_line)) return;
	s_shown_pat = s_pm.pattern; s_shown_line = s_pm.line;
	char title[21], msg[128];
	memcpy(title, s_hdr, 20); title[20] = 0;
	snprintf(msg, sizeof msg, "%s %s\n%s  pos %02d/%02d  row %02d  %dch",
	         s_paused ? "||" : ">", NAME(s_cur), title[0] ? title : "(untitled)",
	         s_pm.pattern, s_pm.length, s_pm.line < 0 ? 0 : s_pm.line, s_pm.num_channels);
	set_now(msg);
}

static void on_frame(void *ud){
	(void)ud;
	if(s_cur >= 0 && !s_paused) feed();
	show();
}

static void on_key(void *ud, int key, int down){
	(void)ud;
	if(!down) return;
	if(key == KF_KEY_ESC){ stop(); K->sys->exit(0); }
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

static void on_close(void *ud){ (void)ud; stop(); }

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
	set_now(s_nfiles ? "Pick a module" : "Put .mod files in " MOD_DIR);

	k->sys->on_key(on_key, 0);
	k->sys->on_frame(on_frame, 0);
	k->sys->on_close(on_close, 0);
	return 0;
}
