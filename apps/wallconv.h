// apps/wallconv.h — on-device JPEG -> RGB565-in-PSRAM converter for wallpapers.
#ifndef KF_WALLCONV_H
#define KF_WALLCONV_H
#include <stdint.h>

/* Read a JPEG's native pixel dimensions (cheap: parses the header only).
   Returns 1 on success, 0 if the file isn't a decodable baseline JPEG. */
int wc_dims(const char *path, int *w, int *h);

/* Box-average the native rectangle (rx0,ry0,rw,rh) of the baseline JPEG `path` down to
   outW x outH RGB565 (row-major, stride outW*2) into PSRAM at byte offset `dst`. Decodes
   at full resolution where it fits a bounded scratch (else a power-of-two descale), then
   area-averages — smooth, not blocky. Uses temporary PSRAM scratch above the current brk,
   freed before return. Returns 1 on success.
   - whole-image thumbnail: wc_render(p, 0,0, W,H, tw,th, dst)
   - square crop bake:      wc_render(p, cx,cy, side,side, 320,320, dst) */
int wc_render(const char *path, int rx0, int ry0, int rw, int rh, int outW, int outH, uint32_t dst);

#endif
