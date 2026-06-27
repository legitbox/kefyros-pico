// apps/wallconv.h — on-device JPEG -> RGB565-in-PSRAM converter for wallpapers.
#ifndef KF_WALLCONV_H
#define KF_WALLCONV_H
#include <stdint.h>

/* Read a JPEG's native pixel dimensions (cheap: parses the header only).
   Returns 1 on success, 0 if the file isn't a decodable baseline JPEG. */
int wc_dims(const char *path, int *w, int *h);

/* Decode `path` (JPEG) at 1/(1<<scale) and copy the window whose top-left in the
   *scaled* image is (sx0,sy0) and whose size is dstW x dstH into PSRAM at byte
   offset `dst`, as RGB565 (row-major, stride dstW*2). Pixels of the window that
   fall outside the decoded image are left untouched. Returns 1 on success. */
int wc_decode_region(const char *path, int scale, int sx0, int sy0,
                     int dstW, int dstH, uint32_t dst);

/* Bake: decode `path`, take the square native window (cx,cy,side) [pixels], and
   resample it to out_px x out_px RGB565 into PSRAM at `dst`. Uses a temporary
   PSRAM scratch region (allocated above the current brk, freed before return).
   Returns 1 on success. */
int wc_bake(const char *path, int cx, int cy, int side, int out_px, uint32_t dst);

#endif
