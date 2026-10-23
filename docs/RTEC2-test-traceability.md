# RTEC2 需求到测试的追踪表

- 文档状态：Phase 0 交付物，随实现逐条勾选
- 编制日期：2026-10-22
- 上游文档：[mdkdebug Trace 接入任务视图：开发计划](mdkdebug-Trace-任务视图-开发计划.md)（§7.2、§11）
- 用法：每条用例有固定 ID，提交信息与测试名里引用这个 ID。状态只有四种：`待写` / `已写` / `已过` / `不适用（写明原因与到期版本）`

## 一、编解码（ENC）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| ENC-01 | 线格式 §3.1 | 每种记录类型各解码一次，字段逐一比对 golden vector | C++/Python | **已过**（`one-of-each`，两侧都过） |
| ENC-02 | §3 | 未知类型整条跳过，`unknownRecords` 加一，不报错 | C++/Python | **已过**（`unknown-type`） |
| ENC-03 | §2.1 | 截断帧被拒，重同步在 4096 字节内找到下一帧 | C++ | 待写 |
| ENC-04 | §2.1 | CRC 错帧被拒，不影响后续帧 | C++ | 待写 |
| ENC-05 | §2.1 | 随机噪声输入不崩溃、不越界 | C++ | **已过**（`NoiseNeitherCrashesNorInventsRecords`） |
| ENC-06 | §3 | 跨帧边界的记录被正确拼接（`isContinuation`） | C++ | 待写 |
| ENC-07 | §10 | 三个解码器对同一 golden 向量输出逐字段一致的 JSON | CI | **已过**（C 期望 / C++ gtest / `compare.py`，5 个向量） |

## 二、时间（TIME）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| TIME-01 | §5.2 | 32 位 ticks 回绕扩展正确（相邻值差 > 2^31 判定为回绕） | C++ | **已过**（`RtosTraceClockTest`，2026-10-22） |
| TIME-02 | §5.2 | `ClockSync` 后跨点区间标 `NotApplicable`，不按旧频率换算 | C++ | **已过**（`RtosTraceClockTest`，2026-10-22） |
| TIME-03 | §5.3 | 无 `targetTicks` 的记录不产生任何时长类结论 | C++ | 待写 |
| TIME-04 | §5.2 | 零 dt 连续出现报 `dt-zero-heavy` | C++ | 待写 |
| TIME-05 | §5.2 | 长静默（超过阈值无事件）报 `long-quiet-window` | C++ | 待写 |

## 三、身份（IDN）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| IDN-01 | §8 | `Create` → `Delete` → 同地址 `Create`：两次实体键不同（代次递增） | C++ | **已过**（`RtosIdentityStoreTest`） |
| IDN-02 | §8 | 同地址连续创建/删除 10,000 次不串线 | C++ | **已过**（`TenThousandLifetimesOfOneAddressNeverShareAKey`） |
| IDN-03 | §8 | 缺 `Create` 的先见事件建立 `generation=unknown`，后续快照只补显示名 | C++ | **已过**（store 层） |
| IDN-04 | §8 | `Gap` 跨过 `Create`/`Delete` 时相关实体降为 `UnknownAfterGap` | C++ | **部分**（store 打标已测；降级到规则层待接线） |
| IDN-05 | §8 | epoch 变化关闭全部实体与未完成区间 | C++ | **已过**（store 层 `clear`） |
| IDN-06 | §7 | 字典槽被复用后旧引用失效，名称按 hash 校验作废 | C++ | 待写 |
| IDN-07 | §4.3 | 同 handle 已打开时再 `Create` → `ImplicitClose` + 数据质量 Finding | C++ | **部分**（`implicitClose` 已测；Finding 待规则层） |

