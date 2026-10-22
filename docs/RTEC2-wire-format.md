# RTEC2 线格式（冻结草案）

- 文档状态：Phase 0 冻结项，本文一经合并即成为 C 目标侧、Python 主机侧、C++ 主机侧的共同依据
- 编制日期：2026-10-22
- 适用协议：`RTEC2`（Rtos Trace Event Contract v2），与 MTF v1、SWD v2 互不复用版本号
- 上游文档：[mdkdebug Trace 接入任务视图：开发计划](mdkdebug-Trace-任务视图-开发计划.md)
- 相关契约：[任务视图：目标侧接入要求](任务视图-目标侧接入要求.md)

本文只定义**字节**。身份规则、时间规则、诊断规则写在计划文档里，实现时不得从本文反推语义。

## 1. 通用编码

| 项 | 规定 |
|---|---|
| 字节序 | 小端 |
| 对齐 | 不要求；所有字段按表中偏移读取，不做隐式填充 |
| 整数 | 无符号定长，`u8` / `u16` / `u32` / `u64` |
| 时间 | `targetTicks` 为未扩展的 32 位目标计数（见 §5） |
| 未知记录类型 | 按本记录声明长度整条跳过，计入 `unknownRecords`，不报错、不推进语义 |
| 未知字段 | 记录长度大于已知字段之和时，多出的字节跳过（前向兼容的唯一手段） |
| 保留字段 | 必须写 0，读取时忽略 |
| 字符串 | 不在记录里内联；见 §7 字典 |

记录长度不自带字段，**由类型表决定**（固定布局）。这是刻意的：定长记录让目标侧写入有界，也让撕裂检测退化成两个哨兵值的比较。

## 2. 三种承载

| 承载 | 帧来源 | 完整性 | 用途 |
|---|---|---|---|
| 内存环缓冲 | 目标 RAM 里的固定槽数组 | 槽级哨兵 | 默认路径，主机按游标读 |
| RTT | 字节流 | 帧 CRC | 带宽够、目标有 RTT |
| SWO/ITM | 字节流 | 帧 CRC | 只有 SWO 时 |

三种承载承载**同一批记录**。差别只在成帧与校验，解码器上层不分叉。

### 2.1 流式帧（RTT / SWO）

```
offset size field
0      2    sync = 0x52 0x54 ('R','T')
2      1    frameVersion = 2
3      1    recordCount
4      2    payloadBytes
6      4    crc32(payload)          // CRC-32/MPEG-2, init 0xFFFFFFFF, no final xor
10     ...  payload = recordCount 条记录首尾相接
...    1    end = 0x0A
```

- 一条帧里的记录共享同一段序号连续性；帧内序号不连续即视为该帧作废。
- 重同步：从任意偏移开始逐字节寻找 `'R''T'`，帧头校验通过（长度合理、CRC 通过、结束符 0x0A）才接受。搜索窗口上限 4096 字节，超过则报 `resync-failed` 并请求新 epoch。
- 帧长度上限 1024 字节。超长视为损坏。

## 3. 记录

固定 24 字节头，两个可选尾。`flags` 决定尾部是否存在。

```
offset size field
0      1    type
1      1    flags        bit0 hasTargetTicks  bit1 hasPayload  bit2 isContinuation  bit3..6 coreId  bit7 reserved
2      2    reserved = 0
4      4    sequence     同一 epoch 内严格递增，0 保留（0 表示空槽/非法记录）
8      4    subject      发起者：任务 TCB 地址、ISR 号（0 = 无）
12     4    object       被操作对象句柄（0 = 无）
16     4    arg0
20     4    arg1
24     4    targetTicks   仅当 flags.bit0
28     1    payloadBytes  仅当 flags.bit1
29     n    payload       仅当 flags.bit1，n ≤ 12
```

`type` 决定 `subject` / `object` 的语义命名空间（任务、同步对象、ISR 各自独立），解码器不得跨命名空间比较句柄。

`coreId` 首期恒为 0；多于 16 核需要扩 flags 之外的字段，属于不兼容变更。

### 3.1 类型表

`A` 列：`arg0` / `arg1` / payload 的含义。

