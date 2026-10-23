"""Author the Chinese translations and emit the table the application reads.

The English side is not typed in by hand: it is taken from the extraction pass
in the order that pass produced, and paired with the translations below by
index. That keeps the keys exact - several of them end in spaces that pad a
column, which is exactly the kind of thing a transcription loses.

A `None` means the string is not language: an id, a symbol, or a proper noun
that reads the same in both languages. Those are neither translated nor wrapped
at the call site.

    python tools/i18n/i18n_table.py    # writes i18n_dict.json

`i18n_sites.json` is kept beside this file because it is the frozen half of the
pairing: the translations below are written against the order it holds, so
regenerating it from today's source would line the two up differently and pair
every translation with the wrong string. It is only replaced on purpose, at the
start of a pass that re-authors the list.

`i18n_gen.py` turns the dictionary this writes into the C++ table.
"""

import io
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SITES = os.path.join(HERE, "i18n_sites.json")

# In the order tools/i18n/i18n_sites.json lists them.
TRANSLATIONS = [
    None,                                              # %s
    "%u 个采样，间隔 %s",
    "%u 个变量，%u 字节",
    "%zu 个%s待保存",
    "（已丢弃较早的 %zu 行）",
    None,                                              # (?)
    "（无输出）",
    "（仅保留最后 %zu 行）",
    "（本计划针对的变量）",
    "*.elf/*.axf 文件:",
    "*.elf/*.axf 解析器:",
    None,                                              # ...
    None,                                              # ...##screenshotDialogSelect
    None,                                              # ...##screenshotSelect
    "<多个变量>",
    None,                                              # @
    None,                                              # first half of a message joined by hand, see EXTRA
    "每一步保持其数值，直到下一步开始。",
    "API 服务",
    "中止",
    "中止##testFlashAbort",
    "关于",
    "采集设置...",
    "采集设置",
    "添加",
    "添加计划",
    "添加变量",
    "允许 API 写入目标",
    "外观",
    "作用于整个界面，包括绘图区。",
    "在采样之后、解释之前应用变换 [(value >> shift) & mask]。",
    "每次询问位置:##askForScreenshotLocation",
    "自动从 TI C2000 代码生成工具 / Code Composer Studio 的安装目录中查找。未能自动找到时，在此手动指定。",
    "自动保存工程",
    "波特率:",
    "检测到 C2000 目标",
    "取消",
    "如有需要可改用其他 GDB 版本。所填程序会被运行，用于更新变量地址。",
    "通道",
    "选择是否从 *.elf/*.axf 文件自动更新变量地址与大小。",
    "选择是否另行指定一个用于采样的变量名。",
    "选择 ELF 解析器。",
    "清空列表",
    "点击上方按钮测试烧录命令。",
    "点击打开采集设置。",
    None,                                              # first half of a message joined by hand, see EXTRA
    "复制",
    "复制到记录器组",
    "复制到采样组",
    "光标:",
    "调试器序列号:",
    "调试器:",
    "删除",
    "删除",
    "删除组",
    "删除图",
    "删除变量",
    "检测",
    "已关闭",
    "是否保存当前配置？\\n",
    "完成",
    "下载",
    "边沿:",
    "可以直接输入新的基数（乘数），也可以从已导入变量中选一个被定义为基数的变量。",
    "启用 MCP 服务##mcp",
    "启用记录器",
    "导出目录:##screenshotDialogDirectory",
    "导出目录:##screenshotDirectory",
    "导出绘图到 *.csv",
    "文件",
    "文件名:##screenshotDialogFileName",
    "文件名:##screenshotFileName",
    "固件文件:",
    "烧录输出",
    "烧录命令输出:",
    "烧录命令:",
    "烧录",
    "烧录设置",
    "GDB 命令:",
    "GDB: 稳定可靠，默认。DWARF: 变量地址更新更快（测试版）。C2000: 面向 TI C2000 目标，使用 ofd2000。",
    "编辑组",
    "组: %s",
    "帮助",
    "端口被占用时，改用下一个空闲端口。",
    "导入变量",
    "导入功能仍处于测试阶段。如果在列表里找不到你的变量，请在 GitHub 上提一个 issue（记得附上你的 *.elf/*.axf 文件）。",
    "从 *.elf/*.axf 导入变量",
    "文件名递增:##incrementFileName",
    "文件名递增:##screenshotDialogIncrement",
    "指示器",
    "指示器用于评估跟踪波形的质量。出现红色指示器说明某一帧可能被错误解析，此时可以试着提高跟踪预分频，或减少启用的跟踪通道数量。",
    "把整数按定点数解释。",
    "强烈建议切换到 C2000 解析器",
    "保留当前解析器",
    "串口的线路速率，必须与目标固件为其 UART 配置的波特率一致。",
    "正在监听 %s（%zu 个工具）",
    "把所有已注册变量的数值记录到指定目录。文件会自动创建，并在每次开始采集时覆盖。",
    "记录到文件:",
    "日志目录:",
    "单条曲线的最大点数，超出后最早的点会被覆盖。",
    "单条曲线在视口中直接显示、不滚动的最大点数。",
    "最大点数:",
    "视口最大点数:",
    "最高采样频率。能否达到取决于所用的调试器。",
    "模式:",
    "未找到。请手动设置 ofd2000 路径",
    "名称必须唯一。如果在下面手动指定，它可以与跟踪的变量名不同。",
    "为该变量可能的取值命名。没有命名的取值会以数字显示。",
    "新建",
    "否",
    "不可用",
    "尚未检测到。",
    "未测试",
    "确定",
    "打开",
    "最近打开",
    "选项",
    "请先在采集设置中启用记录器，才能新建记录器组",
    "请先在采集设置中启用记录器，才能把该组复制为记录器组",
    "图",
    "编辑图",
    "绘图树",
    "绘图导出",
    "绘图",
    "预触发:",
    "偏好设置",
    "工程",
    "属性",
    "退出",
    "记录器",
    "记录器组",
    "*.elf/*.axf 变更时刷新变量:",
    "版本发布",
    None,                                              # first half of a message joined by hand, see EXTRA
    "SWD 速度 [kHz]:",
    "采样值:",
    "触发前保留的采样数。采集窗口总是结束于触发点之后这么多采样。",
    "采样率 [Hz]:",
    "采样组",
    "保存",
    "另存为..",
    "保存绘图",
    "保存绘图为 *.png",
    "保存？",
    "新建前保存？",
    "当工程已修改且已有路径时，自动保存工程文件。",
    "选择普通或高速采样（HSS）模式",
    "从 *.elf/*.axf 文件中选择或输入一个已导入的变量名。",
    "选择区间:",
    "选择调试器类型与序列号，START 按钮才会可用。",
    "请在 选项->采集设置 中选择 *.elf/*.axf 文件，以导入或更新变量。",
    "所选文件路径会替换下方烧录命令中的 {file} 宏。",
    "设置",
    "跳过采样:",
    "来源:",
    "开始",
    "统计",
    "统计:",
    "停止",
    "*.elf/*.axf 变更时停止:",
    "支持 <3",
    "切换到 C2000 解析器（推荐）",
    "目标名称:",
    "测试烧录",
    "测试输出:",
    "缺少 *.png 扩展名时会自动补上。",
    "该路径会替换命令中所有 {file}。命令里没有 {file} 时，追加到末尾。",
    "采集时可见的跟踪时间占比。由于采样周期不固定，以百分比表示。",
    "记录器在目标的定时器中断里采样，并把采样保存在自己的内存中，运行期间不做任何流式传输。",
    "所选文件看起来是 TI C2000 目标二进制。",
    "主题##theme",
    "在检测到记录器之前，时基未知。",
    "时间##add",
    "超时 [s]:",
    "超过这段时间没有收到跟踪数据，跟踪就会被停止。",
    "跟踪绘图",
    "跟踪查看器",
    "取消勾选某个通道即可停用它，释放一部分 SWO 引脚带宽。",
    "使用 *.elf/*.axf 文件:",
    "数值##add",
    "数值:",
    "变量查看器",
    "编辑变量",
    "版本 %u.%u，时基 %u ns，缓冲区 %u 个元素，最多 %u 个变量，浮点 %s",
    "视口宽度 [%%]:",
    "勾选后，{file} 会被 选项 -> 采集 中设置的 *.elf/*.axf 路径替换。",
    "启用后，每次截图都会单独弹出另存为对话框，下面两项只是它的初始值。",
    "启用后，若在设定时间内没有任何输出，烧录会被自动中止。",
    "启用后，写入超出指定范围的数值会被拒绝。",
    "窗口",
    "有多个绘图时，为每个文件编号，避免互相覆盖。",
    "不启用时，set_variable_value 会被拒绝。此外每个变量还可以各自设置最小/最大范围。",
    "写计划器",
    "X 轴标签:",
    "X 轴变量:",
    "Y 轴标签:",
    "是",
    "地址:",
    "别名      ",
    "以及其他 %zu 项",
    "基类型:",
    "基数:",
    "二进制",
    "副本",
    "核心频率 [kHz]   ",
    "十进制",
    "域         ",
    "字段:",
    None,                                              # flashOut
    "字体大小",
    "小数位:",
    "十六进制",
    "间隔 [s]",
    None,                                              # left pane
    "掩码:",
    "最大:",
    "最小:",
    "名称:",
    "ofd2000 路径:",
    "ofd2000 状态:",
    "父组:",
    "端口##mcp",
    "搜索 ",
    "选择...",
    "右移:",
    "复位条件   ",
    "指定跟踪名:",
    "跟踪预分频",
    "跟踪变量:",
    "触发通道",
    "触发电平",
    "类型",
    "类型:",
    "从 *.elf/*.axf 更新:",
    "变量:",
    "写入限制:",
    None,                                              # x
]

