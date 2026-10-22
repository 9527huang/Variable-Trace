# mdkdebug Trace 接入任务视图：开发计划与验收方案

- 文档状态：实施基线草案，协议冻结后进入开发
- 编制日期：2026-10-22
- Variable-Trace 基线：1.2.3
- mdkdebug 基线：0.2.17
- 首期范围：FreeRTOS、32 位 Cortex-M、单核；模型预留多核字段
- 相关契约：[任务视图：目标侧接入要求](任务视图-目标侧接入要求.md)
- 相关主机计划：[任务视图：功能范围与开发计划](../analysis/任务视图-开发计划.md)

本文把 mdkdebug 的 Trace 能力接入 Variable-Trace 任务视图，给出边界、数据契约、模块改动、开发顺序和验收门槛。本文不取代目标侧接入契约；发生冲突时，先修改并版本化契约，再修改实现。

## 1. 目标与非目标

### 1.1 目标

1. Variable-Trace 能直接消费 mdkdebug 0.2.17 的 MTF v1（SWO/RTT）和 SWD v2 事件，先以真实能力降级显示。
2. 新协议完整保留 32 位 TCB/对象句柄，并用生命周期和 epoch 消除短 ID、地址复用导致的历史串线。
3. 任务时间线增加 ISR、Fault、同步和堆四类独立轨道，所有轨道与变量曲线使用同一显示时间轴。
4. 将 mdkdebug 的时间基、丢事件、段重开、长 ISR、Fault、同步配对等诊断合并进 Findings，并明确区分“正常”“无法判断”和“数据不可信”。
5. Variable-Trace 继续负责连续快照、统一模型、等待图、录制回放、查询和交互 UI。
6. mdkdebug 继续负责插桩部署、工程修改、编译、烧录、目标侧组件、无人值守采集和采集包生成。

### 1.2 非目标

- 不通过 MCP JSON 实时转发高频事件；MCP 只做控制、状态和低频摘要。
- 不让 mdkdebug 和 Variable-Trace 同时操作同一探针或同时推进同一个 SWD/RTT 读游标。
- 不把 MTF v1 的主机到达时间伪装成目标执行时间。
- 不用当前快照给历史短 ID 倒填身份。
- 首期不承诺 SMP 因果排序、ETM 指令级关联或低于 1 ms 控制环的硬实时证明。
- 不在第一阶段删除现有 `RtosEventRing` 8 字节切换记录兼容路径。

## 2. 已确认的基线与差距

| 能力 | Variable-Trace 1.2.3 | mdkdebug 0.2.17 | 集成结论 |
|---|---|---|---|
| 快照 | 连续 FreeRTOS 快照、对象枚举、栈水位、CPU 占用 | 单次 RTOS/DWARF 查询 | 保留 Variable-Trace 实现 |
| 精确调度事件 | `RtosEventRing::Record {tcbAddress,timeUs}`，主要是 switch-in | `sched` 事件，但现有路径使用短任务 ID | 先兼容，后升级完整句柄 |
| MTF | 现有任务 ITM 解码器只认 8 字节记录 | MTF v1，SWO/RTT；有类型与顺序，通常无可靠目标时间 | 新增 MTF 适配器，标为顺序型数据 |
| SWD | 现有内存环是项目自有固定记录 | SWD v2，DWT 周期、丢失计数、压缩字典和读游标 | 新增 SWD v2 适配器 |
| 生命周期 | roster 以地址维持会话内任务历史 | 有 create/exit 语义，但仍是短 slot | 新协议增加 32 位句柄与代次 |
| ISR/Fault/sync/heap | 尚未进入统一任务事件模型 | 已采集、统计和诊断 | 扩展统一模型、轨道和录制格式 |
| Findings | 死锁、无界等待、就绪饥饿、优先级继承 | 时间基、丢失、ISR、Fault、同步等规则 | 在统一证据模型上合并 |
| 回放 | 完整快照和切换区间，录制格式 v2 | JSON/HTML 侧重单次分析 | Variable-Trace 录制格式升级 |