## 四、原子性与并发（ATM）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| ATM-01 | §4.3 | 写者写到一半被读：双哨兵检测出撕裂，记录不进模型 | C++ | **已过**（`RtosTracePipelineTest`：撕裂批不进历史） |
| ATM-02 | §4.4 | 整批中有一条失败，`tail` 不前移 | C++ | **已过**（`RtosTraceTypesTest` 的 `committedCursor`，2026-10-22） |
| ATM-03 | §4.3 | 读者落后 capacity 时按编译策略覆盖或丢弃，计数正确 | C++ | **已过**（`FallingMoreThanARingBehindIsCountedAsLost`） |
| ATM-04 | 目标侧 §4.4 | ISR 嵌套时不可重入的写入被拒并计入 `rejected` | C（目标） | 待写 |
| ATM-05 | §4.4 | 连续 3 次撕裂 → 请求新 epoch | C++ | **部分**（撕裂即整批放弃已测；计数与请求新 epoch 待接读取循环） |

## 五、诊断（DIAG）

每条规则需要四个用例：正例、反例、`NotApplicable`、`Unknown`。下表只列规则与门控条件，四个用例在实现时各建一条。

| ID 前缀 | 规则 | 门控 |
|---|---|---|
| DIAG-ND | `no-data` / `kernel-hooks-absent` / `no-sched` | 源已启动且能力探测完成 |
| DIAG-TB | `no-timeline` / `timebase-missing` | 事件存在 |
| DIAG-DT | `dt-frozen` / `dt-zero-heavy` / `long-quiet-window` | 同 epoch 至少两个可信时间点 |
| DIAG-LOSS | `events-lost` / `segments-restarted` | 始终评估，不受其他质量门控 |
| DIAG-ISR | `isr-unpaired` / `isr-long` | 无跨 Gap、时间基可信 |
| DIAG-FAULT | `fault-present` | 只需有效 Fault 帧 |
| DIAG-SYNC | `sync-wait-unpaired` / `sync-wait-ambiguous` | 身份未复用、区间无 Gap |
| DIAG-VT | `priority-inheritance` / `suspected-deadlock` / `ready-starvation` / `unbounded-wait` | 快照一致性满足现有规则 |
| DIAG-NEW | `snapshot-event-divergence` / `identity-ambiguous` / `manifest-elf-mismatch` / `capability-mismatch` | 对账或校验出现冲突 |

执行顺序固定为：能力 → 数据质量 → 身份/时间归一化 → 语义规则；上游不满足则下游 `Unknown`（DIAG-ORDER-01 专门验证这条）。

## 五之二、对账（RECN）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| RECN-01 | 计划 §5.3 | 快照看得到、事件从未提及的任务被报出来 | C++ | **已过**（`RtosTraceReconcilerTest`） |
| RECN-02 | 计划 §5.3 | 事件里开放的生命周期在快照缺席时**不当作删除** | C++ | **已过** |
| RECN-03 | 计划 §5.3 | 两侧名字不同只报告，不改写任何一侧 | C++ | **已过** |
| RECN-04 | 计划 §5.3 | 收尾的生命周期缺席是正常，不产生结论 | C++ | **已过** |
| RECN-05 | 计划 §5.3 | 列表按地址排序，轮次之间不抖动 | C++ | **已过** |

## 五之三、目标侧写者（WRT）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| WRT-01 | 线格式 §4.3 | 写进去的每条记录，读者读回来字段一致 | C/C++ | **已过**（`Rtec2WriterTest`） |
| WRT-02 | §3.4 | 12 字节 Fault 完整往返 | C/C++ | **已过** |
| WRT-03 | §3 | payload 超过 12 字节被拒并计数，head 不动 | C/C++ | **已过** |
| WRT-04 | §4.3 | 环满按策略覆盖，`overwritten` 计数，读者报 `lost` | C/C++ | **已过** |
| WRT-05 | §4.3 | 环满按策略丢弃，`rejected` 计数 | C/C++ | **已过** |
| WRT-06 | §8 | 新 epoch 时 head 归 1，读者判 `restarted` | C/C++ | **已过** |
| WRT-07 | §4.4 | 槽写到一半被读：双哨兵拦下，整批放弃 | C/C++ | **已过** |
| WRT-08 | §7 | 字典接受短名、拒绝超长名；名字哈希稳定 | C/C++ | **已过** |
| WRT-09 | §4.3 | 写入顺序（seqCheck → 内容 → tailCheck → seq → head） | C/C++ | **部分**（顺序写在代码里并有注释；没有专门制造乱序的用例） |

