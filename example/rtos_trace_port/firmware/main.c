/* mdkdebug「RTOS 任务感知」真机验证固件
 * =========================================
 * 目的：让 mdkdebug 的 rtos_info / rtos_tasks / rtos_objects 有一块**真的在跑
 * FreeRTOS** 的目标可以验证，而不是只在 mock 上自说自话。
 *
 * 这个固件刻意造出几种「主机侧必须能分辨」的现场：
 *   led    —— 周期任务（vTaskDelay），都在就绪/阻塞之间来回
 *   prod   —— 往队列发消息（优先级最高）
 *   work   —— 收队列 + 拿互斥量（验证队列与互斥量状态）
 *   deep   —— 递归吃栈，栈水位接近用满（验证 0xA5 填充计数与告警）
 *   stuck  —— 用 portMAX_DELAY 无限阻塞在一个永不释放的信号量上
 *             （FreeRTOS 会把这种任务放进 xSuspendedTaskList，很容易被误报成
 *              「被挂起」——主机侧要靠 xEventListItem.pxContainer 分辨）
 *   susp   —— 真的被 vTaskSuspend 挂起（对照 stuck）
 *
 * 另外两组是后来补的，为的是让「诊断」和「事件流还原」也有基准可对：
 *
 *   inv_low / inv_mid / inv_high —— 故意制造一次优先级反转。
 *             low（优先级 1）先拿到互斥量并持有约 30 毫秒；mid（优先级 2）
 *             在这期间一直占着 CPU；high（优先级 3）想拿那个互斥量，只能等。
 *             用互斥量而不是二值信号量，是因为要的正是**优先级继承**这个信号：
 *             继承发生的那段时间里，low 的 uxPriority 会偏离它的 uxBasePriority。
 *             主机侧的判据就是这两者不一致 —— 见计划 1.5 节。
 *
 *   seq_a / seq_b / seq_c —— 已知的固定切换序列，周期分别是 2、4、8 毫秒，
 *             同优先级。三个任务各自累加自己的计数器（g_seq_a/b/c），主机侧
 *             读出来就能核对比例。用计数而不是精确顺序来核对，是为了不依赖调度
 *             器在同等优先级下的链表顺序。
 *
 *             **注意：比例不是 4 : 2 : 1。** 2026-10-16 实测，三个任务的迭代
 *             速率是 59 / 37 / 26 次每秒（同一个 4 秒窗口）。原因是它们都是优先级 1，
 *             而节拍是 1 毫秒、开了时间片轮转，同优先级有七个任务：一个任务醒来
 *             之后要排队等轮转，真正决定周期的是一次轮转那么长，2 毫秒与 8 毫秒
 *             的差别被淹掉了。要拿到接近 4 : 2 : 1 的比例，得让这三个任务比别的高
 *             一个优先级，或者把节拍调细、同优先级任务数压下来。
 *
 *             这一条对「用计数反推切换次数」那类判据同样成立：先量速率，再谈比例。
 *
 * 另外 `rep` 任务每秒把**内核自己算的** uxTaskGetStackHighWaterMark 写进
 * g_kernel_hwm[]，主机侧读出来就能和 rtos_tasks 报的水位逐项对照——
 * 一致性是算出来的，不是声明出来的。
 */
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#if defined(VTRACE_RTEC2) && (VTRACE_RTEC2 == 1)
#include "vtrace_rtec2.h"
#endif

/* ---- 目标侧自报的对照数据（主机读这些全局符号） ---- */
#define HWM_N 12
TaskHandle_t g_probe_handles[HWM_N];
volatile uint16_t g_kernel_hwm[HWM_N];       /* 单位：字（word），内核自己算的 */
volatile uint32_t g_led_ticks;               /* 心跳：证明调度器在跑、不只是起来了 */
volatile uint32_t g_work_msgs;               /* worker 收了几条消息 */
volatile uint32_t g_inv_high_waits;          /* high 等互斥量等了多少次 */
volatile uint32_t g_seq_a, g_seq_b, g_seq_c; /* 固定序列三个任务的计数，比例应为 4:2:1 */

/* 队列 / 信号量 / 互斥量：都登记进内核注册表，主机侧才枚举得到 */
QueueHandle_t   g_q_sensor;
SemaphoreHandle_t g_sem_stuck;               /* 计数初值 0，永不 give —— stuck 卡在这 */
SemaphoreHandle_t g_mtx_bus;                 /* work 持有 */
SemaphoreHandle_t g_mtx_inv;                 /* 优先级反转用：low 持有，high 等它 */

