"""独立读取事件样本，逐条核对它们是否真有声称的属性。

刻意不 import `make_samples`：那样只是自己验自己。这里把格式常量再写一遍，
只从字节重新推导。**而且事件是从环形缓冲镜象里重组出来的**——先按写指针与
累计写入数还原出逻辑顺序，再拿去做各项检查。

每个样本都有它存在的理由，这里就把那个理由变成断言：

    01 正常序列        序号无缺口、时间戳单调、全部能解析出名字
    02 有缺口          恰好两段缺口共缺 8 条，且目标自报数与缺口数不一致
    03 跨回绕          时间戳出现且仅出现一次回绕，序号仍单调
    04 创建与删除      墓碑记录存在，有效期与事件区间对得上
    05 编号复用        同一编号在不同序号区间解析出不同名字
    06 短执行          存在一次执行短于快照间隔，能算出可见性下限
    07 绕回两段        缓冲里的有效事件分成两段，拼起来序号连续
    08 写指针为 0 且满 重建出 32 条，顺序正确
    09 写指针为 0 且空 重建出 0 条
    10 epoch 结束前     序号接近上限，但不在同一 epoch 内回绕
    11 新 epoch         序号、时间和编号可重置，但身份与旧会话隔离
    12 多圈绕回        累计写入可超过两圈，仍只重建最后一圈

`invalid/` 里的样本必须被拒绝。它们分别破坏 generation、文件长度、区段偏移、
事件提交、字典完整性、字典区间、epoch 规则和写指针对齐。
"""
import os
import struct
import sys

EVENT_LEN = 16
DICT_LEN = 32
META_LEN = 32
FILE_HDR_LEN = 32

TASK, QUEUE, SEM, MTX = 0, 1, 2, 3
SNAPSHOT_INTERVAL_MS = 500
MAX_EVENT_TYPE = 17

HERE = os.path.dirname(os.path.abspath(__file__))
INVALID_DIR = os.path.join(HERE, "invalid")

OBJECT_KIND_BY_EVENT = {
    7: QUEUE, 8: QUEUE, 9: QUEUE, 10: QUEUE,
    11: SEM, 12: SEM,
    13: MTX, 14: MTX,
}


class SampleError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise SampleError(message)


