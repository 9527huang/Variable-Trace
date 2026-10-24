/* 最小启动文件（FreeRTOS 版）：向量表 + 复位入口，不依赖 CMSIS / HAL。
 *
 * 与 rtt_probe 的 startup.c 的关键差别：SysTick / PendSV / SVCall 三个异常
 * **必须**接到 FreeRTOS 移植层的处理函数上。这里也是真踩过的地方——
 * 一旦写错，调度器根本起不来（PendSV 不触发 = 永远只跑第一个任务）。
 */
#include <stdint.h>

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

int main(void);

void Reset_Handler(void);
void Default_Handler(void);

/* FreeRTOS ARM_CM4F 移植层的异常入口（port.c 导出） */
void vPortSVCHandler(void);
void xPortPendSVHandler(void);
void xPortSysTickHandler(void);

/* 走 RTEC2 时，这两条异常要有自己的处理函数：HardFault 记下 CFSR 与出错地址，
   TIM2 记下中断的进出。它们不在 default 里，因为 default 只有一个死循环，
   而那正是「有崩溃、但什么也没留下」的样子。 */
#if defined(VTRACE_RTEC2) && (VTRACE_RTEC2 == 1)
void HardFault_Handler(void);
void TIM2_IRQHandler(void);
#define VTRACE_HARD_FAULT HardFault_Handler
#else
#define VTRACE_HARD_FAULT Default_Handler
#endif

__attribute__((section(".isr_vector"), used))
void (*const g_vectors[])(void) = {
    (void (*)(void))(&_estack),
    Reset_Handler,
    Default_Handler,       /* NMI */
    VTRACE_HARD_FAULT,     /* HardFault */
    Default_Handler,       /* MemManage */
    Default_Handler,       /* BusFault */
    Default_Handler,       /* UsageFault */
    0, 0, 0, 0,            /* 保留 */
    vPortSVCHandler,       /* SVCall：调度器启动时用 SVC 切到第一个任务 */
    Default_Handler,       /* DebugMonitor */
    0,                     /* 保留 */
    xPortPendSVHandler,    /* PendSV：任务切换 */
    xPortSysTickHandler,   /* SysTick：系统节拍 */

    /* 外设中断，IRQ0 起。这份固件只用一个：TIM2 在 IRQ28，给自己造一个
       每 20 毫秒一次的现场，好让「中断」那条轨道有东西可画。
       表必须写到 TIM2 那一项为止——少一项，处理器取到的就是表外的东西。 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ0..3  */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ4..7  */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ8..11 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ12..15 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ16..19 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ20..23 */
    Default_Handler, Default_Handler, Default_Handler, Default_Handler, /* IRQ24..27 */
#if defined(VTRACE_RTEC2) && (VTRACE_RTEC2 == 1)
    TIM2_IRQHandler,                                                   /* IRQ28 TIM2 */
#else
    Default_Handler,
#endif
};

void Default_Handler(void)
{
    for (;;) {
    }
}

void Reset_Handler(void)
{
    uint32_t *src = &_sidata;
    uint32_t *dst = &_sdata;

    while (dst < &_edata) {
        *dst++ = *src++;
    }
    for (dst = &_sbss; dst < &_ebss;) {
        *dst++ = 0u;
    }
    (void)main();
    for (;;) {
    }
}