必须接受的事实：当前 MTF v1 和 SWD v2 只能达到兼容事件层，不能证明完整对象身份。适配器必须保留这种不确定性，不能生成虚假的 32 位句柄或代次。

## 3. 总体架构和所有权

### 3.1 数据面与控制面

```text
mdkdebug 控制面
  部署组件 -> 修改配置 -> 编译 -> 烧录 -> 生成 manifest -> 探针交接
                                                        |
                                                        v
Variable-Trace 数据面                         mdkdebug 无人值守数据面
  探针读取 -> MTF/SWD 适配器                    探针读取 -> 原始采集包
       -> 统一事件 -> 身份/时间/质量处理              -> 离线导入
       -> 快照对账 -> 模型/Findings
       -> 时间线/等待图/录制/UI
```

不建立“mdkdebug MCP 高频事件 -> Variable-Trace”链路。交互观察直接走探针；无人值守采集写入有校验的文件包，Variable-Trace 离线打开或只读跟随该文件。

### 3.2 两种运行模式

| 模式 | 探针所有者 | 事件去向 | 使用场景 |
|---|---|---|---|
| 交互模式 | Variable-Trace | 直接进入统一模型和 UI | 实时排障、变量与任务联合观察 |
| 无人值守模式 | mdkdebug | 采集包，结束后由 Variable-Trace 导入 | 长时间回归、夜间采集、自动化流水线 |

无人值守模式如需实时查看，只允许 Variable-Trace 读取 mdkdebug 已提交的文件块；不得直接访问探针，也不得写采集游标。

### 3.3 探针租约状态机

```text
Idle
  -> MdkExclusive       部署、编译、烧录、复位、无人值守采集
  -> HandoffPreparing   停止采集，刷新文件，释放 RTT/SWO/SWD 会话
  -> ViewerExclusive    Variable-Trace 建立探针会话并采集
  -> HandoffPreparing   Variable-Trace 停止、落盘、释放探针
  -> MdkExclusive
```

租约至少包含 `probeSerial`、`ownerPid`、`mode`、`leaseId`、`acquiredAt` 和心跳时间。租约文件只用于冲突检测，不用于互斥安全；真正打开探针失败时必须中止，不得绕过。进程崩溃后可以回收过期租约，但必须新建 epoch。

## 4. 统一事件契约

### 4.1 分层原则

协议分三层，禁止相互泄漏：

1. **线格式层**：MTF v1、SWD v2、后续完整句柄线格式。
2. **适配层**：验证帧、扩展时间、标记缺口，输出统一事件；不做 UI 和业务诊断。
3. **语义层**：身份、生命周期、快照对账、状态区间、Findings 和显示。

建议把新的独立契约命名为 `Rtos Trace Event Contract v2`，简称 `RTEC2`。它是统一逻辑契约，不复用 MTF v1 或 SWD v2 的版本号，避免出现三个“v2”含义不同。

### 4.2 统一事件最小字段

```cpp
struct TraceEvent {
    SessionId sessionId;          // 主机会话 UUID
    uint64_t epoch;               // 复位、重烧、重启录制后变化
    uint32_t sequence;            // epoch 内严格递增，0 保留
    uint64_t targetTicks;         // 扩展后的目标时钟；未知时不填
    uint32_t targetHz;            // 此事件生效的频率；未知为 0
    uint8_t coreId;
    EventType type;
    EventFlags flags;
    EntityRef subject;            // 发起任务/ISR/堆等
    EntityRef object;             // 被操作对象，可空
    uint32_t arg0;
    uint32_t arg1;
    EvidenceQuality quality;
};

struct EntityRef {
    EntityKind kind;
    uint32_t rawHandle;           // 32 位 TCB/对象句柄
    uint32_t generation;          // 同 epoch、同 kind、同 handle 的代次
    uint16_t legacyId;            // 仅兼容源使用
    IdentityQuality quality;      // Exact / Inferred / Legacy / Unknown
};
```

`rawHandle` 不得被短 ID、哈希或数组 slot 替换。兼容源只有短 ID 时，`rawHandle` 保持未知，数据只在 `(session, epoch, source, kind, legacyId)` 范围内关联，并显示“身份可能复用”。

### 4.3 稳定身份与生命周期

