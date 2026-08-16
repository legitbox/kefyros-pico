// port/imgdec.h — bounded-RAM image decoder for the Spineko browser.
//
// Decodes JPEG, PNG, GIF (first composited frame) and SVG sitting in PSRAM into a downscaled RGB565 bitmap. Every
// path is RAM-bounded and pre-checks the heap, so a huge image fails cleanly (returns 0)
// instead of tripping the SDK's OOM-panic:
//   * JPEG — tjpgd, streamed MCU-by-MCU, subsampled to fit while decoding.
//   * PNG  — miniz tinfl streaming inflate + per-scanline de-filter + subsample; only the
//            32 KB inflate window + two scanlines live in RAM, never the whole image.
//   * SVG  — nanosvg, rasterized directly at the fit scale (output size is bounded).
#ifndef KF_IMGDEC_H
#define KF_IMGDEC_H
#include <stdint.h>

/* Decode the image in PSRAM [base, base+len) into a freshly malloc'd RGB565 bitmap,
   downscaled (integer step) to fit within (max_w, max_h) and max_out_bytes. On success returns 1 and sets
   *out (free() it), optional *alpha (free it; NULL for opaque), *ow, *oh. Returns 0 on unsupported format / decode error / when it
   wouldn't fit safely in the heap. `reserve` bytes of heap are kept free as a margin. */
int kf_img_decode(uint32_t base, uint32_t len, int max_w, int max_h,
                  uint32_t max_out_bytes, uint32_t reserve,
                  uint16_t **out, uint8_t **alpha, int *ow, int *oh);

/* Format sniff for the magic bytes already in `sig` (>=8 bytes). 0 none / 'j' jpeg /
   'p' png / 'g' gif / 's' svg. Lets the caller decide "is this response an image?" cheaply. */
int kf_img_sniff(const uint8_t *sig, int n);

#endif /* KF_IMGDEC_H */
