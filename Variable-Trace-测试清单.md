# Variable-Trace 目标侧测试清单

这块板子（STM32F103C8，100ask DshanMCU-F103）上跑着一份给 Variable-Trace 用的测试程序，
把工具的四条数据通路都接了起来。这份清单说明每条通路怎么测、看什么算通过。

---

## 0 开工前提

| 项 | 值 |
| --- | --- |
| 编译与烧录 | EIDE（VSCode），`build and flash` |
| 符号文件 | `MDK-ARM/0501_led/0501_led.axf` |
| 探针器件名 | `STM32F103C8`（J-Link，不要带封装后缀） |
| 串口 | USART1，PA9 发 / PA10 收，115200，8 位无校验 1 位停止 |

变量要能被工具读到，必须同时满足三条：定义在文件作用域、用 `volatile` 修饰、
编译时带调试信息。这三条本工程都已经满足，不需要额外配置。

---

## 1 先过自检数字

接上探针、导入变量之后，先看这几个量。它们不对，后面任何测试结论都不成立。

| 变量 | 期望值 | 含义 |
| --- | --- | --- |
| `vtraceSysclkHz` | 72000 | 实际主频。显示 8000 说明 HSE 晶振没起振 |
| `vtraceRecorderSize` | 8324 | 记录器结构大小，须与主机的偏移约定一致 |
| `vtraceRecorderVersion` | 8 | 目标侧记录器参考件的版本 |
| `vtraceSerialDriverVersion` | 1 | 目标侧串口驱动参考件的版本 |
| `vtraceStepUs` | 明显小于 50 | 采样中断一轮的耗时，采样时基是 50 µs |

---

## 2 通路 A：变量视图

工具从符号文件里取变量的绝对地址，再用探针（或串口）读 RAM。

### 步骤

1. `Options → Acquisition`，选符号文件 `MDK-ARM/0501_led/0501_led.axf`。
2. 探针选 J-Link，Target name 填 `STM32F103C8`。
3. `Import variables from *.elf` 导入，或逐个 `Add variable` 手动加名字。
4. 点 `Update variable addresses`，所有类型与地址应变成有效值，不出现 `NOT FOUND!`。
5. 把变量拖到绘图区，按 `STOPPED` 开始采集。

### 看什么算通过

| 测什么 | 判据 |
| --- | --- |
| 时间波形 | `sinTest` 与 `cosTest` 是 0.5 Hz 的正交对，幅值 ±1 |
| X-Y 图形 | 横轴 `LissajousX`、纵轴 `LissajousY1`／`Y2`／`Y3`，得到 1:4、1:7、1:15 三种李萨如图形 |
| X-Y 图形 | 横轴 `roseX`、纵轴 `roseY`，得到 8 瓣玫瑰线 |
| 整数宽度与符号 | `u8Test`／`i8Test`／`u16Test`／`i16Test`／`u32Test`／`i32Test` 由同一个相位驱动，任意时刻互相能对上；三个有符号量在负半周会出现负值 |
| 结构体成员 | `vtraceStruct` 的 `a`=12、`b`=155、`c`=-85、`d`=105、`f`=55234、`g`=-5234、`h`=-2141248、`j`=37.21 |
| 数组步长 | `waveTable[0..7]` = 0.125、-0.25、0.5、-0.75、1.0、-1.25、1.5、-1.75 |
| 变量写回 | 把 `petalsNumber` 改成 6／10／12，玫瑰花瓣数跟着变 |

结构体与数组那两组值全部是刻意选的、一眼能认出来的数。对不上说明工具解析成员偏移
或数组步长出了错，这两处是最容易出问题的地方。

---

## 3 通路 B：记录器

记录器在目标侧的定时器中断里把变量搬进一块环形缓冲，缓冲留在目标内存里；
主机在录制期间不占用链路，等缓冲填满后一次搬回去。所以它能录到探针带宽之外的信号。

### 步骤

1. 先按通路 A 接通探针。
2. 建一个记录器组，点 `Detect`。应报：

   ```
   recorder 8.3, 6 variables, 8192 samples, timestep 50000 ns, float triggers
   ```

3. 往组里放不超过 6 个变量。推荐 `fastRamp` + `fastTriangle` + `fastSquare` + `fastSine`。
4. 需要的话设置跳采样与触发条件，然后开始采集。
5. 缓冲填满后主机自动把整块搬回来并画出波形。

### 看什么算通过

