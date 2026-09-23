// apps/music.c — High-Fidelity MP3 player ("iPod-style").
// Features:
//   - Multi-mode library: All Songs, By Artist, By Album, and direct File Browser.
//   - Split-screen UI on 320x320 panel: Top-left browser & top-right 120x120 album art;
//     Bottom section with timeline, elapsed/remaining time, controls, and stereo line visualizer.
//   - Green-to-red dual horizontal stripe stereo line visualizer.
//   - High-quality MP3 playback via dr_mp3 (up to 320 kbps CBR/VBR, stereo 44.1/48 kHz).
//   - Full dual audio backend support: PWM (noise-shaped DMA DAC) and Bluetooth A2DP.
//   - F1 Backlight toggle & HOLD mode lockout (state unfucked after exit/restart).
//   - PSRAM track metadata database, group tables, directory items, and streaming cache.
#include "../kefyros.h"
#include "../ui/theme.h"
#include "../ui/deskconf.h"
#include "../port/bt_audio.h"
#include "pico/stdlib.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <limits.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define DR_MP3_IMPLEMENTATION
#define DR_MP3_NO_SIMD
#define DRMP3_MIN_DATA_CHUNK_SIZE 2048
#define DRMP3_DATA_CHUNK_SIZE     4096
#include "dr_mp3/dr_mp3.h"

#include "src/libs/tjpgd/tjpgd.h"
#include "music_png.h"
#include "music_id3.h"
#include "src/font/lv_binfont_loader.h"

/* ------------------------------------------------------------------ config -- */
#define MUSDIR          "/kefyros/music"
#define MAX_TRACKS      1500
#define MAX_GROUPS      256
#define REC_PATH        224
#define REC_TXT         96
#define VIS_ROWS        7
#define ROW_CENTER      (VIS_ROWS / 2)
#define LEFT_W          182
#define ART_W           (LCD_W - LEFT_W - 6)
#define THUMB_EDGE      120
#define COVER_BYTES     ((size_t)THUMB_EDGE * THUMB_EDGE * 2)
#define MUSIC_RING_FRAMES 4096
#define CHUNK           512

/* Browsing modes */
typedef enum {
    MODE_ALL = 0,
    MODE_ARTIST,
    MODE_ALBUM,
    MODE_FILES,
    MODE_COUNT
} browse_mode_t;

/* Group entry for Artist and Album aggregations (stored in PSRAM) */
typedef struct {
    char     name[REC_TXT];
    char     sub[REC_TXT];
    uint16_t track_count;
    uint16_t first_idx;
} group_entry_t;

/* File entry for direct directory navigation (stored in PSRAM) */
typedef struct {
    char    name[REC_PATH];
    uint8_t is_dir;
} file_item_t;

/* Track record in PSRAM */
typedef struct {
    char     path[REC_PATH];    /* relative to MUSDIR or absolute if from file browser */
    char     title[REC_TXT];
    char     artist[REC_TXT];
    char     album[REC_TXT];
    uint32_t duration;         /* seconds */
    uint32_t rate;             /* sample rate */
    uint32_t cover_off;        /* embedded picture offset */
    uint32_t cover_len;        /* embedded picture length */
    uint16_t track;            /* track number */
    uint8_t  bits;
    uint8_t  channels;
    uint8_t  detail;           /* 1 once ID3 tags parsed */
    uint8_t  cover_kind;       /* 'j' JPEG, 'p' PNG */
    uint8_t  pad[2];
} rec_t;

/* ------------------------------------------------------------------ state --- */
static int            g_active;
static const lv_font_t *g_jpfont;
static int            g_built;

/* PSRAM Allocator Base Pointers */
static uint32_t       g_idx_base     = 0xFFFFFFFFu; /* rec_t[MAX_TRACKS] */
static uint32_t       g_artists_base = 0xFFFFFFFFu; /* group_entry_t[MAX_GROUPS] */
static uint32_t       g_albums_base  = 0xFFFFFFFFu; /* group_entry_t[MAX_GROUPS] */
static uint32_t       g_dir_base     = 0xFFFFFFFFu; /* file_item_t[MAX_DIR_ITEMS] */
static uint32_t       g_drill_base   = 0xFFFFFFFFu; /* uint16_t[MAX_TRACKS] */

static int            g_n;                          /* total indexed tracks */
static int            g_full;
static uint16_t       g_sorted[MAX_TRACKS];         /* all songs order -> rec index */
static uint16_t       g_order[MAX_TRACKS];          /* playback playlist order */
static int            g_order_pos;
static int            g_shuffle = 0;
static int            g_repeat = 0;                 /* 0: off, 1: all, 2: one */

/* Modes & Navigation */
static browse_mode_t  g_mode = MODE_ALL;
static int            g_drill_down = 0;             /* 1 if inside an artist or album tracklist */
static int            g_drill_group = -1;           /* active group index */
static int            g_drill_n = 0;

static int            g_artist_n = 0;
static int            g_album_n = 0;

/* File browser mode state */
static char           g_cur_dir[REC_PATH] = MUSDIR;
#define MAX_DIR_ITEMS 128
static int            g_dir_count = 0;

/* Selection cursors */
static int            g_sel = 0;                    /* selection index in current view */
static int            g_play_track_idx = -1;        /* currently playing rec_t index (-1 none) */

/* Custom logical stream IO callbacks for dr_mp3 to eliminate stdio buffering bugs */
typedef struct {
    FILE *f;
    long  pos;
    long  len;
} mp3_io_t;

static size_t mp3_io_read(void *pUserData, void *pBufferOut, size_t bytesToRead) {
    mp3_io_t *io = (mp3_io_t*)pUserData;
    if (!io || !io->f) return 0;
    size_t r = fread(pBufferOut, 1, bytesToRead, io->f);
    io->pos += (long)r;
    return r;
}

static drmp3_bool32 mp3_io_seek(void *pUserData, int offset, drmp3_seek_origin origin) {
    mp3_io_t *io = (mp3_io_t*)pUserData;
    if (!io || !io->f) return DRMP3_FALSE;
    long target = 0;
    if (origin == DRMP3_SEEK_SET) {
        target = offset;
    } else if (origin == DRMP3_SEEK_CUR) {
        target = io->pos + offset;
    } else if (origin == DRMP3_SEEK_END) {
        target = io->len + offset;
    }
    if (target < 0) target = 0;
    if (target > io->len) target = io->len;
    if (fseek(io->f, target, SEEK_SET) == 0) {
        io->pos = target;
        return DRMP3_TRUE;
    }
    return DRMP3_FALSE;
}

static drmp3_bool32 mp3_io_tell(void *pUserData, drmp3_int64 *pCursor) {
    mp3_io_t *io = (mp3_io_t*)pUserData;
    if (!io || !pCursor) return DRMP3_FALSE;
    *pCursor = io->pos;
    return DRMP3_TRUE;
}

/* Playback & Decoder (Static decoder object in .bss to guarantee zero OOM failures) */
static drmp3          g_dec_obj;
static int            g_dec_active = 0;
static mp3_io_t       g_mp3_io;
static int            g_paused = 0;
static uint32_t       g_native_rate = 0;
static int            g_channels = 2;
static int            g_playhz = 44100;
static uint64_t       g_pos_frames = 0;
static uint64_t       g_total_frames = 0;
static int16_t        g_pcm_raw[CHUNK * 2];
static int16_t        g_pcm_out[CHUNK * 2];

/* Visualizer Levels (0..100) */
static int            g_vis_l = 0;
static int            g_vis_r = 0;

/* HOLD Mode & Backlight */
static int            g_hold_mode = 0;
static int            g_saved_bkl = -1;

/* Cover Art */
static uint16_t      *g_cover_live = NULL;
static uint16_t      *g_thumb = NULL;
static int            g_thumb_edge = 0;
static lv_image_dsc_t g_cover_dsc;
static int            g_cover_ok = 0;
static const char    *g_cover_error = "NO ART";
static int            g_cover_track_idx = -1;
static int            g_focus_dirty = 0;
static uint32_t       g_focus_t = 0;

