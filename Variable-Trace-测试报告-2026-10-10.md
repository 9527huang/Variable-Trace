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
- **换 ST-Link V2 复测（PB3 未接）**：通路 A 全部通过；通路 B 卡在记录器探测（界面线程卡死）；通路 C/D 依然测不了。
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
| `src/Gui/GuiPlotsTree.hpp` | 活动组跟点击走，不再每帧用「当前选中」覆盖（`set_active_group` 因此失效） |
| `Lib/vtrace_port.c`（目标侧） | `vtrace_itm_send()` 移进 20 kHz 采样中断，带 FIFO 就绪位判断 |

回归：`build_test/test/Variable-Trace_test.exe` —— **476 项全部通过**；通路 A 抽查（`vtraceSysclkHz` 72000、`vtraceStepUs` 8、`vtraceStepCycles` 595、结构体与数组的魔数、记录器/串口版本号）全部照旧。

## 未完成与阻塞

| 通路 | 状态 | 说明 |
| --- | --- | --- |
| A 变量视图 | 通过（除一条） | 花瓣数随写入变化被 M2 挡住；换 ST-Link 复测仍通过 |
| B 记录器 | 未跑通 | 换 ST-Link 后卡在 `detect_recorder`（界面线程卡死） |
| D SWO 跟踪 | **全部通过** | 三处应用缺陷 + 目标侧发送率都已处理；仅在 J-Link 下可测 |
| C 串口探针 | 未跑 | 未接 USB-TTL，用户确认跳过 |

## 尚未处理的一项

**F1（目标侧 `vtraceFastFreqHz` 恒为 0）** 还没改。`Lib/vtrace_port.c` 里 `(uint32_t)fastPhaseStep * 1000000000u` 在 32 位下溢出，改成先除后乘即可（推算见前文）。这一项要重编烧录，等你点头再做。

---

# 补测：换成 ST-Link V2（PB3 未接）

探针由 J-Link 换成 **ST-Link V2**（克隆板，固件 `V2J38S7 API v2`，VID:PID `0483:3748`，目标电压 3.22 V，SWD 950 kHz）。PB3 的 SWO 线没接、也没有 USB-TTL，所以**通路 D 与通路 C 都测不了**，本次只补通路 A 与通路 B。

## 通路 A：全部通过

| 测什么 | 实测 | 结论 |
| --- | --- | --- |
| 自检数字 | `vtraceSysclkHz` 72000、`vtraceRecorderSize` 8324、记录器/串口版本 8 / 1、`vtraceStepUs` 8（< 50） | 通过 |
| 结构体成员 | `vtraceStruct` 的 a/b/c/d/f/g/h/j = 12 / 155 / −85 / 105 / 55234 / −5234 / −2141248 / 37.21 | 通过 |
| 数组元素 | `waveTable[0]` 0.125、`waveTable[7]` −1.75 | 通过 |
| 类型宽度互校 | 同一时刻读 `u16Test` = 55899，由它反推 u8 = 218、i8 = −38、u32 = 3663452763、i32 = −631514533 —— 与实际单独读到的四个值**逐个相等** | 通过 |
| 快变量确实在 20 kHz 采 | `fastRamp` 2.03 s 内增 40425，合 **19933 /s** | 通过 |

## 通路 B：没跑通，卡在记录器探测

记录器要求两个条件，都摸清了但没走完：

1. **符号要真正导入，地址才会被解析。** 手工在工程文件里写一条变量（哪怕带 `shouldUpdateFromElf`），加载后地址仍是占位值 `0x20000000`（符号真实地址是 `0x200000D8`），`detect_recorder` 报到「0 variables、buffer 134227225 elements」这种自相矛盾的结果；改用 `add_variable` 导入后这条报错消失。**结论：工程文件里的地址是"存下来的"，手工写进去的不会自动重解析**——正常用法（`add_variable`）不受影响。
2. **记录器组必须是活动组**。`set_active_group` 之前是空操作：`GuiPlotsTree.hpp` 把 `setActiveGroup()` 挂在「当前选中」判断里每帧执行，接口设过的活动组下一帧就被覆盖。**本次已修**（挪到点击分支），修后 `set_active_group` 能保持。
3. 之后 `detect_recorder` **把界面线程卡死**：连续三次 MCP 调用全部超时，只能强杀。本次会话里界面卡死共出现三次（两次在采集启动之后、一次在记录器探测），**未定位**，需要单独排查后才能给通路 B 下结论。

## 过程中排掉的一个假象

第一次用 ST-Link 采集时读回的值全是垃圾（`vtraceRecorderSize` = 524296、`vtraceSerialDriverVersion` = 536870968），看上去像读错了地址。用 **OpenOCD 独立读同一批地址，结果与应用逐字节相同** —— 说明应用的读没有错，是**板上跑的固件不是这一份**：

