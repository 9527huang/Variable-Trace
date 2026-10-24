# RTEC2 目标侧参考实现

`rtec2_writer.c` / `rtec2_writer.h`：按 [线格式](../RTEC2-wire-format.md) §4 写控制块与槽。
没有依赖、没有分配、不等待——写不进去就计数丢掉，因为一个会阻塞调度器的追踪器会改变它要测的东西。

本目录**不参与主工程构建**，只在测试里编：`test/Rtec2WriterTest.cpp` 用 C 写、用 C++ 读，
闭环验证同一份字节。板子上的用法见下。

## 一、接到 FreeRTOS

把两个文件拷进工程，`FreeRTOSConfig.h` 里打开宏，然后把 hook 接到 writer 上。
`rtec2_writer_emit` 只做几次字存储和一个循环，可以在中断里调用，但**只能有一个写者**。

```c
#include "rtec2_writer.h"

/* 一块静态内存：64 字节控制块 + capacity × 56 字节槽。16 个槽是 960 字节。 */
static uint8_t trace_area[RTEC2_CONTROL_BYTES + 16 * RTEC2_SLOT_BYTES];
static uint8_t name_area[RTEC2_DICTIONARY_SLOTS * RTEC2_DICTIONARY_NAME_BYTES];

static rtec2_writer trace;
static rtec2_dictionary names;

void trace_start(uint32_t dwt_hz)
{
    rtec2_writer_init(&trace, trace_area, 16, /*epochLow*/ 1, /*epochHigh*/ 0, dwt_hz,
                      RTEC2_CAP_RAW_HANDLE32 | RTEC2_CAP_LIFECYCLE | RTEC2_CAP_GENERATION |
                      RTEC2_CAP_TARGET_CLOCK | RTEC2_CAP_EVENT_SEQUENCE | RTEC2_CAP_LOSS_DETECTION |
                      RTEC2_CAP_SYNC_EVENTS | RTEC2_CAP_DICTIONARY);

    rtec2_dictionary_init(&names, name_area);
}

/* FreeRTOSConfig.h:
 *   #define traceTASK_CREATE(pxNewTCB)                     rtec2_on_create(pxNewTCB)
 *   #define traceTASK_DELETE(pxTaskToDelete)               rtec2_on_delete(pxTaskToDelete)
 *   #define traceTASK_SWITCHED_IN()                        rtec2_on_switch_in()
 *   #define traceBLOCKING_ON_QUEUE_RECEIVE(pxQueue)        rtec2_on_wait(pxQueue)
 *   #define traceQUEUE_SEND(pxQueue)                       rtec2_on_signal(pxQueue)
 */
void rtec2_on_create(void* tcb)
{
    uint16_t slot = rtec2_dictionary_add(&names, pcTaskGetName(tcb));

    rtec2_event event = {0};
    event.type = 0x10;                       /* TaskCreate */
    event.hasTicks = 1;
    event.ticks = dwt_cycles();
    event.subject = (uint32_t)tcb;
    event.arg0 = (slot == 0xFFFF) ? 0 : slot;
    event.arg1 = uxTaskPriorityGet(tcb);

    rtec2_writer_emit(&trace, &event);
}
```

四条纪律：

1. **`event.type` 的数值就是线格式表里的数值**，不要自己编号。
2. **`hasTicks = 0` 表示没有目标时钟**。没有 DWT 就别填主机时间冒充——下游会把所有时长
   判成"不可计算"，那是对的答案。
3. **payload 最多 12 字节**，超了会被拒（不是截断）。Fault 的 12 字节刚好放下 class、
   flags、CFSR、PC。
4. **`targetHz` 说的是 ticks 的单位，不是 CPU 主频。** 上面这个例子把 DWT 周期数当 ticks，
   所以传 `dwt_hz`；`example/rtos_trace_port/` 那份传的是微秒，所以传 `1000000`。

   这一条看着显然，实际上我在这上面丢过整批事件：把 `configCPU_CLOCK_HZ`（8 MHz）当
   `targetHz` 传进去、而 ticks 填的是微秒，主机侧换算出 1/8 的时间，最后全部判成"没有目标
   时间"——`untimed` 满屏，而数字其实每一个都对。**两者不一致时没有任何一层会报错**，
   只会安静地给出错误的时间轴。

## 二、一份完整的固件接法

`example/rtos_trace_port/event_ring/vtrace_rtec2.c` 是三块板子共用的那一层，可以直接抄。
它比上面的最小例子多做四件事，每一件都对应内核给不出、或者只能由固件自己说的东西：

