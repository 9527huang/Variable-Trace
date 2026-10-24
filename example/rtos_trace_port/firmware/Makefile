# mdkdebug「RTOS 任务感知」验证固件（arm-none-eabi-gcc + make）
#
# 这块固件跑真正的 FreeRTOS-Kernel，用来验证 rtos_info / rtos_tasks /
# rtos_objects 三个工具在真机上的解析是否正确（对照见 main.c 里内核自报的
# g_kernel_hwm[]）。
#
# 支持三块板子，用 BOARD 选：
#     make              -> f401（默认，Cortex-M4F，512K Flash / 96K RAM）
#     make BOARD=f103   -> f103（Cortex-M3，64K Flash / 20K RAM）
#     make BOARD=f407   -> f407（Cortex-M4F，1024K Flash / 128K RAM，STM32F407ZGT6）
# 产物分别在 build_f401/、build_f103/、build_f407/ 下，互不干扰。
#
# 另有一个可选变体，叠加在上面：
#     make BOARD=f103 OPT=1  -> build_f103_opt/，打开「优化任务选择」
# 这一档会把 uxTopReadyPriority 的含义从优先级编号改成位图，见 FreeRTOSConfig.h。
#
# 关于三块板子共用一份源码：CPU 标志上 f401 与 f407 完全相同（都是
# Cortex-M4F + FPv4-SP-D16，移植层都是 ARM_CM4F），差别只在链接脚本的容量
# 与堆大小。三块板子都跑复位后的 HSI（F4 系列 16 MHz、F1 系列 8 MHz），
# main.c 不做任何时钟初始化，所以换板子不需要写 PLL 代码。
#
# FreeRTOS 内核随仓库放在 third_party/FreeRTOS-Kernel，Makefile 的默认
# 内核路径指向它，所以直接 make 就能构建。要换内核用：
#     make FREERTOS_KERNEL=/path/to/FreeRTOS-Kernel
# 或设环境变量 MDKDEBUG_FREERTOS_KERNEL。取内核用 tarball 比 git clone 快得多：
#     curl -sL https://github.com/FreeRTOS/FreeRTOS-Kernel/archive/refs/heads/main.tar.gz -o /tmp/frk.tar.gz
#
# **仓库里那份内核是裁过的**（2026-10-13，21 MB 裁到 1.7 MB）。只保留了这个固件
# 用得着的部分：
#     LICENSE.md README.md
#     include/                       全部头文件
#     croutine.c event_groups.c list.c queue.c stream_buffer.c tasks.c timers.c
#     portable/GCC/ARM_CM3/          G_CORTEX_M3 用
#     portable/GCC/ARM_CM4F/         G_CORTEX_M4F 用
#     portable/MemMang/              堆实现，当前用 heap_4.c
# 裁掉的是其它架构与编译器的移植层（IAR、RVDS、ARMv8M、其它 GCC 架构等）。
# 要换成别的芯片或别的编译器，按上面那条命令重取一份完整内核，指向它即可。
#
# 配方行用 '>' 开头（.RECIPEPREFIX），避免制表符在补丁/复制中丢失。
.RECIPEPREFIX = >
.DEFAULT_GOAL := all

MKFILE_DIR     := $(dir $(realpath $(lastword $(MAKEFILE_LIST))))
FREERTOS_KERNEL ?= $(if $(MDKDEBUG_FREERTOS_KERNEL),$(MDKDEBUG_FREERTOS_KERNEL),$(MKFILE_DIR)../../../third_party/FreeRTOS-Kernel)

TOOLCHAIN ?=
CC        := $(if $(TOOLCHAIN),$(TOOLCHAIN)/arm-none-eabi-gcc,arm-none-eabi-gcc)
OC        := $(if $(TOOLCHAIN),$(TOOLCHAIN)/arm-none-eabi-objcopy,arm-none-eabi-objcopy)
SZ        := $(if $(TOOLCHAIN),$(TOOLCHAIN)/arm-none-eabi-size,arm-none-eabi-size)

KERNEL    := $(FREERTOS_KERNEL)

# 通路 B 的目标侧事件环。默认不编进固件：它要求固件动一处（一个 trace 宏），
# 而「不给固件加约束」是这个项目的前提。要试通路 B 时：make EVENTS=1
EVENTS_ON ?= 0
EVENTS_DIR := ../event_ring
EVENTS_SRC :=
EVENTS_INC :=
EVENTS_DEF :=

# RTEC2 事件环：带类型与载荷的 56 字节槽，取代上面那套八字节记录。
# 默认开——它就是「ISR / Fault / Sync / Heap 四类事件」唯一的载体。
RTEC2_ON ?= 1
RTEC2_DIR := ../../rtec2_port
RTEC2_SRC :=
RTEC2_INC :=
RTEC2_DEF :=

ifeq ($(RTEC2_ON),1)
  RTEC2_SRC := $(RTEC2_DIR)/rtec2_writer.c $(EVENTS_DIR)/vtrace_rtec2.c
  RTEC2_INC := -I$(RTEC2_DIR) -I$(EVENTS_DIR)
  RTEC2_DEF := -DVTRACE_RTEC2=1
endif

# 载体：0 = 目标内存里的环（默认），1 = ITM/SWO，2 = RTT 上行缓冲。
# 现状（2026-10-18）：0 端到端可用；1 目标侧就绪、宿主侧的解码器还没写；
# 2 目标侧就绪，但 J-Link DLL 找得到控制块却读不出数据（见 IDebugProbe::startRtt）。
EVENTS_TRANSPORT ?= 0