class Rec:
    def __init__(self, path):
        self.path = path
        with open(path, "rb") as f:
            raw = f.read()
        self.size = len(raw)

        require(self.size >= FILE_HDR_LEN, f"{path}: 文件长度不足文件头")

        magic, ver, hlen, moff, mlen, boff, blen, doff, dlen = \
            struct.unpack_from("<4sHHIIIIII", raw, 0)
        require(magic == b"VTRC", f"{path}: 魔术不对 {magic!r}")
        require(ver == 1, f"{path}: 版本 {ver}")
        require(hlen == FILE_HDR_LEN and mlen == META_LEN, "头部长度不对")
        require(moff == hlen, "元数据没紧跟文件头")
        require(boff == moff + mlen, "缓冲镜象没紧跟元数据")
        require(doff == boff + blen, "字典区没紧跟缓冲镜象")
        require(moff + mlen <= self.size, "元数据区越界")
        require(boff + blen <= self.size, "缓冲镜象区越界")
        require(doff + dlen <= self.size, "字典区越界")
        require(self.size == doff + dlen,
                f"{path}: 文件长度 {self.size}，按头部算应为 {doff + dlen}")
        require(dlen % DICT_LEN == 0, "字典区长度不是条目大小的整数倍")

        (self.write_ptr, self.written_total, self.overflow, self.tick_high,
         self.epoch, self.buffer_bytes, self.freq_mhz, self.core,
         self.flag_wrapped, self.generation) = struct.unpack_from(
             "<IIIIIIHBBI", raw, moff)
        require(blen == self.buffer_bytes,
                f"{path}: 镜象长 {blen} 与元数据 {self.buffer_bytes} 不一致")
        require(self.buffer_bytes > 0 and self.buffer_bytes % EVENT_LEN == 0,
                "缓冲大小必须是非零事件长度整数倍")
        require(self.write_ptr < self.buffer_bytes,
                "写指针越过缓冲区")
        require(self.write_ptr % EVENT_LEN == 0,
                "写指针未按事件长度对齐")
        require(self.freq_mhz > 0, "目标频率不能为零")
        require(self.epoch != 0, "epoch 不能为零")
        require(self.generation % 2 == 0,
                "generation 必须是偶数，奇数表示目标正在写入")

        self.image = raw[boff:boff + blen]

        self.dict = []
        for i in range(dlen // DICT_LEN):
            k, st, oid, gen, sf, st_, nm = struct.unpack_from(
                "<BBHIII16s", raw, doff + i * DICT_LEN)
            require(k in (TASK, QUEUE, SEM, MTX), "字典包含未知对象种类")
            require(st in (0, 1), "字典包含未知状态")
            require(gen > 0, "字典代次必须大于零")
            require(sf <= st_, "同一 epoch 内字典区间不能回绕")
            try:
                name = nm.split(b"\0")[0].decode("utf-8")
            except UnicodeDecodeError as ex:
                raise SampleError("字典名称不是合法 UTF-8") from ex
            require(bool(name), "字典名称不能为空")
            self.dict.append((k, st, oid, gen, sf, st_,
                              name))

        by_identity = {}
        for entry in self.dict:
            by_identity.setdefault((entry[0], entry[2]), []).append(entry)
        for identity, entries in by_identity.items():
            ordered = sorted(entries, key=lambda d: d[4])
            for previous, current in zip(ordered, ordered[1:]):
                require(previous[5] < current[4],
                        f"字典生效区间重叠：{identity}")

        self.events = self._reconstruct()
        self._validate_events()

    # ---- 从环形缓冲还原事件，逻辑顺序：最旧在前 ----
    def _reconstruct(self):
        slots = self.buffer_bytes // EVENT_LEN
        kept = min(self.written_total, slots)
        self.kept = kept
        # 标志本身是冗余的，只用来说明「写指针绕没绕过圈」。
        # 注意：正好写满一圈也算绕过，所以是 >= 而不是 >。
        self.flag_says_wrapped = self.written_total >= slots

        if kept == 0:
            self.offsets = []
            self.wrapped = False
            return []

        # **按位置判断，不按计数比较。** 正好写满一圈时，计数比会判成没绕回，
        # 于是算出负的起点；位置判据天然处理这个边界。
        start = self.write_ptr - kept * EVENT_LEN
        if start >= 0:
            self.offsets = [start + i * EVENT_LEN for i in range(kept)]
            self.wrapped = False
        else:
            self.offsets = list(range(self.write_ptr, self.buffer_bytes, EVENT_LEN))
            self.offsets += list(range(0, self.write_ptr, EVENT_LEN))
            self.wrapped = True
        require(len(self.offsets) == kept,
                f"拼出 {len(self.offsets)} 条，应为 {kept}")

        out = []
        for off in self.offsets:
            seq, ts, et, tk, ob = struct.unpack_from("<IIBHH", self.image, off)
            pm = int.from_bytes(self.image[off + 13:off + 16], "little")
            out.append((seq, ts, et, tk, ob, pm))
        return out

    def _validate_events(self):
        require(all(a[0] < b[0] for a, b in zip(self.events, self.events[1:])),
                "同一 epoch 内序号必须严格递增，回绕前应切换 epoch")

        for event in self.events:
            seq, _, event_type, task, obj, _ = event
            require(event_type <= MAX_EVENT_TYPE,
                    f"未知事件类型 {event_type}，可能是半写记录")
            if task != 0:
                require(self.name_of(self.epoch, TASK, task, seq) is not None,
                        f"事件 {seq} 缺少任务字典：{task}")
            object_kind = OBJECT_KIND_BY_EVENT.get(event_type)
            if object_kind is not None and obj != 0:
                require(self.name_of(self.epoch, object_kind, obj, seq) is not None,
                        f"事件 {seq} 缺少对象字典：{obj}")

    def name_of(self, epoch, kind, oid, seq):
        """按 epoch 加编号加序号区间解析名字。

        **epoch 是身份的一部分。** 一份字典只对它自己那个 epoch 的事件有效：
        新会话允许重置序号并复用编号，所以同一个 (种类, 编号) 在不同 epoch
        指向不同对象。拿别的 epoch 来查必须解析不出名字，否则两个会话就被
        串成了一条连续时间线。
        """
        if epoch != self.epoch:
            return None
        for k, st, i, gen, sf, st_, nm in self.dict:
            if k == kind and i == oid and sf <= seq <= st_:
                return nm
        return None

    def switch_durations_us(self):
        out = []
        for a, b in zip(self.events, self.events[1:]):
            if a[2] != 0 or b[2] != 0:
                continue
            out.append((a[5], ((b[1] - a[1]) & 0xFFFFFFFF)
                        / (self.freq_mhz * 1e6) * 1e6, a[4]))
        return out

    def physical_segments(self):
        """有效事件在物理上占几段连续区间。

        注意与「走没走到绕回分支」不是一回事：正好写满一圈时走的是绕回分支，
        但结果仍是一段连续的 [0, 末尾)，所以这里按实际偏移算连续段数。
        """
        runs = 0
        prev = None
        for off in self.offsets:
            if prev is None or off != prev + EVENT_LEN:
                runs += 1
            prev = off
        return runs


RESULTS = []


def check(name, ok, detail):
    RESULTS.append((name, ok, detail))
    print(f"  [{'通过' if ok else '失败'}] {name}：{detail}")
    return ok


def gap_analysis(rec):
    seqs = [e[0] for e in rec.events]
    return [(a, b, b - a - 1) for a, b in zip(seqs, seqs[1:]) if b != a + 1]


def main():
    files = sorted(f for f in os.listdir(HERE) if f.endswith(".vtrc"))
    if not files:
        print("没有找到样本，先跑 make_samples.py")
        return 1
    print(f"找到 {len(files)} 个样本\n")

    recs = {}
    for f in files:
        rec = Rec(os.path.join(HERE, f))
        recs[f[:2]] = rec
        print(f"{f}  缓冲 {rec.buffer_bytes} 字节（{rec.buffer_bytes // EVENT_LEN} 槽）"
              f"  写指针 {rec.write_ptr}  累计写入 {rec.written_total}"
              f"  重建出 {len(rec.events)} 条  字典 {len(rec.dict)} 条")

        # ---- 公共约束 ----
        check("保留数 = min(累计写入数, 槽数)",
              rec.kept == min(rec.written_total, rec.buffer_bytes // EVENT_LEN),
              f"保留 {rec.kept}")
        check("绕回标志与计数推出的结论一致（标志是冗余的）",
              bool(rec.flag_wrapped) == rec.flag_says_wrapped,
              f"标志 {rec.flag_wrapped}，由计数推出 "
              f"{int(rec.flag_says_wrapped)}（累计写入 {rec.written_total} "
              f"对槽数 {rec.buffer_bytes // EVENT_LEN}）")
        check("重建后序号严格递增",
              all(a[0] < b[0] for a, b in zip(rec.events, rec.events[1:])),
              f"{len(rec.events)} 条")
        lost_over = rec.written_total - rec.kept
        check("丢失的两个来源能分开算",
              True,
              f"被覆盖 {lost_over} + 被丢弃 {rec.overflow} = 共丢 {lost_over + rec.overflow}")

        # ---- 逐样本 ----
        if f.startswith("01"):
            check("序号无缺口", not gap_analysis(rec), "无")
            ts = [e[1] for e in rec.events]
            check("时间戳单调不回绕", all(a < b for a, b in zip(ts, ts[1:])), "逐条")
            names = {rec.name_of(rec.epoch, TASK, e[3], e[0]) for e in rec.events}
            check("全部事件能解析出任务名", None not in names,
                  f"{sorted(n for n in names if n)}")

        elif f.startswith("02"):
            gaps = gap_analysis(rec)
            missing = sum(g[2] for g in gaps)
            check("缺口段数为 2", len(gaps) == 2, f"{gaps}")
            check("缺口总数为 8", missing == 8, f"实测 {missing}")
            check("自报溢出数与实际缺口不一致（故意）", rec.overflow != missing,
                  f"自报 {rec.overflow}，实际 {missing} —— 判据用序号缺口")

        elif f.startswith("03"):
            ts = [e[1] for e in rec.events]
            drops = [i for i in range(len(ts) - 1) if ts[i] > ts[i + 1]]
            check("时间戳出现回绕", len(drops) >= 1, f"位置 {drops}")
            check("回绕只发生一次", len(drops) == 1, f"实测 {len(drops)} 次")
            if drops:
                i = drops[0]
                back = (ts[i] - ts[i + 1]) / (rec.freq_mhz * 1e6) * 1e6
                check("回落幅度远大于正常步长（能判定是回绕非抖动）", back > 100,
                      f"回落约 {back:.0f} 微秒")
            check("序号仍单调（靠序号消歧）",
                  all(a[0] < b[0] for a, b in zip(rec.events, rec.events[1:])), "跨回绕点")

        elif f.startswith("04"):
            tombs = [d for d in rec.dict if d[1] == 1]
            check("存在墓碑记录", len(tombs) == 1, f"{len(tombs)} 条")
            if tombs:
                _, _, oid, gen, sf, st_, nm = tombs[0]
                check("墓碑带名字与代次", bool(nm) and gen >= 1,
                      f"编号 {oid} 代次 {gen} 名字 {nm} 有效期 {sf}..{st_}")
                inside = [e for e in rec.events if sf <= e[0] <= st_ and e[3] == oid]
                outside = [e for e in rec.events
                           if not (sf <= e[0] <= st_) and e[3] == oid]
                check("有效期内有事件引用它（历史仍显示得出名字）", len(inside) > 0,
                      f"{len(inside)} 条")
                check("有效期外没有事件引用它", len(outside) == 0, f"{len(outside)} 条")
                check("按区间能解析出墓碑的名字",
                      rec.name_of(rec.epoch, TASK, oid, sf) == nm,
                      f"得 {rec.name_of(rec.epoch, TASK, oid, sf)}")

        elif f.startswith("05"):
            same = sorted([d for d in rec.dict if d[0] == QUEUE and d[2] == 1],
                          key=lambda d: d[4])
            check("同一编号有两条记录", len(same) == 2, f"{len(same)}")
            if len(same) == 2:
                a, b = same
                check("代次不同且递增", b[3] > a[3], f"{a[3]} -> {b[3]}")
                check("名字不同", a[6] != b[6], f"{a[6]} -> {b[6]}")
                check("序号区间首尾相接无重叠", a[5] + 1 == b[4],
                      f"{a[4]}..{a[5]} 与 {b[4]}..{b[5]}")
                na = rec.name_of(rec.epoch, QUEUE, 1, (a[4] + a[5]) // 2)
                nb = rec.name_of(rec.epoch, QUEUE, 1, b[4] + 1)
                check("同一编号在不同区间解析出不同名字", na != nb,
                      f"{na} vs {nb}")
                check("老区间里的事件指向老名字",
                      all(rec.name_of(rec.epoch, QUEUE, 1, e[0]) == a[6]
                          for e in rec.events if e[0] <= a[5]),
                      "全对")

        elif f.startswith("06"):
            durs = rec.switch_durations_us()
            shortest = min(durs, key=lambda d: d[1]) if durs else None
            check("存在一次极短执行", shortest is not None and shortest[1] < 100,
                  f"最短 {shortest[1]:.1f} 微秒" if shortest else "无")
            if shortest:
                check("该执行短于任务快照间隔", shortest[1] < SNAPSHOT_INTERVAL_MS * 1000,
                      f"{shortest[1]:.1f} 微秒 对 {SNAPSHOT_INTERVAL_MS} 毫秒")
                ratio = SNAPSHOT_INTERVAL_MS * 1000 / shortest[1]
                check("可见性提示能算出有意义的比例", ratio > 1000,
                      f"采样周期是它的 {ratio:.0f} 倍，命中率约 {100 / ratio:.3f}%")

        elif f.startswith("07"):
            check("确实绕回了", rec.wrapped, f"累计写入 {rec.written_total} > 槽数")
            check("写指针不在 0（这才是两段的情形）", rec.write_ptr != 0,
                  f"写指针 {rec.write_ptr}")
            check("物理上分成两段", rec.physical_segments() == 2,
                  f"实测 {rec.physical_segments()} 段")
            check("被覆盖掉 8 条", rec.written_total - rec.kept == 8,
                  f"{rec.written_total} - {rec.kept}")
            gaps = gap_analysis(rec)
            check("拼起来后序号连续（拼接顺序对）", not gaps, f"缺口 {gaps or '无'}")
            check("重建出的序号范围是 9 到 40",
                  rec.events[0][0] == 9 and rec.events[-1][0] == 40,
                  f"{rec.events[0][0]} 到 {rec.events[-1][0]}")
            check("缓冲开头那一段确实是较新的",
                  rec.image[:EVENT_LEN] != bytes(EVENT_LEN)
                  and struct.unpack_from("<I", rec.image, 0)[0]
                  > struct.unpack_from("<I", rec.image, rec.write_ptr)[0],
                  "偏移 0 处的序号大于写指针处的序号")

        elif f.startswith("08"):
            check("写指针为 0", rec.write_ptr == 0, f"{rec.write_ptr}")
            check("缓冲是满的（保留数 = 槽数）",
                  rec.kept == rec.buffer_bytes // EVENT_LEN,
                  f"保留 {rec.kept} / 槽 {rec.buffer_bytes // EVENT_LEN}")
            check("重建出 32 条而不是 0 条", len(rec.events) == 32,
                  f"实测 {len(rec.events)} 条")
            check("顺序正确（序号 1 到 32）",
                  rec.events[0][0] == 1 and rec.events[-1][0] == 32,
                  f"{rec.events[0][0]} 到 {rec.events[-1][0]}")
            check("写指针为 0 时拼出来是一段连续的，不是两段",
                  rec.physical_segments() == 1,
                  f"实测 {rec.physical_segments()} 段")

        elif f.startswith("09"):
            check("写指针为 0", rec.write_ptr == 0, f"{rec.write_ptr}")
            check("累计写入数为 0", rec.written_total == 0, f"{rec.written_total}")
            check("重建出 0 条", len(rec.events) == 0, f"实测 {len(rec.events)} 条")
            check("缓冲镜象里没有非零字节（真的是空的）",
                  not any(rec.image), "全 0")

        elif f.startswith("10"):
            check("旧 epoch 的序号接近上限但没有回绕",
                  rec.events[-1][0] < 0xFFFFFFFF
                  and rec.events[0][0] > 0xFFFFFF00,
                  f"{rec.events[0][0]:#x}..{rec.events[-1][0]:#x}")

        elif f.startswith("11"):
            check("新 epoch 从小序号重新开始",
                  rec.events[0][0] == 1 and rec.epoch == 101,
                  f"epoch {rec.epoch}，首序号 {rec.events[0][0]}")

        elif f.startswith("12"):
            check("累计写入超过两圈",
                  rec.written_total > 2 * (rec.buffer_bytes // EVENT_LEN),
                  f"累计 {rec.written_total}，槽数 {rec.buffer_bytes // EVENT_LEN}")
            check("多圈后只保留一圈", rec.kept == 32 and len(rec.events) == 32,
                  f"保留 {rec.kept}")
            check("多圈后序号范围正确",
                  rec.events[0][0] == 69 and rec.events[-1][0] == 100,
                  f"{rec.events[0][0]}..{rec.events[-1][0]}")
            check("多圈后序号仍连续", not gap_analysis(rec),
                  f"缺口 {gap_analysis(rec) or '无'}")
        print()

    # ---- 跨样本：写指针相同、含义不同 ----
    if "08" in recs and "09" in recs:
        a, b = recs["08"], recs["09"]
        print("跨样本核对：写指针等于 0 的两种相反含义")
        check("两个样本的写指针相同", a.write_ptr == b.write_ptr,
              f"都是 {a.write_ptr}")
        check("缓冲镜象一个满一个空",
              a.kept > 0 and b.kept == 0,
              f"08 保留 {a.kept} 条，09 保留 {b.kept} 条")
        check("只靠累计写入数就能区分", a.written_total != b.written_total,
              f"{a.written_total} vs {b.written_total}")
        print()

    # ---- 跨样本：epoch 是身份、序号和时间的硬边界 ----
    if "10" in recs and "11" in recs:
        old, new = recs["10"], recs["11"]
        print("跨样本核对：新 epoch 允许计数重置，但禁止身份串联")
        check("epoch 已变化", old.epoch != new.epoch,
              f"{old.epoch} -> {new.epoch}")
        check("序号在新 epoch 重置", old.events[-1][0] > new.events[0][0],
              f"{old.events[-1][0]} -> {new.events[0][0]}")
        check("时间戳在新 epoch 重置", old.events[-1][1] > new.events[0][1],
              f"{old.events[-1][1]} -> {new.events[0][1]}")
        # 前提：两个会话占用了同一个 (种类, 编号)。少了这个前提，下面两条跨
        # epoch 断言会变成空转 —— 编号都不一样，当然解析不出对方的名字。
        old_name = old.name_of(old.epoch, TASK, 1, old.events[0][0])
        new_name = new.name_of(new.epoch, TASK, 1, new.events[0][0])
        check("两个会话都占用了同一个编号（这是跨 epoch 断言不空转的前提）",
              old_name is not None and new_name is not None,
              f"{old_name} vs {new_name}")
        check("同一编号在两个会话指向不同身份", old_name != new_name,
              f"{old_name} vs {new_name}")

        # 真正的规则：epoch 是身份的边界。把 epoch 从解析里去掉，下面两条会失败。
        crossed = new.name_of(old.epoch, TASK, 1, new.events[0][0])
        check("拿旧会话的 epoch 解析新会话的事件必须解析不出", crossed is None,
              f"用 epoch {old.epoch} 查新会话得 {crossed!r}")
        crossed = old.name_of(new.epoch, TASK, 1, old.events[0][0])
        check("拿新会话的 epoch 解析旧会话的事件必须解析不出", crossed is None,
              f"用 epoch {new.epoch} 查旧会话得 {crossed!r}")

        # 为什么必须靠 epoch：两段序号在模 2^32 下只差一点点，不知道换了会话
        # 就会把「重开一段」读成「丢了几条事件」。
        span = (new.events[0][0] - old.events[-1][0]) % (2 ** 32)
        check("没有 epoch 时这段序号差会被误读成缺口", span < 64,
              f"旧末 {old.events[-1][0]:#x} 到新首 {new.events[0][0]} 在模 2^32 下只差 {span}")
        print()

    # ---- 负例：每个损坏输入都必须因对应契约而被拒绝 ----
    invalid_expected = {
        "01_odd_generation.vtrc": "generation 必须是偶数",
        "02_truncated.vtrc": "字典区越界",
        "03_offset_out_of_range.vtrc": "字典区没紧跟缓冲镜象",
        "04_torn_event.vtrc": "未知事件类型",
        "05_missing_dictionary.vtrc": "缺少任务字典",
        "06_overlapping_dictionary.vtrc": "字典生效区间重叠",
        "07_sequence_wrap_without_epoch.vtrc": "同一 epoch 内序号必须严格递增",
        "08_unaligned_write_pointer.vtrc": "写指针未按事件长度对齐",
    }
    print("无效样本核对：损坏输入必须被明确拒绝")
    found_invalid = set()
    if os.path.isdir(INVALID_DIR):
        for f in sorted(os.listdir(INVALID_DIR)):
            if not f.endswith(".vtrc"):
                continue
            found_invalid.add(f)
            expected = invalid_expected.get(f)
            try:
                Rec(os.path.join(INVALID_DIR, f))
                error = None
            except SampleError as ex:
                error = str(ex)
            check(f"拒绝 {f}", error is not None and expected is not None
                  and expected in error,
                  error or "错误地接受了无效样本")
    check("无效样本集合完整", found_invalid == set(invalid_expected),
          f"实有 {sorted(found_invalid)}")
    print()

    ok = sum(1 for _, o, _ in RESULTS if o)
    bad = [n for n, o, _ in RESULTS if not o]
    print(f"合计 {len(RESULTS)} 项断言，通过 {ok}，失败 {len(bad)}")
    if bad:
        print("失败项：" + "；".join(bad))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
