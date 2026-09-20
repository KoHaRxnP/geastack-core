/* geaos loadable-app linker script (spike), two-PAGE irom/drom split.
 * esp_mmu_map maps a flash paddr to exactly ONE vaddr, and the MMU page is
 * 64 KB, so (like the firmware's irom/drom) code and rodata must live in
 * SEPARATE 64 KB flash pages, each mapped once:
 * The MMU maps flash linearly: vaddr = bus_base + paddr (deterministic, and
 * independent of firmware size). With the blob at ota_1 (paddr 0x1010000):
 *   page 0 (blob off 0,       paddr 0x1010000) -> EXEC -> IBUS 0x43060000 (code)
 *   page 1 (blob off 0x10000, paddr 0x1020000) -> READ -> DBUS 0x3d070000 (rodata)
 * .text keeps VMA==LMA (IBUS). .rodata's LMA jumps to the next 64 KB page in
 * the blob; its VMA is the DBUS base. Blob = [page0: header+text][page1: rodata]. */
ENTRY(app_entry)
SECTIONS
{
  . = 0x43060000;
  .geaos_header : { KEEP(*(.geaos_header)) }
  .text : { . = ALIGN(4); *(.literal .literal.* .text .text.*) }
  . = 0x43070000;                 /* next 64 KB page -> blob offset 0x10000 */
  __geaos_rodata_lma = .;
  .rodata 0x3d070000 : AT(__geaos_rodata_lma) { *(.rodata .rodata.* .rodata1) }
  /DISCARD/ : { *(.comment) *(.xtensa.info) *(.note*) *(.xt.*) *(.debug*) }
}