统一身份键为：

```text
(sessionId, epoch, entityKind, rawHandle32, generation)
```

处理规则：

1. `Create` 打开新代次；同一 handle 已打开时，先生成 `ImplicitClose` 和数据质量 Finding，再增加代次。
2. `Delete` 关闭当前代次并保留墓碑；删除后的普通事件不得重新打开旧代次。
3. 中途接入且先看到普通事件时，建立 `generation=unknown` 的临时实体，起点标为未知；后续快照只能补当前显示名，不能改写已有历史。
4. epoch 变化时关闭全部实体和未完成区间；相同地址在新 epoch 中是新实体。
5. 序号缺口跨过创建或删除时，相关身份降为 `UnknownAfterGap`；不得把缺口两侧的对象强行合并。
6. 任务、队列、信号量、互斥量、事件组和 ISR 使用分离命名空间。

### 4.4 RTEC2 线格式冻结项

Phase 0 必须另行提交 `RTEC2-wire-format.md`，至少冻结：

- 小端编码、对齐、头长度、记录长度和未知字段跳过规则；
- capability 位、epoch、core ID、时钟源/频率、构建 ID；
- 每条记录的 sequence、目标 ticks、类型、两个 32 位实体句柄和参数；
- Task/Object create、delete、rename/dictionary、clock-sync、gap、epoch 事件；
- 环缓冲的 generation、提交屏障、覆盖/拒绝计数和撕裂检测；
- 流式后端的帧 CRC、重同步规则和字典定义先于引用规则；
- 版本兼容表和拒绝策略。

为控制带宽，SWD 可以继续使用 slot 压缩，但 slot 只能是传输字典缓存：`DefineSlot` 必须携带完整 `kind + rawHandle32 + generation`，slot 失同步时停止解码并请求新 epoch，不能猜测。

### 4.5 事件类型首期集合

| 分类 | 必需事件 |
|---|---|
| 会话 | `EpochBegin`、`ClockSync`、`Gap`、`SourceStatus` |
| 任务 | `TaskCreate`、`TaskDelete`、`TaskSwitchIn`、`TaskSwitchOut`、`TaskReady`、`TaskBlock`、`TaskWake`、`PriorityChange` |
| 同步 | `WaitBegin`、`WaitEnd`、`Signal`、`Acquire`、`Release`、`Timeout`、对象 create/delete |
| ISR | `IsrEnter`、`IsrExit`，保留嵌套深度和异常号 |
| Fault | `Fault`，包含 class、CFSR、PC、LR、SP；缺失字段显式标记 |
| 堆 | `Alloc`、`Free`，包含地址和大小；不知道大小时不推算 |

## 5. 时间模型、缺口与快照对账

### 5.1 时间域

内部同时保存三种时间，不能只留换算后的 `double seconds`：

- `targetTicks`：原始目标计数，诊断和重算的依据；
- `targetHz`：在该区间有效的频率；
- `displaySeconds`：映射到 Variable-Trace 会话时间轴的显示值。

SWD 以 DWT 周期为首选时间；32 位回绕依靠 sequence 扩展到 64 位。动态变频、DWT 停止、tickless idle 或无法证明连续时必须产生 `ClockSync` 或新 epoch。

MTF v1 只有主机到达时刻时：保留事件顺序和计数，`targetTicks` 为空；可以显示“顺序事件列表”，但不生成 ISR 时长、任务 CPU 时间或同步等待时长，也不把事件精确叠加到变量时间轴。

### 5.2 缺口是一级事件

下列情况统一产生 `Gap`，同时保留具体原因：环覆盖、目标拒绝、SWO 字节丢失、CRC 错误、序号跳变、主机预算跳过、撕裂重试失败、采集段重开。

Gap 的行为：

- 终止所有穿越缺口的任务、ISR、等待和堆归因区间；
- 区间末端画断口，不延长到下一条事件；
- 穿越缺口的耗时和因果诊断标为不可计算；
- Findings 给出计数、时间范围、来源和建议，不把多种丢失合成一个无来源数字。

### 5.3 快照与事件对账

每个发布快照执行一次非破坏性对账：