ifeq ($(EVENTS_ON),1)
  EVENTS_SRC := $(EVENTS_DIR)/vtrace_events.c
  EVENTS_INC := -I$(EVENTS_DIR)
  EVENTS_DEF := -DVTRACE_EVENTS=1 -DVTRACE_TRANSPORT=$(EVENTS_TRANSPORT)
endif

# 三块板子：
#   f401  Cortex-M4F，512K Flash / 96K RAM（默认）
#   f103  本机测试板，Cortex-M3，64K Flash / 20K RAM
#   f407  STM32F407ZGT6，Cortex-M4F，1024K Flash / 128K RAM
# 差别只在 CPU 标志、移植层目录、链接脚本与板级宏；main.c 与
# FreeRTOSConfig.h 三块板共用，源码不复制。
BOARD ?= f401

ifeq ($(BOARD),f103)
  CPUFLAGS   := -mcpu=cortex-m3 -mthumb
  PORT_DIR   := $(KERNEL)/portable/GCC/ARM_CM3
  LDSCRIPT   := linker/f103.ld
  # 事件环 512 条 = 4 KB：这颗片子只有 20 KB RAM，剩下的留给内核与栈。
  BOARD_DEFS := -DPROBE_CPU_HZ=8000000 -DPROBE_HEAP_KB=14 -DVTRACE_EVENT_CAPACITY=512u
else ifeq ($(BOARD),f407)
  # CPU 标志与 f401 逐字相同：同为核心、同一份移植层，只有容量与堆不同。
  # 堆放到 48 KB，为「五十个任务」那一档快照场景留出空间（每个任务约
  # 96 字节控制块加栈，五十个任务约 35 KB）。
  CPUFLAGS   := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  PORT_DIR   := $(KERNEL)/portable/GCC/ARM_CM4F
  LDSCRIPT   := linker/f407.ld
  BOARD_DEFS := -DPROBE_HEAP_KB=48
else
  CPUFLAGS   := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  PORT_DIR   := $(KERNEL)/portable/GCC/ARM_CM4F
  LDSCRIPT   := linker/f401.ld
  BOARD_DEFS :=
endif

# 可选构建变体：打开「优化任务选择」。
#     make BOARD=f103 OPT=1   -> 产物在 build_f103_opt/ 下
# 这个变体只改一个全局符号的语义（uxTopReadyPriority 从优先级编号变位图），
# 不动任何结构布局。任务视图的描述表要按这两种取值分开，见计划 18.3、18.4。
OPT ?= 0
ifeq ($(OPT),1)
  OPT_DEFS     := -DPROBE_OPTIMISED_SELECTION=1
  BUILD_SUFFIX := _opt
else
  OPT_DEFS     :=
  BUILD_SUFFIX :=
endif

BUILD_DIR := build_$(BOARD)$(BUILD_SUFFIX)
TARGET    := $(BUILD_DIR)/freertos_probe

C_SOURCES  := main.c startup.c $(EVENTS_SRC) $(RTEC2_SRC) \
              $(KERNEL)/tasks.c $(KERNEL)/list.c $(KERNEL)/queue.c \
              $(PORT_DIR)/port.c \
              $(KERNEL)/portable/MemMang/heap_4.c
C_INCLUDES := -I. -I$(KERNEL)/include -I$(PORT_DIR) $(EVENTS_INC) $(RTEC2_INC)

CFLAGS   := $(CPUFLAGS) $(BOARD_DEFS) $(OPT_DEFS) $(EVENTS_DEF) $(RTEC2_DEF) -O1 -g3 -Wall -ffunction-sections -fdata-sections -std=c99
LDFLAGS  := $(CPUFLAGS) -T$(LDSCRIPT) -nostartfiles -Wl,--gc-sections -Wl,-Map=$(TARGET).map

# 去掉 -p：本机这份 SysGCC 的 GNU Make 3.82（mingw32 构建）在调用 shell 时会把
# mkdir 的 -p 标志弄丢，于是当前目录里会多出一个名为「-p」的空目录。
# 纯 shell 里 mkdir -p 是正常的，只有经 make 调用才复现。
# 父目录就是本 Makefile 所在目录，一定存在，所以不需要 -p。
# 见 analysis/任务视图-开发计划.md 第二十四节。
$(BUILD_DIR):
> @test -d $(BUILD_DIR) || mkdir $(BUILD_DIR)

# 内核路径没配好时给出人话提示，而不是让 gcc 报一堆 "No such file"
_check_kernel:
> @test -f "$(KERNEL)/tasks.c" || { echo "找不到 FreeRTOS 内核：$(KERNEL)"; echo "用法：make FREERTOS_KERNEL=/path/to/FreeRTOS-Kernel"; echo "获取：git clone --depth 1 https://github.com/FreeRTOS/FreeRTOS-Kernel.git"; exit 1; }

all: $(TARGET).elf $(TARGET).bin $(TARGET).hex

$(TARGET).elf: $(C_SOURCES) $(LDSCRIPT) FreeRTOSConfig.h _check_kernel | $(BUILD_DIR)
> $(CC) $(CFLAGS) $(C_INCLUDES) $(C_SOURCES) $(LDFLAGS) -o $@
> $(SZ) $@

$(TARGET).bin: $(TARGET).elf
> $(OC) -O binary $< $@

$(TARGET).hex: $(TARGET).elf
> $(OC) -O ihex $< $@

.PHONY: all clean _check_kernel
clean:
> $(RM) -r $(BUILD_DIR) ./-p
