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

三条纪律：

1. **`event.type` 的数值就是线格式表里的数值**，不要自己编号。
2. **`hasTicks = 0` 表示没有目标时钟**。没有 DWT 就别填主机时间冒充——下游会把所有时长
   判成"不可计算"，那是对的答案。
3. **payload 最多 12 字节**，超了会被拒（不是截断）。Fault 的 12 字节刚好放下 class、
   flags、CFSR、PC。

## 二、代价

| 动作 | 大致开销 |
|---|---|
| `rtec2_writer_emit`（无 payload） | 约 20 次字节/字存储 + 两次屏障 |
| 同上（12 字节 payload） | 再加 13 次字节存储 |
| 环满且覆盖 | 加一次计数更新 |

没有循环超过记录长度，最坏路径与事件大小线性相关，与环形状态无关（除了那一次容量比较）。
真机上的 P99 需要用 DWT 量，验收门槛见计划 §11.3：单事件 ≤ 2 µs @100 MHz 等效。

## 三、epoch

- 复位、重烧、`trace_start` 重新初始化时 `epoch += 1`，序号从 1 重来。
- 长时间运行且序号接近 32 位回绕时调 `rtec2_writer_next_epoch`：它把 head 归 1，
  读者会看到"游标跑到 head 前面"，据此判重启并清空身份。
- **不要**在运行中把 `capacity` 改掉：读者的索引算术依赖它。

## 四、这个实现没做什么

- **不做 slot 字典压缩**（SWD 的传输优化）。首期带宽够用，压缩是第二阶段的事。
- **不维护墓碑**：删除只发事件，代次由主机侧 `RtosIdentityStore` 记。
- **不做撕裂自检**：那是读者的事（双哨兵），写者只保证顺序。
- **不处理 SMP**：多核写同一个环需要每核一个环或一把锁，首期单核。