| 事件 | 来源 | 为什么要固件自己动手 |
|---|---|---|
| 任务切换 | `traceTASK_SWITCHED_IN` | 内核现成的钩子 |
| 队列送/收 | `traceQUEUE_SEND` / `traceQUEUE_RECEIVE` | 内核现成的钩子 |
| 堆分配/释放 | `traceMALLOC` / `traceFREE` | `heap_4` 现成的钩子 |
| **中断** | 固件自己的定时器 | 内核不拥有外设，没有"中断正在跑"这种钩子 |
| **崩溃** | 自己的 `HardFault_Handler` | 内核把 CPU 交给向量表，之后什么都不知道 |

两处容易被漏掉的地方：

**向量表要写到那个中断号为止。** `startup.c` 里原本只列到 SysTick（16 项），加上一个
IRQ28 的定时器就必须把表补到第 45 项。少一项，处理器取到的是表外的东西——表现是随机
崩溃，而不是"这个中断没有处理函数"。

**中断里调用内核前先把优先级降下来。** 定时器默认在最高优先级，从那里调用带
`FromISR` 语义的东西是未定义行为。`vtrace_rtec2_start_isr_source` 会写
`NVIC->IP[28] = 0xF0`，把定时器放到最低；时间也改用 `xTaskGetTickCountFromISR`。

**想要一次真的崩溃来验证整条链路**，写 `g_fault_demo = 0xC0DE`（`example/rtos_trace_port/`
的固件里有一个：`0x20000008`）。下一秒钟 `rep` 任务会通过空函数指针调用一次，硬件把
CFSR 和出错 PC 交出来，走的就是上面那条路。

一个 RAM 字加一秒一次的比较，同一份二进制——不用为了试故障另编一份固件。**触发之后
不要复位**：RTEC2 环在 RAM 里，复位会把那条记录清零。

## 三、主机侧怎么读它

`Rtec2Adapter` 在 `src/Rtos/Trace/`，读法的两个决定值得照抄：

**每轮采样读一次，不要挂在"任务视图回合"上。** 这个环的大小是按十毫秒设计的：64 槽、
目标每秒一千五百条，就是四十毫秒的历史。挂在一个两秒一次的回合上，读到的永远是零头。
我在真机上量过——`rounds` 十秒涨 22 而采样涨 900，读环被调用的次数只有采样次数的四十分之一。

**读新槽，不读整环。** `Rtec2RingReader::read` 要求整个环的缓冲（它按
`sequence & (capacity - 1)` 定位，缓冲短了就直接返回空），那是给"一次读全"用的。
`readWindow` 从游标起读一段连续槽，按位置索引，一轮两百字节而不是三千五百。

还有一条不属于协议、属于管线：**这一路的 `pump` 要自己调**。八字节那条老路上，
记录经过 `appendEventIntervals` 顺带把管线泵了一次；这一路把记录直接交给适配器，
不泵的话事件就堆在适配器里，界面上一直是零。

## 四、代价

| 动作 | 大致开销 |
|---|---|
| `rtec2_writer_emit`（无 payload） | 约 20 次字节/字存储 + 两次屏障 |
| 同上（12 字节 payload） | 再加 13 次字节存储 |
| 环满且覆盖 | 加一次计数更新 |

没有循环超过记录长度，最坏路径与事件大小线性相关，与环形状态无关（除了那一次容量比较）。
真机上的 P99 需要用 DWT 量，验收门槛见计划 §11.3：单事件 ≤ 2 µs @100 MHz 等效。

## 五、epoch

- 复位、重烧、`trace_start` 重新初始化时 `epoch += 1`，序号从 1 重来。
- 长时间运行且序号接近 32 位回绕时调 `rtec2_writer_next_epoch`：它把 head 归 1，
  读者会看到"游标跑到 head 前面"，据此判重启并清空身份。
- **不要**在运行中把 `capacity` 改掉：读者的索引算术依赖它。

## 六、这个实现没做什么

- **不做 slot 字典压缩**（SWD 的传输优化）。首期带宽够用，压缩是第二阶段的事。
- **不维护墓碑**：删除只发事件，代次由主机侧 `RtosIdentityStore` 记。
- **不做撕裂自检**：那是读者的事（双哨兵），写者只保证顺序。
- **不处理 SMP**：多核写同一个环需要每核一个环或一把锁，首期单核。
