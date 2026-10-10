# Variable-Trace 测试报告（依据《Variable-Trace 测试清单》）

- 测试日期：2026-10-10
- 被测程序：`build/Variable-Trace.exe`（v1.1.0）；通路 A/B 跑的是 09:29 那一版，通路 D 跑的是 15:10 修完跟踪缺陷后的版本
- 目标板：STM32F103C8（100ask DshanMCU-F103），J-Link S/N 69730307
- 目标固件：`<文档目录>/AICodeProject/stm32103/0703_uart_circle_buffer/`，符号 `MDK-ARM/0501_led/0501_led.axf`（通路 A/B 跑的是 10:46 那一版；通路 D 改用 15:29 重编的版本，改动见「通路 D」一节）
- 驱动方式：全程走 MCP（`http://127.0.0.1:7807/mcp`），脚本在本机 `analysis/tmp/`（`vtdrv.py` 是接口封装，`t_p*.py` 是各阶段）——该目录未纳入版本库，仓库里没有这些脚本
- 变量写回需要 `global.json` 的 `api.writesEnabled`：测试期间打开，**测试后已还原为 `false`**

> 路径里的 `<文档目录>` 是占位符，指本机放工程的文档目录。

## 一页结论

- 通路 A（变量视图）**除一条外全部通过**，含最难的结构体成员偏移、数组步长、有符号/无符号宽度、李萨如倍频比、玫瑰花瓣数。
- 通路 D **全部通过**。它先后暴露 **3 个应用缺陷 + 1 个目标侧采样率问题**，全部已修：跟踪一启动就 abort 的断言、目标侧 ITM 未使能、**SWO 采集从未被启动**，以及 CH1/CH2 因发送率太低而混叠。修后三条判据齐活（CH0 脉宽随 `vtraceDemoLoops` 9.72 倍变化；CH1/CH2 各 500.2 Hz、每周期 41 点、相位跨 3128 个周期只散 1 µs；采样率 20 508 Hz 零丢帧）。
- 通路 C 因未接 USB-TTL 未跑。
- 另有 **1 个目标侧固件缺陷（F1，未修）**、**3 个 MCP 缺陷**、**3 起与本轮无关的崩溃或挂死**。

## 通路 A：变量视图（通过项）

| 测什么 | 实测 | 结论 |
| --- | --- | --- |
| 自检 `vtraceSysclkHz` | 72000 | 通过 |
| 自检 `vtraceRecorderSize` | 8324 | 通过 |
| 自检 `vtraceRecorderVersion` / `vtraceSerialDriverVersion` | 8 / 1 | 通过 |
| 自检 `vtraceStepUs` | 8（< 50） | 通过 |
| 结构体 `vtraceStruct` 8 成员 | 12 / 155 / -85 / 105 / 55234 / -5234 / -2141248 / 37.21，逐项精确命中 | 通过 |
| 数组 `waveTable[0..7]` | 0.125 / -0.25 / 0.5 / -0.75 / 1.0 / -1.25 / 1.5 / -1.75 | 通过 |
| 类型与符号（同一采样瞬间交叉核对） | `u8=phase>>8`、`u16=phase`、`u32=phase×65537`、`f32=sin(...)` 全部吻合；有符号组统计到负值、无符号组没有 | 通过 |
| `sinTest` / `cosTest` | 实测 0.5000 Hz、幅值 ±1、互相正交（sin²+cos²≈1，偏差来自降采样插值） | 通过 |
| 李萨如 1:4 / 1:7 / 1:15 | 实测倍频比 3.95 / 6.95 / 14.85 | 通过 |
| 玫瑰线 8 瓣 | `petalsNumber=8` 实测 8 瓣 | 通过 |
| 变量写回 `traceBaseFreqHz` | 写 0.5 / 1.0 / 0.25，`sinTest` 实测 0.5002 / 1.0001 / 0.2502 Hz | 通过 |
| 变量写回 `petalsNumber` | 读回 6 / 10 / 12 全部正确；写入确实落到目标（读回即目标内存值） | 通过（花瓣数变化未能自动判定，见下） |

**未能自动判定的一条**：改 `petalsNumber` 后花瓣数是否跟着变。原因是 MCP 的 `last_s` 对 XY 图失效（见问题 3），测量窗口混着写入前的旧数据；`petalsNumber` 本身读回正确，且 `12` 那一次实测到 12 瓣，所以写入链路是通的，是**测量手段**受限。

