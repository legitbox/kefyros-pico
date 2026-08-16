/* sdk/app.ld — linker script for a Kefyros class-1 app (.kx).
 *
 * The app is linked to run at a FIXED arena base in SRAM and loaded there verbatim, so
 * there are zero load-time relocations. All kernel access is through the `kapi*` pointer
 * passed to app_main() at runtime — the app links NO kernel symbols.
 *
 * ABI 1 reserves the top 48 KiB of RP2350 SRAM at a stable address. This is part of the
 * executable ABI: compatible apps do not move when the firmware is rebuilt.
 */
ENTRY(app_main)

MEMORY {
  ARENA (rwx) : ORIGIN = 0x20074000, LENGTH = 48K
}

SECTIONS {
  . = ORIGIN(ARENA);

  .text : {
    KEEP(*(.text.app_main))     /* keep the entry near the front */
    *(.text*)
    *(.rodata*)
  } > ARENA

  .data : {
    *(.data*)
  } > ARENA

  .bss (NOLOAD) : {
    __bss_start = .;
    *(.bss*)
    *(COMMON)
    __bss_end = .;
  } > ARENA

  /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment) *(.note*) }
}
