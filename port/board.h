// port/board.h — PicoCalc / Pico 2 W (RP2350) hardware map.
// Pin assignments are the stock PicoCalc V2.0 routing, extracted from the ClockworkPi
// reference (lcdspi config + sd_boot/config.h) and the jcsmith picocalc_BIOS UART spec.
#ifndef KF_BOARD_H
#define KF_BOARD_H

/* ---- LCD: ILI9488 320x320 on spi1 ---- */
#define KF_LCD_SPI        spi1
#define KF_LCD_SCK        10
#define KF_LCD_MOSI       11      /* TX  */
#define KF_LCD_MISO       12      /* RX  */
#define KF_LCD_CS         13
#define KF_LCD_DC         14
#define KF_LCD_RST        15
#define KF_LCD_SPI_HZ     50000000   /* start point; tune on HW (demo used 25M, panel takes more) */

/* ---- SD card on spi0 (FatFs) ---- */
#define KF_SD_SPI         spi0
#define KF_SD_SCK         18
#define KF_SD_MOSI        19      /* TX  */
#define KF_SD_MISO        16      /* RX  */
#define KF_SD_CS          17
#define KF_SD_DET         22      /* card-detect (active low) */

/* The PicoCalc powers the SD socket from ALDO1. Identification must run at
   <=400 kHz; the driver enforces that independently of this post-init transfer
   rate. Both modules use the normal 24 MHz SD-SPI data clock. */
#if defined(PIMORONI_PICO_PLUS2_W_RP2350)
#define KF_SD_SPI_HZ      24000000u
#define KF_SD_USE_CRC     0
#else
#define KF_SD_SPI_HZ      24000000u
#define KF_SD_USE_CRC     0
#endif

/* ---- Keyboard: STM32 southbridge on uart1 (jcsmith UART_PICO_INTERFACE) ----
   Stock PicoCalc V2.0: STM32 PC10(TX)->Pico GP9, PC11(RX)<-Pico GP8 => host uart1. */
#define KF_KBD_UART       uart1
#define KF_KBD_TX         8       /* Pico TX1 -> STM32 RX */
#define KF_KBD_RX         9       /* Pico RX1 <- STM32 TX */
#define KF_KBD_BAUD       115200

/* ---- 8 MB QSPI PSRAM (ESP-PSRAM64H) on its own GPIO bus, PIO-driven ----
   Schematic (clockwork mainboard v2.0): RAM_CS=GP20, RAM_SCK=GP21, quad data
   SIO0=GP2, SIO1=GP3, SIO2=GP4, SIO3=GP5. NOT on the RP2350 XIP/QMI bus, so it is
   a software block store (port/psram.c), not memory-mapped. v1 uses 1-bit SPI
   (SIO0=MOSI, SIO1=MISO); SIO2/3 are unused in SPI mode. */
#define KF_PSRAM_SIO0     2       /* MOSI (1-bit) / data0 (quad) */
#define KF_PSRAM_SIO1     3       /* MISO (1-bit) / data1 (quad) */
#define KF_PSRAM_SIO2     4
#define KF_PSRAM_SIO3     5
#define KF_PSRAM_CS       20
#define KF_PSRAM_SCK      21
#define KF_PSRAM_SIZE     (8u*1024u*1024u)

/* ---- debug stdio (free pins): uart0 on GP0/GP1 ---- */
#define KF_DBG_UART       uart0
#define KF_DBG_TX         0
#define KF_DBG_RX         1

/* ---- clock tiers ----
   COLD BOOT at 250 MHz @ 1.10 V: PSRAM- and WiFi-safe, coming up cleanly on every power-up.
   main() then ramps WARM to the normal 300 MHz clock (kf_clock_normal) for smooth 30 FPS menus.
   The tiers:
     * kf_clock_sleep()  -> 150 MHz @ 1.10 V  (idle screen-off; kf_clock_wake() restores the prior tier)
     * kf_clock_eco()    -> 250 MHz @ 1.10 V / 62.5 MHz SPI (WiFi-safe; brief, wraps radio JOIN)
     * kf_clock_normal() -> 300 MHz @ 1.10 V / 75.0 MHz SPI (the UI / apps / audio default in RGB565)
     * kf_clock_boost()  -> 350 MHz @ 1.20 V / 87.5 MHz SPI (turbo: Music decode + calc 3D. Restores normal)
   The QMI flash divider is sized once at boot for the 350 MHz peak, so all tiers are safe. */
#define KF_SYS_KHZ        250000      /* cold-boot clock; warm-ramps to the normal 300 MHz */
#define KF_VREG_MV        1100        /* VREG_VOLTAGE_1_10 (stock voltage for 250/300; raised to 1.20 for boost) */

#if defined(PIMORONI_PICO_PLUS2_W_RP2350)
#define KF_BOARD_NAME     "PicoCalc / Pimoroni Pico Plus 2 W"
#else
#define KF_BOARD_NAME     "PicoCalc / Pico 2 W"
#endif
#endif