## 六、录制（REC）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| REC-01 | 产物 §2.2 | v3 往返：写 100 块，读回逐字节一致 | C++ | **已过**（10 条用例，含 8 种块类型） |
| REC-02 | 产物 §2.5 | 在任意块写入点截断，可恢复到最后一个完整块 | C++ | **已过**（未提交块 / 短 payload / 坏 CRC 三种） |
| REC-03 | 产物 §2.6 | v1/v2 文件仍可打开；缺失块按能力降级 | C++ | **已过**（既有 21 条 v1/v2 用例保持绿；v3 容器带轮次后仍能读回同一轮） |
| REC-04 | 产物 §2.3 | 原始块与归一化事件块序号范围不一致 → 判为损坏 | C++ | 待写 |
| REC-05 | 产物 §2.4 | 有索引检查点时只读检查点即可定位时间窗 | C++ | **部分**（每 64 块写一个、条目指向真实偏移已测；按时间窗跳读待界面） |
| REC-06 | 产物 §3 | 采集包与录制 v3 同构；缺必含字段时拒绝导入 | C++ | 待写 |

## 七、界面（UI）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| UI-01 | 计划 §6.3 | 五类轨道可独立开关；Fault 与 Gap 不被 LOD 隐藏 | 手动 | **部分**（`Tracks` 页签画五条带子，按列聚合；空数据也画框。**未在真机看到**：没有地方调用 `attachTraceSource`，见下表） |
| UI-02 | 计划 §6.3 | 缩放、平移后轨道与变量曲线时间轴对齐 | 手动 | 待写 |
| UI-03 | 计划 §7.1 | `NotApplicable` 与 `Unknown` 不显示成"没有问题" | 手动 | 待写 |
| UI-04 | 计划 §6.3 | 10 万事件流畅；100 万事件可打开并交互 | 基准 | **部分**（`RtosTraceWindow` 的列聚合已就绪，11 条用例；界面尚未接，真机基准未做） |
| UI-05 | 计划 §6.3 | Finding 点击定位到证据范围；无法定位时说明原因 | 手动 | 待写 |

## 八、自动化与部署（AUTO）

| ID | 需求来源 | 用例 | 层 | 状态 |
|---|---|---|---|---|
| AUTO-01 | 计划 §9.2 | `trace_instrument` 重复执行不产生无关 diff | Python | 待写 |
| AUTO-02 | 计划 §9.2 | 编译失败 / 烧录失败 / hash 不符时流程中止且不报成功 | Python | 待写 |
| AUTO-03 | 租约 §3 | 租约冲突被拒；过期租约回收后 `epoch` 递增 | C++ | 待写 |
| AUTO-04 | 租约 §4 | 交接超过 30 秒被强制回收并记 `handoff-timeout` | C++ | 待写 |
| AUTO-05 | 租约 §5 | 探针被拔 → 报错退出，采集不标成功 | 手动 | 待写 |

## 九、HIL 场景（HIL）

| ID | 场景 | 板卡 | 状态 |
|---|---|---|---|
| HIL-01 | 两任务已知周期切换，核对区间与 CPU 比例 | M4/M7 各一 | 待写 |
| HIL-02 | 同一 TCB 地址反复复用，核对实体数与代次 | M4 | 待写 |
| HIL-03 | 两层嵌套 ISR，含一个超阈值长 ISR | M4 | 待写 |
| HIL-04 | semaphore/mutex/queue 的 wait/signal/timeout/acquire/release 与多等待者 | M4 | 待写 |
| HIL-05 | UsageFault 与 BusFault，核对 class/CFSR/PC 与 Fault 前事件 | M4 | 待写 |
| HIL-06 | 高频 alloc/free，含未知大小 free 与地址复用 | M7 | 待写 |
| HIL-07 | 缩小环缓冲制造覆盖；拔探针；复位制造 epoch | M4 | 待写 |
| HIL-08 | 运行时变频或模拟 clock sync，核对区间换算 | M7 | 待写 |

