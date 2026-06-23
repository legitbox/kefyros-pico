// ui/deskconf.h — flat key=value desktop config store (persisted at KF_CONFIG).
// Public API for the icon layout (slot.<appid>=<cell>), wallpaper choice
// (wallpaper=..., fit=..., dim=...) and brightness (bkl/bk2) persistence.
#ifndef KEFYROS_DESKCONF_H
#define KEFYROS_DESKCONF_H

/* Load the config file from disk into memory. Call once at boot. */
void        deskconf_load(void);

/* String get/set. get() returns def if key absent; set() persists immediately. */
const char *deskconf_get(const char *key, const char *def);
void        deskconf_set(const char *key, const char *val);

/* Integer get/set (stored as decimal strings). */
int         deskconf_get_int(const char *key, int def);
void        deskconf_set_int(const char *key, int val);

#endif
