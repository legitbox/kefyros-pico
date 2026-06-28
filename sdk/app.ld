/* sdk/app.ld — linker script for a Kefyros class-1 app (.kx).
 *
 * The app is linked to run at a FIXED arena base in SRAM and loaded there verbatim, so
 * there are zero load-time relocations. All kernel access is through the `kapi*` pointer
 * passed to app_main() at runtime — the app links NO kernel symbols.
 *
 * ARENA base/length are PROVISIONAL (top 128 KB of the 512 KB RAM region); they must match
 * the kernel's reserved arena once the loader lands. mkkx records the base in the .kx header.
 */
ENTRY(app_main)

MEMORY {
  ARENA (rwx) : ORIGIN = 0x20060000, LENGTH = 128K
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
