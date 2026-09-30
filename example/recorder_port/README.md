# 目标侧移植示例：串口驱动 + 内存记录器

这个目录是给**目标固件**用的，不是给主机程序用的。它演示怎么把两个目标侧模块接进一颗
MCU 的工程，让 Variable-Trace 能把变量读到、把记录器的缓冲取回。

## 这两个模块各自负责什么

| 模块 | 作用 | 没有它的后果 |
| --- | --- | --- |
| `SerialDriver/` | 在目标上跑一个极小的串口服务端，支持「读内存」「写内存」「连续读一块内存」三条命令 | 主机只能用 J-Link / ST-Link 一类调试探针，用不了串口 |
| `Recorder/` | 在定时中断里把若干变量搬进一块环形缓冲，缓冲区留在目标内存里 | 采不到调试链路带宽撑不住的高频信号 |

两者互相独立。只用 J-Link / ST-Link 时**只需要 `Recorder/`**：那份缓冲由探针直接搬走。
只有走串口这条路时两个都要。

## 来源与许可

这六个文件是 MCUViewer 发行包随附的「目标侧集成件」，随 `MCUViewer_Windows_1.2.9`
分发，此处按原样复制、未做任何修改。文件头保留了原始版权声明，许可条款写明：允许在
与 MCUViewer 项目相关的前提下以源码或二进制形式使用、修改、分发，前提是版权声明与免责
声明保持不变。

本仓库（Variable-Trace，GPLv3）的其余部分与这六个文件是两套独立的授权。分发本仓库时
请连同这段说明一起保留。

## 接线

以 STM32F103C8 为例，只用一根 USB-TTL 就能跑串口这条路：

| 目标引脚 | 接到 | 说明 |
| --- | --- | --- |
| PA9 (USART1_TX) | USB-TTL 的 RX | 目标发、主机收 |
| PA10 (USART1_RX) | USB-TTL 的 TX | 主机发、目标收 |
| GND | USB-TTL 的 GND | 共地，必须接 |

USART1 配 8 位数据、无校验、1 位停止位，波特率与主机 Acquisition settings 里填的
一致（默认 115200）。

## 移植步骤

1. **放文件**。把 `Recorder/` 与 `SerialDriver/` 两个目录加进固件工程的源码列表和
   头文件搜索路径。两个目录里的 `.c` 要参与编译。

2. **配置参数**。改 `Recorder/recorderDefines.h` 与
   `SerialDriver/serialDriverDefines.h`，逐项含义见下一节。

3. **实现三个回调**。见 `port_example.c`，分别是：

   - `serialDriverSendData()` —— 主机要的字节怎么发出去；
   - 串口接收中断 —— 每收到一个字节调一次 `serialDriverReceiveByte()`；
   - 定时器中断 —— 按 `____RECORDER_TIMEBASE_NS` 的周期调 `recorderStep()`。

4. **确认符号进得了符号表**。主机是靠名字 `____recorder` 与 `____recorderSettings`
   找到这两块内存的，所以这两个全局变量必须留在最终镜像的符号表里。如果 `Recorder/`
   编译成静态库，连接器可能因为「没人引用」把整个目标文件丢掉，那时主机导入变量会报
   「符号表里没有 ____recorder」。绕法是把 `.o` 直接加进连接列表，或者加
   `--whole-archive`，或者从你的代码里引用一次。

   引用它们要自己写 `extern` 声明，头文件里看不到这四个名字：

   ```c
   extern ____Recorder ____recorder;
   extern volatile ____RecorderSettings ____recorderSettings;
   extern ____SerialDriver ____serialDriver;
   extern volatile ____SerialDriverSettings ____serialDriverSettings;
   ```

   两个 settings 结构在定义时带 `volatile`，声明也必须带，否则类型对不上。
   `port_example.c` 末尾就是这么写的。

## 配置项含义

### `recorderDefines.h`