/* 板上主频：默认 16 MHz（F401 复位后的 HSI），F103C8 上由 Makefile 传 8000000。
 * 注意 F1 系列复位后是 HSI 8 MHz，不是 16 MHz —— 照抄 16 MHz 会让 tick 掉到 500 Hz，
 * 功能还在但时间读数整体被拉长一倍。 */
#ifndef PROBE_CPU_HZ
    #define PROBE_CPU_HZ 16000000UL
#endif

/* 主频也要有符号可查：CMSIS 工程里 SystemCoreClock 是个全局变量，主机侧读它
 * 就能把目标周期数换算成秒。
 *
 * 注意一个真踩到的坑：只定义不引用它，会被 -fdata-sections 加 --gc-sections
 * 回收掉，符号表里就查不到（实测确认）。真实工程里 SystemCoreClock 被 HAL
 * 引用着，所以一直都在；这个固件没有 HAL，所以要显式引用一次。
 * 这正是计划里「以为该有的符号其实没有」那类问题的一个实例。 */
uint32_t SystemCoreClock = PROBE_CPU_HZ;
volatile uint32_t g_sysclk_hz;   /* 主机侧可读的主频副本 */

#if defined( __ARMCC_VERSION )
/* Keil 的器件启动文件在进 main 之前会调用 SystemInit。本固件不用 HAL 的时钟
 * 初始化（只用复位后的 HSI），所以给一个空实现，否则链接会报缺符号。
 * GCC 那一侧的 startup.c 不调它，所以这段只在 Keil 编译时生效。 */
void SystemInit( void )
{
}
#endif

/* SysTick 由 FreeRTOS 自己配：xPortStartScheduler() 里的 vPortSetupTimerInterrupt()
 * 用 configCPU_CLOCK_HZ 与 configTICK_RATE_HZ 算出重载值并打开中断、使能计数。
 *
 * **不要在 main 里提前使能 SysTick 中断。** 调度器启动之前 PSP 从未被设置过
 * （复位后是个未定义值），而这个窗口里到来的 tick 会让 xTaskIncrementTick
 * 返回「需要切换」，进而把 PendSV 拉起来；xPortPendSVHandler 的第一句就是
 * `mrs r0, psp`，随后往 PSP 指向的地址压 R4-R11 —— 于是往一个垃圾地址写寄存器，
 * HardFault，PC 停在 startup.c 的 Default_Handler 里死循环。
 *
 * 2026-10-15 在 F103 上首次真跑时踩到，现场：xTickCount 停在 2、PSP = AB14A558、
 * 出错 PC 在 xPortPendSVHandler、压栈帧的 xPSR 显示 IPSR = 14（PendSV）。
 * F401 跑 16 MHz，从 main 入口到 vTaskStartScheduler() 的窗口短于一 tick，
 * 所以这个缺陷一直没暴露；F103 是 8 MHz，窗口约 2 毫秒，够发生两次 tick。 */

/* 打开 DWT 周期计数器，给内核的运行时间统计当时间基准。
 *
 * 地址直接写，不引 CMSIS 头：这份固件只包含 FreeRTOS 自己的头文件，为三个
 * 寄存器引一套器件头文件不划算，而这几个地址在所有 Cortex-M3/M4 上一样。
 * 不产生中断，也不碰任何外设，只在内核启动前打开一次。 */
void probe_start_run_time_counter(void)
{
    *(volatile uint32_t *)0xE000EDFCu |= (1u << 24); /* DEMCR: TRCENA */
    *(volatile uint32_t *)0xE0001004u = 0u;          /* DWT_CYCCNT */
    *(volatile uint32_t *)0xE0001000u |= 1u;         /* DWT_CTRL: CYCCNTENA */
}

static void busy_spin(volatile uint32_t n)
{
    while (n-- != 0u) {
        __asm volatile("nop");
    }
}

