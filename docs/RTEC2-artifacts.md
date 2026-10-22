# RTEC2 产物：目标 manifest、录制 v3、采集包

- 文档状态：Phase 0 冻结项
- 编制日期：2026-10-22
- 适用基线：Variable-Trace 1.2.3 / 录制格式 v1、v2 只读兼容
- 上游文档：[mdkdebug Trace 接入任务视图：开发计划](mdkdebug-Trace-任务视图-开发计划.md)
- 线格式：[RTEC2 线格式](RTEC2-wire-format.md)

三件产物的关系：**目标 manifest** 说的是一次构件的身份；**录制 v3** 说的一次观察的完整证据；**采集包**是录制 v3 的一个受限子集，由 mdkdebug 在无人值守模式下写。三者共用同一个分块容器。

## 一、目标 manifest

路径：工程目录下 `.variable-trace/target-manifest.json`。写入者是 `trace_instrument`，读取者是 Variable-Trace 打开目标时的校验步骤。

```json
{
  "schema": 1,
  "mdkdebug_version": "0.2.17",
  "build_id": "2026-10-22T09:14:03Z/gcc-13.2/-O2",
  "elf_sha256": "…",
  "image_sha256": "…",
  "image_path": "build/firmware.hex",
  "rtos": "FreeRTOS",
  "rtos_version": "10.5.1",
  "trace_backend": "swd",
  "wire_protocol": "RTEC2",
  "wire_version": 2,
  "capabilities": ["rawHandle32", "lifecycleEvents", "eventSequence", "lossDetection"],
  "symbols": {
    "control": "mdk_trace_rtec2_blob",
    "dictionary": "mdk_trace_rtec2_names"
  },
  "clock": { "source": "DWT", "hz": 168000000 },
  "probe": { "serial": "…", "backend": "jlink" },
  "generated_at": "2026-10-22T09:14:05Z"
}
```

| 字段 | 必需 | 校验 |
|---|---|---|
| `schema` | 是 | 不等于 1 时拒绝读取并说明版本 |
| `elf_sha256` | 是 | 与当前打开的 ELF 不符 → **禁止名称解析**，产出 `manifest-elf-mismatch`（高优先级 Finding），仍然允许看地址 |
| `image_sha256` | 是 | 与探针读到的镜像区不符 → 提示"目标上的固件不是这份构建"，不阻止采集 |
| `wire_protocol` / `wire_version` | 是 | 主机不支持时拒绝启动，不降级猜测 |
| `capabilities` | 是 | 与目标控制块上报的能力取**交集**；manifest 声称而目标没有的，按目标为准并记一条 `capability-mismatch` |
| `symbols.control` | 是 | 找不到符号 → 该后端不可用 |
| `clock.hz` | 否 | 缺失时以目标上报的 `ClockSync` 为准 |

manifest 缺失时：允许用"仅快照"模式工作（无事件轨道），并明确显示"没有部署清单"。

## 二、录制 v3

### 2.1 文件头（32 字节）

```
offset size field
0      4    magic = 'V','T','R','C'
4      2    formatVersion = 3
6      2    headerBytes = 32
8      4    flags            bit0 = 只读副本（采集包导入）
12     4    reserved = 0
16     8    createdUnix      // 秒
24     8    reserved = 0
```

v1 与 v2 的 `magic` 是 `VTRECORD`（8 字节）后跟 version；读端先读 8 字节判断：等于 `VTRECORD` 走旧路径，等于 `VTRC` 走 v3。

### 2.2 块头（24 字节）

```
offset size field
0      4    magic = 'B','L','C','K'
4      2    type
6      2    version
8      4    payloadBytes
12     4    sequence        // 块序号，从 1 开始，连续
16     4    crc32           // payload 的 CRC-32/MPEG-2
20     4    commit          // 写完后最后写 0x434F4D54 ('COMT')
24     ...  payload
```

写入顺序固定为：**payload → crc → commit**。`commit` 不是 `'COMT'` 的块视为未完成，读取到此为止。

### 2.3 块类型

