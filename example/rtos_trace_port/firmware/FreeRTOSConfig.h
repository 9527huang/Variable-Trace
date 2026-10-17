/* FreeRTOS 配置：为 mdkdebug 的 rtos_* 工具做真机验证固件。
 *
 * 这里故意把几个「会改变 TCB_t / Queue_t 布局」的选项都打开：
 *   configUSE_TRACE_FACILITY        -> TCB 里多出 uxTCBNumber / uxTaskNumber
 *   configGENERATE_RUN_TIME_STATS   -> TCB 里多出 ulRunTimeCounter（要配一个自由运行的计数器）
 *   configUSE_MUTEXES               -> 多出 uxBasePriority / uxMutexesHeld
 *   configRECORD_STACK_HIGH_ADDRESS -> 多出 pxEndOfStack
 *   configQUEUE_REGISTRY_SIZE > 0   -> 内核维护 xQueueRegistry，主机侧才可能枚举队列
 * 这样 rtos_tasks/rtos_objects 就**必须**走 DWARF 取偏移才可能算对；
 * 任何写死偏移的实现都会在这里读出垃圾。
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include <stdint.h>

extern uint32_t SystemCoreClock;

#define configUSE_PREEMPTION                     1

/* 「优化任务选择」：0 = uxTopReadyPriority 存优先级编号，1 = 存位图。
 *
 * 打开之后移植层的两个宏跟着变（见 portable/GCC/ARM_CM3/portmacro.h）：
 *   portRECORD_READY_PRIORITY  从「赋编号」变成「置位」
 *   portGET_HIGHEST_PRIORITY   从「取编号」变成「用 clz 求最高位」
 * 于是同一个全局符号的值域从 0..31 变成 0x1..0x80000000。按位图当编号读，
 * 得到的是一个合法但错误的数字 —— 这正是任务视图 18.3 要绕开的语义陷阱。
 *
 * 默认 0，与既有产物保持一致；要测变体由 Makefile 传 -DPROBE_OPTIMISED_SELECTION=1：
 *     make BOARD=f103 OPT=1
 */
#ifndef PROBE_OPTIMISED_SELECTION
    #define PROBE_OPTIMISED_SELECTION 0
#endif
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  ( PROBE_OPTIMISED_SELECTION )
#define configUSE_TICKLESS_IDLE                  0
/* 板上参数：默认值对应 STM32F4 系列（复位后 HSI 16 MHz）—— F401 与
 * STM32F407ZGT6 都落在这一档。
 * 构建 STM32F103C8（复位后 HSI 8 MHz、20K RAM）时由 Makefile 传
 * -DPROBE_CPU_HZ=8000000 与 -DPROBE_HEAP_KB=14 覆盖；f407 由 Makefile 传
 * -DPROBE_HEAP_KB=48 覆盖。
 *
 * 为什么堆必须在 F103 上调小：24 KB 的堆光 bss 就超过 F103C8 的 20K 内存，
 * 不改连不上也烧不进。F103 上任务栈加起来约 4.4 KB，14 KB 够用。
 * F407 有 128K RAM，堆放到 48 KB 是为「五十个任务」那一档快照场景留空间
 * （每个任务约 96 字节控制块加栈，五十个任务约 35 KB）。 */
#ifndef PROBE_CPU_HZ
    #define PROBE_CPU_HZ             16000000UL
#endif
#ifndef PROBE_HEAP_KB
    #define PROBE_HEAP_KB            24
#endif

#define configCPU_CLOCK_HZ                       ( PROBE_CPU_HZ )
#define configTICK_RATE_HZ                       ( 1000U )
#define configMAX_PRIORITIES                     ( 5 )
#define configMINIMAL_STACK_SIZE                 ( 128 )
#define configTOTAL_HEAP_SIZE                    ( ( size_t ) ( PROBE_HEAP_KB * 1024 ) )
#define configMAX_TASK_NAME_LEN                  ( 16 )
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1

#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              1
#define configUSE_COUNTING_SEMAPHORES            1
#define configQUEUE_REGISTRY_SIZE                8
#define configUSE_QUEUE_SETS                     0
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_TIME_SLICING                   1
#define configUSE_NEWLIB_REENTRANT               0
#define configENABLE_BACKWARD_COMPATIBILITY      0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS  0
#define configSTACK_DEPTH_TYPE                   uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE         size_t
#define configUSE_C_RUNTIME_TLS_SUPPORT          0

#define configUSE_TRACE_FACILITY                 1

/* 通路 B：任务切换的事件环。宏在内核切完任务后、临界区里被调用，所以那里只做两次
   32 位写和一个计数。实现见 ../../event_ring/vtrace_events.c。 */
#include <stdint.h>

void vtrace_event_switch(uint32_t tcb);

#define traceTASK_SWITCHED_IN()                  vtrace_event_switch((uint32_t)pxCurrentTCB)
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configRECORD_STACK_HIGH_ADDRESS          1
#define configGENERATE_RUN_TIME_STATS            1
#define configRUN_TIME_COUNTER_TYPE              uint32_t

/* 运行时间统计用的计数器：Cortex-M3 自己的 DWT 周期计数器。
 *
 * 选它是因为它什么都不占：一个 32 位自由运行的周期计数器，打开两位就够
 * （DEMCR 的 TRCENA 与 DWT_CTRL 的 CYCCNTENA），不需要配时钟、不需要外设、
 * 不需要中断，main.c 里那份「不做时钟初始化、不碰外设」的约定得以保持。
 * 8 MHz 下 32 位约 537 秒绕回一次，远长于主机侧的观察窗口。
 *
 * 主机侧要的是两次读数之差占全部任务之差的比例，所以单位是周期还是微秒
 * 都不影响占比；要换算成时间才需要 CPU 频率。 */
void probe_start_run_time_counter( void );

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS() ( probe_start_run_time_counter() )
#define portGET_RUN_TIME_COUNTER_VALUE()         ( *( volatile uint32_t * ) 0xE0001004u )

#define configSUPPORT_STATIC_ALLOCATION          0
#define configSUPPORT_DYNAMIC_ALLOCATION         1
#define configCHECK_FOR_STACK_OVERFLOW           0
#define configUSE_MALLOC_FAILED_HOOK             1
#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_TIMERS                         0
#define configNUMBER_OF_CORES                    1
#define configUSE_POSIX_ERRNO                    0

#define INCLUDE_vTaskPrioritySet                 1
#define INCLUDE_uxTaskPriorityGet                1
#define INCLUDE_vTaskDelete                      1
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           1

#define configASSERT( x ) \
    if( ( x ) == 0 ) { taskDISABLE_INTERRUPTS(); for( ;; ) { } }

/* Keil 的器件启动文件把这三个异常叫 SVC_Handler / PendSV_Handler /
 * SysTick_Handler（都是 WEAK），而 FreeRTOS 的移植层导出的是另外三个名字。
 * 这里做同义映射，就不用去改器件启动文件。GCC 那一侧用的是 FreeRTOS 自己的
 * 名字、由 startup.c 直接引用，所以不映射。 */
#if defined( __ARMCC_VERSION )
    #define vPortSVCHandler     SVC_Handler
    #define xPortPendSVHandler  PendSV_Handler
    #define xPortSysTickHandler SysTick_Handler
#endif

/* 中断优先级：Cortex-M4 用 4 位优先级 */
#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      15
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    ( configLIBRARY_LOWEST_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    ( configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << ( 8 - configPRIO_BITS ) )

#endif /* FREERTOS_CONFIG_H */
