"""生成固定事件样本，供分析链路验证用。

为什么要固定样本
================
分析链路（事件解码、序号与丢失检测、时间回绕、代次与墓碑、泳道聚合、
区间查询、环形缓冲重组）必须在**不依赖目标板、不依赖固件**的情况下就能
开发和回归。所以先把内容已知、期望属性已知的记录固化下来，进版本控制。

配套的 `check_samples.py` 是独立实现的读取端：不看这里的中间结果，只从字节
重新推导，用来验证样本真的有它声称的属性。

样本文件格式
============
刻意贴着目标侧缓冲的形态设计：**事件区就是一块环形缓冲的原始镜象**，
不是已经摊平的列表。这样解码器必须真的按写指针与事件计数去重组，同一套
代码既能吃实时下载下来的缓冲，也能吃这些文件。全程小端。

文件 = 文件头 + 目标侧元数据 + 缓冲镜象 + 字典区。

文件头（32 字节）
    0   4   魔术 "VTRC"
    4   2   格式版本          = 1
    6   2   文件头长度        = 32
    8   4   目标侧元数据偏移
   12   4   目标侧元数据长度  = 32
   16   4   缓冲镜象偏移
   20   4   缓冲镜象长度（= 元数据里的缓冲总字节数）
   24   4   字典区偏移
   28   4   字典区长度（条数 × 32）

目标侧元数据（32 字节）
    0   4   写指针（字节偏移，指向下一个要写的位置）
    4   4   累计写入事件数（含已经被覆盖掉的）
    8   4   溢出计数（因满而**丢弃**的事件数，与「被覆盖」不是一回事）
   12   4   周期计数高位（时基快照）
   16   4   事件源标识（每次启停采集换一个）
   20   4   缓冲总字节数
   24   2   主频（兆赫兹）
   26   1   核编号
   27   1   标志（位 0 = 已绕回；冗余字段，见下）
   28   4   代次（目标侧防撕裂用）

缓冲镜象（缓冲总字节数 字节）
    一块环形缓冲的原始内容。事件是 16 字节定长，但在缓冲里的落点是环形的。

字典项（32 字节）
    0   1   种类（0 任务 / 1 队列 / 2 信号量 / 3 互斥量）
    1   1   状态（0 在用 / 1 已删除）
    2   2   编号
    4   4   代次
    8   4   生效起始事件序号
   12   4   生效结束事件序号（0xFFFFFFFF 表示仍然有效）
   16  16   名字（UTF-8，不足补 0）

解码器必须实现的重组规则
========================
**只靠位置判断，不靠标志位。** 给定缓冲大小、写指针、累计写入数三个数，
有效事件及其顺序就唯一确定了：

    槽数   = 缓冲总字节数 / 16
    保留数 = min(累计写入数, 槽数)

    若 写指针 - 保留数 × 16 >= 0：
        有效区间 = [写指针 - 保留数×16, 写指针)      —— 单段，正序
    否则：
        有效区间 = [写指针, 末尾) 拼上 [0, 写指针)    —— 两段

**为什么不用「累计写入数 > 槽数」来判断有没有绕回**：正好写满一圈时
（累计写入数 == 槽数），按计数比会判成没绕回，于是算出
`写指针 - 保留数×16 = 0 - 满缓冲字节数` 这个负数起点。实际状态是缓冲
满是数据、正序摆放。位置判据天然处理这个边界，计数比不处理。

**绕回标志是冗余的**，只用来说明「本次采集期间写指针是否绕过圈」，
实现时以位置推出来的结论为准，标志用来交叉校验。

三处必须分辨的缓冲状态（写指针都是 0）
======================================
写指针等于 0 不等于空：

    累计写入数 = 0                    -> 真的是空的            （样本 09）
    累计写入数 >= 槽数，写指针 = 0    -> 正好绕回一圈，缓冲满  （样本 08）

只看写指针会把这三种状态里的两种混为一谈。

丢失的两个来源要分开算
======================
    被覆盖 = 累计写入数 - 保留数      （环形缓冲满，最旧的被盖掉）
    被丢弃 = 溢出计数                （满了就不写，直接丢新的）

    总共丢失 = 被覆盖 + 被丢弃

样本 02 用的是「被丢弃」，样本 07、08 用的是「被覆盖」。
"""
import os
import struct
import sys

EVENT_TYPE = {
    "task_switch": 0, "task_create": 1, "task_delete": 2,
    "task_ready": 3, "task_block": 4, "task_wake": 5,
    "task_priority": 6, "queue_send": 7, "queue_recv": 8,
    "queue_send_fail": 9, "queue_recv_timeout": 10,
    "sem_give": 11, "sem_take": 12,
    "mtx_lock": 13, "mtx_unlock": 14,
    "isr_enter": 15, "isr_exit": 16,
    "time_sync": 17,
}

