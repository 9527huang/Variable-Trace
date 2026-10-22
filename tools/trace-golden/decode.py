"""RTEC2 golden vector decoder, Python side.

The third implementation of the same contract (docs/RTEC2-wire-format.md §3).
It exists to disagree loudly: if this and the C++ decoder read one stream
differently, one of them is wrong and the protocol is not frozen yet.

Same two rules as the C++ side:
  * an unknown record type is skipped and counted, never guessed at;
  * a record that runs off the end is reported as truncated bytes, and what
    came before it is kept.
"""

import json
import struct
import sys
from pathlib import Path

HEADER = 24
MAX_PAYLOAD = 12

TYPES = {
    0x01: "EpochBegin",
    0x02: "ClockSync",
    0x03: "Gap",
    0x04: "SourceStatus",
    0x10: "TaskCreate",
    0x11: "TaskDelete",
    0x12: "TaskRename",
    0x13: "TaskSwitchIn",
    0x14: "TaskSwitchOut",
    0x15: "TaskReady",
    0x16: "TaskBlock",
    0x17: "TaskWake",
    0x18: "PriorityChange",
    0x20: "SyncCreate",
    0x21: "SyncDelete",
    0x22: "WaitBegin",
    0x23: "WaitEnd",
    0x24: "Signal",
    0x25: "Acquire",
    0x26: "Release",
    0x30: "IsrEnter",
    0x31: "IsrExit",
    0x40: "Fault",
    0x50: "Alloc",
    0x51: "Free",
}

TASK_SUBJECT = {
    "TaskCreate", "TaskDelete", "TaskRename", "TaskSwitchIn", "TaskSwitchOut",
    "TaskReady", "TaskBlock", "TaskWake", "PriorityChange",
    "WaitBegin", "WaitEnd", "Signal", "Acquire", "Release",
}


def record_bytes(data, at):
    """How many bytes the record at `at` takes, or 0 when it does not fit."""
    if len(data) - at < HEADER:
        return 0

    flags = data[at + 1]
    used = HEADER

    if flags & 0x01:
        if len(data) - at < used + 4:
            return 0
        used += 4

    if flags & 0x02:
        if len(data) - at < used + 1:
            return 0
        payload = data[at + used]
        if payload > MAX_PAYLOAD or len(data) - at < used + 1 + payload:
            return 0
        used += 1 + payload

    return used


def decode(data, name="stream"):
    records = []
    unknown = 0
    truncated = 0
    at = 0

    while at < len(data):
        used = record_bytes(data, at)
        if used == 0:
            truncated = len(data) - at
            break

        raw_type = data[at]
        type_name = TYPES.get(raw_type, "Unknown")
        sequence, subject, obj, arg0, arg1 = struct.unpack_from("<IIIII", data, at + 4)

        cursor = HEADER
        has_ticks = 0
        ticks = 0
        if data[at + 1] & 0x01:
            has_ticks = 1
            (ticks,) = struct.unpack_from("<I", data, at + cursor)
            cursor += 4

        payload_bytes = 0
        if data[at + 1] & 0x02:
            payload_bytes = data[at + cursor]

        if type_name == "Unknown":
            unknown += 1
        else:
            # An interrupt is about neither a task nor an object: its number is
            # not a handle. Same rule as the C++ decoder, kept as a set rather
            # than a second table so the two cannot drift.
            if type_name in TASK_SUBJECT:
                subject_handle = subject
            else:
                subject_handle = 0

            object_handle = obj if type_name in {
                "SyncCreate", "SyncDelete", "WaitBegin", "WaitEnd", "Signal",
                "Acquire", "Release", "TaskBlock", "TaskWake",
            } else 0

            records.append({
                "index": len(records),
                "type": raw_type,
                "typeName": type_name,
                "sequence": sequence,
                "subject": subject_handle,
                "object": object_handle,
                "arg0": arg0,
                "arg1": arg1,
                "hasTargetTicks": has_ticks,
                "targetTicks": ticks,
                "payloadBytes": payload_bytes,
            })

        at += used

    return {
        "file": name,
        "records": records,
        "unknownTypes": unknown,
        "truncatedBytes": truncated,
    }


def main(argv):
    if len(argv) != 2:
        print("usage: decode.py <file.bin>", file=sys.stderr)
        return 2

    path = Path(argv[1])
    result = decode(path.read_bytes(), path.stem)
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