| | 初始 SP（`0x08000000`） | 复位向量（`0x08000004`） |
| --- | --- | --- |
| 我们这份 hex | `0x20002F00` | `0x08000189` |
| 板上实际 | **`0x20000820`** | `0x08000189` |

烧回我们的固件并校验后，`0x200000C0` 读出 72000、`0x200000D4` 读出 8324，全部归位。

## 顺带确认的两件事

- **ST-Link 也能烧写**：`openocd -f interface/stlink.cfg -f target/stm32f1x.cfg -c "program <hex> verify reset exit"` 写入并校验通过。

### 撤回一条先前写错的结论

上一版这里写着「`StlinkDebugProbe::readMemory` 有一处静默错误：`bool result = ...` 算完从不使用」。**这条不成立**——函数的最后一行是 `return result;`，读失败会正确地报给调用方（`ViewerDataHandler` 收到 false 会停下采集）。当时只看了中间那段移位逻辑就下了判断，没有读到函数末尾。读回垃圾值的原因已经查清，是板上固件不同，与这段代码无关。

---

# 界面卡死：根因与修复

## 症状

界面停止重绘，MCP 调用全部超时（只能强杀进程），进程内存持续膨胀。早先记下的「`set_sampling{max_points}` 之后停/启，窗口无响应、进程涨到 8.4 GB」与本次是**同一个病**。

## 抓法

按当时的步骤复现（打开带记录器组的工程 → `add_variable` 导入三个记录器符号 → 把 `____recorderSettings` 挂进图 → 启动采集），界面立刻不响应。趁进程还在，直接 attach：

```
gdb -batch -p <pid> -ex "thread apply all bt 30" -ex detach
```

界面线程的栈：

```
#0 msvcrt!memmove
#1 ImVector<char>::reserve (new_capacity=1073746881)   ← 申请 1 GB
#2 ImGuiTextBuffer::append
#3 ImPlotTicker::AddTick
#4 ImPlot::Locator_Default (vertical=false)
#5 ImPlot::SetupFinish
#6 ImPlot::SetupLock
#7 ImPlot::IsPlotHovered
#8 Gui::plotViewWasAskedForByHand
#9 Gui::drawPlotCurve
#10 Gui::drawPlots
```

界面线程**不是在等锁**，而是在无休止地生成刻度，把刻度标签往一个文本缓冲里塞。

## 根因

`Plot::liveTimeWindow()` 会把窗口左端夹到 `oldest` 上：

```cpp
const double left = std::min(std::max(newest - windowWidth, oldest), newest);
```

缓冲里只有一个样本时 `oldest == newest`，夹完 `left == newest` —— **区间宽度为零**。ImPlot 的刻度定位器在这种范围上不会收敛，每帧都往缓冲里追加，直到内存被吃掉。启动采集后的头几帧正好落在这个窗口里，界面从此不再重绘。配置宽度为 0、或两端出现 NaN 时同理。

## 修复与复验

| 位置 | 处理 |
| --- | --- |
| `Plot::liveTimeWindow` | 保证返回真实区间：夹不出宽度就用配置的宽度；宽度配成 0 时退到最小可用宽度（1 s）；两端不是数时给一个确定的安全区间 |
| `GuiSwoPlots.cpp` 的跟踪 X 范围 | 只在 `max > min` 时才设限（写成一个比较，NaN 自然落选） |
| `test/PlotTest.cpp` | 旧用例里**钉住零宽度的那条期望已改正**，并补三条：单样本、宽度为 0、两端为 NaN |

复验：同一条复现路径重跑，界面连续 12 秒保持响应，`detect_recorder` 与状态查询都正常返回；482 项单元测试全部通过。

## 同一个栈里发现的另一个独立问题

采集线程卡在 ST-Link 的 USB 写里：

```
#9  send_recv            ← libusb_bulk_transfer / WinUsb_WritePipe
#10 _stlink_usb_read_debug32
#11 stlink_read_debug32
#12 StlinkDebugProbe::readMemory (address=536870912 = 0x20000000)
#13 ViewerDataHandler::dataHandler:187
```

`0x20000000` 是**地址没解析成功时的占位值**——也就是说采集线程正在按占位地址采样，而且这次读没有返回。这与已知的那条「地址没解析成功的变量会保留占位值 0x20000000 并且照样按它采样」是同一件事，属于**独立于本次卡死**的问题（本次卡死修的就是界面的刻度爆炸）。它挡住的是通路 B（记录器）。

---

# 地址解析：`add_variable` 从不解析地址（已修）

## 隔离

