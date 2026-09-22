#ifndef KF_MUSIC_PNG_H
#define KF_MUSIC_PNG_H

#include <stdint.h>
#include <stdio.h>

/* Decode an embedded PNG directly from its bounded FLAC file region into an
   RGB565 thumbnail. Returns 0 for unsupported/corrupt images or low memory. */
int music_png_thumb(FILE *f, long start, long len, uint16_t *dst,
                    int max_edge, int *out_w, int *out_h);

#endif
