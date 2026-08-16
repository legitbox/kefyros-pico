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
#define VAL_MAX  128
#define CONF_DIR  KF_ROOT       /* "/kefyros" */
#define CONF_PATH KF_CONFIG     /* "/kefyros/config.txt" */

/* Keep only pointer pairs in .bss and allocate the strings at their real lengths.
   The old 96 x (40+128) table permanently burned 16 KB even on a fresh install. */
static struct { char *k,*v; } kv[KV_MAX];
static int nkv = 0;

void deskconf_load(void){
	for(int i=0;i<nkv;i++){free(kv[i].k);free(kv[i].v);kv[i].k=kv[i].v=NULL;}
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
		if(strlen(line)>39)line[39]=0;
		if(strlen(v)>=VAL_MAX)v[VAL_MAX-1]=0;
		char *kcopy=strdup(line),*vcopy=strdup(v);
		if(kcopy&&vcopy){kv[nkv].k=kcopy;kv[nkv].v=vcopy;nkv++;}
		else{free(kcopy);free(vcopy);}
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
	if(!ok){ remove(tmp); return; }           /* write failed: existing config intact */
	if(rename(tmp, CONF_PATH) != 0){          /* FatFs f_rename won't overwrite -> drop old, retry */
		remove(CONF_PATH);
		if(rename(tmp, CONF_PATH) != 0) remove(tmp);
	}
}

void deskconf_set(const char *key, const char *val){
	int i;
	for(i=0;i<nkv;i++) if(!strcmp(kv[i].k, key)) break;
	if(i < nkv && !strcmp(kv[i].v, val)) return;   /* value unchanged: skip the no-op write */
	if(i == nkv){
		if(nkv >= KV_MAX) return;
		char kb[40];snprintf(kb,sizeof kb,"%s",key);
		char vb[VAL_MAX];snprintf(vb,sizeof vb,"%s",val);
		char *kc=strdup(kb),*vc=strdup(vb);
		if(!kc||!vc){free(kc);free(vc);return;}
		kv[i].k=kc;kv[i].v=vc;nkv++;
	} else {
		char vb[VAL_MAX];snprintf(vb,sizeof vb,"%s",val);
		char *vc=strdup(vb);if(!vc)return;
		free(kv[i].v);kv[i].v=vc;
	}
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
