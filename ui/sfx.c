// ui/sfx.c — OS sound effects. REMOVED (2026-08-05): the wav-streaming + audio-ring
// lifecycle leaked ~8.4 KB of heap per app open/close cycle (proven by the Memory
// monitor: app_base flat with sfx off, +8.4K with it on). The KAPI device-table
// entry (k_sfx) keeps calling these no-ops so the ABI doesn't move.
#include "../kefyros.h"

void kf_sfx_play(const char *name){ (void)name; }   /* no-op: effects removed */
void sfx_poll(void){ }                               /* no-op */
