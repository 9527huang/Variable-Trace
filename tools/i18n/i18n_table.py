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
    # The two halves of a sentence with the mode name in the middle. The name
    # comes from the combo above it, which is already translated by then.
    "Mode '": "模式「",
    "' waits for a trigger. Pick a source variable below first.": "」需要触发。请先在下方选择触发源变量。",

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