/* UI Widgets */
static lv_obj_t      *g_scr;
static lv_obj_t      *g_box_browser, *g_tabs[MODE_COUNT], *g_rows[VIS_ROWS];
static lv_obj_t      *g_box_art, *g_img_cover, *g_lbl_noart, *g_lbl_art_badge;
static lv_obj_t      *g_box_player;
static lv_obj_t      *g_lbl_title, *g_lbl_meta;
static lv_obj_t      *g_bar_timeline, *g_lbl_time, *g_lbl_status;
static lv_obj_t      *g_bar_vis_l, *g_bar_vis_r;
static lv_obj_t      *g_lbl_empty;

/* =================================================================== PSRAM == */
static void rec_get(int i, rec_t *r) {
    if (g_idx_base == 0xFFFFFFFFu || i < 0 || i >= g_n) {
        memset(r, 0, sizeof(*r));
        return;
    }
    kf_psram_read(g_idx_base + (uint32_t)i * sizeof(rec_t), r, sizeof(*r));
}

static void rec_put(int i, const rec_t *r) {
    if (g_idx_base == 0xFFFFFFFFu || i < 0 || i >= MAX_TRACKS) return;
    kf_psram_write(g_idx_base + (uint32_t)i * sizeof(rec_t), r, sizeof(*r));
}