**两个必须先知道的用法**：

1. **工具只对「已加入绘图区的变量」采样**。只导入不画图的变量 `is_sampled=false`，`get_curve_data` 直接回「not added to a plot」。清单第 1 节的自检数字也得先挂进一个图表图才读得到。
2. **XY 图的 X 轴没有 MCP 工具**。X 轴存在工程文件的 `xAxisVariable` 字段里，本次是「先建图 → 存盘 → 改工程文件 → 重新打开」接上的。

## 确认的问题

### 目标侧固件（不是工具的问题）

| 编号 | 现象 | 根因 | 改法 |
| --- | --- | --- | --- |
| F1 | `vtraceFastFreqHz` 恒为 0（期望 ≈499） | `vtrace_port.c:400` 的 `(uint32_t)fastPhaseStep * 1000000000u` 在 32 位里溢出（1638×10⁹≈1.64×10¹² 远超 4.29×10⁹），商为 0。`vtrace_port.h:69` 的注释与算式同错 | 改成 `fastPhaseStep * (1000000000u / ____RECORDER_TIMEBASE_NS) / 65536u`，或中间用 `uint64_t` |

用 `JLink.exe` 直接读 `0x200000D0` 也是 0、`0x2000007C`（`fastPhaseStep`）是 1638，确认工具读数正确。

### 工具 / MCP 层

| 编号 | 现象 | 位置 | 影响 |
| --- | --- | --- | --- |
| M1 | `set_active_group` 调用成功但不生效 | `GuiPlotsTree.hpp:469-472` 的 `drawGroupNode()` 每帧执行 `if (selectedGroup == name) setActiveGroup(name)`，GUI 侧选中项始终是原组，MCP 设完下一帧被覆盖 | 自动化无法切到记录器组；返回值假装成功，排查费时。绕开：让目标组成为唯一组 |
| M2 | `last_s` 对 XY 图无意义 | XY 图的 `times` 是横轴变量的值（`DataTools.cpp:66` 取 `plot->getXAxisSeries()`），按秒裁剪等于拿横轴值当秒数比 | `last_s=8` 与不传返回同一批 10000 点，测不出「写入后变化」 |
| M3 | 只作 X 轴的变量报错自相矛盾 | `collectSamples` 只查 `seriesMap`（`DataTools.cpp:47`），而 `isVariableSampled` 认 `getIsCurrentlySampled()` | `get_variable_value` 说 `is_sampled:true`，`get_curve_data` / `get_variable_stats` 只回一句光秃秃的「No samples collected yet.」 |
| M4 | `detect_recorder` 紧跟 `start_acquisition` 有竞态 | `get_acquisition_state` 已报 RUN，但 J-Link 链路尚未开好 | 首次报 `Reading 24 bytes at 0x200000D8 failed: `（错误串是空的），隔几秒重试即成功 |

记录器本身的工作是正常的：`detect_recorder` 返回 `version 8 / revision 3 / max_variables 6 / max_buf_size 8192 / timestep_ns 50000 / float_support true`，与清单一致。

### 崩溃与挂死（各复现一次，需要单独定性）

| 编号 | 触发 | 现象 |
| --- | --- | --- |
| C1 | RUN 状态下 `remove_group` 删掉活动采样组 | 后续 MCP 调用连接被重置、进程退出、**日志一行没有** |
| C2 | `set_sampling{max_points:1000}` → `stop` → `start` | 窗口无响应（`IsHungAppWindow`=1），进程内存涨到 **8.4 GB**（新实例基线 191 MB），日志停在挂起前 |
| C3 | `set_recorder_mode("run")` 之后 `get_recorder_settings` | 先回 `Internal error: std::bad_alloc`，再取一次进程**直接退出**；记录器 `running` 始终 false |

C1 与 C2 不像同一个点，但都属于「采集线程 / 界面 / 模型」之间的生命周期或并发问题（`MEMORY.md` 里已列的那几处未加锁的隐患是同类）。C3 与 `bad_alloc` 值得单独查。

## 通路 D：SWO 跟踪（三处缺陷已修，波形到手）

### 已修：跟踪一启动就 abort

现象是进程直接终止、CSV 建出来但永远是 0 字节。gdb 抓到的断言：