| 测什么 | 判据 |
| --- | --- |
| 有没有丢样本 | `fastRamp` 相邻样本相差 1（未设跳采样时） |
| 波形周期 | 三角／方波／正弦的周期 = 65536 ÷ 1638 × 50 µs ≈ **2.0 ms**，即 499.9 Hz |
| 时间轴 | 录制窗口 = 8192 ÷ 每次采样的字节数 × 50 µs；放四个 float 变量时约 25 ms |
| 触发 | 把 `fastTriangle` 当触发源，分别试上升沿与下降沿，看波形起点与预触发段 |
| 跳采样 | 设成 n 之后，`fastRamp` 相邻样本应相差 n+1 |

### 几个数是怎么来的

- 缓冲 8192 字节，存放的是原始字节流，一个样本占「被测变量字节数之和」。四个 float 变量一个样本 16 字节。
- 每次采样在定时器中断里发生，间隔 50 µs，也就是 20 kHz 采样率。
- 高频信号的相位每个采样周期推进 `fastPhaseStep` 步，整个圆周 65536 步，所以周期是 65536 ÷ 1638 ≈ 40 个采样周期。

---

## 4 通路 C：串口探针

这条通路不用调试探针，一根 USB-TTL 就够。目标侧跑着一个极小的服务端，
支持三条命令：读内存、写内存、连续读一块内存。连续读那条是记录器取缓冲用的。

### 步骤

1. 接线：PA9（目标发）→ 适配器 RX，PA10 ← 适配器 TX，GND 共地。
2. 探针类型选 Serial，端口选对应的 COM 口，波特率 115200。
3. 点 `Detect`，应同时找到记录器与串口驱动两组设置。

### 看什么算通过

| 测什么 | 判据 |
| --- | --- |
| 变量读取 | 同一个变量走串口读到的值，与走探针读到的值一致 |
| 记录器取回 | 记录器缓冲也能走串口搬回来，波形与走探针时一致 |
| 变量写回 | 通过串口改写 `fastPhaseStep`，高频信号频率跟着变 |

---

## 5 通路 D：跟踪视图

跟踪视图走 SWO，不占探针的读内存通道，可以看比探针采样率高得多的信号。
本板 PB3 已经配成 `SYS_JTDO-TRACESWO`，不需要改硬件。

### 步骤

1. 在 Acquisition 设置里把 System Core Clock 填成 **72000**（单位 kHz）。这个数决定横轴比例，填错曲线整体拉伸。
2. 挑一个低于 J-Link 的 SWO 上限的 prescaler。
3. 通道类型：0 号设数字，1 号设 analog float32，2 号设 analog u16。
4. 开始录制。

### 通道分配

| 通道 | 内容 | 说明 |
| --- | --- | --- |
| 0 | 数字标记 `0xAA` / `0xBB` | 主循环里成对写入，高电平那段就是被标记代码的耗时 |
| 1 | analog float32 | `fastSine` 的原始位型，499.9 Hz |
| 2 | analog u16 | `fastTriangle`，499.9 Hz |

### 看什么算通过

- 0 号通道交替出现 `0xAA` 与 `0xBB`，高电平段宽度由 `vtraceDemoLoops` 决定，
  默认约百微秒（实际值随编译器优化程度变化，以实测为准）。
- 在工具里改 `vtraceDemoLoops`，0 号通道的高电平段跟着变宽或变窄。
- 1 号与 2 号都是 499.9 Hz 的规则波形，两者相位关系固定。

---

## 6 可以实时改写的旋钮

这五个量就是用来测「主机写回变量」这个能力的。改完下一轮生效，不需要复位。

| 变量 | 默认值 | 改它看什么 |
| --- | --- | --- |
| `traceBaseFreqHz` | 0.5 | 慢速轨迹全部按比例变速 |
| `petalsNumber` | 8 | 玫瑰线花瓣数 |
| `fastPhaseStep` | 1638 | 三路高频信号一起变速 |
| `vtraceDemoLoops` | 800 | SWO 0 号通道的脉宽，上限 20000，超过会被夹住 |

---

## 7 变量总表

### 慢速轨迹（变量视图，更新周期 1 ms）

| 变量 | 类型 | 默认值 / 含义 |
| --- | --- | --- |
| `sinTest` | float | 0.5 Hz 正弦 |
| `cosTest` | float | 0.5 Hz 余弦 |
| `LissajousX` | float | 0.5 Hz，作横轴 |
| `LissajousY1` | float | 2 Hz（4 倍频） |
| `LissajousY2` | float | 3.5 Hz（7 倍频） |
| `LissajousY3` | float | 7.5 Hz（15 倍频） |
| `roseX` | float | 玫瑰线横轴 |
| `roseY` | float | 玫瑰线纵轴 |
| `smallRoseY` | float | 0.4 倍幅的玫瑰线纵轴 |
| `petalsNumber` | float | 8，可写 |
| `traceBaseFreqHz` | float | 0.5，可写 |
| `traceLoopCount` | uint32 | 累计更新轮次 |
| `traceUpdateCycles` | uint32 | 一轮更新用掉的 CPU 周期 |
| `traceUpdateUs` | uint32 | 上面那个换算成微秒 |