static void artist_get(int i, group_entry_t *e) {
    if (g_artists_base == 0xFFFFFFFFu || i < 0 || i >= g_artist_n) { memset(e, 0, sizeof(*e)); return; }
    kf_psram_read(g_artists_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}
static void artist_put(int i, const group_entry_t *e) {
    if (g_artists_base == 0xFFFFFFFFu || i < 0 || i >= MAX_GROUPS) return;
    kf_psram_write(g_artists_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}

static void album_get(int i, group_entry_t *e) {
    if (g_albums_base == 0xFFFFFFFFu || i < 0 || i >= g_album_n) { memset(e, 0, sizeof(*e)); return; }
    kf_psram_read(g_albums_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}
static void album_put(int i, const group_entry_t *e) {
    if (g_albums_base == 0xFFFFFFFFu || i < 0 || i >= MAX_GROUPS) return;
    kf_psram_write(g_albums_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}

static void dir_get(int i, file_item_t *e) {
    if (g_dir_base == 0xFFFFFFFFu || i < 0 || i >= g_dir_count) { memset(e, 0, sizeof(*e)); return; }
    kf_psram_read(g_dir_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}
static void dir_put(int i, const file_item_t *e) {
    if (g_dir_base == 0xFFFFFFFFu || i < 0 || i >= MAX_DIR_ITEMS) return;
    kf_psram_write(g_dir_base + (uint32_t)i * sizeof(*e), e, sizeof(*e));
}

static uint16_t drill_get(int i) {
    if (g_drill_base == 0xFFFFFFFFu || i < 0 || i >= g_drill_n) return 0;
    uint16_t idx = 0;
    kf_psram_read(g_drill_base + (uint32_t)i * sizeof(uint16_t), &idx, sizeof(uint16_t));
    return idx;
}
static void drill_put(int i, uint16_t idx) {
    if (g_drill_base == 0xFFFFFFFFu || i < 0 || i >= MAX_TRACKS) return;
    kf_psram_write(g_drill_base + (uint32_t)i * sizeof(uint16_t), &idx, sizeof(uint16_t));
}

void music_psram_invalidate(void) {
    g_idx_base = g_artists_base = g_albums_base = g_dir_base = g_drill_base = 0xFFFFFFFFu;
    g_built = 0;
    g_n = g_full = 0;
    g_artist_n = g_album_n = 0;
}

/* ================================================================= Helpers == */
static int has_mp3(const char *n) {
    const char *d = strrchr(n, '.');
    return d && (!strcasecmp(d, ".mp3"));
}

static void make_full_path(char *out, int outsz, const char *path) {
    if (!path || !path[0]) {
        snprintf(out, outsz, "%s", MUSDIR);
    } else if (path[0] == '/') {
        snprintf(out, outsz, "%s", path);
    } else {
        snprintf(out, outsz, "%s/%s", MUSDIR, path);
    }
}

/* Ensure detail (ID3 tags and art region) is extracted for track record */
static void ensure_track_detail(int track_idx) {
    if (track_idx < 0 || track_idx >= g_n) return;
    rec_t r;
    rec_get(track_idx, &r);
    if (r.detail) return;

    char full[REC_PATH + 32];
    make_full_path(full, sizeof(full), r.path);

    music_id3_info_t id3;
    if (music_id3_parse(full, &id3)) {
        if (id3.title[0]) strncpy(r.title, id3.title, sizeof(r.title) - 1);
        if (id3.artist[0]) strncpy(r.artist, id3.artist, sizeof(r.artist) - 1);
        if (id3.album[0]) strncpy(r.album, id3.album, sizeof(r.album) - 1);
        if (id3.track) r.track = id3.track;
        if (id3.duration_sec) r.duration = id3.duration_sec;
        if (id3.cover_len) {
            r.cover_off = id3.cover_off;
            r.cover_len = id3.cover_len;
            r.cover_kind = id3.cover_kind;
        }
    }
    r.detail = 1;
    rec_put(track_idx, &r);
}

/* ========================================================= Cover Art Decode = */
typedef struct { FILE *f; long start; long pos; long end; } cov_src;
static int g_cstep, g_tw, g_th;

static size_t cover_in(JDEC *jd, uint8_t *buf, size_t nd) {
    cov_src *s = (cov_src*)jd->device;
    long avail = s->end - s->pos;
    if ((long)nd > avail) nd = (size_t)(avail < 0 ? 0 : avail);
    if (nd == 0) return 0;
    if (buf) {
        fseek(s->f, s->start + s->pos, SEEK_SET);
        size_t r = fread(buf, 1, nd, s->f);
        s->pos += (long)r;
        return r;
    }
    s->pos += (long)nd;
    return nd;
}

static int cover_out(JDEC *jd, void *bitmap, JRECT *rect) {
    (void)jd;
    const uint8_t *src = (const uint8_t*)bitmap;
    int rw = rect->right - rect->left + 1;
    for (int y = rect->top; y <= rect->bottom; y++) {
        if (y % g_cstep) continue;
        int ty = y / g_cstep;
        if (ty >= g_th) continue;
        for (int x = rect->left; x <= rect->right; x++) {
            if (x % g_cstep) continue;
            int tx = x / g_cstep;
            if (tx >= g_tw) continue;
            const uint8_t *px = src + (((y - rect->top) * rw) + (x - rect->left)) * 3;
            g_thumb[ty * g_tw + tx] = (uint16_t)(((px[2] & 0xF8) << 8) | ((px[1] & 0xFC) << 3) | (px[0] >> 3));
        }
    }
    return 1;
}

static int decode_jpeg(FILE *f, long start, long len) {
    static uint8_t pool[4096];
    if (!g_thumb) return 0;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    if (start < 0 || start >= fsize) return 0;
    if (len <= 0 || start + len > fsize) len = fsize - start;

    cov_src src = { f, start, 0, len };
    JDEC jd;
    if (jd_prepare(&jd, cover_in, pool, sizeof(pool), &src) != JDR_OK) return 0;
    int mx = jd.width > jd.height ? jd.width : jd.height;
    g_cstep = (mx + g_thumb_edge - 1) / g_thumb_edge;
    if (g_cstep < 1) g_cstep = 1;
    g_tw = jd.width / g_cstep;
    g_th = jd.height / g_cstep;
    if (g_tw < 1) g_tw = 1;
    if (g_th < 1) g_th = 1;
    if (g_tw > g_thumb_edge) g_tw = g_thumb_edge;
    if (g_th > g_thumb_edge) g_th = g_thumb_edge;
    if (jd_decomp(&jd, cover_out, 0) != JDR_OK) return 0;
    return 1;
}

static void load_cover_art(int track_idx) {
    lv_image_set_src(g_img_cover, NULL);
    lv_image_cache_drop(&g_cover_dsc);
    g_cover_ok = 0;
    g_cover_error = "NO ART";

    if (track_idx < 0 || track_idx >= g_n) return;
    ensure_track_detail(track_idx);

    rec_t r;
    rec_get(track_idx, &r);
    char full[REC_PATH + 32];
    make_full_path(full, sizeof(full), r.path);

    if (!g_cover_live) { g_cover_error = "ART ARENA"; return; }
    g_thumb = g_cover_live;
    g_thumb_edge = THUMB_EDGE;

    /* Try embedded cover first */
    if (r.cover_len) {
        FILE *f = fopen(full, "rb");
        if (f) {
            if (r.cover_kind == 'j') {
                g_cover_error = "JPEG ERROR";
                g_cover_ok = decode_jpeg(f, (long)r.cover_off, (long)r.cover_len);
            } else if (r.cover_kind == 'p') {
                g_cover_error = "PNG ERROR";
                g_cover_ok = music_png_thumb(f, (long)r.cover_off, (long)r.cover_len,
                                             g_thumb, g_thumb_edge, &g_tw, &g_th);
            }
            fclose(f);
        } else {
            g_cover_error = "ART FILE";
        }
    }

    /* Try sidecar files in parent directory */
    if (!g_cover_ok) {
        char dir[REC_PATH + 32];
        strncpy(dir, full, sizeof(dir) - 1);
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;

        static const struct { const char *name; int kind; } sidecars[] = {
            { "Folder.jpg", 'j' }, { "folder.jpg", 'j' }, { "cover.jpg", 'j' },
            { "Cover.jpg", 'j' },  { "front.jpg", 'j' },  { "Folder.png", 'p' },
            { "folder.png", 'p' }, { "cover.png", 'p' },  { "Cover.png", 'p' },
            { "front.png", 'p' }
        };
        for (unsigned i = 0; i < sizeof(sidecars) / sizeof(sidecars[0]) && !g_cover_ok; i++) {
            char sc[REC_PATH + 64];
            snprintf(sc, sizeof(sc), "%s/%s", dir, sidecars[i].name);
            FILE *f = fopen(sc, "rb");
            if (!f) continue;
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (sz > 0) {
                if (sidecars[i].kind == 'j') g_cover_ok = decode_jpeg(f, 0, sz);
                else g_cover_ok = music_png_thumb(f, 0, sz, g_thumb, g_thumb_edge, &g_tw, &g_th);
            }
            fclose(f);
        }
    }

    if (g_cover_ok) {
        lv_memzero(&g_cover_dsc, sizeof(g_cover_dsc));
        g_cover_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        g_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
        g_cover_dsc.header.w = g_tw;
        g_cover_dsc.header.h = g_th;
        g_cover_dsc.header.stride = g_tw * 2;
        g_cover_dsc.data = (const uint8_t*)g_cover_live;
        g_cover_dsc.data_size = (uint32_t)g_tw * g_th * 2;
    }
    g_thumb_edge = 0;
}

static void show_cover_art(void) {
    if (g_cover_ok) {
        lv_image_set_src(g_img_cover, &g_cover_dsc);
        int z = 120 * 256 / (g_tw > g_th ? g_tw : g_th);
        lv_image_set_scale(g_img_cover, z < 256 ? z : 256);
        lv_obj_clear_flag(g_img_cover, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(g_lbl_noart, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_image_set_src(g_img_cover, NULL);
        lv_obj_add_flag(g_img_cover, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(g_lbl_noart, g_cover_error);
        lv_obj_clear_flag(g_lbl_noart, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ========================================================= Aggregations ===== */
static int cmp_rec_path(const void *a, const void *b) {
    rec_t ra, rb;
    rec_get(*(const uint16_t*)a, &ra);
    rec_get(*(const uint16_t*)b, &rb);
    return strcmp(ra.path, rb.path);
}

static void build_groups(void) {
    g_artist_n = 0;
    g_album_n = 0;

    for (int i = 0; i < g_n; i++) {
        uint16_t idx = g_sorted[i];
        rec_t r;
        rec_get(idx, &r);

        const char *art = r.artist[0] ? r.artist : "Unknown Artist";
        int afound = -1;
        for (int a = 0; a < g_artist_n; a++) {
            group_entry_t ent;
            artist_get(a, &ent);
            if (!strcasecmp(ent.name, art)) { afound = a; break; }
        }
        if (afound >= 0) {
            group_entry_t ent;
            artist_get(afound, &ent);
            ent.track_count++;
            artist_put(afound, &ent);
        } else if (g_artist_n < MAX_GROUPS) {
            group_entry_t ent;
            memset(&ent, 0, sizeof(ent));
            strncpy(ent.name, art, REC_TXT - 1);
            ent.track_count = 1;
            ent.first_idx = idx;
            artist_put(g_artist_n, &ent);
            g_artist_n++;
        }

        const char *alb = r.album[0] ? r.album : "Unknown Album";
        int lfound = -1;
        for (int l = 0; l < g_album_n; l++) {
            group_entry_t ent;
            album_get(l, &ent);
            if (!strcasecmp(ent.name, alb)) { lfound = l; break; }
        }
        if (lfound >= 0) {
            group_entry_t ent;
            album_get(lfound, &ent);
            ent.track_count++;
            album_put(lfound, &ent);
        } else if (g_album_n < MAX_GROUPS) {
            group_entry_t ent;
            memset(&ent, 0, sizeof(ent));
            strncpy(ent.name, alb, REC_TXT - 1);
            strncpy(ent.sub, art, REC_TXT - 1);
            ent.track_count = 1;
            ent.first_idx = idx;
            album_put(g_album_n, &ent);
            g_album_n++;
        }
    }
}

/* ====================================================== Directory Browser === */
static void scan_directory(const char *dirpath) {
    g_dir_count = 0;
    DIR *d = opendir(dirpath);
    if (!d) return;

    /* Add ".." if not at root */
    if (strcmp(dirpath, "/") && strcmp(dirpath, MUSDIR)) {
        file_item_t item;
        memset(&item, 0, sizeof(item));
        strncpy(item.name, "..", REC_PATH - 1);
        item.is_dir = 1;
        dir_put(g_dir_count++, &item);
    }

    struct dirent *e;
    while ((e = readdir(d)) && g_dir_count < MAX_DIR_ITEMS) {
        if (e->d_name[0] == '.') continue;
        if (e->d_type == DT_DIR) {
            file_item_t item;
            memset(&item, 0, sizeof(item));
            strncpy(item.name, e->d_name, REC_PATH - 1);
            item.is_dir = 1;
            dir_put(g_dir_count++, &item);
        } else if (has_mp3(e->d_name)) {
            file_item_t item;
            memset(&item, 0, sizeof(item));
            strncpy(item.name, e->d_name, REC_PATH - 1);
            item.is_dir = 0;
            dir_put(g_dir_count++, &item);
        }
    }
    closedir(d);
}

/* ======================================================== Indexing Library == */
static void scan_progress(int n) {
    if (!g_lbl_empty) return;
    lv_label_set_text_fmt(g_lbl_empty, "Indexing MP3s...\n%d tracks", n);
    lv_refr_now(NULL);
}

static void build_index(void) {
    g_n = 0;
    g_full = 0;
    if (g_idx_base == 0xFFFFFFFFu) {
        g_idx_base     = kf_psram_alloc((uint32_t)MAX_TRACKS * sizeof(rec_t));
        g_artists_base = kf_psram_alloc((uint32_t)MAX_GROUPS * sizeof(group_entry_t));
        g_albums_base  = kf_psram_alloc((uint32_t)MAX_GROUPS * sizeof(group_entry_t));
        g_dir_base     = kf_psram_alloc((uint32_t)MAX_DIR_ITEMS * sizeof(file_item_t));
        g_drill_base   = kf_psram_alloc((uint32_t)MAX_TRACKS * sizeof(uint16_t));
        if (g_idx_base == 0xFFFFFFFFu || g_artists_base == 0xFFFFFFFFu ||
            g_albums_base == 0xFFFFFFFFu || g_dir_base == 0xFFFFFFFFu ||
            g_drill_base == 0xFFFFFFFFu) {
            g_built = 1;
            return;
        }
    }
    mkdir(MUSDIR, 0755);

    #define STACK_DEPTH 64
    char (*stack)[REC_PATH] = malloc((size_t)STACK_DEPTH * REC_PATH);
    if (!stack) { g_built = 1; return; }
    int sp = 0;
    stack[sp++][0] = 0;

    char full[REC_PATH + 32];
    while (sp > 0 && g_n < MAX_TRACKS) {
        char rel[REC_PATH];
        strncpy(rel, stack[--sp], REC_PATH - 1);
        make_full_path(full, sizeof(full), rel);
        DIR *d = opendir(full);
        if (!d) continue;

        struct dirent *e;
        while ((e = readdir(d)) && g_n < MAX_TRACKS) {
            if (e->d_name[0] == '.') continue;
            char child[REC_PATH];
            if (rel[0]) snprintf(child, sizeof(child), "%s/%s", rel, e->d_name);
            else snprintf(child, sizeof(child), "%s", e->d_name);

            if (e->d_type == DT_DIR) {
                if (sp < STACK_DEPTH) strncpy(stack[sp++], child, REC_PATH - 1);
            } else if (has_mp3(e->d_name)) {
                rec_t r;
                memset(&r, 0, sizeof(r));
                strncpy(r.path, child, sizeof(r.path) - 1);

                char track_full[REC_PATH + 32];
                make_full_path(track_full, sizeof(track_full), child);
                music_id3_info_t id3;
                if (music_id3_parse(track_full, &id3)) {
                    strncpy(r.title, id3.title, sizeof(r.title) - 1);
                    strncpy(r.artist, id3.artist, sizeof(r.artist) - 1);
                    strncpy(r.album, id3.album, sizeof(r.album) - 1);
                    r.track = id3.track;
                    r.duration = id3.duration_sec;
                    r.cover_off = id3.cover_off;
                    r.cover_len = id3.cover_len;
                    r.cover_kind = id3.cover_kind;
                    r.detail = 1;
                }

                rec_put(g_n, &r);
                g_sorted[g_n] = (uint16_t)g_n;
                g_n++;
                if ((g_n & 31) == 0) scan_progress(g_n);
            }
        }
        closedir(d);
    }
    free(stack);
    #undef STACK_DEPTH

    if (g_n >= MAX_TRACKS) g_full = 1;
    qsort(g_sorted, g_n, sizeof(g_sorted[0]), cmp_rec_path);
    build_groups();
    scan_directory(g_cur_dir);
    g_built = 1;
}

/* ======================================================== Playback Engine == */
static void close_dec(void) {
    kf_audio_stop();
    if (g_dec_active) {
        drmp3_uninit(&g_dec_obj);
        g_dec_active = 0;
    }
    if (g_mp3_io.f) {
        fclose(g_mp3_io.f);
        g_mp3_io.f = NULL;
    }
    g_paused = 0;
    g_pos_frames = 0;
    g_total_frames = 0;
    g_native_rate = 0;
    g_vis_l = g_vis_r = 0;
}

static void open_track(int track_idx) {
    close_dec();
    if (track_idx < 0 || track_idx >= g_n) return;

    ensure_track_detail(track_idx);
    rec_t r;
    rec_get(track_idx, &r);
    char full[REC_PATH + 32];
    make_full_path(full, sizeof(full), r.path);

    FILE *f = fopen(full, "rb");
    if (!f) {
        lv_label_set_text(g_lbl_title, "! cannot open file");
        return;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);

    g_mp3_io.f = f;
    g_mp3_io.pos = 0;
    g_mp3_io.len = flen;

    DRMP3_ZERO_OBJECT(&g_dec_obj);
    if (!drmp3_init(&g_dec_obj, mp3_io_read, mp3_io_seek, mp3_io_tell, NULL, &g_mp3_io, NULL)) {
        fclose(f);
        g_mp3_io.f = NULL;
        lv_label_set_text(g_lbl_title, "! cannot decode mp3");
        return;
    }
    g_dec_active = 1;

    g_channels = (int)g_dec_obj.channels;
    g_native_rate = g_dec_obj.sampleRate;
    if (g_channels == 0 || g_native_rate == 0) {
        close_dec();
        lv_label_set_text(g_lbl_title, "! invalid stream");
        return;
    }

    if (g_dec_obj.totalPCMFrameCount != DRMP3_UINT64_MAX) {
        g_total_frames = g_dec_obj.totalPCMFrameCount;
    } else if (r.duration) {
        g_total_frames = (uint64_t)r.duration * (uint64_t)g_native_rate;
    } else {
        g_total_frames = 0;
    }

    g_playhz = (int)g_native_rate;
    g_pos_frames = 0;
    g_paused = 0;
    g_play_track_idx = track_idx;

    /* Start audio output (PWM DAC or Bluetooth) using scratch arena */
    int audio_ok = 0;
    if (g_cover_live) {
        uint32_t *storage = (uint32_t*)((uint8_t*)g_cover_live + COVER_BYTES);
        audio_ok = kf_audio_start_buffered_external(g_playhz, MUSIC_RING_FRAMES, storage);
    }
    if (!audio_ok) audio_ok = kf_audio_start_buffered(g_playhz, 4096);
    if (!audio_ok) audio_ok = kf_audio_start_buffered(g_playhz, 2048);

    if (!audio_ok) {
        close_dec();
        g_play_track_idx = -1;
        lv_label_set_text(g_lbl_title, "! audio memory");
        return;
    }

    lv_label_set_text(g_lbl_title, r.title[0] ? r.title : r.path);
    lv_label_set_text_fmt(g_lbl_meta, "%s %s%s",
        r.artist[0] ? r.artist : "Unknown Artist",
        r.album[0] ? "— " : "",
        r.album[0] ? r.album : "");
}

static int get_current_view_count(void) {
    if (g_mode == MODE_ALL) return g_n;
    if (g_mode == MODE_ARTIST) return g_drill_down ? g_drill_n : g_artist_n;
    if (g_mode == MODE_ALBUM) return g_drill_down ? g_drill_n : g_album_n;
    if (g_mode == MODE_FILES) return g_dir_count;
    return 0;
}

static int get_track_idx_at_sel(int sel) {
    if (sel < 0) return -1;
    if (g_mode == MODE_ALL) {
        if (sel < g_n) return g_sorted[sel];
    } else if (g_mode == MODE_ARTIST || g_mode == MODE_ALBUM) {
        if (g_drill_down && sel < g_drill_n) return drill_get(sel);
        if (!g_drill_down) {
            if (g_mode == MODE_ARTIST && sel < g_artist_n) {
                group_entry_t ent; artist_get(sel, &ent); return ent.first_idx;
            }
            if (g_mode == MODE_ALBUM && sel < g_album_n) {
                group_entry_t ent; album_get(sel, &ent); return ent.first_idx;
            }
        }
    } else if (g_mode == MODE_FILES) {
        if (sel < g_dir_count) {
            file_item_t item; dir_get(sel, &item);
            if (!item.is_dir) {
                char target[REC_PATH + 32];
                snprintf(target, sizeof(target), "%s/%s", g_cur_dir, item.name);
                for (int i = 0; i < g_n; i++) {
                    rec_t r;
                    rec_get(g_sorted[i], &r);
                    char full[REC_PATH + 32];
                    make_full_path(full, sizeof(full), r.path);
                    if (!strcmp(full, target)) return g_sorted[i];
                }
            }
        }
    }
    return -1;
}

static void rebuild_order(void) {
    for (int i = 0; i < g_n; i++) g_order[i] = g_sorted[i];
    if (g_shuffle) {
        uint32_t s = time_us_32() ^ 0xC0FFEEu;
        for (int i = g_n - 1; i > 0; i--) {
            s = s * 1664525u + 1013904223u;
            int j = (int)(s % (uint32_t)(i + 1));
            uint16_t t = g_order[i]; g_order[i] = g_order[j]; g_order[j] = t;
        }
    }
    g_order_pos = 0;
    if (g_play_track_idx >= 0) {
        for (int i = 0; i < g_n; i++) {
            if (g_order[i] == g_play_track_idx) { g_order_pos = i; break; }
        }
    }
}

static void refresh_focused_cover(int track_idx) {
    g_focus_dirty = 0;
    if (track_idx < 0) {
        g_cover_ok = 0;
        show_cover_art();
        return;
    }
    load_cover_art(track_idx);
    show_cover_art();
    g_cover_track_idx = track_idx;
}

static void focus_selection(int sel) {
    g_sel = sel;
    int track_idx = get_track_idx_at_sel(sel);
    g_focus_dirty = 1;
    g_focus_t = lv_tick_get();

    /* Update right box metadata label */
    if (track_idx >= 0) {
        rec_t r;
        rec_get(track_idx, &r);
        lv_label_set_text_fmt(g_lbl_art_badge, "MP3 %luk\n%s",
            r.rate ? (unsigned long)(r.rate / 1000) : 44,
            kf_audio_bt_route() ? "[BT AUDIO]" : "[PWM SPK]");
    } else {
        lv_label_set_text(g_lbl_art_badge, kf_audio_bt_route() ? "[BT AUDIO]" : "[PWM SPK]");
    }
}

static void play_track_idx(int track_idx) {
    close_dec();
    if (track_idx < 0 || track_idx >= g_n) return;
    refresh_focused_cover(track_idx);
    open_track(track_idx);
    rebuild_order();
}

static void on_eof(void) {
    if (g_repeat == 2) { open_track(g_play_track_idx); return; }
    int next_pos = g_order_pos + 1;
    if (next_pos >= g_n) {
        if (g_repeat == 1) next_pos = 0;
        else { close_dec(); return; }
    }
    close_dec();
    g_order_pos = next_pos;
    int next_track = g_order[next_pos];
    refresh_focused_cover(next_track);
    open_track(next_track);
}

static void do_seek(int dsec) {
    if (!g_dec_active || !g_native_rate) return;
    int64_t target = (int64_t)g_pos_frames + (int64_t)dsec * (int64_t)g_native_rate;
    if (target < 0) target = 0;
    if (g_total_frames && (uint64_t)target >= g_total_frames) target = (int64_t)g_total_frames - 1;
    if (target < 0) target = 0;

    if (drmp3_seek_to_pcm_frame(&g_dec_obj, (drmp3_uint64)target)) {
        g_pos_frames = (uint64_t)target;
        kf_audio_flush();
    }
}

/* Audio decode pump */
static void pump(void) {
    if (!g_dec_active || g_paused) return;
    int guard = 0;

    while (kf_audio_space() > CHUNK && guard++ < 4) {
        drmp3_uint64 got = drmp3_read_pcm_frames_s16(&g_dec_obj, CHUNK, (drmp3_int16*)g_pcm_raw);
        if (got == 0) { on_eof(); return; }
        g_pos_frames += got;

        /* Stereo packing + Peak level analysis for the Visualizer */
        int32_t peak_l = 0, peak_r = 0;
        for (drmp3_uint64 i = 0; i < got; i++) {
            int16_t L, R;
            if (g_channels == 1) {
                L = R = g_pcm_raw[i];
            } else {
                L = g_pcm_raw[i * g_channels + 0];
                R = g_pcm_raw[i * g_channels + 1];
            }
            g_pcm_out[2 * i + 0] = L;
            g_pcm_out[2 * i + 1] = R;

            int32_t al = abs((int32_t)L);
            int32_t ar = abs((int32_t)R);
            if (al > peak_l) peak_l = al;
            if (ar > peak_r) peak_r = ar;
        }

        /* Update VU Visualizer metrics with decay */
        int cur_l = (int)(peak_l * 100 / 32767);
        int cur_r = (int)(peak_r * 100 / 32767);
        if (cur_l > g_vis_l) g_vis_l = cur_l;
        else g_vis_l = (g_vis_l * 86) / 100;
        if (cur_r > g_vis_r) g_vis_r = cur_r;
        else g_vis_r = (g_vis_r * 86) / 100;

        kf_audio_write(g_pcm_out, (int)got);
    }
}

/* ======================================================== UI Updates ======== */
static void refresh_browser_list(void);

static void refresh_now(void) {
    int playing = g_dec_active;
    int el = (playing && g_native_rate) ? (int)(g_pos_frames / g_native_rate) : 0;
    int du = (playing && g_native_rate && g_total_frames) ? (int)(g_total_frames / g_native_rate) : 0;
    int rem = du > el ? du - el : 0;
    int pct = du ? (int)((long long)el * 100 / du) : 0;

    lv_bar_set_value(g_bar_timeline, pct, LV_ANIM_OFF);
    lv_label_set_text_fmt(g_lbl_time, "%s %02d:%02d / %02d:%02d  (-%02d:%02d)",
        playing ? (g_paused ? "||" : ">") : "-",
        el / 60, el % 60, du / 60, du % 60, rem / 60, rem % 60);

    /* Update live stereo visualizer bars */
    lv_bar_set_value(g_bar_vis_l, playing && !g_paused ? g_vis_l : 0, LV_ANIM_OFF);
    lv_bar_set_value(g_bar_vis_r, playing && !g_paused ? g_vis_r : 0, LV_ANIM_OFF);

    /* Status line */
    const char *rep_str = g_repeat == 2 ? "REP1" : g_repeat == 1 ? "REP" : "";
    const char *bt_str = kf_audio_bt_route() ? "BT" : "PWM";
    lv_label_set_text_fmt(g_lbl_status, "%s %s %s %s",
        g_shuffle ? "SHUF" : "", rep_str, bt_str, g_hold_mode ? "[HOLD]" : "");

    /* Now playing title & meta */
    if (g_play_track_idx >= 0) {
        rec_t r;
        rec_get(g_play_track_idx, &r);
        lv_label_set_text(g_lbl_title, r.title[0] ? r.title : "(untitled)");
        lv_label_set_text_fmt(g_lbl_meta, "%s %s%s",
            r.artist[0] ? r.artist : "Unknown Artist",
            r.album[0] ? "— " : "",
            r.album[0] ? r.album : "");
    } else {
        lv_label_set_text(g_lbl_title, "No track playing");
        lv_label_set_text(g_lbl_meta, "Select a track to begin playback");
    }
}

static void refresh_browser_list(void) {
    int total = get_current_view_count();
    if (g_sel >= total) g_sel = total > 0 ? total - 1 : 0;
    if (g_sel < 0) g_sel = 0;

    /* Update tabs appearance */
    for (int t = 0; t < MODE_COUNT; t++) {
        int active = (t == (int)g_mode);
        lv_obj_set_style_bg_color(g_tabs[t], active ? KF_AMBER : KF_CARD, 0);
        lv_obj_set_style_text_color(lv_obj_get_child(g_tabs[t], 0), active ? KF_BG_DEEP : KF_AMBER, 0);
    }

    /* Fill rows centered around g_sel */
    for (int i = 0; i < VIS_ROWS; i++) {
        int idx = g_sel - ROW_CENTER + i;
        lv_obj_t *row = g_rows[i];
        int center = (i == ROW_CENTER);

        if (idx >= 0 && idx < total) {
            if (g_mode == MODE_ALL) {
                rec_t r;
                rec_get(g_sorted[idx], &r);
                lv_label_set_text(row, r.title[0] ? r.title : r.path);
            } else if (g_mode == MODE_ARTIST) {
                if (g_drill_down) {
                    uint16_t tidx = drill_get(idx);
                    rec_t r;
                    rec_get(tidx, &r);
                    lv_label_set_text(row, r.title[0] ? r.title : r.path);
                } else {
                    group_entry_t ent;
                    artist_get(idx, &ent);
                    lv_label_set_text_fmt(row, "> %s (%d)", ent.name, ent.track_count);
                }
            } else if (g_mode == MODE_ALBUM) {
                if (g_drill_down) {
                    uint16_t tidx = drill_get(idx);
                    rec_t r;
                    rec_get(tidx, &r);
                    lv_label_set_text_fmt(row, "%02d %s", r.track, r.title[0] ? r.title : r.path);
                } else {
                    group_entry_t ent;
                    album_get(idx, &ent);
                    lv_label_set_text_fmt(row, "> %s", ent.name);
                }
            } else if (g_mode == MODE_FILES) {
                file_item_t item;
                dir_get(idx, &item);
                if (item.is_dir) {
                    lv_label_set_text_fmt(row, "[DIR] %s", item.name);
                } else {
                    lv_label_set_text(row, item.name);
                }
            }
        } else {
            lv_label_set_text(row, "");
        }

        lv_obj_set_style_bg_opa(row, center ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_color(row, KF_AMBER, 0);
        lv_obj_set_style_text_color(row, center ? KF_BG_DEEP : KF_AMBER, 0);
        lv_obj_set_style_border_width(row, center ? 1 : 0, 0);
    }
}

/* ======================================================== Key Handlers ====== */
static void toggle_hold_and_backlight(void) {
    if (!g_hold_mode) {
        g_saved_bkl = deskconf_get_int("bkl", 5);
        if (g_saved_bkl <= 0) g_saved_bkl = 5;

        uint8_t zero = 0;
        reg_write(REG_BKL, &zero, 1);
        g_hold_mode = 1;
    } else {
        g_hold_mode = 0;
        if (g_saved_bkl <= 0) g_saved_bkl = deskconf_get_int("bkl", 5);
        if (g_saved_bkl <= 0) g_saved_bkl = 5;

        uint8_t v = (uint8_t)g_saved_bkl;
        reg_write(REG_BKL, &v, 1);
    }
    refresh_now();
}

static void m_exit(void) {
    /* Guaranteed "state unfucked" backlight restore */
    int bkl = deskconf_get_int("bkl", 5);
    if (bkl <= 0) bkl = 5;
    uint8_t v = (uint8_t)bkl;
    reg_write(REG_BKL, &v, 1);
    g_hold_mode = 0;

    close_dec();
    kf_clock_normal();
    g_active = 0;
    g_cover_ok = 0;
    g_cover_track_idx = -1;
    lv_image_set_src(g_img_cover, NULL);
    lv_image_cache_drop(&g_cover_dsc);
    g_thumb = NULL; g_thumb_edge = 0;
    g_cover_live = NULL;

    if (g_jpfont && g_jpfont != KF_FONT) lv_binfont_destroy((lv_font_t*)g_jpfont);
    g_jpfont = NULL;
    kf_grab_input(0);
    kf_back_to_launcher();
}

static void enter_active_selection(void) {
    if (g_mode == MODE_ALL) {
        int track_idx = get_track_idx_at_sel(g_sel);
        if (track_idx >= 0) {
            if (g_dec_active && track_idx == g_play_track_idx) g_paused = !g_paused;
            else play_track_idx(track_idx);
        }
    } else if (g_mode == MODE_ARTIST) {
        if (!g_drill_down) {
            if (g_sel < g_artist_n) {
                g_drill_down = 1;
                g_drill_group = g_sel;
                g_drill_n = 0;
                group_entry_t ent; artist_get(g_sel, &ent);
                const char *art = ent.name;
                for (int i = 0; i < g_n && g_drill_n < MAX_TRACKS; i++) {
                    rec_t r; rec_get(g_sorted[i], &r);
                    const char *a = r.artist[0] ? r.artist : "Unknown Artist";
                    if (!strcasecmp(a, art)) {
                        drill_put(g_drill_n++, g_sorted[i]);
                    }
                }
                g_sel = 0;
                refresh_browser_list();
                focus_selection(0);
            }
        } else {
            int track_idx = get_track_idx_at_sel(g_sel);
            if (track_idx >= 0) {
                if (g_dec_active && track_idx == g_play_track_idx) g_paused = !g_paused;
                else play_track_idx(track_idx);
            }
        }
    } else if (g_mode == MODE_ALBUM) {
        if (!g_drill_down) {
            if (g_sel < g_album_n) {
                g_drill_down = 1;
                g_drill_group = g_sel;
                g_drill_n = 0;
                group_entry_t ent; album_get(g_sel, &ent);
                const char *alb = ent.name;
                for (int i = 0; i < g_n && g_drill_n < MAX_TRACKS; i++) {
                    rec_t r; rec_get(g_sorted[i], &r);
                    const char *l = r.album[0] ? r.album : "Unknown Album";
                    if (!strcasecmp(l, alb)) {
                        drill_put(g_drill_n++, g_sorted[i]);
                    }
                }
                g_sel = 0;
                refresh_browser_list();
                focus_selection(0);
            }
        } else {
            int track_idx = get_track_idx_at_sel(g_sel);
            if (track_idx >= 0) {
                if (g_dec_active && track_idx == g_play_track_idx) g_paused = !g_paused;
                else play_track_idx(track_idx);
            }
        }
    } else if (g_mode == MODE_FILES) {
        if (g_sel < g_dir_count) {
            file_item_t item; dir_get(g_sel, &item);
            if (item.is_dir) {
                if (!strcmp(item.name, "..")) {
                    char *slash = strrchr(g_cur_dir, '/');
                    if (slash && slash != g_cur_dir) *slash = 0;
                    else strcpy(g_cur_dir, "/");
                } else {
                    char next[REC_PATH + 32];
                    if (!strcmp(g_cur_dir, "/")) snprintf(next, sizeof(next), "/%s", item.name);
                    else snprintf(next, sizeof(next), "%s/%s", g_cur_dir, item.name);
                    strncpy(g_cur_dir, next, sizeof(g_cur_dir) - 1);
                }
                scan_directory(g_cur_dir);
                g_sel = 0;
                refresh_browser_list();
                focus_selection(0);
            } else {
                int track_idx = get_track_idx_at_sel(g_sel);
                if (track_idx >= 0) {
                    if (g_dec_active && track_idx == g_play_track_idx) g_paused = !g_paused;
                    else play_track_idx(track_idx);
                } else if (g_n < MAX_TRACKS) {
                    /* Not yet indexed: add dynamically and start playing */
                    rec_t r;
                    memset(&r, 0, sizeof(r));
                    char path[REC_PATH];
                    if (!strcmp(g_cur_dir, "/")) snprintf(path, sizeof(path), "/%s", item.name);
                    else snprintf(path, sizeof(path), "%s/%s", g_cur_dir, item.name);
                    strncpy(r.path, path, sizeof(r.path) - 1);
                    int new_idx = g_n;
                    rec_put(new_idx, &r);
                    ensure_track_detail(new_idx);
                    g_sorted[g_n] = (uint16_t)new_idx;
                    g_n++;
                    play_track_idx(new_idx);
                }
            }
        }
    }
}

static void m_key(uint8_t k) {
    switch (k) {
    case DK_ESC: case DK_BREAK:
        m_exit();
        return;

    case DK_F1:
        toggle_hold_and_backlight();
        return;

    case DK_TAB: case 'm': case 'M':
        g_drill_down = 0;
        g_mode = (g_mode + 1) % MODE_COUNT;
        g_sel = 0;
        refresh_browser_list();
        focus_selection(0);
        return;

    case DK_BACKSPACE:
        if (g_drill_down) {
            g_drill_down = 0;
            g_sel = g_drill_group >= 0 ? g_drill_group : 0;
            refresh_browser_list();
            focus_selection(g_sel);
        } else if (g_mode == MODE_FILES) {
            char *slash = strrchr(g_cur_dir, '/');
            if (slash && slash != g_cur_dir) *slash = 0;
            else strcpy(g_cur_dir, "/");
            scan_directory(g_cur_dir);
            g_sel = 0;
            refresh_browser_list();
            focus_selection(0);
        }
        return;

    case DK_UP: {
        int total = get_current_view_count();
        if (g_sel > 0) {
            focus_selection(g_sel - 1);
            refresh_browser_list();
        }
        return;
    }

    case DK_DOWN: {
        int total = get_current_view_count();
        if (g_sel < total - 1) {
            focus_selection(g_sel + 1);
            refresh_browser_list();
        }
        return;
    }

    case DK_LEFT:
        do_seek(-5);
        return;

    case DK_RIGHT:
        do_seek(+5);
        return;

    case DK_ENTER: case 0x20: /* Space */
        enter_active_selection();
        return;

    case '[': case '<': {
        int prev = g_order_pos - 1;
        if (prev < 0) prev = g_n > 0 ? g_n - 1 : 0;
        if (g_n > 0) play_track_idx(g_order[prev]);
        return;
    }

    case ']': case '>': {
        int next = g_order_pos + 1;
        if (next >= g_n) next = 0;
        if (g_n > 0) play_track_idx(g_order[next]);
        return;
    }

    case 's': case 'S':
        g_shuffle = !g_shuffle;
        rebuild_order();
        refresh_now();
        return;

    case 'r': case 'R':
        g_repeat = (g_repeat + 1) % 3;
        refresh_now();
        return;

    case 'b': case 'B': {
        /* Toggle audio routing: Speaker PWM vs Bluetooth */
        int cur_bt = kf_audio_bt_route();
        kf_audio_set_bt_route(!cur_bt);
        focus_selection(g_sel);
        refresh_now();
        return;
    }

    case 0x85: /* F5 */
        close_dec();
        g_built = 0;
        g_play_track_idx = -1;
        build_index();
        g_sel = 0;
        rebuild_order();
        refresh_browser_list();
        focus_selection(0);
        return;

    default:
        return;
    }
}

void music_poll(void) {
    if (!g_active) return;
    uint8_t st, key;
    while (uart_pop_key(&st, &key)) {
        if (st == KS_RELEASE) continue;
        if (g_hold_mode) {
            if (key == DK_F1) toggle_hold_and_backlight();
            continue; /* No other input accepted in HOLD mode */
        }
        m_key(key);
    }

    if (g_focus_dirty && (lv_tick_get() - g_focus_t) > 120) {
        int track_idx = get_track_idx_at_sel(g_sel);
        refresh_focused_cover(track_idx);
    }

    pump();
    refresh_now();
}

/* ========================================================= Build UI ========= */
static lv_obj_t *mk_label(lv_obj_t *par, const lv_font_t *font, lv_color_t col, int w) {
    lv_obj_t *l = lv_label_create(par);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, col, 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    }
    return l;
}

static void on_play_pause_click(lv_event_t *e) {
    (void)e;
    if (g_dec_active) {
        g_paused = !g_paused;
        refresh_now();
    } else {
        enter_active_selection();
    }
}

static void on_footer_click(lv_event_t *e) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);
    lv_obj_t *obj = lv_event_get_target(e);
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    int rel_x = p.x - a.x1;
    int w = a.x2 - a.x1 + 1;
    if (rel_x < w / 3) {
        /* Prev */
        int prev = g_order_pos - 1;
        if (prev < 0) prev = g_n > 0 ? g_n - 1 : 0;
        if (g_n > 0) play_track_idx(g_order[prev]);
    } else if (rel_x < 2 * w / 3) {
        /* Play / Pause */
        on_play_pause_click(NULL);
    } else {
        /* Next */
        int next = g_order_pos + 1;
        if (next >= g_n) next = 0;
        if (g_n > 0) play_track_idx(g_order[next]);
    }
}

static void on_row_click(lv_event_t *e) {
    int row_idx = (int)(intptr_t)lv_event_get_user_data(e);
    int total = get_current_view_count();
    int target_sel = g_sel - ROW_CENTER + row_idx;
    if (target_sel >= 0 && target_sel < total) {
        if (target_sel == g_sel) {
            enter_active_selection();
        } else {
            focus_selection(target_sel);
            refresh_browser_list();
        }
    }
}

static void on_tab_click(lv_event_t *e) {
    int mode = (int)(intptr_t)lv_event_get_user_data(e);
    g_drill_down = 0;
    g_mode = (browse_mode_t)mode;
    g_sel = 0;
    refresh_browser_list();
    focus_selection(0);
}

void app_music_open(void) {
    g_cover_track_idx = -1;
    g_cover_live = kapi_idle_scratch(COVER_BYTES + MUSIC_RING_FRAMES * sizeof(uint32_t));

    g_jpfont = lv_binfont_create("A:/kefyros/fonts/jp.bin");
    if (!g_jpfont) g_jpfont = KF_FONT;

    g_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(g_scr, KF_BG_DEEP, 0);
    lv_obj_set_style_bg_opa(g_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(g_scr, 0, 0);
    lv_obj_remove_flag(g_scr, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cont = lv_obj_create(g_scr);
    lv_obj_remove_style_all(cont);
    lv_obj_set_pos(cont, 0, KF_CONTENT_Y);
    lv_obj_set_size(cont, LCD_W, KF_CONTENT_H);
    lv_obj_set_style_bg_color(cont, KF_BG_DEEP, 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_remove_flag(cont, LV_OBJ_FLAG_SCROLLABLE);

    /* 1. TOP-LEFT: File Browser / Mode List (w = 182, h = 176) */
    g_box_browser = lv_obj_create(cont);
    lv_obj_remove_style_all(g_box_browser);
    lv_obj_set_pos(g_box_browser, 2, 2);
    lv_obj_set_size(g_box_browser, LEFT_W, 176);
    lv_obj_set_style_bg_color(g_box_browser, KF_CARD, 0);
    lv_obj_set_style_bg_opa(g_box_browser, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_box_browser, 1, 0);
    lv_obj_set_style_border_color(g_box_browser, KF_BORDER, 0);
    lv_obj_remove_flag(g_box_browser, LV_OBJ_FLAG_SCROLLABLE);

    /* Mode Tabs Header */
    static const char *tab_names[MODE_COUNT] = { "ALL", "ART", "ALB", "DIR" };
    int tab_w = (LEFT_W - 8) / MODE_COUNT;
    for (int t = 0; t < MODE_COUNT; t++) {
        lv_obj_t *b = lv_button_create(g_box_browser);
        lv_obj_set_pos(b, 2 + t * tab_w, 2);
        lv_obj_set_size(b, tab_w, 18);
        lv_obj_set_style_radius(b, 2, 0);
        lv_obj_set_style_pad_all(b, 0, 0);
        lv_obj_add_event_cb(b, on_tab_click, LV_EVENT_CLICKED, (void*)(intptr_t)t);

        lv_obj_t *lbl = lv_label_create(b);
        lv_obj_set_style_text_font(lbl, KF_FONT, 0);
        lv_label_set_text(lbl, tab_names[t]);
        lv_obj_center(lbl);
        g_tabs[t] = b;
    }

    /* List Rows */
    int row_h = (176 - 24) / VIS_ROWS;
    for (int i = 0; i < VIS_ROWS; i++) {
        lv_obj_t *row = lv_label_create(g_box_browser);
        lv_obj_set_style_text_font(row, g_jpfont, 0);
        lv_obj_set_pos(row, 2, 22 + i * row_h);
        lv_obj_set_size(row, LEFT_W - 4, row_h);
        lv_label_set_long_mode(row, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_color(row, KF_AMBER, 0);
        lv_obj_set_style_pad_left(row, 3, 0);
        lv_obj_set_style_pad_top(row, 2, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_row_click, LV_EVENT_CLICKED, (void*)(intptr_t)i);
        g_rows[i] = row;
    }

    /* 2. TOP-RIGHT: Album Cover Box (w = 130, h = 176) */
    g_box_art = lv_obj_create(cont);
    lv_obj_remove_style_all(g_box_art);
    lv_obj_set_pos(g_box_art, LEFT_W + 4, 2);
    lv_obj_set_size(g_box_art, ART_W, 176);
    lv_obj_set_style_bg_color(g_box_art, KF_CARD, 0);
    lv_obj_set_style_bg_opa(g_box_art, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_box_art, 1, 0);
    lv_obj_set_style_border_color(g_box_art, KF_BORDER, 0);
    lv_obj_add_flag(g_box_art, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_box_art, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    lv_obj_remove_flag(g_box_art, LV_OBJ_FLAG_SCROLLABLE);

    /* 120x120 Album Cover Image */
    g_img_cover = lv_image_create(g_box_art);
    lv_obj_align(g_img_cover, LV_ALIGN_TOP_MID, 0, 4);

    g_lbl_noart = lv_label_create(g_box_art);
    lv_obj_set_style_text_font(g_lbl_noart, KF_FONT, 0);
    lv_obj_set_style_text_color(g_lbl_noart, KF_TEXT_MUTED, 0);
    lv_label_set_text(g_lbl_noart, "NO ART");
    lv_obj_align(g_lbl_noart, LV_ALIGN_TOP_MID, 0, 50);

    /* Format & Audio Output Badge */
    g_lbl_art_badge = mk_label(g_box_art, KF_FONT, KF_AMBER_DIM, ART_W - 8);
    lv_obj_set_style_text_align(g_lbl_art_badge, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_lbl_art_badge, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_label_set_text(g_lbl_art_badge, "MP3");

    /* 3. BOTTOM: Player, Timeline, Stereo Visualizer & Controls (h = 114) */
    g_box_player = lv_obj_create(cont);
    lv_obj_remove_style_all(g_box_player);
    lv_obj_set_pos(g_box_player, 2, 180);
    lv_obj_set_size(g_box_player, LCD_W - 4, 114);
    lv_obj_set_style_bg_color(g_box_player, KF_CARD, 0);
    lv_obj_set_style_bg_opa(g_box_player, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_box_player, 1, 0);
    lv_obj_set_style_border_color(g_box_player, KF_BORDER, 0);
    lv_obj_set_style_pad_all(g_box_player, 4, 0);
    lv_obj_remove_flag(g_box_player, LV_OBJ_FLAG_SCROLLABLE);

    int pw = LCD_W - 16;
    g_lbl_title = mk_label(g_box_player, g_jpfont, KF_AMBER_BR, pw);
    lv_obj_set_pos(g_lbl_title, 4, 2);
    lv_obj_add_flag(g_lbl_title, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_lbl_title, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(g_lbl_title, "Kefyros Music Player");

    g_lbl_meta = mk_label(g_box_player, g_jpfont, KF_TEXT_DIM, pw);
    lv_obj_set_pos(g_lbl_meta, 4, 20);
    lv_label_set_text(g_lbl_meta, "MP3 Stereo 320k");

    /* Timeline Bar */
    g_bar_timeline = lv_bar_create(g_box_player);
    lv_obj_set_size(g_bar_timeline, pw, 5);
    lv_obj_set_pos(g_bar_timeline, 4, 40);
    lv_bar_set_range(g_bar_timeline, 0, 100);
    lv_obj_set_style_bg_color(g_bar_timeline, KF_BORDER_HI, 0);
    lv_obj_set_style_bg_color(g_bar_timeline, KF_AMBER, LV_PART_INDICATOR);

    /* Time Readout & Status */
    g_lbl_time = mk_label(g_box_player, KF_FONT, KF_AMBER, pw - 80);
    lv_obj_set_pos(g_lbl_time, 4, 48);
    lv_obj_add_flag(g_lbl_time, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_lbl_time, on_play_pause_click, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(g_lbl_time, "- 00:00 / 00:00");

    g_lbl_status = mk_label(g_box_player, KF_FONT, KF_ACTIVE, 80);
    lv_obj_set_style_text_align(g_lbl_status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(g_lbl_status, pw - 80, 48);
    lv_label_set_text(g_lbl_status, "PWM");

    /* Stereo Line Visualizer: Two horizontal stripes (Green -> Red gradient) */
    g_bar_vis_l = lv_bar_create(g_box_player);
    lv_obj_set_size(g_bar_vis_l, pw, 3);
    lv_obj_set_pos(g_bar_vis_l, 4, 68);
    lv_bar_set_range(g_bar_vis_l, 0, 100);
    lv_obj_set_style_bg_color(g_bar_vis_l, KF_CARD_HI, 0);
    lv_obj_set_style_bg_color(g_bar_vis_l, KF_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(g_bar_vis_l, lv_color_hex(0xff3333), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_bar_vis_l, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    g_bar_vis_r = lv_bar_create(g_box_player);
    lv_obj_set_size(g_bar_vis_r, pw, 3);
    lv_obj_set_pos(g_bar_vis_r, 4, 73);
    lv_bar_set_range(g_bar_vis_r, 0, 100);
    lv_obj_set_style_bg_color(g_bar_vis_r, KF_CARD_HI, 0);
    lv_obj_set_style_bg_color(g_bar_vis_r, KF_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_color(g_bar_vis_r, lv_color_hex(0xff3333), LV_PART_INDICATOR);
    lv_obj_set_style_bg_grad_dir(g_bar_vis_r, LV_GRAD_DIR_HOR, LV_PART_INDICATOR);

    /* Controls & Shortcuts Footer */
    lv_obj_t *lbl_footer = mk_label(g_box_player, KF_FONT, KF_TEXT_MUTED, pw);
    lv_obj_set_pos(lbl_footer, 4, 86);
    lv_obj_add_flag(lbl_footer, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(lbl_footer, on_footer_click, LV_EVENT_CLICKED, NULL);
    lv_label_set_text(lbl_footer, "[ |<< ]  [ > / || ]  [ >>| ]   F1:HOLD  Tab:Mode  b:BT");

    /* Empty/Scan Progress Overlay */
    g_lbl_empty = lv_label_create(cont);
    lv_obj_set_style_text_color(g_lbl_empty, KF_TEXT_MUTED, 0);
    lv_obj_align(g_lbl_empty, LV_ALIGN_CENTER, 0, 0);
    lv_label_set_text(g_lbl_empty, "");

    kf_clock_normal();
    g_active = 1;
    lv_screen_load(g_scr);

    if (!g_built) {
        lv_label_set_text(g_lbl_empty, "Indexing MP3s...");
        lv_refr_now(NULL);
        build_index();
    }

    if (g_n == 0 && g_dir_count == 0) {
        lv_label_set_text(g_lbl_empty, "no .mp3 found under\n" MUSDIR);
    } else {
        lv_obj_add_flag(g_lbl_empty, LV_OBJ_FLAG_HIDDEN);
        if (g_sel >= g_n) g_sel = 0;
        rebuild_order();
        refresh_browser_list();
        focus_selection(g_sel);
        refresh_now();
    }
    kf_grab_input(1);
}