/* ---- 任务 ---- */
static void led_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_led_ticks++;
        busy_spin(20000u);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void prod_task(void *arg)
{
    uint32_t v = 0u;
    (void)arg;
    for (;;) {
        v++;
        (void)xQueueSend(g_q_sensor, &v, 0u);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void work_task(void *arg)
{
    uint32_t v = 0u;
    (void)arg;
    for (;;) {
        if (xQueueReceive(g_q_sensor, &v, pdMS_TO_TICKS(200)) == pdPASS) {
            if (xSemaphoreTake(g_mtx_bus, pdMS_TO_TICKS(50)) == pdPASS) {
                g_work_msgs++;
                busy_spin(5000u);
                (void)xSemaphoreGive(g_mtx_bus);
            }
        }
    }
}

/* 递归吃栈：3 层，每层一个 48 字的局部数组，栈只给 160 字 —— 水位应接近用满 */
static volatile uint32_t deep_sink;

static void deep_recurse(int level)
{
    volatile uint32_t buf[40];
    uint32_t i;

    for (i = 0u; i < 40u; i++) {
        buf[i] = i + (uint32_t)level;
    }
    if (level > 0) {
        deep_recurse(level - 1);
    }
    deep_sink = buf[0] + buf[39];
}

static void deep_task(void *arg)
{
    (void)arg;
    for (;;) {
        deep_recurse(2);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void stuck_task(void *arg)
{
    (void)arg;
    /* 初值 0 的信号量 + 无限等待 = 永远停在这条等待链表上 */
    (void)xSemaphoreTake(g_sem_stuck, portMAX_DELAY);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void susp_task(void *arg)
{
    (void)arg;
    /* 自己把自己挂起：之后一直停在 xSuspendedTaskList 上（对照 stuck 的无限阻塞） */
    vTaskDelay(pdMS_TO_TICKS(200));
    vTaskSuspend(NULL);
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---- 优先级反转：low 持有互斥量，mid 占着 CPU，high 只能等 ---- */

static void inv_low_task(void *arg)
{
    (void)arg;
    for (;;) {
        if (xSemaphoreTake(g_mtx_inv, portMAX_DELAY) == pdPASS) {
            busy_spin(30000u);          /* 按住约二十毫秒（按 8 兆赫兹估算） */
            (void)xSemaphoreGive(g_mtx_inv);
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void inv_mid_task(void *arg)
{
    (void)arg;
    for (;;) {
        busy_spin(10000u);              /* 不碰互斥量，只占 CPU */
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static void inv_high_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(5));       /* 先让 low 拿到互斥量 */
    for (;;) {
        if (xSemaphoreTake(g_mtx_inv, pdMS_TO_TICKS(200)) == pdPASS) {
            g_inv_high_waits++;
            (void)xSemaphoreGive(g_mtx_inv);
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ---- 已知的固定切换序列：周期 2 / 4 / 8 毫秒，同优先级 ---- */

static void seq_a_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_seq_a++;
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void seq_b_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_seq_b++;
        vTaskDelay(pdMS_TO_TICKS(4));
    }
}

static void seq_c_task(void *arg)
{
    (void)arg;
    for (;;) {
        g_seq_c++;
        vTaskDelay(pdMS_TO_TICKS(8));
    }
}

static void rep_task(void *arg)
{
    int i;
    (void)arg;
    for (;;) {
        for (i = 0; i < HWM_N; i++) {
            if (g_probe_handles[i] != NULL) {
                g_kernel_hwm[i] = (uint16_t)uxTaskGetStackHighWaterMark(g_probe_handles[i]);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

void vApplicationMallocFailedHook(void)
{
    for (;;) {
    }
}

int main(void)
{
    /* 引用一次主频，别让它被链接期回收掉 */
    g_sysclk_hz = SystemCoreClock;

#if defined(VTRACE_RTEC2) && (VTRACE_RTEC2 == 1)
    /* 事件环要在第一个任务跑起来之前就位：第一刀切换紧跟 vTaskStartScheduler。 */
    vtrace_rtec2_init();

    /* ISR 事件得有个中断才会发生。TIM2 每 20 毫秒一次，五十对进出每秒——
       够看出中断把谁打断了，又不至于把环塞满。 */
    vtrace_rtec2_start_isr_source();
#endif

    g_q_sensor  = xQueueCreate(4, sizeof(uint32_t));
    g_sem_stuck = xSemaphoreCreateCounting(1, 0);   /* 初值 0 */
    g_mtx_bus   = xSemaphoreCreateMutex();
    /* 优先级反转用互斥量：要的正是「优先级继承」这个信号，不是无界反转。
     * 继承期间 low 的 uxPriority 会偏离 uxBasePriority，主机侧据此判定。 */
    g_mtx_inv   = xSemaphoreCreateMutex();

    if ((g_q_sensor == NULL) || (g_sem_stuck == NULL) || (g_mtx_bus == NULL)
        || (g_mtx_inv == NULL)) {
        for (;;) {
        }
    }

    /* 内核注册表：只有登记过的队列/信号量，主机侧才枚举得到 */
    vQueueAddToRegistry(g_q_sensor, "q_sensor");
    vQueueAddToRegistry(g_sem_stuck, "sem_stuck");
    vQueueAddToRegistry(g_mtx_bus, "mtx_bus");
    vQueueAddToRegistry(g_mtx_inv, "mtx_inv");

    (void)xTaskCreate(led_task,   "led",   128, NULL, 2, &g_probe_handles[0]);
    (void)xTaskCreate(deep_task,  "deep",  192, NULL, 1, &g_probe_handles[1]);
    (void)xTaskCreate(stuck_task, "stuck", 128, NULL, 1, &g_probe_handles[2]);
    (void)xTaskCreate(susp_task,  "susp",   96, NULL, 1, &g_probe_handles[3]);
    (void)xTaskCreate(prod_task,  "prod",  128, NULL, 3, &g_probe_handles[4]);
    (void)xTaskCreate(work_task,  "work",  256, NULL, 2, &g_probe_handles[5]);
    (void)xTaskCreate(rep_task,   "rep",   192, NULL, 1, NULL);

    /* 优先级反转三人组：low 优先级最低但持有互斥量，mid 中等且占 CPU，
     * high 最高且要那个互斥量 —— 排序刻意让 mid 卡在中间 */
    (void)xTaskCreate(inv_low_task,  "inv_low",  128, NULL, 1, &g_probe_handles[6]);
    (void)xTaskCreate(inv_mid_task,  "inv_mid",  128, NULL, 2, &g_probe_handles[7]);
    (void)xTaskCreate(inv_high_task, "inv_high", 128, NULL, 3, &g_probe_handles[8]);

    /* 已知的固定切换序列：周期 2 / 4 / 8 毫秒，同优先级 */
    (void)xTaskCreate(seq_a_task, "seq_a", 128, NULL, 1, &g_probe_handles[9]);
    (void)xTaskCreate(seq_b_task, "seq_b", 128, NULL, 1, &g_probe_handles[10]);
    (void)xTaskCreate(seq_c_task, "seq_c", 128, NULL, 1, &g_probe_handles[11]);

    /* 内核会在启动调度器前调用它，这里只是把函数放得离 main 近一点。 */
    vTaskStartScheduler();

    /* 走到这里只可能是堆不够 */
    for (;;) {
    }
}

#if defined(VTRACE_RTEC2) && (VTRACE_RTEC2 == 1)

/*
 * The two handlers the event ring needs from the firmware itself.
 *
 * Neither is a kernel hook: the kernel does not offer "an interrupt is running"
 * because it does not own the interrupts. This firmware owns one, so it says
 * so. A fault is the same story - the kernel hands the CPU to the vector table
 * and knows nothing about it afterwards.
 */
void TIM2_IRQHandler(void)
{
    vtrace_rtec2_isr_tick();
}

/*
 * The fault arrives with the three numbers somebody will ask for first: CFSR,
 * the PC that faulted, and the LR it was called from.
 *
 * `lr` is read before anything else, and by hand: the compiler is free to use
 * it as a scratch register afterwards, and by then the EXC_RETURN value that
 * says which stack the frame is on has been overwritten.
 */
void HardFault_Handler(void)
{
    uint32_t excReturn = 0;
    uint32_t psp = 0;
    uint32_t msp = 0;

    __asm volatile ("MOV %0, lr" : "=r" (excReturn));
    __asm volatile ("MRS %0, psp" : "=r" (psp));
    __asm volatile ("MRS %0, msp" : "=r" (msp));

    const uint32_t cfsr = *(volatile uint32_t *)0xE000ED28u;

    /* The frame the hardware pushed: r0, r1, r2, r3, r12, lr, pc, xpsr. */
    const volatile uint32_t *frame = ((excReturn & 0x4u) != 0u) ? (const volatile uint32_t *)psp : (const volatile uint32_t *)msp;

    vtrace_rtec2_fault(cfsr, frame[6], frame[5]);

    for (;;) {
    }
}

#endif