```
Assertion failed: (GImPlot->CurrentPlot != NULL && !GImPlot->CurrentPlot->SetupLocked)
  "Setup needs to be called after BeginPlot and before any setup locking functions (e.g. PlotX)!"
  third_party/implot/implot.cpp:2084
#40 ImPlot::SetupAxis (idx=3, label=NULL, flags=9)
#41 Gui::drawPlotCurveSwo (...)  src/Gui/GuiSwoPlots.cpp:67
#42 Gui::drawPlotsSwo ()         src/Gui/GuiSwoPlots.cpp:20
```

在 `GuiSwoPlots.cpp:67` 断点观察 `ImPlot::GImPlot->CurrentPlot->SetupLocked`，得到「停止时连续多帧 `locked=0`，启动后**第一帧** `locked=1`」，链条随即清楚：

- 第 59 行 `plotViewWasAskedForByHand()` 的第一句是 `ImPlot::IsPlotHovered()`；
- `IsPlotHovered()` 会调用 `SetupLock()`，把该图本帧的 setup 锁死；
- 而 `SetupAxis(ImAxis_Y1, …)` 排在第 67/71 行，**在它后面** → 断言。

`git blame` 显示第 41–50、59–63 行出自本地提交 `0763386`（上游基线 `cef8f67` 在该分支什么都不做），属本地引入的回归；查看器侧同样的手势判断（`GuiPlots.cpp:335`）在所有 Setup 调用**之后**，所以查看器没受影响。

修法：把 Y 轴 `SetupAxis` 提到最前，`SetupAxisLimits` 紧随其后，**手势判断挪到所有 Setup 之后**（顺带消掉了「停止时 Y1 被 SetupAxis 两次」）。另外 `drawPlotsSwo()` 在**零可见通道**时会把 0 行交给 `BeginSubplots`（另一个断言 `rows > 0`，implot.cpp:3266），已夹到至少 1。

### 已修：跟踪期间目标侧 ITM 未使能

读目标寄存器发现 `ITM_TCR = 0`、`ITM_TER = 0`（`TPIU_ACPR` 倒是被应用正确设成了 11）。也就是说紧跟 `JLINKARM_SWO_EnableTarget` 之后，**ITM 从来没被打开**：固件 `vtrace_itm_init()` 写在启动时，被调试器连接清掉，之后无人再开。

`JlinkTraceProbe::startTrace` 里补上这三笔写。**注意 `JLINKARM_WriteMemEx` 对 SCS（`0xE0000xxx`）无效**——返回成功却毫无效果，必须走 `JLINKARM_WriteU32`（SEGGER 的 `jlink.h` 是子集且授权不许改，因此在本文件里单独声明）。写入后**会话期间回读 `ITM_TCR = 0x00010001`**，确认生效。

### 第三个缺陷：SWO 采集从没被启动过（已修，波形随即到手）

ITM 打开之后仍然是 `No trace registered for 2s!`。对照 **ST-Link 的那份实现**（`StlinkTraceProbe.cpp`，同一套接口的另一实现）逐项比对，发现两处：

1. **`JLINKARM_SWO_Control(JLINKARM_SWO_CMD_START, …)` 从来没有被调用过。** 全项目只有 STOP 与 FLUSH 两处调用。`JLINKARM_SWO_EnableTarget` 只是把目标侧的 TPIU 配好，**采集本身要单独一条命令才开始**；不调用它，`JLINKARM_SWO_Read` 永远没有东西可交。SDK 的 `JLINKARM_SWO_START_INFO`（`SizeofStruct` / `Interface` / `Speed`）不在随包的头文件子集里，按 pylink 的定义补齐。
2. ITM_TCR 少了**时间戳使能位**（bit 1）。参考实现是 `TRACE_BUS_ID_1 | TS_ENA | ITM_ENA`，并且还写了 `ITM_TPR = 0x000F`。应用的时间轴是靠 ITM 时间戳拼的（SDK 侧也开了 `TSEnable=1`），没有这一位帧就没有位置。

补上这两处之后，**SWO 立刻通了**。

### 通路 D 实测结果（全部通过）

