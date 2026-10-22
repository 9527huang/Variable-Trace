# RTEC2 golden corpus

一套二进制样例，三端（C 目标侧编码器、C++ 主机解码器、Python 主机解码器）都读它，
比对的是**解码结果**而不是字节——字节本来就应该一样，值得钉住的是"这些字节是什么意思"。

## 文件

| 文件 | 内容 | 期望解码结果 |
|---|---|---|
| `test/trace-golden/one-of-each.bin` | 每种记录一条，含 `EpochBegin` 的能力位与 `Fault` 的 12 字节 payload | `one-of-each.expected.json` |
| `session.bin` | 一段会话：`EpochBegin` + `ClockSync` + 三次切换 + `Gap` + 两次切换 | `session.expected.json` |
| `wrap.bin` | ticks 从 `0xFFFFFFF0` 走到 `0x10` 再走到 `400` | `wrap.expected.json` |
| `unknown-type.bin` | 两条已知记录中间夹一条 `0x7F` | `unknown-type.expected.json`（未知类型只计数，不进 `records`） |
| `truncated.bin` | 两条完整记录 + 10 字节残记录 | `truncated.expected.json`（`truncatedBytes: 10`） |

## 重新生成

```bash
gcc -std=c99 -O1 -o /tmp/rtec2_gen tools/trace-golden/gen.c
/tmp/rtec2_gen test/trace-golden
```

`gen.c` 从同一批结构体里既写 `.bin` 也写 `.expected.json`，所以期望值不会和文件本身走散。
改了线格式就必须重新生成，并且三端一起改——CI 会在任何一端落后时报错。

## 比对

```bash
# C++ 侧：测试二进制里的 Rtec2DecoderTest 读同一批文件
./build_test/test/Variable-Trace_test.exe --gtest_filter="Rtec2Decoder*"

# Python 侧：与同一批期望值逐字段比对
python tools/trace-golden/compare.py
```

两边都必须通过。`compare.py` 退出码非零表示 Python 解码器与 C 编码器不一致。

## 约定

- 未知类型**跳过并计数**，不进 `records`。期望文件里也不列它。
- 残记录只报 `truncatedBytes`，不猜后面的内容。
- `index` 是解码器自己的顺序号（从 0 开始，只数已识别的记录），不是线上字段。
- 断言比的是结构化输出（JSON），不是内存映像：三端可以各自存得不同，说法必须一致。