| 冲突 | 处理 |
|---|---|
| 快照当前任务与事件当前执行任务不同 | 保留两者，产生 `snapshot-event-divergence`，不回写历史 |
| 快照看到未知 TCB | 建立当前实体，身份来源为 Snapshot；等待生命周期事件确认 |
| 事件实体当前快照不存在 | 若有 Delete 则正常；否则标记 `not-observed-in-snapshot`，不自动删除 |
| 名称不同 | 按生效区间保存别名；目标字典优先于当前快照历史倒填 |
| 对象等待关系不同 | 当前 UI 用快照，历史因果用事件；Finding 明示证据来源 |

## 6. Variable-Trace 主机改造

### 6.1 新增模块

建议在 `src/Rtos/Trace/` 建立独立模块：

| 文件/类 | 责任 |
|---|---|
| `RtosTraceTypes.*` | 统一事件、实体引用、能力和质量枚举 |
| `IRtosTraceSource.*` | `start/poll/stop/reset/capabilities` 数据源接口 |
| `MtfTraceAdapter.*` | MTF v1 帧验证、解码和降级能力声明 |
| `MdkSwdTraceAdapter.*` | SWD v2 控制块、压缩 token、游标和丢失处理 |
| `Rtec2TraceAdapter.*` | 新完整句柄协议解码 |
| `RtosTraceClock.*` | 回绕扩展、频率区间和显示时间映射 |
| `RtosIdentityStore.*` | 生命周期、代次、墓碑、名称生效区间 |
| `RtosTraceReducer.*` | 事件归约为任务/ISR/同步/堆区间 |
| `RtosTraceQuality.*` | 缺口、冲突、能力与可信度汇总 |

`IRtosTraceSource::poll()` 返回一个不可变 `TraceBatch`：事件、原始游标、源统计、能力和适配器诊断必须属于同一次读取。读取失败不得推进游标。

### 6.2 既有模块改动

| 既有模块 | 改动 |
|---|---|
| `ViewerDataHandler` | 只负责调度数据源和探针预算；移出具体 MTF/SWD 解码状态 |
| `RtosModel` | 保存统一事件历史、实体字典、质量状态；发布快照时执行对账 |
| `RtosEventHistory` | 从纯 `Interval` 扩展为按类型索引的事件/区间存储；保留旧接口兼容 |
| `RtosTimeline` | 继续保存采样状态；精确事件作为独立覆盖层，不互相覆盖原始证据 |
| `RtosDiagnostics` | 接受 `DiagnosticContext`，合并快照规则、事件规则和质量门控 |
| `RtosRecording` | 升级到 v3 分块格式并保留 v1/v2 只读兼容 |
| `GuiRtosPanelTimeline` | 增加轨道组、虚拟化、缺口、证据跳转和来源标识 |
| `RtosTools.cpp` | 暴露统一模型的低频查询，不触发额外探针读取 |

旧 `RtosEventRing` 和 `RtosItmDecoder` 在兼容期保留；新模型稳定后再决定是否只作为 legacy adapter 内部实现。

### 6.3 独立轨道

| 轨道 | 表示方式 | 关键交互 |
|---|---|---|
| Task | 每个实体代次一条泳道；运行/就绪/阻塞/挂起区间 | 选任务、看等待原因、跳事件 |
| ISR | 默认按 IRQ 聚合，可展开；嵌套用层级 | 显示 enter/exit、持续时间、被抢占任务 |
| Fault | 单独固定轨道，瞬时标记加严重级别 | 点击显示寄存器与前后事件 |
| Sync | 按对象或类型聚合；wait/acquire/release/signal 标记和区间 | 双向跳任务/对象、看配对质量 |
| Heap | alloc/free 标记，加净变化曲线；仅证据足够时显示在用量 | 按任务/地址筛选、定位异常分配 |
| Gap/Clock | 固定质量轨道 | 显示丢失、epoch、变频和不连续区间 |

轨道渲染必须按可见时间窗查询并做像素级聚合，不能每帧遍历完整录制。Fault 和 Gap 永不被抽样隐藏。

## 7. Findings 合并方案

### 7.1 统一 Finding 结构