| 宏 | 示例值 | 含义与取舍 |
| --- | --- | --- |
| `____RECORDER_TIMEBASE_NS` | `10000` | 两次 `recorderStep()` 之间相隔多少纳秒。**这个数必须等于定时器中断的真实周期**，主机不会去验证它，填错不会报错，只会让主机的横轴比例整体偏掉 |
| `____RECORDER_BUFFERSIZE` | `32768` | 环形缓冲字节数。越大录得越久，代价是常驻 RAM |
| `____RECORDER_MAXVARS` | `12` | 一次最多录几个变量。宿主写超过这个数量的配置会被拒绝 |
| `____RECORDER_FLOAT_SUPPORT` | `1` | 是否支持浮点触发比较。没有 FPU 的片子设 `0` 可以省掉三个 float 成员 |
| `____RECORDER_C2000_SUPPORT` | `0` | 目标是不是 C2000 系列。设错会让地址与宽度的处理全错 |

### `serialDriverDefines.h`

| 宏 | 示例值 | 含义与取舍 |
| --- | --- | --- |
| `____SERIAL_DRIVER_MAXVARS` | `10` | 一次「读」命令最多带几个地址。每个地址最多 4 字节，所以单次响应最大 `4 × 这个数 + 2` 字节，而长度字段是一个字节，因此这个数不能超过 63 |
| `____SERIAL_DRIVER_C2000_SUPPORT` | `0` | 同上，目标是不是 C2000 |

## 一次记录是怎么跑的

主机的动作顺序是这样的，理解它对排查问题很有用：

1. 主机从 `.elf` 里读出 `____recorderSettings` 的地址，读它拿到版本、时基、缓冲大小
   与变量数上限。这一步对应界面上的 **Detect**。
2. 主机把配置写进 `____recorder`：跳采样数、变量个数、每包字节数、缓冲可用字节数、
   逐个变量的地址与宽度、触发源地址与三套阈值。此时 `state` 还是 `INIT`。
3. 主机把 `state` 写成 `RUNNING`。目标从下一个定时中断开始采样。
4. 目标填满缓冲后自己停下（`state` 变成 `FULL`）。
5. 主机用「连续读」把整块缓冲搬回来，再按环形缓冲的头尾指针把样本拼回时间轴。

所以主机在录制期间**不占用链路**，采样率只受目标 CPU 限制。这也是它能录到调试探针
带宽之外的原因。

## 容易踩的地方

- **中断里的耗时**。`recorderStep()` 本身只是几次内存拷贝，但它跟着的定时器周期可能
  很密（默认 10 µs）。变量又多又宽时，中断占用会明显上升，真实采样周期会被拉长，
  而主机仍然按 `TIMEBASE_NS` 画横轴。发现曲线时间轴整体不准时先看这里。
- **`serialDriverSendData()` 在接收中断里被调用**，不能在里面阻塞等发送完成。示例给了
  一个「环形缓冲 + 发送中断」的写法。
- **触发源的类型**由主机的变量声明决定，写进 `____recorder.trigger.type`。这个类型
  必须与被触发变量的真实宽度一致，否则目标会按错误的宽度去读那个地址。
- **`maxActualBufferSize` 是主机算的**，只会是「整包」的整数倍。目标侧还依赖这一点，
  它用 `head + samplesPackSize` 与这个值比较来决定何时绕回，不是整包的缓冲会让它写越界。
  改缓冲大小时不需要自己动这个字段。
- **在 64 位机器上试编译会看到一批警告**。参考件把 `uint32_t` 地址直接转成指针，在
  32 位目标上是正好的，在 PC 上编译会报 `cast to pointer from integer of different
  size`。这是宿主机位宽造成的，不是缺陷；`recorder.h` 里还有一条
  `operand of '?:' changes signedness` 的警告，来自上游代码本身。
- **记录器不需要串口驱动也能用**。只用 J-Link / ST-Link 时可以把 `SerialDriver/`
  整个目录从工程里去掉，主机走探针读那块缓冲。反过来，只用串口不想录高频信号时，
  可以只留 `SerialDriver/`，主机会像读普通变量一样按地址轮询。