## 十、发布门槛对应

| 门槛（计划 §11.3） | 由哪些用例证明 |
|---|---|
| 身份 0 次跨代串线 | IDN-01、IDN-02、IDN-04、HIL-02 |
| 三方解码逐字段一致 | ENC-07 |
| 所有注入丢失都产生 Gap，0 次跨 Gap 计算时长 | DIAG-LOSS、TIME-03、HIL-07 |
| 撕裂/半写 0 次进模型 | ATM-01、ATM-02、REC-02 |
| 实时与回放一致 | REC-01、REC-06 |
| 100 万事件可交互 | UI-04 |
| 目标开销（12 KiB RAM / 16 KiB Flash / P99 2 µs） | 待 Phase 0 基准冻结后补用例 |
| 8 小时无人值守稳定 | AUTO-05、HIL-07 |

## 十一、当前进度

| 日期 | 完成 | 提交 |
|---|---|---|
| 2026-10-22 | Phase 0 文档四份（线格式、产物、租约、本表） | `04c313a` |
| 2026-10-22 | `RtosTraceTypes` + `IRtosTraceSource`；`TIME-01`、`TIME-02`、`ATM-02` 已过 | `61af9e9`、`b48314a` |
| 2026-10-22 | `RtosIdentityStore`；`IDN-01`、`IDN-03`、`IDN-05` 已过，`IDN-04`、`IDN-07` 部分 | `c6d5926` |
| 2026-10-22 | `RtosTracePipeline` + `RtosModel` 双历史；`ATM-01` 已过；**source → model → query 通路打通** | `ca4b211` |
| 2026-10-22 | golden corpus（`gen.c` 生成、`Rtec2Decoder` 与 `decode.py` 消费、`compare.py` 比对）；ENC-01/02/05/07 已过 | `35b01d0` |
| 2026-10-22 | Phase 2 适配器：`MtfTraceAdapter` / `SwdTraceAdapter`，能力缺口名单；`IDN-02` 已过 | `0cd82b4` |
| 2026-10-22 | Phase 3 主机侧：`Rtec2RingReader`（双哨兵、整批放弃、覆盖计数）、`RtosTraceReconciler`（三条对账） | `141f936` |
| 2026-10-22 | Phase 3 目标侧参考实现：`example/rtec2_port/`（C 写、C++ 读闭环）；WRT-01..08 已过 | `bce6222` |
| 2026-10-22 | 录制 v3 容器：`RtosRecordingV3`（分块、CRC-32/MPEG-2、commit 标记、崩溃只恢复到最后一个完整块、索引检查点）；REC-01/02 已过、REC-05 部分 | `5fe0779` |
| 2026-10-22 | Phase 4 投影层与 `Tracks` 页签：`RtosTraceReducer`（ISR 栈式配对、wait 按任务+对象配对、Fault 寄存器按 flags 取、堆净值与未知大小分开）；UI-01 部分 | `d559ddd` |
| 2026-10-22 | Phase 4 窗口查询：`RtosTraceWindow`（列聚合、跨边界计入、无时间条目不落轴、Fault/Gap 永不被聚合掩盖）；UI-04 部分 | `65b7ad8` |
| 2026-10-22 | 带状渲染：`drawTrackBand`（每像素列一标记、三档密疏、Fault 红 / Gap 橙、右侧计数与未完成、无时间计数） | `7e9f9b1` |
| 2026-10-23 | v3 能装轮次：`encodeRound`/`decodeRound` 从 v2 抽出共用，`packRounds`/`unpackRounds`，v2 文件转 v3 逐轮一致；REC-03 已过 | 本次提交 |