```cpp
struct Finding {
    FindingRuleId rule;
    Severity severity;
    FindingState state;       // Triggered / Clear / NotApplicable / Unknown
    TimeRange evidenceRange;
    std::vector<EntityKey> entities;
    EvidenceQuality quality;
    std::string summary;
    std::string detail;
    std::string remediation;
};
```

UI 不能把 `NotApplicable` 或 `Unknown` 计作“没有问题”。规则阈值、规则版本和证据范围必须进入录制元数据，回放时可选择按原规则或当前规则重算。

### 7.2 首期迁移规则

| 规则 ID | 来源 | 门控条件 |
|---|---|---|
| `no-data` / `kernel-hooks-absent` / `no-sched` | mdkdebug | 源已启动且能力探测完成 |
| `no-timeline` / `timebase-missing` | mdkdebug | 事件存在 |
| `dt-frozen` / `dt-zero-heavy` / `long-quiet-window` | mdkdebug | 同 epoch 有至少两个可信时间点 |
| `events-lost` / `segments-restarted` | mdkdebug + 主机 | 始终评估，不受其他质量门控 |
| `isr-unpaired` / `isr-long` | mdkdebug | 无穿越 Gap、时间基可信 |
| `fault-present` | mdkdebug | 只需有效 Fault 帧；不因缺口隐藏 |
| `sync-wait-unpaired` / `sync-wait-ambiguous` | mdkdebug | 身份未复用、配对区间无 Gap |
| `priority-inheritance` / `suspected-deadlock` | Variable-Trace | 快照一致性满足当前规则 |
| `ready-starvation` / `unbounded-wait` | Variable-Trace | 历史覆盖和对应时间证据满足规则 |
| `snapshot-event-divergence` / `identity-ambiguous` | 新增 | 对账或身份状态出现冲突 |

诊断执行顺序固定为：能力检查 -> 数据质量 -> 身份/时间归一化 -> 语义规则。上游质量不满足时，下游规则返回 `Unknown`，不得继续给确定结论。

## 8. 录制与采集包

### 8.1 Variable-Trace 录制 v3

采用可追加分块容器，每块包含类型、版本、长度、时间范围、序号范围和 CRC。至少支持：

- `SessionManifest`
- `RawTraceBlock`
- `NormalizedEvents`
- `SnapshotRound`
- `IdentityDictionary`
- `SourceDiagnostics`
- `RuleConfiguration`
- `IndexCheckpoint`

写入顺序为“数据块 -> CRC -> 提交标记 -> 索引检查点”。程序崩溃时读到最后一个完整提交块即可恢复。v1/v2 文件继续可读；新字段缺失时按能力降级，不批量重写旧文件。

原始块和归一化事件都要保存：原始块用于新解码器复算和协议争议，归一化事件用于快速回放。两者必须用同一 source cursor 和序号范围关联。

### 8.2 mdkdebug 无人值守采集包

采集包与 Variable-Trace v3 使用同一容器或明确的可转换子集，不再以巨型 JSON 作为正式交换格式。采集包必须包含：

- 部署 manifest、ELF/AXF SHA-256、镜像 SHA-256、构建 ID；
- 目标/探针信息、协议版本、capabilities、时钟配置；
- 原始 trace 块、读游标、丢失计数、epoch；
- 采集开始/结束原因和 mdkdebug 版本；
- 可选 ELF 路径；文件本体是否打包由用户配置决定。

## 9. mdkdebug 改造

### 9.1 目标组件

在 `components/trace/` 完成：

1. 保留现有 MTF v1、BUFF 和 SWD v2 ABI，增加明确的 legacy capability。
2. 新增 RTEC2 记录 API，任务和对象参数使用 `uint32_t` 原生句柄。
3. FreeRTOS hook 适配层发布任务/对象 create/delete、切换、阻塞、唤醒和优先级变化。
4. ISR/Fault 路径保持有界、无分配、不等待；嵌套无法安全写入时增加拒绝计数。
5. 环缓冲使用奇偶 generation、发布屏障、最后提交 sequence；主机可检测撕裂。
6. 输出 capability/描述块和 epoch；序号接近回绕时强制新 epoch。
7. 给每个编译等级输出实际 Flash、RAM 和事件路径开销报告。

