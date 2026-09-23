// apps/music_id3.c — High-reliability ID3v2.2/v2.3/v2.4 and ID3v1 parser.
// Extracts title, artist, album, track number, duration, and embedded front cover (JPEG/PNG).
// Converts ISO-8859-1 and UTF-16 strings to clean UTF-8 for the Kefyros font renderer.
#include "music_id3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

static void safe_utf8_copy(char *dst, const char *src, int dstsz) {
    if (!dst || dstsz <= 0) return;
    if (!src) { dst[0] = 0; return; }
    int n = 0, max = dstsz - 1;
    while (src[n] && n < max) n++;
    if (src[n]) {
        while (n > 0 && (src[n] & 0xC0) == 0x80) n--;
        if (n > 0 && (src[n - 1] & 0x80)) n--;
    }
    memcpy(dst, src, n);
    dst[n] = 0;
}

/* Convert 16-bit Unicode codepoint to UTF-8 */
static int codepoint_to_utf8(uint32_t cp, char *out, int out_left) {
    if (cp < 0x80) {
        if (out_left < 1) return 0;
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        if (out_left < 2) return 0;
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        if (out_left < 3) return 0;
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else if (cp < 0x110000) {
        if (out_left < 4) return 0;
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
    return 0;
}

/* Decode raw tag payload into UTF-8 destination string */
static void decode_id3_text(uint8_t encoding, const uint8_t *data, uint32_t len, char *dst, int dstsz) {
    if (!dst || dstsz <= 0) return;
    dst[0] = 0;
    if (!data || len == 0) return;

    if (encoding == 0) {
        /* ISO-8859-1 / Latin-1 */
        int w = 0;
        for (uint32_t i = 0; i < len && w < dstsz - 1; i++) {
            uint8_t c = data[i];
            if (c == 0) break;
            w += codepoint_to_utf8(c, dst + w, dstsz - 1 - w);
        }
        dst[w] = 0;
    } else if (encoding == 3) {
        /* UTF-8 */
        uint32_t take = len < (uint32_t)(dstsz - 1) ? len : (uint32_t)(dstsz - 1);
        char tmp[ID3_TXT_LEN + 1];
        if (take > sizeof(tmp) - 1) take = sizeof(tmp) - 1;
        memcpy(tmp, data, take);
        tmp[take] = 0;
        safe_utf8_copy(dst, tmp, dstsz);
    } else if (encoding == 1 || encoding == 2) {
        /* UTF-16 with BOM (1) or UTF-16BE (2) */
        int big_endian = (encoding == 2) ? 1 : 0;
        uint32_t offset = 0;
        if (encoding == 1 && len >= 2) {
            if (data[0] == 0xFE && data[1] == 0xFF) {
                big_endian = 1;
                offset = 2;
            } else if (data[0] == 0xFF && data[1] == 0xFE) {
                big_endian = 0;
                offset = 2;
            }
        }
        int w = 0;
        while (offset + 1 < len && w < dstsz - 1) {
            uint16_t wch;
            if (big_endian) {
                wch = ((uint16_t)data[offset] << 8) | data[offset + 1];
            } else {
                wch = (uint16_t)data[offset] | ((uint16_t)data[offset + 1] << 8);
            }
            offset += 2;
            if (wch == 0) break;
            w += codepoint_to_utf8(wch, dst + w, dstsz - 1 - w);
        }
        dst[w] = 0;
    }
}

/* Parse "NN Artist - Title.mp3" as fallback */
static void parse_filename_fallback(const char *path, music_id3_info_t *info) {
    const char *slash = strrchr(path, '/');
    const char *fname = slash ? slash + 1 : path;
    char stem[128];
    safe_utf8_copy(stem, fname, sizeof(stem));
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    const char *p = stem;
    uint16_t trk = 0;
    while (isdigit((unsigned char)*p)) {
        trk = trk * 10 + (*p - '0');
        p++;
    }
    while (*p == ' ' || *p == '.' || *p == '-' || *p == '_') p++;
    if (trk > 0 && !info->track) info->track = trk;

    const char *sep = strstr(p, " - ");
    if (sep) {
        int al = (int)(sep - p);
        if (al > ID3_TXT_LEN - 1) al = ID3_TXT_LEN - 1;
        char tmp[ID3_TXT_LEN];
        memcpy(tmp, p, al);
        tmp[al] = 0;
        if (!info->artist[0]) safe_utf8_copy(info->artist, tmp, sizeof(info->artist));
        if (!info->title[0]) safe_utf8_copy(info->title, sep + 3, sizeof(info->title));
    } else {
        if (!info->title[0]) safe_utf8_copy(info->title, p, sizeof(info->title));
    }
    if (!info->title[0]) safe_utf8_copy(info->title, fname, sizeof(info->title));
}

int music_id3_parse(const char *filepath, music_id3_info_t *info) {
    if (!filepath || !info) return 0;
    memset(info, 0, sizeof(*info));

    FILE *f = fopen(filepath, "rb");
    if (!f) {
        parse_filename_fallback(filepath, info);
        return 0;
    }

    /* 1. Check ID3v2 header (10 bytes) */
    uint8_t hdr[10];
    int best_cover_prio = 0;

    if (fread(hdr, 1, 10, f) == 10 && memcmp(hdr, "ID3", 3) == 0) {
        uint8_t ver_major = hdr[3];
        uint8_t flags = hdr[5];
        /* syncsafe 28-bit tag size */
        uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) |
                            ((uint32_t)(hdr[7] & 0x7F) << 14) |
                            ((uint32_t)(hdr[8] & 0x7F) << 7)  |
                            (uint32_t)(hdr[9] & 0x7F);

        long pos = 10;
        if (flags & 0x40) {
            /* Extended header present: skip it */
            uint8_t ext_hdr[4];
            if (fread(ext_hdr, 1, 4, f) == 4) {
                uint32_t ext_sz = ((uint32_t)ext_hdr[0] << 24) | ((uint32_t)ext_hdr[1] << 16) |
                                  ((uint32_t)ext_hdr[2] << 8)  | ext_hdr[3];
                if (ver_major == 4) {
                    ext_sz = ((uint32_t)(ext_hdr[0] & 0x7F) << 21) |
                             ((uint32_t)(ext_hdr[1] & 0x7F) << 14) |
                             ((uint32_t)(ext_hdr[2] & 0x7F) << 7)  |
                             (uint32_t)(ext_hdr[3] & 0x7F);
                }
                if (ext_sz >= 4 && ext_sz <= tag_size) {
                    fseek(f, ext_sz - 4, SEEK_CUR);
                    pos += ext_sz;
                }
            }
        }

        long tag_end = 10 + (long)tag_size;

        while (pos + 10 <= tag_end) {
            fseek(f, pos, SEEK_SET);
            uint8_t fhdr[10];
            if (ver_major == 2) {
                /* ID3v2.2: 3-byte id, 3-byte size */
                if (fread(fhdr, 1, 6, f) != 6) break;
                if (fhdr[0] == 0) break; /* padding */
                uint32_t flen = ((uint32_t)fhdr[3] << 16) | ((uint32_t)fhdr[4] << 8) | fhdr[5];
                long fstart = pos + 6;
                pos = fstart + (long)flen;
                if (pos > tag_end) break;

                /* PIC frame (v2.2) */
                if (memcmp(fhdr, "PIC", 3) == 0 && flen >= 5) {
                    fseek(f, fstart, SEEK_SET);
                    uint8_t pic_enc;
                    char pic_fmt[4] = {0};
                    uint8_t pic_type;
                    if (fread(&pic_enc, 1, 1, f) == 1 &&
                        fread(pic_fmt, 1, 3, f) == 3 &&
                        fread(&pic_type, 1, 1, f) == 1) {
                        long cur = fstart + 5;
                        int prio = (pic_type == 3) ? 2 : 1;
                        if (pic_enc == 1 || pic_enc == 2) {
                            while (cur + 1 < pos) {
                                int b1 = fgetc(f);
                                int b2 = fgetc(f);
                                if (b1 == EOF || b2 == EOF) break;
                                cur += 2;
                                if (b1 == 0 && b2 == 0) break;
                            }
                        } else {
                            while (cur < pos) {
                                int b = fgetc(f);
                                if (b == EOF) break;
                                cur++;
                                if (b == 0) break;
                            }
                        }
                        if (cur + 4 <= pos && prio > best_cover_prio) {
                            fseek(f, cur, SEEK_SET);
                            uint8_t magic[4] = {0};
                            if (fread(magic, 1, 4, f) == 4) {
                                int kind = 0;
                                long img_off = cur;
                                if (magic[0] == 0xFF && magic[1] == 0xD8) kind = 'j';
                                else if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N') kind = 'p';
                                else if (magic[1] == 0xFF && magic[2] == 0xD8) { img_off = cur + 1; kind = 'j'; }
                                else if (!strcasecmp(pic_fmt, "JPG") || !strcasecmp(pic_fmt, "JPE")) kind = 'j';
                                else if (!strcasecmp(pic_fmt, "PNG")) kind = 'p';

                                if (kind && img_off < pos) {
                                    info->cover_off = (uint32_t)img_off;
                                    info->cover_len = (uint32_t)(pos - img_off);
                                    info->cover_kind = (uint8_t)kind;
                                    best_cover_prio = prio;
                                    info->has_tags = 1;
                                }
                            }
                        }
                    }
                }
                continue;
            }

            /* ID3v2.3 / ID3v2.4 */
            if (fread(fhdr, 1, 10, f) != 10) break;
            if (fhdr[0] == 0) break; /* padding */

            uint32_t flen = 0;
            if (ver_major == 4) {
                flen = ((uint32_t)(fhdr[4] & 0x7F) << 21) |
                       ((uint32_t)(fhdr[5] & 0x7F) << 14) |
                       ((uint32_t)(fhdr[6] & 0x7F) << 7)  |
                       (uint32_t)(fhdr[7] & 0x7F);
            } else {
                flen = ((uint32_t)fhdr[4] << 24) |
                       ((uint32_t)fhdr[5] << 16) |
                       ((uint32_t)fhdr[6] << 8)  |
                       (uint32_t)fhdr[7];
            }

            long fstart = pos + 10;
            pos = fstart + (long)flen;
            if (pos > tag_end) break;
            if (flen == 0) continue;

            /* Parse Text Frames */
            if (memcmp(fhdr, "TIT2", 4) == 0 || memcmp(fhdr, "TPE1", 4) == 0 ||
                memcmp(fhdr, "TALB", 4) == 0 || memcmp(fhdr, "TRCK", 4) == 0 ||
                memcmp(fhdr, "TLEN", 4) == 0) {
                uint8_t enc;
                if (fread(&enc, 1, 1, f) == 1 && flen > 1) {
                    uint32_t tlen = flen - 1;
                    if (tlen > 512) tlen = 512;
                    uint8_t *tbuf = malloc(tlen);
                    if (tbuf && fread(tbuf, 1, tlen, f) == tlen) {
                        char out[ID3_TXT_LEN];
                        decode_id3_text(enc, tbuf, tlen, out, sizeof(out));
                        if (memcmp(fhdr, "TIT2", 4) == 0 && out[0]) safe_utf8_copy(info->title, out, sizeof(info->title));
                        else if (memcmp(fhdr, "TPE1", 4) == 0 && out[0]) safe_utf8_copy(info->artist, out, sizeof(info->artist));
                        else if (memcmp(fhdr, "TALB", 4) == 0 && out[0]) safe_utf8_copy(info->album, out, sizeof(info->album));
                        else if (memcmp(fhdr, "TRCK", 4) == 0) info->track = (uint16_t)atoi(out);
                        else if (memcmp(fhdr, "TLEN", 4) == 0) info->duration_sec = (uint32_t)(atoi(out) / 1000);
                        info->has_tags = 1;
                    }
                    free(tbuf);
                }
            } else if (memcmp(fhdr, "APIC", 4) == 0 && flen >= 5) {
                /* APIC attached picture */
                fseek(f, fstart, SEEK_SET);
                uint8_t enc;
                if (fread(&enc, 1, 1, f) == 1) {
                    long cur = fstart + 1;
                    char mime[64] = {0};
                    int mpos = 0;
                    while (cur < pos) {
                        int mc = fgetc(f);
                        if (mc == EOF) break;
                        cur++;
                        if (mc == 0) break;
                        if (mpos < (int)sizeof(mime) - 1) mime[mpos++] = (char)mc;
                    }
                    mime[mpos] = 0;

                    if (cur < pos) {
                        uint8_t pic_type = (uint8_t)fgetc(f);
                        cur++;
                        int prio = (pic_type == 3) ? 2 : 1; /* 3 = front cover */

                        /* Skip description string based on encoding */
                        if (enc == 1 || enc == 2) {
                            /* UTF-16: two null bytes */
                            while (cur + 1 < pos) {
                                int b1 = fgetc(f);
                                int b2 = fgetc(f);
                                if (b1 == EOF || b2 == EOF) break;
                                cur += 2;
                                if (b1 == 0 && b2 == 0) break;
                            }
                        } else {
                            /* ISO-8859-1 or UTF-8: single null byte */
                            while (cur < pos) {
                                int b = fgetc(f);
                                if (b == EOF) break;
                                cur++;
                                if (b == 0) break;
                            }
                        }

                        /* Now cur points to the start of the image data! */
                        if (cur + 4 <= pos) {
                            fseek(f, cur, SEEK_SET);
                            uint8_t magic[4] = {0};
                            if (fread(magic, 1, 4, f) == 4) {
                                int kind = 0;
                                long img_off = cur;
                                if (magic[0] == 0xFF && magic[1] == 0xD8) {
                                    kind = 'j';
                                } else if (magic[0] == 0x89 && magic[1] == 'P' && magic[2] == 'N' && magic[3] == 'G') {
                                    kind = 'p';
                                } else if (magic[1] == 0xFF && magic[2] == 0xD8) {
                                    img_off = cur + 1;
                                    kind = 'j';
                                } else {
                                    /* Scan forward up to 64 bytes for JPEG/PNG marker */
                                    uint8_t sbuf[64];
                                    fseek(f, cur, SEEK_SET);
                                    size_t sgot = fread(sbuf, 1, sizeof(sbuf), f);
                                    for (size_t s = 0; s + 1 < sgot; s++) {
                                        if (sbuf[s] == 0xFF && sbuf[s+1] == 0xD8) {
                                            img_off = cur + (long)s;
                                            kind = 'j';
                                            break;
                                        }
                                        if (s + 3 < sgot && sbuf[s] == 0x89 && sbuf[s+1] == 'P' && sbuf[s+2] == 'N' && sbuf[s+3] == 'G') {
                                            img_off = cur + (long)s;
                                            kind = 'p';
                                            break;
                                        }
                                    }
                                }

                                if (kind && prio > best_cover_prio && img_off < pos) {
                                    info->cover_off = (uint32_t)img_off;
                                    info->cover_len = (uint32_t)(pos - img_off);
                                    info->cover_kind = (uint8_t)kind;
                                    best_cover_prio = prio;
                                    info->has_tags = 1;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    /* 2. Check ID3v1 at end of file (128 bytes) if title or artist is missing */
    if (!info->title[0] || !info->artist[0]) {
        fseek(f, -128, SEEK_END);
        uint8_t v1[128];
        if (fread(v1, 1, 128, f) == 128 && memcmp(v1, "TAG", 3) == 0) {
            info->has_tags = 1;
            char buf[31];
            if (!info->title[0]) {
                memcpy(buf, v1 + 3, 30); buf[30] = 0;
                int len = 29; while (len >= 0 && (buf[len] == ' ' || buf[len] == 0)) buf[len--] = 0;
                if (buf[0]) decode_id3_text(0, (uint8_t*)buf, strlen(buf), info->title, sizeof(info->title));
            }
            if (!info->artist[0]) {
                memcpy(buf, v1 + 33, 30); buf[30] = 0;
                int len = 29; while (len >= 0 && (buf[len] == ' ' || buf[len] == 0)) buf[len--] = 0;
                if (buf[0]) decode_id3_text(0, (uint8_t*)buf, strlen(buf), info->artist, sizeof(info->artist));
            }
            if (!info->album[0]) {
                memcpy(buf, v1 + 63, 30); buf[30] = 0;
                int len = 29; while (len >= 0 && (buf[len] == ' ' || buf[len] == 0)) buf[len--] = 0;
                if (buf[0]) decode_id3_text(0, (uint8_t*)buf, strlen(buf), info->album, sizeof(info->album));
            }
            if (!info->track && v1[125] == 0 && v1[126] != 0) {
                info->track = v1[126];
            }
        }
    }

    fclose(f);

    /* 3. Filename fallback */
    parse_filename_fallback(filepath, info);

    return 1;
}
