/* STM32F407ZGT6（Cortex-M4F，1024K Flash / 192K RAM）链接脚本。
 *
 * 与 f401.ld 的差别只有容量：
 *   Flash 512K -> 1024K
 *   RAM    96K ->  128K
 *
 * RAM 只纳入 SRAM1 与 SRAM2 —— 两者地址连续（0x20000000 起 128K），内核堆与
 * 任务栈都放这里。CCM（64K @ 0x10000000）不纳入：它不能被 DMA 访问，本固件
 * 也用不上那份额外的速度。
 *
 * 复位后跑在 HSI 16 MHz 上，不需要 Flash 等待周期，所以链接脚本里不做任何
 * 时钟相关的假设 —— 与 main.c 里 systick_init_1khz() 保持一致。
 */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 1024K
  RAM  (rwx) : ORIGIN = 0x20000000, LENGTH = 128K
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