### 9.2 Python 主机工具

重点改动位置：

| 模块 | 工作 |
|---|---|
| `traceproto.py` | 保留 MTF v1；增加 RTEC2 编解码和 golden vector |
| `swd.py` / `trace.py` | 读取新描述块、原始块落盘、严格游标提交、采集包写入 |
| `trstats.py` | 规则逻辑抽成与 C++ 共用的规则说明和测试向量；现有输出兼容 |
| `trace_instrument` | 部署新源文件、生成配置、验证 hooks、生成 manifest |
| build/flash 工具 | 将实际 ELF/hash/烧录结果写入 manifest，失败时不允许开始采集 |

`trace_instrument` 必须幂等：相同配置重复执行不产生无关 diff；升级组件前保留可恢复备份或生成补丁预览。无人值守流程任何一步失败都不得继续到“采集成功”。

### 9.3 部署 manifest

建议文件名 `.variable-trace/target-manifest.json`，最少字段：

```json
{
  "schema": 1,
  "mdkdebug_version": "0.2.17+",
  "build_id": "...",
  "elf_sha256": "...",
  "image_sha256": "...",
  "rtos": "FreeRTOS",
  "trace_backend": "swd",
  "wire_protocol": "RTEC2",
  "wire_version": 2,
  "capabilities": ["rawHandle32", "lifecycleEvents", "eventSequence", "lossDetection"],
  "symbols": {"control": "mdk_trace_rtec2_blob"},
  "clock": {"source": "DWT", "hz": 168000000},
  "probe": {"serial": "..."}
}
```

Variable-Trace 打开目标时校验 manifest、当前 ELF 和目标描述块。hash 不一致时禁止自动名称解析，并产生高优先级 Finding。

## 10. 分阶段开发计划

以下估算以两名工程师为前提：A 负责 C++ 主机/UI，B 负责 C/Python 目标与自动化；测试和协议共同负责。总量约 65～75 人日，关键路径 9～11 周。

### Phase 0：冻结契约与基准（4～5 人日）

步骤：

1. 写 `RTEC2-wire-format.md`、事件词汇表和状态机。
2. 冻结探针租约和两种运行模式。
3. 冻结 manifest、采集包和录制 v3 的 schema。
4. 采集当前 MTF/SWD 吞吐、事件成本、RAM/Flash 和 Variable-Trace 渲染基线。
5. 建立需求到测试用例的追踪表。

退出条件：所有未决字段都有 owner；C/C++/Python 对同一 golden vector 的解释一致；不再存在“短 ID 是否可当身份”之类开放语义。

### Phase 1：主机统一模型骨架（6～8 人日）

步骤：

1. 新建统一事件、能力、质量和数据源接口。
2. 实现时钟扩展、Gap、epoch 和身份存储的纯逻辑模块。
3. `RtosModel` 接入双历史：现有快照历史 + 新事件历史。
4. 保持现有 UI 和录制测试全绿，旧事件环行为不变。

退出条件：构造事件可完整经过 source -> model -> query；句柄复用、回绕、缺口单元测试通过。

### Phase 2：MTF v1 与 SWD v2 兼容适配器（8～10 人日）

步骤：

1. 用 mdkdebug golden vector 实现 MTF v1 解码；CRC/截断/重同步有测试。
2. 实现 SWD v2 控制块和 token 解码，严格遵守“整批成功才推进 drained”。
3. capability 探测和降级文案进入模型。
4. 接入 `ViewerDataHandler` 的轮询预算和探针生命周期。
5. 交叉对比 Python 与 C++ 解码输出。

退出条件：同一输入两端事件序列逐字段一致；MTF 无目标时间时所有时长规则均为 `NotApplicable`；SWD 丢失和重启能画 Gap。

### Phase 3：RTEC2 与完整身份（10～12 人日）

步骤：

