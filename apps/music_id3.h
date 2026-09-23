// apps/music_id3.h — ID3v2 (v2.2, v2.3, v2.4) and ID3v1 metadata and cover art parser.
#ifndef KF_MUSIC_ID3_H
#define KF_MUSIC_ID3_H

#include <stdint.h>
#include <stdio.h>

#define ID3_TXT_LEN 96

typedef struct {
    char     title[ID3_TXT_LEN];
    char     artist[ID3_TXT_LEN];
    char     album[ID3_TXT_LEN];
    uint32_t duration_sec;    /* 0 if not declared in TLEN */
    uint32_t cover_off;       /* byte offset in the MP3 file of embedded picture */
    uint32_t cover_len;       /* length of embedded picture in bytes (0 if none) */
    uint16_t track;           /* track number */
    uint8_t  cover_kind;      /* 'j' for JPEG, 'p' for PNG, 0 if none */
    uint8_t  has_tags;        /* 1 if valid ID3 tags were found */
} music_id3_info_t;

/* Parse ID3v2 and ID3v1 tags from an MP3 file.
   Returns 1 if file was opened, fills info struct (empty strings on missing tags). */
int music_id3_parse(const char *filepath, music_id3_info_t *info);

#endif /* KF_MUSIC_ID3_H */
