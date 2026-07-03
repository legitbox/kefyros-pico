// ui/deskconf.c — flat key=value config persisted at /kefyros/config.txt (KF_CONFIG).
// Used for the desktop icon layout (slot.<appid>=<cell>) and the wallpaper
// choice (wallpaper=..., fit=...). Loaded once at boot, saved on every set().
#include "deskconf.h"
#include "../kefyros.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define KV_MAX   96     /* was 48 — the table filled (13 app slots + settings + wifi list +
                           accumulated dead keys from renamed/archived apps), so new keys like
                           slot.term / slot.demo were silently dropped and their icon moves never
                           persisted. 96 is comfortable headroom over the realistic live-key count. */
#define VAL_MAX  128    /* per-value buffer. Was 256, which at KV_MAX entries dominated .bss for no
                           reason — every real value (paths, WPA passphrase ≤63, api key ~35) fits
                           in <128. Shrinking this keeps the bigger table CHEAPER than the old 48×256
                           (avoids stealing boot heap from the WiFi stack + icon PNG decodes). */
#define CONF_DIR  KF_ROOT       /* "/kefyros" */
#define CONF_PATH KF_CONFIG     /* "/kefyros/config.txt" */

static struct { char k[40]; char v[VAL_MAX]; } kv[KV_MAX];
static int nkv = 0;

void deskconf_load(void){
	nkv = 0;
	FILE *f = fopen(CONF_PATH, "r");
	if(!f) return;
	char line[320];
	while(fgets(line, sizeof line, f) && nkv < KV_MAX){
		char *eq = strchr(line, '=');
		if(!eq) continue;
		*eq = 0;
		char *v = eq + 1;
		char *nl = strpbrk(v, "\r\n"); if(nl) *nl = 0;
		snprintf(kv[nkv].k, sizeof kv[nkv].k, "%s", line);
		snprintf(kv[nkv].v, sizeof kv[nkv].v, "%s", v);
		nkv++;
	}
	fclose(f);
}

const char *deskconf_get(const char *key, const char *def){
	for(int i=0;i<nkv;i++) if(!strcmp(kv[i].k, key)) return kv[i].v;
	return def;
}

static void deskconf_save(void){
	mkdir(CONF_DIR, 0755);
	char tmp[256];
	snprintf(tmp, sizeof tmp, "%s.tmp", CONF_PATH);
	FILE *f = fopen(tmp, "w");
	if(!f) return;
	int ok = 1;
	for(int i=0;i<nkv;i++)
		if(fprintf(f, "%s=%s\n", kv[i].k, kv[i].v) < 0){ ok = 0; break; }
	if(fclose(f) != 0) ok = 0;
	if(!ok || rename(tmp, CONF_PATH) != 0){   /* leave the existing config intact on failure */
		remove(tmp);
		return;
	}
}

void deskconf_set(const char *key, const char *val){
	int i;
	for(i=0;i<nkv;i++) if(!strcmp(kv[i].k, key)) break;
	if(i < nkv && !strcmp(kv[i].v, val)) return;   /* value unchanged: skip the no-op write */
	if(i == nkv){
		if(nkv >= KV_MAX) return;
		nkv++;
		snprintf(kv[i].k, sizeof kv[i].k, "%s", key);
	}
	snprintf(kv[i].v, sizeof kv[i].v, "%s", val);
	deskconf_save();
}

int deskconf_get_int(const char *key, int def){
	const char *v = deskconf_get(key, NULL);
	return v ? atoi(v) : def;
}
void deskconf_set_int(const char *key, int val){
	char b[16]; snprintf(b, sizeof b, "%d", val);
	deskconf_set(key, b);
}