1. 实现目标侧记录器、控制块、原子提交和完整句柄事件。
2. 接 FreeRTOS 任务/对象生命周期 hooks。
3. 实现 C++/Python RTEC2 解码器和 slot 缓存失同步恢复。
4. 快照/事件对账、代次、墓碑和名称生效区间进入模型。
5. 加入旧协议与新协议并存、能力协商和拒绝未知主版本。

退出条件：在同一地址连续创建/删除 10,000 次不串线；复位、重烧、断连均产生新 epoch；撕裂数据从不进入模型。

### Phase 4：独立轨道与 Findings（10～12 人日）

步骤：

1. reducer 生成任务、ISR、同步、堆和 Fault 投影。
2. UI 完成轨道分组、筛选、悬停、选择联动和缺口显示。
3. 迁移 mdkdebug 诊断规则，建立质量门控。
4. Findings 点击跳到证据范围；Fault 显示寄存器和前后事件。
5. 大事件量下实现窗口查询、LOD 聚合和轨道虚拟化。

退出条件：五类轨道可独立开关；Fault/Gap 不被 LOD 隐藏；每条 Finding 能定位证据或明确说明不可定位原因。

### Phase 5：录制 v3 与无人值守导入（7～9 人日）

步骤：

1. 实现分块、CRC、提交标记、索引和崩溃恢复。
2. 写入原始块、统一事件、快照、字典和 manifest。
3. 保持 v1/v2 回放兼容。
4. mdkdebug 输出采集包，Variable-Trace 导入并逐事件比对。

退出条件：进程在任意块写入点终止后可恢复到最后完整块；实时和回放的轨道、Findings、身份结果一致。

### Phase 6：部署、烧录与探针交接（7～9 人日）

步骤：

1. `trace_instrument` 支持 RTEC2 和 FreeRTOS hooks，生成 manifest。
2. 串起 deploy -> build -> flash -> verify -> handoff。
3. 实现租约、崩溃回收、失败回滚和审计日志。
4. 实现无人值守采集的时长/大小/触发停止条件。

退出条件：全新示例工程一条自动化流程可部署并采集；任一步失败均有可执行错误信息且不会误报成功；两个进程不能同时拥有探针。

### Phase 7：HIL、性能与发布（10～12 人日）

步骤：

1. 在至少两块 Cortex-M 板、两档主频、两种探针上执行硬件矩阵。
2. 注入高频切换、嵌套 ISR、Fault、对象复用、事件风暴、变频、复位和断连。
3. 测量关闭/轻量/完整三档开销。
4. 完成迁移说明、用户文档和故障排查手册。

退出条件：第 11 节全部 release gate 通过，或每个豁免均有负责人、影响和到期版本。

## 11. 测试矩阵与发布门槛

### 11.1 自动化测试

| 层 | 必测内容 |
|---|---|
| 编解码 | 每种事件、最大值、未知类型、截断、CRC 错、随机噪声、跨块帧 |
| 时间 | 32 位 DWT 回绕、变频、零 dt、长静默、epoch 边界、无主频 |
| 身份 | create/delete、同地址复用、缺 create、缺 delete、Gap 跨生命周期、命名变更 |
| 并发/原子性 | generation 奇偶、读取期间覆盖、ISR 嵌套拒绝、游标回滚 |
| 诊断 | 每条规则的正例、反例、NotApplicable 和 Unknown |
| 录制 | v1/v2 兼容、v3 往返、损坏块、崩溃恢复、超大文件索引 |
| UI | 轨道开关、选择联动、缩放、回放一致性、10 万/100 万事件性能 |
| 自动化 | 部署幂等、编译失败、烧录失败、hash 不符、租约冲突、无人值守恢复 |

必须维护一套由 C 目标编码器生成的二进制 golden corpus，Python 和 C++ 解码器在 CI 中逐字段比对。禁止三套实现各写只验证自己的测试数据。

### 11.2 HIL 场景

1. 两个任务按已知周期切换，核对区间和 CPU 比例。
2. 同一 TCB 地址反复复用，核对历史实体数和代次。
3. 两层嵌套 ISR，包含一个超过阈值的长 ISR。
4. semaphore/mutex/queue 的 wait、signal、timeout、acquire、release 和多等待者歧义。
5. 触发 UsageFault 和 BusFault，核对 class、CFSR、PC 以及 Fault 前事件。
6. 高频 alloc/free，包含未知大小 free 和地址复用。
7. 缩小环缓冲制造覆盖，拔掉探针制造断连，复位制造 epoch。
8. 运行时变频或模拟 clock sync，核对区间换算。

