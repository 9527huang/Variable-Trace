/* STM32F103C8（Cortex-M3，64K Flash / 20K RAM）链接脚本。
 *
 * 与 f401.ld 的唯一差别是存储容量。堆大小不在这里控制，走
 * FreeRTOSConfig.h 里的 PROBE_HEAP_KB（F103 上必须调小到 12 KB，
 * 否则 bss 会超过 20K 的内存）。
 *
 * 注意栈：本脚本没有单独保留栈区，栈顶就是 _estack（RAM 末尾），
 * 向下生长。所以 bss 的末尾到 RAM 末尾之间必须留得下中断用的栈。
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 64K
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 20K
}

_estack = ORIGIN(RAM) + LENGTH(RAM);

SECTIONS
{
  .isr_vector : {
    KEEP(*(.isr_vector))
  } > FLASH

  .text : {
    *(.text*)
    *(.rodata*)
    . = ALIGN(4);
    _etext = .;
  } > FLASH

  _sidata = LOADADDR(.data);

  .data : {
    . = ALIGN(4);
    _sdata = .;
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } > RAM AT > FLASH

  .bss : {
    . = ALIGN(4);
    _sbss = .;
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
  } > RAM

  . = ALIGN(4);
  _end = .;
  PROVIDE(end = .);
}