用「存工程再读回变量的 `address` 字段」来量（工程文件里地址是存下来的，一看就知道应用解析出了什么）：

| 操作 | `txTail`（普通符号，gdb 查到 `0x20000082`） | 三个 `____` 记录器符号 |
| --- | --- | --- |
| `add_variable` 之后 | **`0x20000000`** | **`0x20000000`** |
| 再 `refresh_variable_addresses` 之后 | `0x20000082` ✅ | 全部正确 ✅ |

也就是说：**连普通符号也一样**，与下划线、与结构体都无关。`add_variable` 只做 `addNewVariable` + `setType`，从头到尾没碰地址。

同时说明**真正的解析方是 `refresh_variable_addresses`**：它之后 `found 59 / not_found 0`，四个符号的地址全部正确。

## 为什么查不到：符号表只有开过导入对话框才有内容

应用本来打算用解析器的符号表回答「这个名字的地址是多少」。但全项目**唯一**调用 `parse()`（填表）的地方是导入变量对话框：

```
GuiImportVariables.hpp:64   refreshThread = std::async(..., &IElfParser::parse, parser, ...);
```

纯 MCP 会话里没人打开那个对话框，所以 `getParsedData()` 一直是空的——查表必然查不到。

## 修复

给 MCP 上下文加了一个「按名字查地址」的回调（`McpContext::lookupSymbolAddresses`，由 Gui 侧从符号表批量查一次），`add_variable` 现在：

- 表里**查到** → 立刻把地址写进变量 ✅；
- 表里**查不到** → 如实回 `"is_found": false` 并附一句 hint 指向 `refresh_variable_addresses`，**不再用一个光秃秃的 `status: "created"` 假装创建成功**。

不在这里直接跑解析是刻意的：解析要起 gdb 进程，而按这个项目的约定，「起解析进程」的活儿应该在请求线程上跑，而这个 handler 跑在界面线程上。

## 复验

```
add     : [{"is_found":false, "name":"txTail", "hint":"The address is not resolved yet, ... Call refresh_variable_addresses ..."}, ...]
refresh : {"found":59, "not_found":0}
保存后读回：____recorderSettings 0x200000D8 ✅  ____recorder 0x2000077C ✅
            ____serialDriverSettings 0x200000F0 ✅  txTail 0x20000082 ✅
```

482 项单元测试全部通过。

## 顺带的收获：通路 B 的拦路虎没了

地址一正确，**记录器探测在 ST-Link 下直接通过**：

```json
{"detected":true,"version":8,"revision":3,"max_variables":6,
 "max_buf_size":8192,"timestep_ns":50000,"sampling_hz":20000,"float_support":true}
```

正是清单期望的「recorder 8.3，6 variables，8192 samples，timestep 50000 ns，float triggers」。通路 B 现在只剩「缓冲回读与波形」那几步没验。

## 顺带发现的另一处（未改）

`GdbParser::checkAddress` 里 `stoi` 抛异常被 `catch (...)` 吞掉之后，仍然 `return addressValue`（此时是 **0**）而不是 `nullopt`——解析失败会变成一个看起来合法的地址 0。这一处先记下，没动。

---

# 通路 B 的当前进度：主机侧全通，卡在「记录器只能从界面启动」

## 已经过了的

| 步骤 | 结果 |
| --- | --- |
| `detect_recorder` | `version 8 / revision 3 / max_variables 6 / max_buf_size 8192 / timestep_ns 50000 / float_support true` ✅ |
| `set_recorder_enabled(true)` | ✅ |
| `set_recorder_trigger`（`fastTriangle` / rising / 预触发 100） | ✅（**`normal` 模式必须先设触发源**，否则报 `Mode 'normal' needs a trigger`） |
| `set_recorder_mode('normal')` | ✅ |

## 卡在哪

`set_recorder_enabled` **只切一个标志**，真正的 start / stop 在记录器面板的两个按钮上（`GuiRecorder.cpp`）：

```
Start    → recorderHandler->detect(...) && recorderHandler->start(...)
Download → 缓冲填满后把整块搬回来
```

提示原文就是「Recording. The target fills its buffer on its own, press Download when it has filled.」。
所以**API 能配置记录器，但不能启动它、也不能回读它**——`get_recorder_settings` 里 `running` 一直是 `false` 就是这个原因。

**没有为此加工具**：`test/McpServerTest.cpp:704-728` 把工具面锁成 **46 个**并逐个列名，注释写明这些名字来自参考构建、是给 agent prompt 用的契约（`EXPECT_EQ(registered.size(), 46u)`）。要加就得先改这份契约，那是另一个决定。

## 本轮的另一个障碍：UI 自动化在这个环境里失效