| type | 名称 | 必需 | payload |
|---|---|---|---|
| 1 | `SessionManifest` | 是 | JSON（UTF-8）：会话 id、工程、ELF 路径与 hash、wire 版本、capabilities、时钟、目标与探针信息、Variable-Trace 版本 |
| 2 | `RawTraceBlock` | 是 | 原始记录字节（环缓冲槽或流式帧），带 `sourceCursor` 与序号范围 |
| 3 | `NormalizedEvents` | 是 | 归一化事件，与 `RawTraceBlock` 用同一序号范围关联 |
| 4 | `SnapshotRound` | 是 | 每轮快照（沿用 v2 的轮次编码，整块存放一批轮次） |
| 5 | `IdentityDictionary` | 否 | 实体键 → 名称、别名与生效区间 |
| 6 | `SourceDiagnostics` | 否 | 能力、丢失计数、epoch 列表、重同步次数 |
| 7 | `RuleConfiguration` | 否 | Findings 规则阈值与规则版本 |
| 8 | `IndexCheckpoint` | 是 | 见 §2.4 |

`RawTraceBlock` 的第一个字段是 `sourceCursor`（8 字节），主机用它把"读到哪儿"与录制对齐；原始块与归一化事件块必须成对出现，序号范围一致，否则视为录制损坏。

### 2.4 索引检查点

每 64 个数据块追加一个 `IndexCheckpoint`，payload 是数组：

```
每条 24 字节：
  u32 blockSequence
  u64 payloadOffset     // 文件绝对偏移
  u32 payloadBytes
  u64 firstTicks        // 该块最早事件的目标 ticks（无则为 0）
  u64 lastTicks
```

打开时只读**最后一个**检查点即可定位任意时间窗；没有检查点时退回全文件扫描（与 v2 行为一致）。

### 2.5 崩溃恢复

读取规则：

1. 校验文件头；
2. 逐块读块头：`magic`、`sequence` 连续、`payloadBytes` 在文件内、`crc` 通过、`commit` 正确；
3. 任一条不成立 → 停止，**保留此前所有完整块**，并在 UI 说明"记录在块 N 处结束"。

写入规则：每块写完 `commit` 之后才写下一块；进程在任意点终止，最后一个完整块之前的数据都可读。不提供"修复"功能——把截断的记录伪装成完整的记录，比少读一点更糟。

### 2.6 与 v1 / v2 的关系

- v1 / v2 继续只读；新字段缺失时按能力降级（没有 ISR/Fault 轨道、没有原始块）。
- **不批量重写旧文件**：v2 → v3 的转换只在用户显式导出时进行，并写成新文件。

## 三、mdkdebug 采集包

采集包就是录制 v3，只是由 mdkdebug 写、字段受限。不再使用巨型 JSON 作为交换格式。

与录制 v3 的差异：

| 项 | 录制 v3 | 采集包 |
|---|---|---|
| 写入者 | Variable-Trace | mdkdebug |
| `SessionManifest` 多字段 | `tools`、`ui_theme` 等 | 必含 `mdkdebug_version`、部署 manifest 路径、烧录结果 |
| `NormalizedEvents` | 有 | 可缺（导入时由 Variable-Trace 从原始块生成） |
| `RuleConfiguration` | 有 | 可缺（用导入方的默认规则，并在 UI 标明） |
| 结尾 | 正常 `commit` | 追加 `SessionEnd` 字段：原因（时长/大小/触发/错误） |

必含内容（缺任一项即拒绝导入）：

1. `elf_sha256` 与 `image_sha256`；
2. 目标与探针信息、`wire_version`、`capabilities`；
3. `clock.hz` 或至少一条 `ClockSync`；
4. 原始 trace 块（含 `sourceCursor`、丢失计数、epoch 列表）；
5. 采集起止时间与结束原因；
6. mdkdebug 版本。

导入时逐事件比对：采集包里的归一化事件（若有）与 Variable-Trace 从原始块重算的结果必须一致；不一致时以原始块为准，并产出 `replay-mismatch`。

## 四、命名与位置

| 产物 | 默认位置 | 命名 |
|---|---|---|
| 目标 manifest | `<工程>/.variable-trace/target-manifest.json` | 固定 |
| 录制 | 用户在保存对话框选 | `<前缀>_<开始时刻>.vtrec` |
| 采集包 | mdkdebug 输出目录 | `<工程>_<开始时刻>.vtrace` |

两种文件扩展名不同，内容同构；Variable-Trace 的"打开"对话框同时接受 `.vtrec` 与 `.vtrace`。

## 五、冻结状态

1. v3 容器分块，块头 24 字节，`commit` 最后写。
2. 原始块与归一化事件块成对，序号范围一致。
3. 崩溃恢复只保留到最后一个完整块，不提供修复。
4. v1/v2 只读，不批量重写。
5. 采集包 = 受限的录制 v3，扩展名 `.vtrace`。