| type | 值 | 名称 | subject | object | A |
|---|---|---|---|---|---|
| 0x01 | 1 | `EpochBegin` | 0 | 0 | arg0 = 上一 epoch 低 32 位；payload = capability 位图（4 字节） |
| 0x02 | 2 | `ClockSync` | 0 | 0 | arg0 = 新 targetHz；arg1 = 该同步点的 targetTicks 低 32 位 |
| 0x03 | 3 | `Gap` | 0 | 0 | arg0 = 丢失条数（0 表示未知）；arg1 = 原因码（§4.2） |
| 0x04 | 4 | `SourceStatus` | 0 | 0 | arg0 = 拒绝计数；arg1 = 覆盖计数 |
| 0x10 | 16 | `TaskCreate` | tcb | 0 | arg0 = 任务号；arg1 = 初始优先级 |
| 0x11 | 17 | `TaskDelete` | tcb | 0 | — |
| 0x12 | 18 | `TaskRename` | tcb | 0 | payload = 字典引用（§7） |
| 0x13 | 19 | `TaskSwitchIn` | tcb | 0 | arg0 = 被切出的 tcb（0 = 首次切入） |
| 0x14 | 20 | `TaskSwitchOut` | tcb | 0 | arg0 = 切入的 tcb（0 = 空闲） |
| 0x15 | 21 | `TaskReady` | tcb | 0 | — |
| 0x16 | 22 | `TaskBlock` | tcb | 同步对象 | arg0 = 超时 ticks（0xFFFFFFFF = 永久） |
| 0x17 | 23 | `TaskWake` | tcb | 同步对象 | — |
| 0x18 | 24 | `PriorityChange` | tcb | 0 | arg0 = 新优先级；arg1 = 旧优先级 |
| 0x20 | 32 | `SyncCreate` | 0 | 对象 | arg0 = 对象种类（§3.2）；arg1 = 容量 |
| 0x21 | 33 | `SyncDelete` | 0 | 对象 | — |
| 0x22 | 34 | `WaitBegin` | tcb | 对象 | arg0 = 超时 ticks |
| 0x23 | 35 | `WaitEnd` | tcb | 对象 | arg0 = 结果（§3.3） |
| 0x24 | 36 | `Signal` | tcb 或 0 | 对象 | arg0 = 唤醒的任务数 |
| 0x25 | 37 | `Acquire` | tcb | 对象 | arg0 = 递归深度 |
| 0x26 | 38 | `Release` | tcb | 对象 | arg0 = 剩余递归深度 |
| 0x30 | 48 | `IsrEnter` | 0 | 0 | arg0 = IRQ 号；arg1 = 嵌套深度 |
| 0x31 | 49 | `IsrExit` | 0 | 0 | arg0 = IRQ 号；arg1 = 嵌套深度 |
| 0x40 | 64 | `Fault` | 0 | 0 | payload = §3.4 |
| 0x50 | 80 | `Alloc` | 0 | 0 | arg0 = 地址；arg1 = 字节数（已知时） |
| 0x51 | 81 | `Free` | 0 | 0 | arg0 = 地址；arg1 = 字节数（未知写 0） |

`0x08`–`0x0F`、`0x19`–`0x1F`、`0x27`–`0x2F`、`0x32`–`0x3F`、`0x41`–`0x4F`、`0x52`–`0x7F` 为保留区间，收到即按未知类型跳过。

### 3.2 对象种类

| 值 | 名称 |
|---|---|
| 1 | queue |
| 2 | semaphore |
| 3 | mutex |
| 4 | event group |
| 5 | stream buffer |
| 6 | software timer |

### 3.3 等待结果

| 值 | 名称 |
|---|---|
| 0 | signalled |
| 1 | timeout |
| 2 | aborted |
| 3 | deleted |

### 3.4 Fault payload（12 字节）

```
0  u8  faultClass     // 1 UsageFault, 2 BusFault, 3 MemManage, 4 HardFault
1  u8  flags           // bit0 hasCFSR bit1 hasPC bit2 hasLR bit3 hasSP bit4 hasHFSR
2  u16 cfsr 低 16 位
4  u32 pc
8  u32 lr
```

`SP` 与 `HFSR` 放不进 12 字节时置 `flags` 对应位为 0 并在 `arg0` 放 `hfsr`、`arg1` 放 `sp`。**缺失必须靠 flags 说明**，不能写 0 冒充读数。

## 4. 内存环缓冲

### 4.1 控制块

主机通过符号 `mdk_trace_rtec2_blob` 找到控制块，控制块是 64 字节：

