// port/sys.c — Kefyros system control (reboot / BOOTSEL / power-off).
// Implements the kf_reboot / kf_bootsel / kf_poweroff decls in kefyros.h.
#include <stdint.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"          /* reset_usb_boot */
#include "hardware/watchdog.h"     /* watchdog_reboot */

#include "kefyros.h"               /* reg_write(), REG_OFF */

/* Hard reboot of the RP2350 via the watchdog (immediate, no scratch payload). */
void kf_reboot(void){
	watchdog_reboot(0, 0, 0);
}

/* Drop into the bootrom USB mass-storage bootloader (BOOTSEL / UF2 mode).
 * Args (0,0): no GPIO activity mask, no interface-disable mask -> both
 * PICOBOOT and the UF2 drive are available. */
void kf_bootsel(void){
	reset_usb_boot(0, 0);
}

/* Tell the PicoCalc STM32 southbridge to cut power: REG_OFF (0x0E) with bit7
 * (shutdown) set. The southbridge owns the power rail, so this is a full
 * device power-off rather than a core sleep. */
void kf_poweroff(void){
	uint8_t b = 0x80;              /* bit7 = shutdown (bit6 would be sleep) */
	reg_write(REG_OFF, &b, 1);
}