### 11.3 Release gate

- 身份正确性：已知生命周期下 0 次跨代串线；不确定情况 100% 显式标记。
- 解码正确性：golden corpus C/Python/C++ 逐字段完全一致。
- 缺口正确性：所有注入丢失均产生 Gap；0 次跨 Gap 计算持续时间。
- 原子性：撕裂/半写记录 0 次进入模型。
- 回放一致性：实时与回放的事件数、实体键、轨道区间和 Finding rule ID 一致。
- 性能：100 万事件文件在基准机器上可打开并交互；具体启动时间和帧率阈值在 Phase 0 以现有 UI 基线冻结。
- 目标开销初始门槛：轻量档静态 RAM 不超过 12 KiB、Flash 增量不超过 16 KiB；P99 单事件记录不超过 2 us、最长额外关中断不超过 2 us（以 100 MHz 等效归一化）。若目标类别不适用，必须在 Phase 0 改写门槛，不能在发布时临时豁免。
- 稳定性：8 小时无人值守采集无进程泄漏、死锁或静默停止；有丢失时计数与注入量可解释。
- 回归：Variable-Trace 既有 RTOS 测试全部通过；mdkdebug 现有 trace/component 测试全部通过。CMSIS-SVD、GNU make/size 等环境缺失项单独标记，不得混作功能回归。

## 12. 风险与明确决策

| 风险 | 决策/缓解 |
|---|---|
| MTF v1 无目标时间 | 只提供顺序、计数和无时长语义；不做主机时刻拟合 |
| SWD v2 短 ID 复用 | 标 legacy；完整身份只由 RTEC2 提供 |
| 两端解码器漂移 | 单一规范 + golden corpus + CI 逐字段比对 |
| 探针争用 | 显式租约和交接状态机；交互/无人值守模式互斥 |
| 事件风暴拖慢目标 | 分级编译、非阻塞策略、拒绝计数、压力基准 |
| 快照与事件矛盾 | 保留双证据并报告冲突，不静默选边或改写历史 |
| 录制格式继续膨胀 | 分块、版本化、未知块跳过和索引检查点 |
| UI 大数据卡顿 | 时间窗索引、轨道虚拟化、LOD；Fault/Gap 强制保留 |
| Fault 路径破坏现场 | Fault 宏必须为 handler 首句；最小无锁写入，寄存器缺失显式标记 |
| 自动插桩修改用户工程 | 幂等补丁、变更预览、构建验证和可恢复备份 |

以下决策在 Phase 0 不应再开放讨论：

1. Variable-Trace 是交互模型和 UI 的唯一事实入口；mdkdebug 不维护第二套任务历史 UI。
2. 高频数据不经 MCP JSON。
3. 短 ID 不是稳定身份；32 位原生句柄和生命周期是完整历史的最低条件。
4. Gap、epoch 和数据质量是模型的一部分，不是日志文本。
5. 无目标时间戳就不计算时间型指标。
6. 实时读取失败时不推进目标或主机游标。

## 13. 首个迭代的任务拆分

第一迭代只做 Phase 0 和 Phase 1，建议按以下顺序提交，降低大分支长期漂移：

1. 文档 PR：RTEC2、manifest、录制 v3、租约状态机和测试追踪表。
2. C++ PR：`RtosTraceTypes`、`IRtosTraceSource` 和纯逻辑测试，不接 UI。
3. C++ PR：`RtosTraceClock`、`RtosIdentityStore`、Gap/epoch 测试。
4. C++ PR：`RtosModel` 双历史接线，使用 fake source 验证，不接真实探针。
5. CI PR：建立跨仓库 golden corpus 生成与消费流程。

第一迭代评审通过后，才开始 MTF/SWD 适配。这样若协议字段或身份规则需要调整，修改范围仍局限在纯逻辑层，不会同时返工目标组件、录制和 UI。