```
offset size field
0      4    magic = 'R','T','C','2'
4      2    wireVersion = 2
6      2    slotBytes     // 首期 48
8      4    capacity      // 槽数，2 的幂
12     4    epoch 低 32 位
16     4    epoch 高 32 位
20     4    targetHz
24     4    head          // 写者负责：下一个要写的序号（单调，不复位）
28     4    tail          // 读者负责：已提交序号（主机单写）
32     4    generation    // 奇数 = 写者正在写某个槽，偶数 = 空闲
36     4    rejected      // 因槽放不下/不可重入而丢弃的记录数
40     4    overwritten   // 因覆盖未提交槽而丢弃的记录数
44     4    committedSeq  // 最后一条已完整提交的序号
48     4    capabilities  // §6
52     4    faultCount    // Fault 记录被拒的次数（正常为 0）
56     8    reserved
```

`head` 与 `tail` 是**序号**不是索引；槽索引 = `序号 & (capacity - 1)`。序号 32 位回绕由主机侧扩展（§5.2），目标侧只要求单调递增。

### 4.2 槽（48 字节）

```
offset size field
0      4    seq         // 0 = 空；写入时最后写
4      4    seqCheck    // = ~seq；写入时最先写
8      1    type
9      1    flags
10     2    coreId
12     4    subject
16     4    object
20     4    arg0
24     4    arg1
28     4    targetTicks   // 0 表示该事件无目标时间（配合 flags.bit0 判定）
32     8    payload       // 前 payloadBytes 字节有效
40     4    tailCheck    // = ~seq；写入时第二最后写
44     4    reserved = 0
```

### 4.3 写入顺序（写者）

1. `seqCheck = ~seq`（先作废旧内容）
2. 写 `type` / `flags` / `coreId` / `subject` / `object` / `arg0` / `arg1` / `targetTicks` / `payload`
3. `tailCheck = ~seq`
4. `seq = 新序号`
5. `head = 新序号 + 1`（在 4 之后，保证读者看不到"序号新但内容旧"的槽）

第 5 步之前若检测到 `tail + capacity <= 新序号`（读者没跟上），按策略处理：`overwrite`（默认写满即覆盖并 `overwritten++`）或 `drop`（丢弃并 `rejected++`）。策略由编译选项决定，运行时不改。

### 4.4 读取与撕裂（读者）

对序号 `n` 的槽：

1. 读 `seq`；若 `seq != n` → 该槽尚未写入（停止，等下一轮）
2. 读 `seqCheck`，必须 `== ~n`
3. 读内容
4. 读 `tailCheck`，必须 `== ~n`
5. 读 `seq` 再确认 `== n`（防止读期间被覆盖）

任一步不成立 → 该记录作废，向上一级报 `torn`，**不推进游标**。连续 3 次撕裂 → 请求新 epoch（`resync-required`）。

**整批成功才提交**：一轮读取把 `tail` 从 `t0` 推进到 `t1`，写回 `tail = t1` 必须在整批记录都校验通过之后；任一失败则 `tail` 保持 `t0`。

## 5. 时间

### 5.1 三种时间

| 层 | 字段 | 说明 |
|---|---|---|
| 原始 | `targetTicks`（32 位） | 目标计数，诊断与重算的唯一依据 |
| 频率 | `targetHz` | 该事件生效的频率，由 `EpochBegin` / `ClockSync` 给出 |
| 显示 | `displaySeconds` | 主机映射到会话时间轴的派生值，不落盘 |

### 5.2 回绕扩展

主机维护 `(lastRaw, wrapCount)`：`raw < lastRaw && (lastRaw - raw) > 0x80000000` 时 `wrapCount++`。扩展后 `ticks64 = wrapCount * 2^32 + raw`。这条规则只在同一 epoch、且 `ClockSync` 未取消连续性时成立。

`ClockSync` 出现在非预期位置（变频、DWT 停止、tickless idle 之后）时，**不得**用旧频率换算跨点区间；跨同步点的区间一律标 `NotApplicable`。

### 5.3 无目标时间

`flags.bit0 == 0` 的记录没有目标时间（MTF v1 兼容源、或目标没有 DWT）。这类记录只提供**顺序与计数**：不得生成 ISR 时长、任务占用、等待时长，也不得叠到变量时间轴上。

## 6. 能力位