**带状渲染为什么还没被看见**：`Tracks` 页签的数据来自 `model.trace()`，而工程里**没有任何地方调用
`RtosModel::attachTraceSource`**——事件源只在测试里挂过。所以界面上的 `Tracks` 只会显示"没有接事件源"。
要让它有东西可画，下一步是让采集线程挂上 `SwdTraceAdapter`/`MtfTraceAdapter`（Phase 5 第 2 步），
或者等 mdkdebug 的 RTEC2 源。

第一迭代的五步做完；Phase 2 的适配器层已落地（`MtfTraceAdapter`、`SwdTraceAdapter`）。
三端钉在同一批字节上（`tools/trace-golden/`）。

### Phase 2 完成情况与两条边界

已完成：

- 两种**本仓库已有的载体**包成 `IRtosTraceSource`：SWO/ITM 字节（经 `RtosItmDecoder`
  分帧）与内存环记录（`RtosEventRing::Record`）。二者都产 `TaskSwitchIn` 事件，
  `timeUs` 作为 1 MHz 的目标时间。
- `lost` 变成 `Gap` 事件而不是更长的一段运行；环重新开始时发 `EpochBegin` 并递增 epoch，
  而不是把两次会话接在一根轴上。
- 能力位如实上报：MTF 没有丢失检测与生命周期，SWD 有丢失检测；两者都没有序列号。
  `RtosTracePipeline::missingCapabilities()` 给出**名字**而不是位数。

边界（未做，原因在右）：

| 未做 | 原因 |
|---|---|
| mdkdebug 的 MTF v1 / SWD v2 **线格式**解码 | 这两份规格与 golden vector 在 mdkdebug 仓库，本仓库没有；现在的适配器包的是本仓库自己的两种载体，接口与能力位已就位，拿到规格后只替换解码入口 |
| `ViewerDataHandler` 切换事件路径 | 现有 `RtosEventRing` → `eventHistory` 在生产使用，替换需要真机验证；接线点已留（`RtosModel::attachTraceSource`） |
| 与 Python 解码器交叉比对 MTF/SWD | 同上：需要 mdkdebug 产出的 golden 向量 |

### Phase 3 完成情况

已完成（主机侧）：

- `Rtec2RingReader`：控制块解析（magic、wire 版本、slotBytes、capacity 必须是 2 的幂），
  逐槽双哨兵校验，**一个坏槽放弃整批**，落后超过一圈计 `lost`，head 倒退判 `restarted`。
- `RtosTraceReconciler`：快照与事件双向对账三条（只在快照、只在事件、名字不同），
  只报告不改写；列表按地址排序不抖动。

**发现并改了文档的一处错**：线格式初稿写槽 48 字节、payload 8 字节，算下来最长记录
41 字节加两个哨兵要 49——连一条 Fault 都放不下。已把槽改成 56 字节，并注明改的是
文档而不是把 payload 截短。

目标侧（参考实现已完成，`example/rtec2_port/`）：

- 控制块与槽写入、五步提交顺序、奇偶 generation、拒绝与覆盖计数、epoch 归零、
  名字字典（FNV-1a + 16 字节截断拒绝）。
- **闭环验证**：同一份 C 代码写进内存，`Rtec2RingReader` 读回来，8 条用例覆盖
  往返、12 字节 Fault、超长拒绝、覆盖/丢弃、新 epoch、撕裂、字典。不需要板子。

仍未做（需要 mdkdebug 仓库与板子）：

- FreeRTOS 的 create/delete/switch/block/wake hook 真接（README 给了宏映射，未编译验证）；
- 复位/重烧/断连在真机上产生新 epoch；
- 真机下的单事件开销（P99 ≤ 2 µs @100 MHz）与 8 小时稳定性。
