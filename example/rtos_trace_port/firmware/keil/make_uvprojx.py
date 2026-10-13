#!/usr/bin/env python3
"""从 0501_led.uvprojx 派生一个 Keil 工程：STM32F103C8 + FreeRTOS 验证固件。

为什么用派生而不是手写：uvprojx 里有一堆必须存在且取值合法的样板
（片内存储器布局、烧写算法、调试器设置、工具链版本），手写容易漏字段导致
UV4 报一句没头绪的错。派生只改我们关心的部分，其余原样保留。
"""
import re
import sys
import xml.etree.ElementTree as ET

TEMPLATE = r"D:/Documents/AICodeProject/stm32103/0703_uart_circle_buffer/MDK-ARM/0501_led.uvprojx"
OUT = r"D:/Documents/AICodeProject/MCUViewer-1.1.0/example/rtos_trace_port/firmware/keil/freertos_probe_f103.uvprojx"

# 从 keil/ 出发的相对路径
K = "../../../../third_party/FreeRTOS-Kernel"
FW = ".."

def find(el, tag):
    return el.find(tag)

def must(el, tag, ctx=""):
    got = el.find(tag)
    if got is None:
        sys.exit(f"模板里找不到必需的字段 {ctx}{tag}")
    return got

tree = ET.parse(TEMPLATE)
root = tree.getroot()
target = must(root, "Targets").find("Target")
if target is None:
    sys.exit("模板里找不到 Target")

# ---- 诊断：确认关键样板在不在 ----
tco = must(target, "TargetOption").find("TargetCommonOption")
if tco is None:
    sys.exit("模板里找不到 TargetCommonOption")
print("模板里的关键字段：")
for name in ("Device", "Vendor", "PackID", "Cpu", "OutputName", "OutputDirectory",
             "CreateExecutable", "CreateHexFile", "DebugInformation", "OnChipMemories"):
    e = tco.find(name)
    print(f"  {name:20s} = {'有' if e is not None else '缺'}"
          + (f"  ({e.text.strip()[:60]})" if e is not None and e.text else ""))
for name in ("uAC6", "ToolsetName", "pCCUsed"):
    e = target.find(name)
    print(f"  {name:20s} = {'有' if e is not None else '缺'}"
          + (f"  ({e.text})" if e is not None and e.text else ""))

# ---- 编译器切到 ARMCLANG（模板原本配的是 ARMCC V5）----
# 模板是 AC5（armcc V5.06）。偏移验证用的是 armclang，两边保持一致更省事，
# 而且 AC6 是 MDK5 的现行编译器。
must(target, "uAC6").text = "1"
must(target, "pCCUsed").text = "6160000::V6.16::ARMCLANG"

# FreeRTOS 的 GCC 移植层用 GNU 风格内联汇编（`: "=r"(x) : ... : "memory"`），
# 模板是从 AC5 工程来的、GNU 扩展是关的（uGnu=0 / v6Lang=5），切到 AC6 后
# 这种写法直接报「expected a ")"」。所以语言档要选 GNU 那一档。
cads_el = target.find("TargetOption/TargetArmAds/Cads")
must(cads_el, "uGnu").text = "1"
must(cads_el, "v6Lang").text = "6"      # AC6 语言档：6 = gnu11

# ---- 删掉 AC5 的编译器路径字段 ----
# 模板里有个 <pArmCC> 指着 ARMCC V5.06。留着它，即使 uAC6=1，UV4 还是会用
# AC5 编译（日志第一行会写 Using Compiler 'V5.06'），于是 FreeRTOS 移植层的
# GNU 风格内联汇编全被 #18 报错挡下。删掉它才会真正走 ARMCLANG。
armcc = target.find("pArmCC")
if armcc is not None:
    target.remove(armcc)
    print("已删除 pArmCC（AC5 残留）")

# ---- 改目标名与输出 ----
# 输出目录用反斜杠风格，与模板一致。写成正斜杠时 UV4 拼出来的产物名会丢掉
# 基名（生成出来的是隐藏文件 .axf 这种）。
must(target, "TargetName").text = "freertos_probe_f103"
must(tco, "OutputName").text = "freertos_probe_f103"
must(tco, "OutputDirectory").text = ".\\Objects\\"
must(tco, "CreateExecutable").text = "1"
must(tco, "CreateHexFile").text = "1"
must(tco, "DebugInformation").text = "1"

# ---- 编译选项：宏与头文件路径 ----
cads = target.find("TargetOption/TargetArmAds/Cads")
if cads is None:
    sys.exit("模板里找不到 Cads")
vc = cads.find("VariousControls")
if vc is None:
    sys.exit("模板里找不到 Cads/VariousControls")
must(vc, "Define").text = "PROBE_CPU_HZ=8000000,PROBE_HEAP_KB=14"
must(vc, "IncludePath").text = ";".join([
    FW,
    f"{K}/include",
    f"{K}/portable/GCC/ARM_CM3",
])

# ---- 重建文件分组 ----
groups_el = must(target, "Groups")
for g in list(groups_el):
    groups_el.remove(g)

def add_group(name, files):
    g = ET.SubElement(groups_el, "Group")
    ET.SubElement(g, "GroupName").text = name
    fs = ET.SubElement(g, "Files")
    for fname, fpath, ftype in files:
        f = ET.SubElement(fs, "File")
        ET.SubElement(f, "FileName").text = fname
        ET.SubElement(f, "FileType").text = str(ftype)
        ET.SubElement(f, "FilePath").text = fpath

add_group("Startup", [("startup_stm32f103xb.s", "startup_stm32f103xb.s", 2)])
add_group("User", [
    ("main.c", f"{FW}/main.c", 1),
])
add_group("FreeRTOS", [
    ("tasks.c",      f"{K}/tasks.c", 1),
    ("list.c",       f"{K}/list.c", 1),
    ("queue.c",      f"{K}/queue.c", 1),
    ("port.c",       f"{K}/portable/GCC/ARM_CM3/port.c", 1),
    ("heap_4.c",     f"{K}/portable/MemMang/heap_4.c", 1),
])

tree.write(OUT, encoding="UTF-8", xml_declaration=True)
print(f"\n已写出 {OUT}")