| bit | 名称 | 含义 |
|---|---|---|
| 0 | `rawHandle32` | 句柄是真实 32 位地址/句柄，不是短 ID |
| 1 | `lifecycleEvents` | 会发 `TaskCreate` / `TaskDelete` / `SyncCreate` / `SyncDelete` |
| 2 | `generation` | 支持同一 handle 的代次（§8） |
| 3 | `targetClock` | `flags.bit0` 可靠 |
| 4 | `eventSequence` | `sequence` 严格递增且不跳（除 `Gap` 明示） |
| 5 | `lossDetection` | 会主动发 `Gap` |
| 6 | `isrEvents` | 发 `IsrEnter` / `IsrExit` |
| 7 | `faultEvents` | 发 `Fault` |
| 8 | `syncEvents` | 发同步类事件 |
| 9 | `heapEvents` | 发 `Alloc` / `Free` |
| 10 | `dictionary` | 支持 §7 字典 |
| 11–31 | 保留，读 0 | |

能力不足时上游规则返回 `NotApplicable`，UI 必须显示"这项没有数据"，不能显示"没有问题"。

## 7. 字典

名字不进事件流。`TaskRename` / `SyncCreate` 的 payload 是 6 字节引用：

```
0  u16 dictSlot
2  u32 nameHash    // FNV-1a 32
```

字典本身是一块静态区（128 槽 × 16 字节，名字截断到 15 字节 + 结束符），由主机通过一次内存读取整块取回。规则：

- 引用先于字典读取时，主机挂起名字解析，等字典到手再补；**不得**用当前快照的名字倒填历史。
- `nameHash` 与字典内容不符 → 报 `dictionary-mismatch`，该名字作废（显示地址）。
- 字典槽被复用时，旧引用失效：`SyncCreate` / `TaskRename` 必须在写入引用**之前**写好字典，且字典写入跨越槽边界时整个名字先置空再写入。

SWD 的 slot 压缩只能用于**传输缓存**：`DefineSlot` 必须携带完整 `kind + rawHandle32 + generation`；slot 失同步时停止解码并请求新 epoch，禁止猜测。

## 8. 代次与 epoch

- `generation` 由目标侧维护：`TaskCreate`/`SyncCreate` 时对同一 `rawHandle` 递增（从 1 开始），随创建事件的 payload 一起给出（`arg0` 高位或 `SyncCreate.arg1` 的上 16 位；首期取 `arg0 & 0xFFFF`）。
- 主机身份键：`(sessionId, epoch, kind, rawHandle32, generation)`。
- `epoch` 在复位、重烧、重启录制、序号接近回绕时递增。**epoch 变化即关闭全部实体与未完成区间**，相同地址在新 epoch 里是新实体。
- 缺 `Create` 的先见事件：建 `generation = unknown` 的临时实体，起点标未知；后续快照只补显示名，不改写历史。

## 9. 版本与拒绝

| 情况 | 行为 |
|---|---|
| `wireVersion` 主版本 != 2 | 拒绝启动，报 `unsupported-wire-version` |
| `wireVersion` 次版本更高 | 接受；未知类型跳过；未知能力位忽略 |
| `magic` 不符 | 不是 RTEC2 数据源，报 `not-a-trace-source` |
| `capacity` 非 2 的幂或 < 16 | 拒绝，报 `control-block-corrupt` |
| `slotBytes != 48` | 拒绝（首期定长；扩展槽需要新 wireVersion） |
| epoch 回退 | 拒绝该批，请求新 epoch |

## 10. Golden vector

Phase 1 之前必须产出 `test/trace-golden/` 下的二进制样例，C 编码器生成，Python 与 C++ 解码器共同消费：

- 每种记录各一条（含最短、最长 payload）
- 一条含 `EpochBegin` + `ClockSync` + `Gap` 的完整会话
- 一条 32 位序号回绕样例
- 一条撕裂槽样例（`seqCheck` 不匹配）
- 一条未知类型样例（必须被跳过而不是报错）

三个解码器对同一向量必须逐字段一致；CI 里比对的是**解码结果的结构化输出**（JSON），不是内存映像。

## 11. 冻结状态

以下条目自本文合并起不再开放讨论，变更需新版本：

1. 记录固定 24 字节头；可选尾只有 `targetTicks` 与 `payload`。
2. 槽 48 字节，`seqCheck` / `tailCheck` 双哨兵。
3. 记录里不带代次；代次只在 `Create` 上给。
4. 名字只经字典，不进事件流。
5. `Fault` 缺失字段用 flags 声明，不用 0 代替。
6. 读失败不推进游标。