| 判据 | 实测 | 结论 |
| --- | --- | --- |
| CH0 数字通道交替 0xAA / 0xBB | 列里只有 0 与 1，映射与 `TraceDataHandler.cpp:100` 一致 | 通过 |
| 高电平段宽度 = 标记之间那段代码的耗时 | 中位 **184 µs** | 通过 |
| 改 `vtraceDemoLoops` 会改变高电平宽度 | 800 → **184 µs**；8000 → **1788 µs**，比值 **9.72**（写入 10 倍） | 通过 |
| CH1 是 499.9 Hz 规则波形 | **500.16 Hz**，每周期 41 个样本；单周期内取值 0.05 / 0.20 / 0.35 / 0.50 / 0.63 / 0.74 / 0.84 / 0.91 / 0.96 / 0.99 / 1.00 —— 正弦的 10° 步进 | 通过 |
| CH2 是 499.9 Hz 规则波形 | **500.23 Hz**，u16 值域 0…65535，上升段每样本 +3276 | 通过 |
| 两路相位关系固定 | 相位偏移 p10 / 中位 / p90 = **1549.4 / 1549.4 / 1550.4 µs**（周期 1999.4 µs）；前半程与后半程中位相同，**漂移 0.0 µs** | 通过 |
| 采样率与丢帧 | **20 508 Hz**（配置 20 000 Hz），**零丢帧** | 通过 |

### 目标侧改动：`vtrace_itm_send()` 移进 20 kHz 采样中断

改之前 CH1/CH2 是假波形：主循环只有 ~500 Hz，拿它去采 499.9 Hz 的正弦只会得到拍频（实测第一次 0.12 Hz 慢扫、第二次 3.9 Hz，都精确等于「发送率 − 499.9」）。改动在 `Lib/vtrace_port.c`：

1. `vtrace_itm_send()` 由主循环移进 `vtrace_recorder_tick()`（20 kHz 采样节拍），放在 `vtraceStepCycles` 那次测量**之后** —— 于是 `vtraceStepUs` / `vtraceStepCycles` 量的仍然是「变量更新」本身，含义不变（实测仍是 **8 µs / 595 周期**），类型测试那条「< 50 µs」不用重新定标。
2. 发送前先探 STIM 寄存器的 FIFO 就绪位（bit 0），不就绪就整拍丢掉，**不在中断里停等**；两路要么一起发要么一起不发，否则主机侧相位对不上。
3. CH0 的 `vtrace_itm_enter/exit` 留在主循环包住那段演示负载，标记语义不变。
4. 该函数定义在后面，前面补了一行前置声明。

**丢帧率可核且不需要新增变量**：数 CSV 里 CH1 的样本率，与 20 000 Hz 一比即知——本次 20 508 Hz，零丢帧。

编译烧录：Keil MDK-ARM Plus 5.36 / Armcc V5.06u7，`UV4.exe -j0 -b 0501_led.uvprojx` → **0 error 0 warning**；`JLink.exe loadfile` 写入 14 336 字节并校验通过。

### 附：改动清单

| 文件 | 改动 |
| --- | --- |
| `src/Gui/GuiSwoPlots.cpp` | Setup 调用前移／手势判断后移；`BeginSubplots` 行数夹到 ≥1 |
| `src/TraceReader/JlinkTraceProbe.cpp` | 声明并调用 `JLINKARM_WriteU32`；SWO 使能后打开 ITM（含时间戳位与 TPR）；补 `JLINKARM_SWO_Control(…_START, …)` |
| `Lib/vtrace_port.c`（目标侧） | `vtrace_itm_send()` 移进 20 kHz 采样中断，带 FIFO 就绪位判断 |

回归：`build_test/test/Variable-Trace_test.exe` —— **476 项全部通过**；通路 A 抽查（`vtraceSysclkHz` 72000、`vtraceStepUs` 8、`vtraceStepCycles` 595、结构体与数组的魔数、记录器/串口版本号）全部照旧。

## 未完成与阻塞

| 通路 | 状态 | 说明 |
| --- | --- | --- |
| A 变量视图 | 通过（除一条） | 花瓣数随写入变化被 M2 挡住 |
| D SWO 跟踪 | **全部通过** | 三处应用缺陷 + 目标侧发送率都已处理 |
| C 串口探针 | 未跑 | 未接 USB-TTL，用户确认跳过 |

## 尚未处理的一项

**F1（目标侧 `vtraceFastFreqHz` 恒为 0）** 还没改。`Lib/vtrace_port.c` 里 `(uint32_t)fastPhaseStep * 1000000000u` 在 32 位下溢出，改成先除后乘即可（推算见前文）。这一项要重编烧录，等你点头再做。