三种注入方式**全部无效**：`SendInput` 绝对坐标、投递 `WM_LBUTTONDOWN/UP`（带客户端坐标）、`mouse_event`。而界面确实在重绘（前后两次截图差异区域 `(729,55)-(1901,1005)`，页签区不动），窗口确实是前景窗口，`WindowFromPoint` 命中的也正是它——**输入进不去，不是应用死了**。

顺带查出并修掉自己的一处 bug：测试脚本里的鼠标移动按**虚拟桌面**换算绝对坐标是错的。这台机器虚拟桌面是 **3627×1399**（1920×1080 主屏之外还有一块屏），实测 `move(150, 61)` 把光标放到了 `(79, 293)`。改成 `SetCursorPos` 后标定 `(100,100) / (500,400) / (1500,900)` 全部精确命中——但点击仍进不去，所以问题不在这一步。

## 要往下走，需要其中之一

1. 人工点一下（应用已经停在这个状态：`addrcheck6` 工程、采集 RUN、记录器已 enable、触发已设）——`Window → Recorder → Start`，等缓冲填满再按 `Download`；
2. 或者决定把记录器的启动/回读也做成 API 工具（要同时改那份 46 工具的契约与对应测试）。

---

# 补了取数路径，随即暴露：记录器回读的**数值是错的**

## 补的是什么

| 改动 | 说明 |
| --- | --- |
| `Variable::transformToDouble(uint32_t raw)` 重载 | 同一套类型转换，但处理传进来的原始值，不必先把值写进模型；原无参版本改成它的薄封装，两处不会漂移 |
| `Download` 时按类型转换一次 | 预览与导出共用同一份值——float 通道不再是 1e9 量级的位模式 |
| 面板新增 `Export capture to *.csv` | 写到工程文件旁边（`<工程名>_recorder_<时间戳>.csv`），**不弹对话框**，自动化可直接读取 |

## 结果：路径通了，数值不对

导出的 CSV 结构完全正确——745 行 = 表头 + **744 个样本**，时间列 0 / 5e-05 / 0.0001…（**50 µs 步距**，与 20 kHz 一致）：

```
time [s],fastRamp,fastSine,fastSquare,fastTriangle
0,       2.44495e+09, nan,        11,   1793
5e-05,   2.44498e+09, 3.97221e+35, 191,  15291
0.0001,  2.43834e+09, 7.40419e+28, 191,  -28415
```

| 列 | 类型 | 应当 | 实测 |
| --- | --- | --- | --- |
| `fastRamp` | u32 | ≈ 7.4e7，相邻差 1 | **2.44e9**，跳变 |
| `fastSquare` | u8 | 只有 0 / 1 | **11 / 191 / 15** |
| `fastSine` | f32 | −1…1 | **nan / 3.97e35** |
| `fastTriangle` | i16 | 平滑三角 | 1793 / 15291 / −28415 |

`fastSquare` 是 u8，**只可能是 0 或 1**，读出 11/191/15 就说明**字节本身错了**（类型转换不会把字节搞乱）。

## 已经排除的

- **拆包逻辑没问题**：`decodeRing` 按小端组装，`value |= buffer[offset + byte] << (8 * byte)`，`offset` 按每个槽的宽度前进；宽度之和与 `ring.packSize` 还做过一致性校验（不一致会直接报错而不是硬解）。
- **两侧的布局假设一致**：`apply` 写样本表是按 `config.variables` 的顺序（地址 + 宽度），`download` 拆包用同一个列表、同一个顺序。
- **窗口与步距对**：744 个样本 × 50 µs = 37.2 ms，`decodeRing` 的 `samplePeriodSeconds = timestepNs × (samplesToSkip + 1)`，skip = 0 时正是 50 µs。

## 还没排除的

差异只能在**「目标侧按什么布局打包」**与**「主机侧按什么布局拆分」**之间，或者**环形窗口的取法**（`validRanges(window, …)` 绕环的那一段）：
若目标侧每个样本实际写入的字节数与打包顺序与主机假设的不同，按 11 字节步长去切就会整列错位——这与「u8 列读出 191」正好吻合。

## 本轮确定的

- **记录器的机制全通**：探测（`recorder 8.3, 6 variables, 8192 samples, timestep 50000 ns, float triggers`）、配置下发（4 变量 / 11 字节）、触发、运行、回读（`Copied 8184 bytes: 744 samples`）都有实测数字。
- **窗口判据通过**：744 × 50 µs = 37.2 ms（清单写 25 ms 是按 4 个 float = 16 字节算的）。
- **数值判据不通过**：相邻样本差 1、2.0 ms 周期、跳采样 n+1 都无从谈起，因为字节就是错的。
- 补的这条取数路径本身是对的——**正是它把「数值全错」这件事从「看不见」变成了「一眼可见」**。