### 类型与符号覆盖

| 变量 | 类型 | 特征 |
| --- | --- | --- |
| `u8Test` / `i8Test` | uint8 / int8 | 每 100 ms 推进一步 |
| `u16Test` / `i16Test` | uint16 / int16 | 同上 |
| `u32Test` / `i32Test` | uint32 / int32 | 同上，乘 65537，扫过整个 32 位范围 |
| `f32Test` | float | 同一个相位驱动的正弦 |
| `vtraceStruct` | 结构体 | 8 个成员固定值，偏移 0/1/2/3/4/6/8/12 |
| `waveTable[8]` | float 数组 | 8 个固定值，核数组步长 |

### 高频信号（采样节拍，20 kHz）

| 变量 | 类型 | 特征 |
| --- | --- | --- |
| `fastRamp` | uint32 | 每拍加一，核对丢样本用 |
| `fastPhase` | uint16 | 相位累加器 |
| `fastPhaseStep` | uint16 | 1638，可写 |
| `fastTriangle` | int16 | 满幅三角波 |
| `fastSquare` | uint8 | 0 / 1 方波，适合数字图 |
| `fastSine` | float | 正弦 |
| `fastNoise` | int16 | 线性反馈移位寄存器产生的伪随机噪声 |

### 自检与状态

| 变量 | 类型 | 期望值 / 含义 |
| --- | --- | --- |
| `vtraceSysclkHz` | uint32 | 72000 |
| `vtraceRecorderSize` | uint32 | 8324 |
| `vtraceRecorderVersion` | uint32 | 8 |
| `vtraceSerialDriverVersion` | uint32 | 1 |
| `vtraceIsrCount` | uint32 | 采样中断累计次数 |
| `vtraceStepCycles` | uint32 | 采样中断一轮的 CPU 周期 |
| `vtraceStepUs` | uint32 | 同上换算成微秒 |
| `vtraceFastFreqHz` | uint32 | 高频信号当前实际频率，约 499 |
| `vtraceDemoLoops` | uint16 | 800，可写 |

### 主机按名字查找的全局量

工具靠这四个名字定位记录器与串口驱动，它们必须留在符号表里。

| 变量 | 大小 | 用途 |
| --- | --- | --- |
| `____recorder` | 8324 字节 | 环形缓冲、采样列表与触发条件 |
| `____recorderSettings` | 24 字节 | 时基、缓冲大小、变量上限、版本 |
| `____serialDriver` | 160 字节 | 串口协议解析状态机 |
| `____serialDriverSettings` | 12 字节 | 串口驱动版本与变量上限 |

---

## 8 需要注意的地方

- **USART1 归串口驱动使用**，开机不再发送任何非协议内容。原来的 circle buffer
  接收路径保留着，收到的字节会同时喂给串口驱动和环形缓冲。
- **`HAL_TIM_MODULE_ENABLED` 是手动打开的**，TIM2 没有登记进 `0501_led.ioc`。
  用 CubeMX 重新生成工程时它会回到关闭状态，表现是编译报 `TIM_HandleTypeDef` 不认识
  —— 这是个编译错误，不会静默跑错。
- **`0501_led.ioc` 里的时钟与晶振引脚是手写的**，没有在 CubeMX 里打开验证过。
  用 CubeMX 重新生成之前先在时钟页核对一遍。
- **记录器上限**是 6 个变量、8192 字节缓冲。放超过 6 个变量时主机会直接拒绝并报错。
- **采样率上限**：一个采样周期里相位最多推进一个整周，所以 `fastPhaseStep` 超过 65536
  会被夹住，对应 20 kHz。
- **`driver_oled.c` 缺少 `<string.h>`**。AC5 容忍隐式声明，GCC 14 及以上会报错。
  这是既有的问题，与本测试程序无关。

---

## 9 什么时候算测通了

1. 第 1 节的五个自检数字全部符合期望。
2. 通路 A：三种李萨如图形与玫瑰线画得出来，结构体与数组的固定值读得对，
   改 `petalsNumber` 花瓣数会变。
3. 通路 B：`Detect` 报出的版本与容量正确，录回来的 `fastRamp` 相邻样本差 1，
   波形周期约 2.0 ms。
4. 通路 C：同一条曲线走串口与走探针画出来一致。
5. 通路 D：0 号通道能看到规则方波，改 `vtraceDemoLoops` 脉宽会变；
   1、2 号是 499.9 Hz 的规则波形。
