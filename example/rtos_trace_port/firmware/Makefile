# mdkdebug「RTOS 任务感知」验证固件（arm-none-eabi-gcc + make）
#
# 这块固件跑真正的 FreeRTOS-Kernel，用来验证 rtos_info / rtos_tasks /
# rtos_objects 三个工具在真机上的解析是否正确（对照见 main.c 里内核自报的
# g_kernel_hwm[]）。
#
# 支持两块板子，用 BOARD 选：
#     make            -> f401（默认，Cortex-M4F，512K Flash / 96K RAM）
#     make BOARD=f103 -> f103（Cortex-M3，64K Flash / 20K RAM）
# 产物分别在 build_f401/ 与 build_f103/ 下，互不干扰。
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

# 两块板子：
#   f401  验证固件原本的目标，Cortex-M4F，512K Flash / 96K RAM（默认）
#   f103  本机测试板，Cortex-M3，64K Flash / 20K RAM
# 差别只在 CPU 标志、移植层目录、链接脚本与两个板级宏；main.c 与
# FreeRTOSConfig.h 两块板共用，源码不复制。
BOARD ?= f401

ifeq ($(BOARD),f103)
  CPUFLAGS   := -mcpu=cortex-m3 -mthumb
  PORT_DIR   := $(KERNEL)/portable/GCC/ARM_CM3
  LDSCRIPT   := linker/f103.ld
  BOARD_DEFS := -DPROBE_CPU_HZ=8000000 -DPROBE_HEAP_KB=14
else
  CPUFLAGS   := -mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard
  PORT_DIR   := $(KERNEL)/portable/GCC/ARM_CM4F
  LDSCRIPT   := linker/f401.ld
  BOARD_DEFS :=
endif

BUILD_DIR := build_$(BOARD)
TARGET    := $(BUILD_DIR)/freertos_probe

C_SOURCES  := main.c startup.c \
              $(KERNEL)/tasks.c $(KERNEL)/list.c $(KERNEL)/queue.c \
              $(PORT_DIR)/port.c \
              $(KERNEL)/portable/MemMang/heap_4.c
C_INCLUDES := -I. -I$(KERNEL)/include -I$(PORT_DIR)

CFLAGS   := $(CPUFLAGS) $(BOARD_DEFS) -O1 -g3 -Wall -ffunction-sections -fdata-sections -std=c99
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
