// port/storage_sd.c — SD-card storage HAL (pico-vfs POSIX FS over FatFs + SD-SPI).
//
// Implements the kefyros.h storage contract:
//   int kfs_mount(void)  — bring up the SD card on spi0, mount FAT at "/",
//                          ensure the /kefyros directory tree exists. 0 ok, <0 fail.
//   int kfs_ready(void)  — 1 once the card is mounted, else 0.
//
// Stack: pico-vfs (lib/pico-vfs) layers a POSIX/newlib file API over a FAT
// filesystem object (ChaN FatFs) backed by an SPI-connected SD block device.
//
//   blockdevice_sd_create(spi, mosi, miso, sck, cs, hz, crc) -> blockdevice_t*
//   filesystem_fat_create()                                  -> filesystem_t*
//   fs_mount("/", fat, sd)                                    -> 0 ok / -1 errno
//
// After fs_mount succeeds, pico-vfs's vfs.c provides the newlib syscalls
// (_open/_read/_write/_lseek/_fstat) and the POSIX wrappers (mkdir/stat/
// opendir/readdir/rename/...), so the apps' existing fopen/fgets/fwrite/
// opendir/readdir/stat/mkdir calls route through the mounted SD automatically.
// No per-call shim is needed here.
//
// SD-SPI init clock: blockdevice_sd_create() takes a single "transfer" clock
// (here CONF_SD_TRX_FREQUENCY = 24 MHz from blockdevice/sd.h). The SD spec's
// mandatory <=400 kHz init handshake is handled *inside* the pico-vfs SD driver
// (src/blockdevice/sd.c) — it clocks the CMD0/CMD8/ACMD41 sequence slow on its
// own and only then runs at the requested rate. So we pass the steady-state
// rate and let the driver downshift during init.
//
// Resilience: if no card is present (or init/mount fails) we return <0 and
// leave mounted=0; the rest of the OS must still boot. We do NOT auto-format,
// so a fresh/blank card returns failure rather than silently wiping the user's
// data — the apps simply run without storage until a formatted card is inserted.

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "pico/stdlib.h"
#include "hardware/gpio.h"
#include "hardware/clocks.h"       // MHZ (used by blockdevice/sd.h CONF_SD_TRX_FREQUENCY)

#include "kefyros.h"               // KF_ROOT / KF_NOTES / KF_WALLS, kfs_mount/kfs_ready
#include "port/board.h"            // KF_SD_* pin + spi instance map

#include "blockdevice/sd.h"        // blockdevice_sd_create, CONF_SD_TRX_FREQUENCY
#include "filesystem/fat.h"        // filesystem_fat_create
#include "filesystem/vfs.h"        // fs_mount

// Steady-state SPI clock and CRC policy are target-specific in board.h. The
// driver self-throttles to <=400 kHz during the init handshake regardless.
#define KFS_SD_HZ   KF_SD_SPI_HZ

static int   mounted = 0;
static blockdevice_t *sd_bd = NULL;
static filesystem_t  *fat_fs = NULL;
static int card_was_absent = 1;

// mkdir that tolerates an already-existing directory.
static int ensure_dir(const char *path) {
    if (mkdir(path, 0777) == 0)
        return 0;
    if (errno == EEXIST)
        return 0;
    return -1;
}

int kfs_mount(void) {
    if (mounted)
        return 0;

    // Optional card-detect gate (active low). If the line reads high there is
    // no card seated; bail early instead of waiting on a doomed SPI init.
    gpio_init(KF_SD_DET);
    gpio_set_dir(KF_SD_DET, GPIO_IN);
    gpio_pull_up(KF_SD_DET);
    if (gpio_get(KF_SD_DET)) {
        printf("kfs: no card (DET high)\n");
        card_was_absent = 1;
        mounted = 0;
        return -1;
    }

    /* On cold boot and hot insertion, keep CS inactive while the mechanical
     * contacts and the card's internal power-on reset settle. Without this,
     * the 2 Hz gate retries can repeatedly catch a half-seated card. */
    gpio_init(KF_SD_CS);
    gpio_put(KF_SD_CS, 1);
    gpio_set_dir(KF_SD_CS, GPIO_OUT);
    gpio_pull_up(KF_SD_CS);
    if (card_was_absent) {
        sleep_ms(100);
        if (gpio_get(KF_SD_DET)) {
            mounted = 0;
            return -1;
        }
    }
    card_was_absent = 0;

    // Create the SD-SPI block device on spi0 with the PicoCalc SD pins.
    // Arg order is (spi, mosi/TX, miso/RX, sck, cs, hz, enable_crc).
    sd_bd = blockdevice_sd_create(KF_SD_SPI,
                                  KF_SD_MOSI,   // TX
                                  KF_SD_MISO,   // RX
                                  KF_SD_SCK,
                                  KF_SD_CS,
                                  KFS_SD_HZ,
                                  KF_SD_USE_CRC != 0);
    if (sd_bd == NULL) {
        printf("kfs: blockdevice_sd_create failed\n");
        mounted = 0;
        return -1;
    }

    fat_fs = filesystem_fat_create();
    if (fat_fs == NULL) {
        printf("kfs: filesystem_fat_create failed\n");
        blockdevice_sd_free(sd_bd);
        sd_bd = NULL;
        mounted = 0;
        return -1;
    }

    // Mount the existing FAT volume at "/". We intentionally do NOT format on
    // failure: a -1 here means "no card / unformatted / unreadable", and we
    // would rather fail soft than erase the user's card.
    int err = fs_mount("/", fat_fs, sd_bd);
    if (err != 0) {
        printf("kfs: fs_mount failed: %s\n", strerror(errno));
        filesystem_fat_free(fat_fs);
        fat_fs = NULL;
        blockdevice_sd_free(sd_bd);
        sd_bd = NULL;
        mounted = 0;
        return -1;
    }

    // Card is mounted — from here POSIX calls work. Build the /kefyros tree.
    // EEXIST is fine; any other mkdir failure is non-fatal (storage is still
    // usable, the apps just may not have their preferred subdirs).
    if (ensure_dir(KF_ROOT)  != 0)
        printf("kfs: mkdir %s failed: %s\n", KF_ROOT, strerror(errno));
    if (ensure_dir(KF_NOTES) != 0)
        printf("kfs: mkdir %s failed: %s\n", KF_NOTES, strerror(errno));
    if (ensure_dir(KF_WALLS) != 0)
        printf("kfs: mkdir %s failed: %s\n", KF_WALLS, strerror(errno));

    mounted = 1;
    printf("kfs: mounted SD at / (FAT, %lu Hz)\n", (unsigned long)KFS_SD_HZ);
    return 0;
}

int kfs_ready(void) {
    return mounted;
}
