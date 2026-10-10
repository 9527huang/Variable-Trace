# Variable-Trace 测试环境与版本

**快照日期：2026-10-10**（本机实测，不是抄文档）

---

> 路径里的 `<用户名>` / `<安装目录>` / `<文档目录>` 是占位符，分别对应本机用户目录、装工具软件的位置、放工程的文档目录。

## 一、速查

| 项目 | 版本 / 型号 | 位置 |
| --- | --- | --- |
| 调试探针 | **J-Link CE**，S/N 69730307，USB | — |
| 探针固件 | J-Link V9，compiled **Dec 13 2022** | — |
| 探针硬件版本 | **V9.70** | — |
| 应用用的 J-Link DLL | SEGGER J-Link interface DLL (x64) **8.10k** | `MCUViewer-1.1.0/third_party/jlink/lib/windows/JLink_x64.dll` |
| 本机 J-Link 软件 | JLink.exe / JLinkARM.dll **V6.40**（2018-10-26） | `<安装目录>\Segger\JLink_V640\` |
| 目标板 | STM32F103C8（Cortex-M3 r1p1），72 MHz | — |
| 固件工程 | `0501_led`，Armcc V5.06 update 7 | `<文档目录>\AICodeProject\stm32103\0703_uart_circle_buffer\` |
| 被测应用 | Variable-Trace **1.1.0**，commit `67d0c6c` | `MCUViewer-1.1.0/build/Variable-Trace.exe` |
| Keil | **MDK-ARM Plus 5.36.0.0** | `<安装目录>\keilcore\` |
| GDB | **17.1** | `C:\msys64\mingw64\bin\gdb.exe` |
| OpenOCD | **0.12.0**（2026-03-02，sysprogs 版） | `<文档目录>\OpenOCD-20260302-0.12.0\bin` |
| STM32CubeProgrammer | **未安装** | — |
| 操作系统 | Windows 11，10.0.28000，AMD64 | — |

---

## 二、调试探针（J-Link）

- **型号**：J-Link CE（`ProductName: J-Link CE`），USB 连接
- **序列号**：69730307
- **探针固件**：`J-Link V9 compiled Dec 13 2022 11:14:50`
- **硬件版本**：`V9.70`
- **目标接口**：SWD，4000 kHz（`SWD` 是应用里唯一选的接口，`JLINKARM_TIF_SWD`）
- **VTref**：3.267 V
- **SWO 接线**：已接到目标 PB3（TRACESWO），实测可通

### 两套 J-Link 软件，别混

| | 版本 | 用在哪 |
| --- | --- | --- |
| 应用链接的 DLL | **8.10k**（PE 时间戳 2024-12-05，23 940 560 字节） | Variable-Trace 运行时 |
| 安装的 CLI/DLL | **V6.40**（编译于 2018-10-26） | 手工排障用 `JLink.exe` |

**别用 `JLink.exe` 的行为去推断应用的行为**——它们不是一套 DLL。另外 V6.40 的 CLI **没有 SWO 命令组**（`SWO EnableTarget` / `SWO Read` 都报 `Unknown command`），做不了 SWO 的独立旁证；`mem32` / `w4` 读写寄存器是可用的。

---

## 三、目标板与固件

- **板卡**：STM32F103C8（J-Link 识别为 Cortex-M3 r1p1，Little endian），SystemCoreClock 72 MHz
- **SWO 引脚**：PB3 = `SYS_JTDO-TRACESWO`，`__HAL_AFIO_REMAP_SWJ_NOJTAG()` 在 `Core/Src/stm32f1xx_hal_msp.c:77`
- **固件工程**：`<文档目录>\AICodeProject\stm32103\0703_uart_circle_buffer\`，Keil 工程 `MDK-ARM/0501_led.uvprojx`，target `0501_led`
- **本次快照的产物**（2026-10-10 15:29 构建）：
  - `MDK-ARM/0501_led/0501_led.axf` 645 704 字节
  - `MDK-ARM/0501_led/0501_led.hex` 46 250 字节（烧录写入 14 336 字节）
- **关键参数**：`____RECORDER_TIMEBASE_NS = 50000`（20 kHz 采样节拍）、`fastPhaseStep = 1638`（≈499.9 Hz）、`SystemCoreClock` 72000 kHz
- **SWO 通道分配**（与《测试清单》一致）：CH0 数字标记（主循环进/出）、CH1 analog float32（`fastSine`）、CH2 analog u16（`fastTriangle`）
- **ITM 发送位置**：`vtrace_itm_send()` 在 20 kHz 采样中断里（2026-10-10 改；放主循环会因 500 Hz 采样率把 499.9 Hz 混叠成拍频）

---

## 四、被测应用 Variable-Trace

| 项 | 值 |
| --- | --- |
| 版本 | 1.1.0（基于上游 MCUViewer 1.1.0） |
| commit | `67d0c6c8de2a9f2b5b359d8a09d09c4493fecafa` |
| 可执行文件 | `build/Variable-Trace.exe`，257 214 418 字节，构建于 2026-10-10 15:10 |
| MCP 接口 | `http://127.0.0.1:7807/mcp`（裸 JSON-RPC，46 个工具，脚本调用要绕开系统代理） |
| 配置/数据目录 | `%APPDATA%\Variable-Trace\`（`global.json` / `imgui.ini` / `logs\logfile.txt`） |
| 随包运行库 | `JLink_x64.dll` / `glfw3.dll` / `libusb-1.0.dll` / `libdwarf-2.dll` / `libzstd.dll` / `zlib1.dll` |

---

## 五、主机工具链

| 工具 | 版本 | 位置 |
| --- | --- | --- |
| GCC（MinGW-w64，MSYS2） | **15.2.0**（Rev13） | `C:\msys64\mingw64\bin` |
| CMake | **4.4.3** | `C:\Users\<用户名>\.workbuddy\binaries\python\envs\default\Lib\site-packages\cmake\data\bin\cmake.exe` |
| Ninja | **1.13.2** | `C:\Users\<用户名>\AppData\Local\Microsoft\WinGet\Packages\Ninja-build...\ninja.exe` |
| Python（跑测试脚本） | **3.13.14** | `C:\Users\<用户名>\.workbuddy\binaries\python\envs\default\Scripts\python.exe` |

应用构建要点：`-DPLATFORM=winnative`（原生分支默认硬编码 `-fuse-ld=lld`，本机没有）。

---

## 六、其他工具

- **GDB 17.1**（`C:\msys64\mingw64\bin\gdb.exe`）：应用的 ELF 符号解析默认走它（`acquisition.elfParser = "gdb"`，命令 `gdb` 从 PATH 取）。
- **OpenOCD 0.12.0**（2026-03-02，sysprogs 编译版，含 libusb1）：本机装有，本次测试未用到。
- **Keil MDK-ARM Plus 5.36.0.0** + Armcc V5.06 update 7 (build 960)：编译固件用。批量编译：
  ```
  <安装目录>\keilcore\UV4\UV4.exe -j0 -b 0501_led.uvprojx -o <日志>
  ```
  退出码 0 = 无警告，1 = 有警告，≥2 = 有错误。
- **STM32CubeProgrammer：未安装**。因此源码里 ST-Link 的 CubeProgrammer API 探针与原生烧写两项无法链接、无法验证。
- **烧录用例**（本次用 J-Link 直接烧）：
  ```
  JLink.exe 脚本：device STM32F103C8 / si SWD / speed 4000 / connect
                  loadfile <hex> / r / g / q
  ```

---

## 七、测试脚本位置

本机 `MCUViewer-1.1.0/analysis/tmp/`（`analysis/` 未纳入版本库，仓库里没有这些脚本）：

| 文件 | 用途 |
| --- | --- |
| `vtdrv.py` | MCP 调用封装（已绕开系统代理） |
| `inp.py` | 鼠标输入封装（**必须用 `SendInput`，`mouse_event` 在本机无效**） |
| `ui.py` | 窗口查找、截图、长按点击 |
| `t_d_run.py` / `t_d_analyse.py` | 跑一次 SWO 跟踪并分析 CSV |
| `pathtrace*.mcuvproj` | 跟踪侧通道配置（工程文件里写 `trace` 段即可，无需界面操作） |

---

## 八、两个必须记住的坑

1. **J-Link DLL 不是一套**：应用用随包的 **8.10k**，命令行 `JLink.exe` 是 **V6.40**。用 CLI 试出来的结论不能直接套到应用上。
2. **Keil 不在常见路径**：`<安装目录>\keilcore`（不是 `C:\Keil_v5`），编译器在 `ARM\ARMCC\Bin`。