# Labels whose column was written into the string as a run of trailing spaces.
# `drawTextAlignedToSize` and `padToColumn` measure that column in pixels at
# the call site now, because a space in the Chinese font is not the width of a
# space in the Latin one and the count would land the control somewhere else.
# The spaces therefore come out of the key and out of the translation with it.
COLUMNS = [
    "alias",
    "core frequency [kHz]",
    "domain",
    "search",
    "should reset",
    "trace prescaler",
    "trigger channel",
    "trigger level",
    "type",
]

# Strings that never reach the extraction pass: the connection indicator builds
# its text into a variable, the tables name their columns through
# TableSetupColumn, and a few places pad a label by hand. They are added here by
# their exact text.
EXTRA = {
    "Disconnected": "未连接",
    "Connected": "已连接",
    "Connecting": "连接中",
    "Disconnecting": "断开中",
    "Variable Viewer acquisition": "变量查看器采集",
    "Trace Viewer acquisition": "跟踪查看器采集",
    "sampling: %.2f Hz": "采样率: %.2f Hz",
    "sampling: 1000000.00 Hz": "采样率: 1000000.00 Hz",
	"CSV active": "CSV 正在记录",
	"CSV idle": "CSV 空闲",
	"CSV stopped": "CSV 已停止",
	"CSV status": "CSV 状态",
	"Queue overload": "队列过载",
	"File open failed": "文件打开失败",
	"File write failed": "文件写入失败",
	"Queue": "队列",
	"Queue:": "队列:",
	"Peak": "峰值",
	"Samples:": "样本:",
	"Written": "已写入",
	"Failed": "写入失败",
	"Rejected": "已拒绝",
	"Reason": "原因",
	"Reason:": "原因:",
	"Status:": "状态:",
	"Failure time:": "失败时间:",
	"Probe latency": "探针延迟",
	"Address fragmentation": "地址碎片",
	"Render contention": "绘图争用",
	"CSV backlog": "CSV 积压",
	"Deadline limited": "期限受限",
	"No bottleneck": "无性能瓶颈",
	"Warming up": "正在预热",
	"Bottleneck": "性能瓶颈",
	"Requested / actual": "请求 / 实际",
	"Rate attained": "速率达成",
	"Deadline misses": "期限丢失",
	"Probe time": "探针耗时",
	"Buffer wait": "缓冲区等待",
	"CSV queue": "CSV 队列",
    "State": "状态",
    "Probe": "调试器",
    "Serial port": "串口",
    "Serial number": "序列号",
    "Baudrate": "波特率",
    "SWD speed": "SWD 速度",
    "Target name": "目标名称",
    "Mode": "模式",
    "NORMAL": "普通",
    "Variables": "变量",
    "Name": "名称",
    "Address": "地址",
    "Type": "类型",
    "visible plot": "张可见的图",
    "visible plots": "张可见的图",
    "Curve": "曲线",
    "Bar": "柱状图",
    "Table": "表格",
    "XY": "XY",
    "Delete": "删除",
    "File": "文件",
    "Error": "错误",
    "Info": "提示",
    "Project could not be saved!": "工程未能保存！",
    "Legacy project could not be read!": "旧版工程无法读取！",
    "Project files": "工程文件",
    "Legacy projects": "旧版工程",
    "All files": "所有文件",
    "Variable-Trace | ": "Variable-Trace | ",
    "Save": "保存",
    "Cancel": "取消",
    "Yes": "是",
    "No": "否",
    "Refresh": "刷新",
    "Disabled": "已关闭",
    "Autosave project": "自动保存工程",
    "Language##language": "语言##language",
    "English": "English",
    "Chinese": "中文",
    "Applies to the whole interface. The panels keep their places.": "作用于整个界面。各面板保留原来的位置。",

    # The column headers of the plot, enum and write planner tables; the
    # variable and import tables use the three above. The ids the saved column
    # widths are filed under are taken from the English at the call site, so
    # translating these does not cost the widths.
    "Read value": "读取值",
    "Write value": "写入值",
    "Label": "标签",
    "Value": "数值",
    "Time": "时间",

    # The two halves of the trace domain.
    "analog": "模拟",
    "digital": "数字",

    # The heading over the variable table, drawn through the centring helper
    # rather than through ImGui::Text, so the extraction pass does not see it.
    "Variables": "变量",

    # The rest of the headings the panels draw through the centring helper, and
    # the sentences they use to say a pane is empty. Same reason as above.
    # The three About lines stay in English: they are a signature, and the
    # project name and the upstream author's name are not translated.
    "General": "通用",
    "Debug Probe": "调试器",
    "Logging": "日志记录",
    "Advanced": "高级",
    "Target": "目标",
    "Trigger": "触发",
    "Capture": "捕获",
    "Group": "组",
    "Plans": "计划",
    "Steps": "步骤",
    "Postprocessing": "后处理",
    "Interpretation": "解释",
    "No plan selected": "未选择计划",
    "Select a plan to edit": "选择要编辑的计划",
    "No plot selected": "未选择图",
    "Variable-Trace": "Variable-Trace",
    "by Piotr Wasilewski (klonyyy)": "by Piotr Wasilewski (klonyyy)",
    "based on MCUViewer 1.1.0 (GPLv3)": "based on MCUViewer 1.1.0 (GPLv3)",

    # The one-line warnings the acqusition and recorder panels draw centred.
    "Target name is empty, the J-Link cannot connect. Enter the device name, e.g. STM32F103C8.": "目标名称为空，J-Link 无法连接。请输入器件名称，例如 STM32F103C8。",
    "This build was compiled without the DWARF library, so this parser cannot read a file.": "此构建编译时未包含 DWARF 库，该解析器无法读取文件。",
    "The recorder is not available in this build.": "此构建中没有记录器。",
    "There is no active recorder group.": "没有活动的记录器组。",
    "Add a group of type 'recorder' in the plot tree and make it the active one.": "在绘图树中新建一个「记录器」类型的组，并把它设为活动组。",
    "The acquisition is stopped. Start it first, the recorder uses that connection.": "采集已停止。请先启动采集，记录器要使用那条连接。",
    "Select firmware file and flash command to use for flashing.": "选择用于烧录的固件文件与烧录命令。",

    # The SWO quality indicators. The run of spaces each one ends in is the
    # column its number is printed in; drawDescriptionWithNumber reads that
    # run off the English and pads the translation to the same width.
    "frames total:           ": "帧总数:",
    "sleep cycles:           ": "休眠周期:",
    "error frames total:     ": "错误帧总数:",
    "error frames in view:   ": "视野内错误帧:",
    "delayed timestamp 1:    ": "延迟时间戳 1:",
    "delayed timestamp 2:    ": "延迟时间戳 2:",
    "delayed timestamp 3:    ": "延迟时间戳 3:",
    "delayed timestamp 3 in view:    ": "视野内延迟时间戳 3:",

    # The two long messages the source splits over two literals so the line fits.
    # C++ joins the pieces before tr sees them, so the key is the whole message
    # and the two halves no longer exist as strings of their own - the entries
    # they used to occupy in the list above are marked None.
    "A local HTTP endpoint that exposes the project, the variable table and the acquisition as tools. The server listens on the loopback interface only. It is off until it is switched on here.": "一个本地 HTTP 接口，把工程、变量表与采集作为工具暴露出来。服务器只监听回环接口。只有在这里打开之后它才会启动。",
    "Run captures continuously and keeps the newest samples. Normal and single wait for the trigger; single stops after one capture, normal can be started again.": "连续模式持续采集并保留最新采样。普通与单次模式等待触发；单次模式捕获一次后停止，普通模式可以再次启动。",
    "Copies the whole buffer out of the target and rebuilds the samples from the ring state. The sampling loop pauses for the duration of the copy.": "把整个缓冲区从目标读出，并依据环形状态重建采样。复制期间采样循环会暂停。",

    # The messages the popups carry. They are built by popup.show rather than
    # drawn through an ImGui call, so the extraction pass does not see them.
    "Error!": "错误！",
    "Warning!": "警告！",
    "Saving successful!": "保存成功！",
    "Selected path contains spaces!": "所选路径包含空格！",
    "No debug probes found!": "未找到调试器！",
    "Legacy project imported. Use Save to store it as .mcvproj.": "旧版工程已导入。使用「保存」把它存为 .mcvproj。",
    "Group already exists!": "组已存在！",
    "Plot already exists!": "图已存在！",
    "Variable already exists!": "变量已存在！",
    "Tracked variable doesn't exist!": "被跟踪的变量不存在！",
    "Min value cannot be greater than max value!": "最小值不能大于最大值！",
    "Max value cannot be less than min value!": "最大值不能小于最小值！",
    "Label cannot be empty!": "标签不能为空！",
    "Label already exists!": "标签已存在！",
    "Value must be a valid integer!": "数值必须是有效的整数！",
    "A plan with that name already exists!": "已存在同名计划！",
    "Update error. Please check the *.elf/*.axf file path!": "更新出错。请检查 *.elf/*.axf 文件路径！",
    # Written with the backslash doubled: the key is the source text, which
    # holds the two characters `\` and `n`, not a newline.
    "The following variables were already imported: \\n": "以下变量已经导入过：\\n",
    "No device was picked in the J-Link dialog, the current Target name was kept.": "J-Link 对话框中没有选择器件，保留了当前的目标名称。",

    # The flash window.
    "A flash is already in progress.": "烧录已在进行中。",
    "A flash is in progress. See the Flash Output window.": "正在烧录。请查看烧录输出窗口。",
    "Set a flash command above first.": "请先在上方设置烧录命令。",
    "Set a flash command in Options -> Flashing.": "请在「选项 -> 烧录」中设置烧录命令。",
    "No firmware file configured. Provide 'file' or set it in Options -> Flashing.": "未配置固件文件。请提供 file 参数或在「选项 -> 烧录」中设置。",
    "Runs the flash command from Options -> Flashing on the firmware file.": "对固件文件运行「选项 -> 烧录」中的烧录命令。",
    "Flashing in progress...": "正在烧录...",
    "Flash successful!": "烧录成功！",

    # The plot export dialog.
    "Plot image save canceled.": "已取消保存图像。",
    "No visible plot to save.": "没有可见的图可以保存。",
    "Failed to save plot image: file handler is not available for Save As dialog.": "无法保存图像：另存为对话框的文件处理不可用。",
    "Failed to save plot image: invalid screenshot data returned by renderer.": "无法保存图像：渲染器返回了无效的截图数据。",
    "Empty means the directory the program was started from.": "留空表示程序启动时所在的目录。",
    "Increment file name is off, so every plot is written to the same file and the last one is the one that survives.": "文件名递增已关闭，因此每张图都写入同一个文件，只有最后一张会保留。",
    " plot image.": " 张图片。",
    " plot images.": " 张图片。",

    # The two settings panels, which pick their heading between two written-out
    # alternatives rather than through a lookup of their own.
    "These settings belong to the VARIABLE VIEWER": "这些设置属于变量查看器",
    "These settings belong to the TRACE VIEWER": "这些设置属于跟踪查看器",
    "Debug probe, SWD speed and Target name here drive the RAM variable acquisition.": "这里的调试器、SWD 速度与目标名称驱动 RAM 变量采集。",
    "Debug probe, SWD speed and Target name here drive the SWO trace acquisition only.": "这里的调试器、SWD 速度与目标名称只驱动 SWO 跟踪采集。",
    "Serial port:": "串口:",
    "ofd2000 executable": "ofd2000 可执行文件",
    "Elf files": "符号文件",
    "PNG image (*.png)": "PNG 图像 (*.png)",
    "NOT FOUND!": "未找到！",
    "variable does not exist!": "变量不存在！",
    "Not listening: %s": "未在监听: %s",

    # Wording that no pattern reaches: a label built by snprintf, a combo whose
    # items are an array, a name kept in a std::string, a row drawn by one of
    # the small local helpers, an axis of an inline plot.
    "Flash": "烧录",
    "Update variable addresses": "更新变量地址",
    "Click to reload *.elf/*.axf changes!": "点击重新加载 *.elf/*.axf 的变更！",
    "Select Variables": "选择变量",
    "top level": "顶层",
    "(no label)": "（无标签）",
    "version: ": "版本: ",
    "time [s]": "时间 [s]",
    "time[s]": "时间[s]",
    "value": "数值",
    # The badge the viewer draws over the plot area, and the state the button
    # under it carries. Both name a state rather than quote a menu.
    "RUNNING": "运行中",
    "STOPPED": "已停止",
    # The control a plot puts up once the reader has moved its view away from
    # the newest data, and the button under the trace that brings it back.
    "Back to live": "回到实时",
    "mean:": "平均:",
    "stddev:": "标准差:",
    "delta (max-min):": "极差 (最大-最小):",
    "%.4g s per sample (%.4g Hz)": "%.4g 秒/采样 (%.4g Hz)",
    "run": "连续",
    "normal": "普通",
    "single": "单次",
    "rising": "上升沿",
    "falling": "下降沿",
    "curve": "曲线",
    "bar": "柱状图",
    "table": "表格",

    # What the recorder window reports below its controls. The line is drawn
    # with TextWrapped over a std::string, so only the wording here decides
    # what the reader sees.
    "The recorder was disabled and the target was stopped.": "记录器已关闭，目标已停止。",
    "The recorder was disabled, but the target could not be stopped: ": "记录器已关闭，但目标无法停止: ",
    "The recorder could not be restarted: ": "记录器无法重新启动: ",
    "Restarted with the new %s.": "已按新的 %s 重新启动。",
    "The symbol table has no ____recorderSettings or ____recorder. Import the variables from an *.elf/*.axf built with the recorder sources.": "符号表中没有 ____recorderSettings 或 ____recorder。请从按记录器源变量构建的 *.elf/*.axf 导入变量。",
    "Recorder detected.": "已检测到记录器。",
    "Recorder detected. %s.": "已检测到记录器。%s。",
    "The target was stopped.": "目标已停止。",
    "Recording. The target fills its buffer on its own, press Download when it has filled.": "正在记录。目标自行填充缓冲区，填满后按「下载」。",
    "The target is sampling.": "目标正在采样。",
    "The target is idle.": "目标空闲。",
    # The four trigger states, shown where the target state is shown. The
    # last one is the wording the upstream window uses while it waits.
    "The trigger has fired.": "触发已发生。",
    "Waiting for trigger...": "正在等待触发……",
    # The two halves of a sentence with the mode name in the middle. The name
    # comes from the combo above it, which is already translated by then.
    "Mode '": "模式「",
    "' waits for a trigger. Pick a source variable below first.": "」需要触发。请先在下方选择触发源变量。",
    # The trigger source combo only lists plain variables. A worked out one has
    # no bytes of its own on the target, so the target cannot compare it.
    "No variable of this group can be a trigger source.": "本组中没有变量可以作为触发源。",
    "A variable worked out from another one has no bytes of its own on the target, so the recorder cannot compare it. Add a plain variable to the group to trigger on it.": "由其他变量算出的变量在目标上没有自己的字节，记录器无法比较它。请在组里加入一个普通变量作为触发源。",
    # Whether the copy lost samples. Drawn under the sample count.
    "Older samples were overwritten before the copy.": "复制之前较早的样本已被覆盖。",
    "No samples were lost.": "没有样本丢失。",
    # The target reported a start that was not where a sample begins, so the
    # window was moved down to the boundary below it before the bytes were
    # split. The values are still good; only the first sample may be later.
    "The capture start did not sit on a sample boundary, so it was moved down to the nearest one.": "捕获起点未落在样本边界上，已向下移动到最近的边界。",

    # The plot export messages, and the short descriptions the persistence
    # layer returns for a project it could not open. Those four are the one
    # place a message from outside the interface reaches the screen, so they
    # are put through the table where they are shown rather than where they are
    # written: the same call answers the API and the log.
    "Saved ": "已保存 ",
    "Failed to save plot image to: ": "图像保存失败，路径: ",
    "The server could not be started: ": "服务器无法启动: ",
    "cannot open file": "无法打开文件",
    "not a valid project file": "不是有效的工程文件",
    "created by a newer version": "由更新的版本创建",
    "unknown error": "未知错误",

    # Keeping the probe connection open past a run. Keyed rather than
    # positional, so it goes in here and i18n_sites.json is left alone.
    "Keep connection:": "保持连接:",
    "Keeps the connection to the probe open when a run ends. Closing it halts the target on some setups, and that halt is what the next run has to undo before it can read anything.": "停止采集后不关闭与探针的连接。部分目标在关闭连接时会把内核停住，而那个停顿正是下一次采集要先解开的。",
    "Viewport width [ms]:": "视窗宽度 [毫秒]:",
    "How much time the viewport shows while acquisition is running. What can be shown is bounded by the samples that are kept: Max points at the sampling frequency.": "采集时视窗显示多长一段时间。真正能显示多少由保留下来的样本决定，也就是 Max points 除以采样率。",

    # Reaching a J-Link over the network, and running a script of one's own.
    "Connection:": "连接方式:",
    "How the J-Link is reached: over USB, or over the network at the address below.": "J-Link 怎么连:USB,或者走网络并用下面的地址。",
    "IP address:": "IP 地址:",
    "A probe reached this way is picked by its address instead of by its serial number.": "这样连接的探针按地址选取,不再按序列号。",
    "JLink script:": "J-Link 脚本:",
    "Runs a J-Link script of your own before the target is connected - for targets that need a watchdog stopped or a reset in a particular order.": "连接目标之前先运行一份自己的 J-Link 脚本——有些目标需要先关看门狗,或者按特定顺序复位。",
    "script file:": "脚本文件:",

    # Reading through a GDB server somebody else started.
    "GDB server:": "GDB 服务器:",
    # The connection indicator names it in a row of its own, the way the rows
    # beside it are named, so the label carries no colon of its own.
    # The button in the table plot's write cell. The acquisition window already
    # has a "Write value" column heading, which is translated separately.
    "write": "写入",
    "type a name": "输入器件名",
    "GDB server": "GDB 服务器",
    "Port": "端口",
    "Address of the machine running the GDB server. OpenOCD, and the servers shipped with probes, both speak the protocol this reads through.": "运行 GDB 服务器那台机器的地址。OpenOCD 与各家探针自带的服务器都说这套协议,本软件就是通过它读内存的。",
    "Port:": "端口:",
    "Port the GDB server serves on. OpenOCD's default is 3333.": "GDB 服务器提供服务的端口。OpenOCD 默认是 3333。",
    "No GDB server address, the acquisition cannot connect without one.": "没有填 GDB 服务器地址,采集无法连接。",
    # The command channel. Its help text is long enough that the extraction pass
    # reads it as prose rather than as one literal, so it is named here instead.
    "Command channel": "命令通道",
    "Enable command channel##command": "启用命令通道##command",
    "port##command": "端口##command",
    "A plain TCP line protocol for callers that cannot speak HTTP, such as a PLC or a console script. Each line is one JSON request naming a tool; each reply is one JSON line. It runs the same tools as the API server.": "面向无法使用 HTTP 的调用方的纯 TCP 行协议,例如 PLC 或控制台脚本。每行是一次 JSON 请求,指明要调用的工具;每次回复是一行 JSON。它运行的工具与 API 服务相同。",
    "Listening on %s": "正在监听 %s",
    "Needs the API server to be enabled.": "需要先启用 API 服务。",
    # The Help menu and the About box.
    "User Guide": "用户指南",
    "a modified version of MCUViewer 1.1.0, under the GPLv3": "MCUViewer 1.1.0 的修改版,以 GPLv3 分发",
    "Source": "源码",
    "License": "许可证",

    # The task view's stack column. The numbers are worked out on this side
    # from the pattern a new stack is filled with, so the one thing that has to
    # be said is what to do when the target fills nothing, and how old a
    # reading is - the sweep takes a few tasks a round.
    "Stack used": "栈占用",
    "Task Viewer": "任务查看器",

    # The timeline strip under the task table. The state names it counts are
    # not translated: they come from the same `toString` the table's column
    # uses, so the two can never disagree about what a state is called.
    "Timeline": "时间线",

    # The task states, as `RtosTableModel::taskStateLabel` writes them. The API
    # keeps the English from `toString`; only the screen comes through here.
    "running": "运行",
    "ready": "就绪",
    "blocked": "阻塞",
    "suspended": "挂起",
    "pending ready": "待就绪",
    "deleted": "已删除",
    "unknown": "未知",
    "click a time to pin it": "点一下时间轴可钉住某一刻",
    "The timeline fills up as rounds are published.": "时间线随快照发布逐步填满。",
    "Nothing was published in this window.": "这个窗口里没有已发布的快照。",
    "How many seconds of history to show.": "显示多少秒的历史。",
    "seconds back": "秒之前",
    "one mark per round, %.2f s apart": "每轮一个标记,间隔 %.2f 秒",
    "one mark per round": "每轮一个标记",
    "No tasks in this round.": "这一轮里没有任务。",
    "at": "时刻",
    "not read": "个没读到",
    "blank": "空白",
    "- the round did not read that task, which is not the same as idle": "——那一轮没读到这个任务,不等于它空闲",
    "One mark is one round, not an interval: what was true at that instant, and nothing about the time between two of them.": "一个标记就是一轮,不是一段区间:它只说明那一刻为真,两轮之间的事一概不知道。",
    "thin line": "细线",
    "runs from the target's switch records, %.0f s held": "区间来自目标的切换记录,保留 %.0f 秒",
    "back to latest": "回到最新",
    "priority inheritance": "优先级继承",
    "ready, never switched in": "就绪但从未被切入",
    "waiting with no timeout": "无超时等待",
    "Blocked with no timeout: the kernel keeps the state of a task that waits forever on its suspended list, because there is no time to wake it at. It runs again only when the object is given, and if that never happens it waits for as long as the target is powered.": "阻塞且没有超时:内核把「永远等下去」的任务的状态项放在挂起表上,因为没有唤醒时刻可放。它只有在那个对象被给出时才会再跑;若始终不给,它就等到断电为止。",
    "for %.1f s over %u rounds": "持续 %.1f 秒,来自 %u 轮",
    "A task higher in priority than the one that ran, ready in every round of the last couple of seconds. A preemptive scheduler cannot do that, except while the running task holds a lock someone above it wants - and that case is not reported here.": "有一个比实际在跑的那个优先级更高、且最近两秒每一轮都是就绪的任务。抢占式调度器不该这样——唯一合法的情况是正在跑的任务持着它想要的锁(那时按继承的优先级在跑),那种情况这里不报。",
    "suspected deadlock": "疑似死锁",
    "Two or more tasks are each waiting for a lock the next one holds. A round is a sample: if the ring clears between two of them it leaves no trace, which is why this is suspected rather than proven.": "两个或更多任务各自在等下一个任务手里的锁。一轮只是一个采样:环若在两轮之间自行解开就不留痕迹,所以说疑似而不说确证。",
    "The task on the right is running above the priority it was created with, which the kernel only does while a higher priority task waits for a lock it holds. The lock is how long it has been held.": "右边那个任务正以高于创建时的优先级运行,内核只在有更高优先级任务等它持的锁时这样做。这把锁就是它持有了多久。",
    "unpin": "取消固定",
    "%u samples, %.2f s apart, %u unread": "%u 个采样,间隔 %.2f 秒,%u 处没读到",
    "%u samples, %u unread": "%u 个采样,%u 处没读到",
    "A round that could not read one of the kernel's lists leaves its tasks out of that sample. The gap is not a task that was idle.": "某一轮读不到内核的某张链表时,表上的任务就不在这个采样里。那个空档不是「任务空闲」。",
    "following %u plot cursor(s)": "跟随 %u 个图光标",
    "follow plot cursors": "跟随图光标",
    "a cursor in the variable view": "变量视图里的光标",

    # The stack water mark alert.
    "stack alert at": "栈告警阈值",
    "%u tasks": "任务 %u 个",
    "stack alerts": "个栈告警",
    "filtered": "（已筛选）",
    "priority inherited": "个优先级继承",
    "A task whose current priority is above its base one is holding a lock that a higher priority task is waiting for.": "当前优先级高于基础优先级的任务,正持有一个更高优先级任务在等的锁。",
    "stack alerts only": "只看栈告警",
    "clear filters": "清除筛选",
    "reset sort": "重置排序",
    "A task whose stack is this empty or less is marked. Zero turns the marking off, and a task with no reading is never marked.": "栈剩余空间小于等于该百分比的任务会被标记。填 0 关闭标记;没有读数(没扫到)的任务永远不会被标记。",
    "%u%% of this stack is still free, at or below the alert threshold of %u%%.": "这个栈还有 %u%% 空闲,已达到或低于 %u%% 的告警阈值。",
    "The mark is on the used part, which is what the row shows.": "标记在已用部分上,那是这一行显示的数字。",

    # The CPU column. The numbers are a share of what the tasks used, not of
    # the wall clock, so the tooltip says what the whole is.
    "CPU": "CPU 占用",
    "Task share": "任务时间占比",
    "The target was not built with run time statistics (configGENERATE_RUN_TIME_STATS), so how much of the time each task used cannot be worked out.": "目标没有启用运行时间统计(configGENERATE_RUN_TIME_STATS),因此算不出每个任务占用了多少时间。",
    "The run time counter did not move over the last window, so the shares cannot be worked out. The timer behind it is probably not running.": "运行时间计数器在上一个窗口里没有变化,因此算不出占比。它背后的定时器很可能没有运行。",
    "Share of the time the kernel counted across all tasks over the last %.2f s.": "在所有任务被计到的时间里的占比,窗口为最近 %.2f 秒。",
    "%d of the %d tasks the window knows about could be measured, over the last %.2f s.": "窗口见过的 %d 个任务里有 %d 个测得到,窗口长度为最近 %.2f 秒。",
    "The shares are of the time those tasks used. A task the round could not read has no counter to add to the whole, and time the kernel attributes to no task - the interrupts mostly - was never in it.": "占比是这些任务用掉的时间里的份额。本轮读不到的任务没有计数器可以加进总数,而内核不计到任何任务上的时间(主要是中断)本来就不在里面。",
    "Time the kernel attributes to no task, the interrupts mostly, is not part of the whole.": "内核不计到任何任务上的时间(主要是中断)不在这个总数里。",

    # The line above the table that says how the round was read. It is drawn
    # plainly every round and only turns into a warning above the snapshot's
    # own limit, so the loud wording has to carry the conclusion in words too.
    "Read over %.1f ms; every kernel list held together.": "读取历时 %.1f 毫秒;每个内核链表都前后一致。",
    "Read over %.1f ms; %d place(s) caught the target moving.": "读取历时 %.1f 毫秒;有 %d 处读到目标正在变动。",
    "Read over %.1f ms; %d place(s) caught the target moving, so the parts of this table are not all from one moment.": "读取历时 %.1f 毫秒;有 %d 处读到目标正在变动,这张表各部分并非取自同一时刻。",
    "The target does not fill its stacks, so how much of one is used cannot be worked out. FreeRTOS fills them when configCHECK_FOR_STACK_OVERFLOW is above 1, or configUSE_TRACE_FACILITY, or INCLUDE_uxTaskGetStackHighWaterMark, or INCLUDE_uxTaskGetStackHighWaterMark2 is set.": "目标没有填充栈,因此算不出每个栈用了多少。FreeRTOS 在下面任一条件成立时会填充:configCHECK_FOR_STACK_OVERFLOW 大于 1、configUSE_TRACE_FACILITY、INCLUDE_uxTaskGetStackHighWaterMark、INCLUDE_uxTaskGetStackHighWaterMark2。",
    "Scanned on this side over %d task(s); the oldest reading is %.1f s old.": "本机侧扫描,覆盖 %d 个任务;最旧的读数已过 %.1f 秒。",
    "A few tasks are scanned per round, so the readings are not all from the same moment. A reading marked with a leading <= stopped at the scan limit, so the used part is at most what it says.": "每轮只扫几个任务,所以这些读数并非取自同一时刻。前面带 <= 的读数在扫描上限处停下,因此已用部分至多为该值。",

    # The two halves of the task view, and the words the timeline's header
    # carries: a window title, the menu entry that puts the layout back, and
    # the two words around the window length box.
    "Task Analysis": "任务分析",
    # The column names of the two task tables. `Name`, `Address`, `State`,
    # `Stack used` and `Task share` were already in the list; these are the
    # rest of the headers, which stayed English in a Chinese window.
    "Priority": "优先级",
    "Stack": "栈",
    "Waiting on": "等待对象",
    "Kind": "类型",
    "Items / capacity": "项目 / 容量",
    "Waiting to send": "发送等待",
    "Waiting to receive": "接收等待",
    "Synchronisation objects": "同步对象",

    # The two tooltips on the acquisition button. The button itself says
    # "Start" or "Stop"; these say what that means for the run.
    "Stops the acquisition. The space bar does the same.": "停止采集。空格键同效。",
    "Starts the acquisition. The space bar does the same.": "开始采集。空格键同效。",

    # The tooltip behind the "Deadline limited" segment: what was asked for,
    # what arrived, and that nothing is broken.
    "Asking for %.1f Hz; over the last measurement window the run is getting %.1f Hz, with %.1f%% of the sample periods missed.": "请求 %.1f Hz;最近一个统计窗口里的实际值 %.1f Hz,有 %.1f%% 的采样周期错过了截止时间。",
    "Asking for a lower rate, or reading fewer bytes each round, closes the gap. Nothing is wrong with the target.": "把请求采样率调低,或者每轮少读一些字节,就能把差距补回来。目标侧没有故障。",

    # The findings section: its title, and the duration at the end of a row.
    "Findings": "发现",
    "%.1f s / %u rounds": "%.1f 秒 / %u 轮",

    # The filter bar: the state menu, and the two thresholds written as
    # comparisons with their units.
    "states": "状态",
    "show every state": "显示全部状态",
    "stack free": "栈剩余",
    "priority >=": "优先级 >=",

    # The selected-task panel and its copy button.
    "Selected task": "选中任务",
    "Click a row in the table, a finding or a lane to read one task here.": "点表格里的一行、一条发现或一条泳道,在这里读单个任务。",
    "copy": "复制",
    "The task that was selected is not in this round.": "选中的任务不在这一轮读数里。",
    "Found on": "所在链表",
    "%u (was %u) - inherited while a lock is held": "%u(原为 %u)——持有锁期间被继承",
    "%s%u / %u B, %u%% free - read %.1f s ago": "%s%u / %u 字节,剩余 %u%%,读数距今 %.1f 秒",
    "%.1f%% of the time the tasks used, over the last %.2f s": "占任务总时间的 %.1f%%,窗口为最近 %.2f 秒",

    # The lane order of the timeline: the switch and the two sentences that say
    # which order is in force.
    "follow table order": "跟随表格顺序",
    "lanes in table order": "泳道按表格顺序排列",
    "lanes in base priority order": "泳道按基础优先级排列",

    # The task view's control block switch, and why it exists.
    "Task view: read control blocks:": "任务视图:读取任务控制块:",
    "Copy the chain": "复制这条链",

    # The pinned moment panel.
    "Pinned moment": "钉住的时刻",

    # The findings block keeps two rows; these say what the extra ones are and
    # what an empty round looks like.
    "the most serious": "最严重的在列",
    "nothing in this round": "这一轮没有发现",
    "Click the strip above to pin a moment.": "点上面那条色带钉住一个时刻。",
    "showing %.2f s of %.0f s asked, %u rounds": "显示 %.2f 秒（请求 %.0f 秒）共 %u 轮",
    "showing %.2f s, %u rounds": "显示 %.2f 秒, 共 %u 轮",
    "not read": "未采样",
    "busiest %s %.1f%%": "最忙 %s %.1f%%",
    "longest wait %s %.0f s": "最长等待 %s %.0f 秒",
    "Last ran": "最近运行",
    "right now": "就是现在",
    "%.2f s ago": "%.2f 秒前",
    "not in the last %.0f s": "最近 %.0f 秒内没跑过",
    "sorted by problem: stack alerts, blocked, inherited, then by base priority": "按问题排序: 栈告警、阻塞、继承，然后按基础优先级",
    "sorted by the column in bold": "按加粗那一列排序",
    "back to problem order": "回到问题排序",
    "not read is not the same as idle": "未采样不等于没在跑",
    "rounds %.2f s apart": "每 %.2f 秒一轮",
    "one band per state held until the next round": "每一段状态保持到下一轮",
    "That moment is no longer among the rounds the view keeps.": "那一刻已经不在视图保留的轮次里了。",
    "%u running, %u ready, %u blocked, %u suspended": "%u 个运行, %u 个就绪, %u 个阻塞, %u 个挂起",
    "Off leaves the task table with the states and addresses the kernel's lists give: no names, priorities or stack ranges. Turn it off when a run stops by itself a second or two after it starts - that is a probe hanging on those reads, and it takes the variable view down with it.": "关掉后任务表只剩内核链表给出的状态和地址,没有名字、优先级和栈范围。采集启动一两秒后自己停住时就关它:那是探针卡在这些读上,连变量视图一起停。",
    "Reset layout": "重置界面布局",
    "last": "最近",
    "s": "秒",

    # The four questions the task table is asked. The tooltips say what each
    # one keeps, because "problems" is a judgement and a reader is entitled to
    # know whose it is.
    "all tasks": "全部任务",
    "problems only": "只看异常",
    "blocked only": "只看阻塞",
    "high share only": "只看高占用",
    "Every task the round read, whatever state it is in.": "这一轮读到的每个任务，不论状态。",
    "A stack at or below the alert threshold, a priority raised by inheritance, and every task the findings above name - a deadlock ring, a wait with no timeout, a task ready and passed over. A task waiting on an object with a timeout is not one of these: in a healthy kernel that is most of them.":
        "栈水位到或低于告警阈值、优先级被继承抬高、以及上面发现区点到名的任务——疑似死锁环、无超时等待、就绪却一直没被切入。带超时地等一个对象不算在内:内核健康时那是大多数任务。",
    "Tasks waiting for something, whether or not the wait has a timeout.": "在等某个东西的任务，不管这段等待有没有超时。",
    "The tasks whose share of the task time is at or above the floor. A share needs two rounds and a target built with run time statistics.":
        "任务时间占比到或高于下限的任务。占比需要两轮数据，而且目标要开了运行时间统计。",
    "task share": "任务占比",
    "A task whose share of the task time is at or above this is kept. Zero keeps every task that has a share. The share is of the time the tasks used, not of the wall clock.":
        "任务时间占比到或高于这个数的任务留下。填 0 则留下所有有占比的任务。占比的分母是任务用掉的时间，不是墙钟时间。",
    "Nothing in this round looks wrong: no stack at the alert threshold, no inherited priority, and no finding naming a task.":
        "这一轮看不出问题:没有栈水位到阈值的，没有优先级被继承的，也没有任何一条发现点到某个任务。",
    "No task is blocked in this round.": "这一轮没有阻塞的任务。",
    "No task is at or above that share of the task time.": "没有任务的任务时间占比到或高于那个数。",
    "The target does not count the time each task used, so no share can be above the floor.":
        "目标没有统计每个任务用掉的时间，所以没有占比能高于下限。",
    "No task matches these filters.": "没有任务符合这些筛选条件。",
    "Problems": "问题",
    "%u of %u tasks have lanes": "%u / %u 个任务有泳道",
    "The filters left no task to draw. Clear them to see the lanes again.": "筛选之后没有任务可画。清除筛选就能重新看到泳道。",

    # A sweep over the rest of the interface found ten more hand written keys
    # with no entry - the same shape of leak as the twelve above: in Chinese the
    # English stays, silently.
    "Puts the windows, the docking and the table columns back the way a first run has them. Sizes and column choices are remembered per window, so a new layout would otherwise never reach an installation that has been started before.":
        "把窗口、停靠和表格列恢复成第一次运行时的样子。尺寸和列的选择是按窗口记住的，否则新布局永远到不了一个已经启动过几次的安装上。",
    "Error:": "错误:",
    "File:": "文件:",
    "Export capture to *.csv": "导出采集到 *.csv",
    "There is nothing to write yet. Press Download first.": "还没有可写的内容。先按「下载」。",
    "Writes the capture beside the project file, one column per variable, with the values the variables' own types give them.":
        "把采集写到工程文件旁边，一个变量一列，数值按变量自己的类型给出。",
    "Warning": "警告",
    "(derived)": "(派生)",

    # Three recorder messages that used to be built by gluing translated and
    # untranslated pieces together. One format string each, so the order can move
    # between languages and nothing is left in English.
    # 右栏的事件列表（切换记录）。
    "Events": "事件",
    "pause": "暂停",
    "%u new while paused": "暂停期间新增 %u 条",
    "%u runs": "%u 段",
    "last %u of %u runs": "最近 %u 段，共 %u 段",
    "Task": "任务",
    "Ran for": "跑了多久",
    "No switch records yet. The target writes them from its own timer interrupt, or sends them over SWO/RTT; the timeline's runs are the same records.":
        "还没有收到切换记录。这些记录由目标在自己的定时中断里写入，或者经 SWO/RTT 送过来；时间线里的那一段段就是同一批记录。",

    # 这一轮没读到的任务：行留着，整行画斜杠。
    "This round did not read this task, so every column is a slash. The row stays: the task is there, the reading is not. Its last reading was %.1f s before this round.":
        "这一轮没有读到这个任务，所以每一列都是斜杠。行留着:任务还在，读数没有。上一次读到它是在 %.1f 秒之前。",

    # 离线记录：录一段、后面不接探针也能看。判据与文件格式在 RtosRecording。
    "record": "录制",
    "stop recording": "停止录制",
    "recording": "正在录制",
    "open a recording": "打开记录",
    "playing back": "回放",
    "back to live": "回到实时",
    "Task view recordings": "任务视图记录",
    "%u rounds": "%u 轮",
    "round %u of %u": "第 %u 轮，共 %u 轮",
    # %.0f KB / %.1f MB 只在两种语言里一样，所以不进词表（英文原样返回）。
    "Writes every round the view is shown to a file, so that the same table, findings and lanes can be read again with no probe attached.":
        "把界面看到的每一轮写进文件，之后不接探针也能重新看同一张表、同样的发现和泳道。",

    # RtosRecording 的失败原因。英文那句同时是日志和词表的键，所以只写一次。
    "the file could not be created": "文件无法创建",
    "the file could not be opened": "文件打不开",
    "this is not a task view recording": "这不是任务视图的记录文件",
    "this recording was written by another version of the program": "这个记录是别的版本的程序写的",
    "this recording holds no rounds": "这个记录里没有轮次",
    "a recording is already open for reading": "已经打开了一个记录在看",
    "a recording is being written": "正在录制，先停下再打开",

    "Copied %u bytes: %u samples.": "已读出 %u 字节: %u 个样本。",
    "Wrote %s (%u samples).": "已写入 %s（%u 个样本）。",
    "The capture could not be written to %s.": "采集写不进 %s。",

    # Twelve strings the panel asked for by hand and the table never had, so
    # they stayed English in the Chinese interface. Found by asking every
    # `I18n::tr` literal in the task view which entry it resolves to.
    "No task data yet.": "还没有任务数据。",
    "The task view reads its rounds while an acquisition is running.": "任务视图在采集运行时读取每一轮。",
    "No round has been published yet.": "还没有发布任何一轮。",
    "%d of them were counted into the whole at the rate they were last seen to use.":
        "其中 %d 个按上次测到的速率计入总量。",
    "The shares are of the time the tasks used. Time the kernel attributes to no task - the interrupts mostly - was never in it.":
        "占比的分母是任务用掉的时间。内核归不到任何任务的时间(主要是中断)从来不在其中。",
    "search by name": "按名字搜索",
    "%d of %d shown": "显示 %d / %d",
    "Running": "运行中",
    "Tasks": "任务",
    "%.1f s": "%.1f 秒",
    "This kernel keeps no registry of its queues.": "这个内核不保留队列登记表。",
    "None are registered.": "没有登记任何对象。",

    # The event list narrowed to one task: the chip that says so, the way back
    # out of it, and the detail block's short run of the same records.
    "only %s": "只看 %s",
    "Show every task again.": "恢复显示全部任务",
    "This task has not been switched in during the runs the view keeps.": "在这段记录里，这个任务没有被切入过。",
    "Switches (5 s)": "切入次数(5 秒)",
    "Show only this task's switches in the event list.": "事件列表里只看这个任务的切换。",

    # 把记录导成 CSV：按钮、它的说明、以及三种结果。
    "export CSV": "导出 CSV",
    "Writes every round to a spreadsheet: one line per task per round, with the columns the table shows. A field the round did not measure is left empty rather than written as zero.":
        "把每一轮写成表格文件：每个任务每轮一行，列和界面上的表一样。这一轮没测到的格子留空，不写 0。",
    "This recording holds no rounds.": "这份记录里没有任何轮次。",
    "The file could not be created.": "文件无法创建。",
    "Wrote %u rows from %u rounds to %s": "从 %u 轮写出 %u 行，保存到 %s",
    "Wrote %u rows; %u rounds could not be read and were left out.": "写了 %u 行；有 %u 轮读不出来，已略过。",
    "CSV files": "CSV 文件",

    # 轨道页签：五条轨道的摘要与"这份源不提供什么"。
    "Tracks": "轨道",
    "intervals": "个区间",
    "marks": "条标记",
    "waits": "次等待",
    "records lost": "条记录丢失",
    "unfinished": "未完成",
    "This source does not carry: %s": "这份源不提供：%s",
    "No event source is attached: this view is built from snapshots only.": "没有接事件源：这个视图只用快照构建。",
    "no target time · %s": "没有目标时间 · %s",
    "%s · %u events": "%s · %u 个事件",
    "switches": "次切换",
    "this source does not carry it": "这份源不提供",
    "none": "—",
    "window": "时间窗",
    "untimed": "条无时间",
    "No event here carries a target time, so there is nothing to place on an axis.":
        "这里没有任何事件带目标时间，放不到时间轴上。",

    # 阻塞关系图：三个页签之一，以及链上的两种箭头与环的收尾。
    "Waiting chain": "等待链",
    "waits for": "等待",
    "held by": "持有者是",
    "back to %s": "回到 %s",
    "It is waiting on nothing, so nothing is blocking it.": "它没有在等任何东西，也就没有谁在挡它。",
    "It waits on %s, which the kernel's registry does not list, so the chain stops at that name.":
        "它在等 %s，这个名字不在内核的登记表里，所以链到这里就断了。",
    "(unnamed)": "(无名)",
    "%s, priority %u": "%s，优先级 %u",
    "%s %u/%u, %u waiting": "%s %u/%u，%u 个在等",

    # 关系图是定格的：表头要说清这一张是什么时候的，以及两个按钮。
    "refresh": "刷新",
    "follow": "跟随",
    "following the newest round": "跟随最新一轮",
    "held from %.1f s ago": "定格于 %.1f 秒前",
    "held from %.1f s ago; the chain has changed since": "定格于 %.1f 秒前，链已经变了",
    "held from %.1f s ago; the task is not in this round": "定格于 %.1f 秒前，这个任务这一轮不在表里",

    # 旧版记录里没有切换记录，事件列表要说清楚这是"没记"而不是"没切"。
    "This recording was written before switch records were kept in it, so there is nothing to list. Recording a new one writes them.":
        "这份记录写在开始保留切换记录之前，所以没有可列的内容。录一份新的就会带上。",
}


def main():
    sites = json.load(io.open(SITES, encoding="utf-8"))
    if len(sites) != len(TRANSLATIONS):
        print("the extraction has %d strings, there are %d translations" % (len(sites), len(TRANSLATIONS)))
        return 1

    table = {}
    skip = []
    for english, chinese in zip(sites, TRANSLATIONS):
        if chinese is None:
            skip.append(english)
            continue

        stripped = english.rstrip()

        if stripped != english and stripped in COLUMNS:
            english = stripped
            chinese = chinese.rstrip()

        table[english] = chinese

    table.update(EXTRA)

    with io.open(os.path.join(HERE, "i18n_dict.json"), "w", encoding="utf-8") as handle:
        json.dump({"translations": table, "skip": skip}, handle, ensure_ascii=False, indent=1)

    print("translated: %d" % len(table))
    print("left as they are: %d" % len(skip))
    return 0


if __name__ == "__main__":
    sys.exit(main())