KIND = {"task": 0, "queue": 1, "sem": 2, "mtx": 3}
STAT_ALIVE, STAT_TOMB = 0, 1

FILE_HDR_LEN = 32
META_LEN = 32
EVENT_LEN = 16
DICT_LEN = 32

FREQ_HZ = 8_000_000
CYC_PER_US = FREQ_HZ // 1_000_000

OUT_DIR = os.path.dirname(os.path.abspath(__file__))


class Sample:
    def __init__(self, name, note, buffer_bytes, written_total=None,
                 core=0, epoch=1, overflow=0):
        self.name = name
        self.note = note
        self.buffer_bytes = buffer_bytes
        self.core = core
        self.epoch = epoch
        self.overflow = overflow
        self.events = []          # 缓冲里保留的，按时间从旧到新
        # 累计写入数默认等于保留数（没被覆盖过）；绕回的样本显式给一个更大的数
        self.written_total = written_total
        self.dict = []

    def add(self, seq, ts, etype, task=0, obj=0, param=0):
        self.events.append((seq & 0xFFFFFFFF, ts & 0xFFFFFFFF,
                            EVENT_TYPE[etype], task & 0xFFFF,
                            obj & 0xFFFF, param & 0xFFFFFF))

    def define(self, kind, oid, gen, name, seq_from=0, seq_to=0xFFFFFFFF,
               status=STAT_ALIVE):
        self.dict.append((KIND[kind], status, oid & 0xFFFF, gen,
                          seq_from, seq_to, name.encode("utf-8")[:16]))

    # ---- 把事件摆进环形缓冲 ----
    def _buffer_image(self):
        slots = self.buffer_bytes // EVENT_LEN
        assert self.buffer_bytes % EVENT_LEN == 0, "缓冲大小要是 16 的整数倍"
        written = self.written_total if self.written_total is not None \
            else len(self.events)

        kept = min(written, slots)
        assert kept == len(self.events), \
            f"{self.name}: 保留数应为 {kept}，实际摆了 {len(self.events)} 条"
        assert written - kept <= slots, f"{self.name}: 绕过的圈数不合法"

        first_slot = (written - kept) % slots if slots else 0
        img = bytearray(self.buffer_bytes)
        for i, (seq, ts, et, tk, ob, pm) in enumerate(self.events):
            slot = (first_slot + i) % slots
            off = slot * EVENT_LEN
            # 前 13 字节是定长字段，后 3 字节是参数
            img[off:off + 13] = struct.pack("<IIBHH", seq, ts, et, tk, ob)
            img[off + 13:off + 16] = pm.to_bytes(3, "little")
        # 自检：切片赋值给短于写入长度的区间会让 bytearray 变长，
        # 那样文件头里的镜象长度就会对不上，而报错点离现场很远。
        assert len(img) == self.buffer_bytes, \
            f"{self.name}: 镜象被撑到 {len(img)}，应为 {self.buffer_bytes}"
        write_ptr = ((first_slot + kept) % slots) * EVENT_LEN if slots else 0
        # 标志只说「写指针绕没绕过圈」。注意正好写满一圈也算绕过（>=）。
        # 它不是解码的依据，真正的依据是下面的位置判据，见文件头说明。
        wrapped = 1 if written >= slots else 0
        return bytes(img), write_ptr, written, wrapped

    def write(self):
        img, write_ptr, written, wrapped = self._buffer_image()

        ev_off = FILE_HDR_LEN + META_LEN
        dict_off = ev_off + len(img)
        dict_len = len(self.dict) * DICT_LEN

        hdr = struct.pack("<4sHHIIIIII",
                          b"VTRC", 1, FILE_HDR_LEN,
                          FILE_HDR_LEN, META_LEN,
                          ev_off, len(img), dict_off, dict_len)
        assert len(hdr) == FILE_HDR_LEN

        meta = struct.pack("<IIIIIIHBBI",
                           write_ptr, written, self.overflow, 0,
                           self.epoch, self.buffer_bytes,
                           FREQ_HZ // 1_000_000, self.core, wrapped, 0)
        assert len(meta) == META_LEN

        dbody = b""
        for k, st, oid, gen, sf, st_, nm in self.dict:
            dbody += struct.pack("<BBHIII16s", k, st, oid, gen, sf, st_, nm)
        assert len(dbody) == dict_len

        path = os.path.join(OUT_DIR, self.name + ".vtrc")
        with open(path, "wb") as f:
            f.write(hdr + meta + img + dbody)
        return path, len(hdr) + len(meta) + len(img) + len(dbody)


# ============================================================ 01 正常序列
def s01():
    s = Sample("01_normal_switch", "正常切换序列", buffer_bytes=1024)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")
    s.define("task", 3, 1, "LogTask")
    s.define("task", 4, 1, "IDLE")
    s.define("queue", 1, 1, "FrameQ")
    s.define("sem", 1, 1, "UartTx")

    step = 200 * CYC_PER_US
    seq, ts = 1, 1_000_000

    def ev(etype, task=0, obj=0, param=0):
        nonlocal seq, ts
        s.add(seq, ts, etype, task=task, obj=obj, param=param)
        seq += 1
        ts += step

    for _ in range(4):
        ev("task_switch", 1); ev("task_switch", 2)
        ev("task_switch", 3); ev("task_switch", 4)
    ev("task_switch", 1)
    ev("task_block", 1, obj=1, param=1)
    ev("task_switch", 2)
    ev("queue_send", 2, obj=1, param=3)
    ev("task_wake", 2, obj=1, param=1)
    ev("task_switch", 2)
    ev("task_switch", 1)
    ev("queue_recv", 1, obj=1, param=2)
    ev("task_switch", 3)
    ev("task_switch", 4)
    ev("task_switch", 1)
    return s


# ============================================================ 02 被丢弃
def s02():
    """序号有缺口 —— 丢失检测与缺口显示。

    第 20 到 23、第 40 到 43 个事件没有被记下来（共缺 8 条），序号照跑。
    目标自报溢出计数故意写成 7，与实际缺 8 不一致：判据必须用序号缺口。
    """
    s = Sample("02_dropped_events", "序号有缺口（被丢弃 8，自报 7）",
               buffer_bytes=1024, overflow=7)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")
    s.define("task", 4, 1, "IDLE")

    step = 200 * CYC_PER_US
    seq, ts = 1, 500_000
    for i in range(60):
        if 20 <= i < 24 or 40 <= i < 44:
            seq += 1
            ts += step
            continue
        s.add(seq, ts, "task_switch", task=(1, 2, 4)[i % 3])
        seq += 1
        ts += step
    return s


# ============================================================ 03 时间回绕
def s03():
    """时间戳跨过回绕点 —— 回绕消歧。"""
    s = Sample("03_timestamp_wrap", "时间戳跨回绕点", buffer_bytes=512)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")
    step = 200 * CYC_PER_US
    ts, seq = 0xFFFFFF00, 1000
    for i in range(24):
        s.add(seq, ts, "task_switch", task=(1, 2)[i % 2])
        seq += 1
        ts = (ts + step) & 0xFFFFFFFF
    return s


# ============================================================ 04 墓碑
def s04():
    """任务创建与删除 —— 代次与墓碑。"""
    s = Sample("04_generation_tombstone", "任务创建、删除与墓碑", buffer_bytes=512)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 4, 1, "IDLE")
    s.define("task", 2, 1, "WorkerA", seq_from=2, seq_to=11, status=STAT_TOMB)
    s.define("task", 3, 1, "Logger", seq_from=18)

    step = 200 * CYC_PER_US
    seq, ts = 1, 2_000_000
    order = [("task_switch", 1), ("task_create", 2),
             ("task_switch", 2), ("task_switch", 1),
             ("task_switch", 2), ("task_switch", 1),
             ("task_switch", 2), ("task_switch", 1),
             ("task_switch", 2), ("task_switch", 1),
             ("task_delete", 2),
             ("task_switch", 1), ("task_switch", 4),
             ("task_switch", 1), ("task_switch", 4),
             ("task_switch", 1), ("task_switch", 4),
             ("task_create", 3),
             ("task_switch", 3), ("task_switch", 1),
             ("task_switch", 3), ("task_switch", 1),
             ("task_switch", 3), ("task_switch", 1)]
    for etype, task in order:
        s.add(seq, ts, etype, task=task, param=(10 if "create" in etype else 0))
        seq += 1
        ts += step
    return s


