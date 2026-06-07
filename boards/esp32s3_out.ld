/*
 * This is a custom linker script fragment.
 * It creates a new output section `.psram_bss` and directs it to the external PSRAM region.
 * Any variables declared with `__attribute__((section(".psram_bss")))` will be placed here.
 */
SECTIONS
{
  .psram_bss (NOLOAD) :
  {
    . = ALIGN(4);
    _psram_bss_start = .;
    *(.psram_bss .psram_bss.*)
    _psram_bss_end = .;
  } > ext_ram
}