# ============================================================ 05 编号复用
def s05():
    """编号被回收后再分配 —— 不串号。"""
    s = Sample("05_id_reuse", "编号回收后再分配", buffer_bytes=512)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")
    s.define("queue", 1, 1, "q_old", seq_from=0, seq_to=11)
    s.define("queue", 1, 2, "q_new", seq_from=12)

    step = 200 * CYC_PER_US
    seq, ts = 1, 3_000_000
    for i in range(8):
        s.add(seq, ts, "queue_send", task=1, obj=1, param=i % 4)
        seq += 1; ts += step
    s.add(seq, ts, "task_switch", task=2); seq += 1; ts += step
    s.add(seq, ts, "queue_send", task=2, obj=1, param=9); seq += 1; ts += step
    s.add(seq, ts, "task_switch", task=1); seq += 1; ts += step
    for i in range(6):
        s.add(seq, ts, "queue_recv", task=1, obj=1, param=i % 4)
        seq += 1; ts += step
    return s


# ============================================================ 06 短执行
def s06():
    """短于采样间隔的执行 —— 可见性提示。"""
    s = Sample("06_too_short_run", "短于采样间隔的执行（30 微秒）",
               buffer_bytes=256)
    s.define("task", 1, 1, "ControlTask")
    s.define("task", 2, 1, "BlinkTask")
    s.define("task", 4, 1, "IDLE")

    long_run = 4_000 * CYC_PER_US
    short_run = 30 * CYC_PER_US
    seq, ts = 1, 4_000_000
    s.add(seq, ts, "task_switch", 1); seq += 1; ts += long_run
    s.add(seq, ts, "task_switch", 2); seq += 1; ts += short_run
    s.add(seq, ts, "task_switch", 1); seq += 1; ts += long_run
    s.add(seq, ts, "task_switch", 4); seq += 1; ts += long_run
    s.add(seq, ts, "task_switch", 1); seq += 1
    return s


# ============================================================ 07 绕回（两段）
def s07():
    """环形缓冲绕回，写指针不在 0 —— 事件在缓冲里分成两段。

    缓冲 512 字节（32 个槽），一共写过 40 条，所以最旧的 8 条被盖掉了。
    保留的 32 条从第 8 个槽开始摆，写指针停在 128 字节处。也就是说：
    最旧的一段在缓冲后半（128..512），最新的一段绕回开头（0..128）。

    解码器必须把这两段按顺序拼起来，而且不能靠写指针判断哪段新哪段旧。
    """
    s = Sample("07_ring_wrapped", "绕回，写指针在 128（事件分两段）",
               buffer_bytes=512, written_total=40)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")
    s.define("task", 4, 1, "IDLE")

    step = 200 * CYC_PER_US
    ts = 1_000_000
    # 造 40 条，只留最后 32 条（前 8 条被环形缓冲盖掉）
    all_ev = []
    for i in range(40):
        all_ev.append((i + 1, ts, "task_switch", (1, 2, 4)[i % 3]))
        ts += step
    for seq, t, etype, task in all_ev[8:]:
        s.add(seq, t, etype, task=task)
    return s


# ============================================================ 08 绕回（正好一圈）
def s08():
    """写指针正好回到 0，而缓冲是满的 —— 与「空的」只差累计写入数。

    缓冲 512 字节（32 个槽），正好写过 32 条，一条不多一条不少。写指针
    回到 0。**光看写指针的话，这和「刚启动还没写过」长得一模一样。**
    分辨全靠累计写入数。

    这一类就是记录器 `head == 0` 那个坑的同源问题。
    """
    s = Sample("08_ring_wr_off_zero", "写指针=0 且缓冲满（与空只差计数）",
               buffer_bytes=512, written_total=32)
    s.define("task", 1, 1, "SensorTask")
    s.define("task", 2, 1, "CommTask")

    step = 200 * CYC_PER_US
    ts = 2_000_000
    for i in range(32):
        s.add(i + 1, ts, "task_switch", task=(1, 2)[i % 2])
        ts += step
    return s


# ============================================================ 09 空
def s09():
    """写指针 0 且一条都没写过 —— 真的空的。

    与 08 只差一个累计写入数（这里 0，那里 32），而缓冲镜象里两者的写指针
    都是 0。两个样本摆在一起，才能把「写指针等于 0 不等于空」钉死。
    """
    s = Sample("09_ring_empty", "写指针=0 且真的空", buffer_bytes=512,
               written_total=0)
    s.define("task", 1, 1, "SensorTask")
    return s


SAMPLES = [s01, s02, s03, s04, s05, s06, s07, s08, s09]


def main():
    print(f"输出目录 {OUT_DIR}")
    print(f"主频 {FREQ_HZ} 赫兹（一个周期 125 纳秒，32 位计数约 "
          f"{2 ** 32 / FREQ_HZ:.0f} 秒回绕一次）")
    print("事件 16 字节，缓冲镜象按环形摆放\n")
    total = 0
    for fn in SAMPLES:
        s = fn()
        path, size = s.write()
        total += size
        slots = s.buffer_bytes // EVENT_LEN
        written = s.written_total if s.written_total is not None else len(s.events)
        print(f"  {os.path.basename(path):30s} 缓冲 {s.buffer_bytes:5d} 字节"
              f"（{slots:2d} 槽）  保留 {len(s.events):2d}  累计写入 {written:3d}"
              f"  溢出 {s.overflow:2d}  {size:5d} 字节  {s.note}")
    print(f"\n共 {len(SAMPLES)} 个文件，{total} 字节")
    return 0


if __name__ == "__main__":
    sys.exit(main())
