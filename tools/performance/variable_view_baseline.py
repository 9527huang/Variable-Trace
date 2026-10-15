"""Capture one Variable Viewer performance baseline window over MCP.

Prepare and start the desired acquisition in Variable-Trace first. This script
is intentionally read-only: project, probe, variable layout and GUI/CSV state
remain explicit parts of the test scenario instead of being silently changed.
"""

import argparse
import datetime
import json
import platform
import time
import urllib.request


def call_tool(endpoint, name, arguments=None, timeout=30):
    payload = {
        "jsonrpc": "2.0",
        "id": 1,
        "method": "tools/call",
        "params": {"name": name, "arguments": arguments or {}},
    }
    request = urllib.request.Request(
        endpoint,
        data=json.dumps(payload).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Accept": "application/json, text/event-stream",
        },
    )
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with opener.open(request, timeout=timeout) as response:
        envelope = json.loads(response.read().decode("utf-8"))

    if "error" in envelope:
        raise RuntimeError(json.dumps(envelope["error"], ensure_ascii=False))

    result = envelope.get("result", {})
    if result.get("isError"):
        raise RuntimeError("\n".join(part.get("text", "") for part in result.get("content", [])))

    text = "\n".join(part.get("text", "") for part in result.get("content", []))
    return json.loads(text)


def metric_delta(before, after, name):
    return after.get(name, 0) - before.get(name, 0)


def main():
    parser = argparse.ArgumentParser(description="Capture a read-only Variable Viewer performance baseline window")
    parser.add_argument("--endpoint", default="http://127.0.0.1:7777/mcp")
    parser.add_argument("--seconds", type=float, default=40.0)
    parser.add_argument("--label", required=True, help="Stable scenario name, for example jlink-normal-8-contiguous")
    parser.add_argument("--output", help="Append the result as one JSON line")
    args = parser.parse_args()

    if args.seconds <= 0:
        parser.error("--seconds must be greater than zero")

    before_state = call_tool(args.endpoint, "get_acquisition_state")
    if before_state.get("state") != "RUN":
        raise RuntimeError("acquisition must already be RUN")

    before = before_state.get("metrics")
    if not isinstance(before, dict):
        raise RuntimeError("this build does not expose acquisition metrics")

    began = time.monotonic()
    time.sleep(args.seconds)
    ended = time.monotonic()

    after_state = call_tool(args.endpoint, "get_acquisition_state")
    after = after_state.get("metrics", {})
    if after_state.get("state") != "RUN":
        raise RuntimeError("acquisition stopped during the baseline window")
    if after.get("run_id") != before.get("run_id"):
        raise RuntimeError("acquisition restarted during the baseline window")
    for field in ("sampled_variables", "merged_reads_enabled", "planned_probe_calls_per_sample", "merged_read_spans"):
        if after.get(field) != before.get(field):
            raise RuntimeError(f"acquisition layout or read strategy changed during the baseline window: {field}")

    wall_seconds = ended - began
    completed = metric_delta(before, after, "completed_samples")
    probe_calls = metric_delta(before, after, "probe_calls")
    probe_read_ns = metric_delta(before, after, "probe_read_ns")
    model_update_ns = metric_delta(before, after, "model_update_ns")

    result = {
        "schema": 1,
        "captured_at": datetime.datetime.now(datetime.timezone.utc).astimezone().isoformat(),
        "label": args.label,
        "endpoint": args.endpoint,
        "host": {"platform": platform.platform(), "python": platform.python_version()},
        "window_seconds": wall_seconds,
        "run_id": after.get("run_id"),
        "sampled_variables": after.get("sampled_variables"),
        "merged_reads_enabled": after.get("merged_reads_enabled"),
        "planned_probe_calls_per_sample": after.get("planned_probe_calls_per_sample"),
        "merged_read_spans": after.get("merged_read_spans"),
        "completed_samples": completed,
        "failed_samples": metric_delta(before, after, "failed_samples"),
        "actual_sampling_hz": completed / wall_seconds,
        "deadline_misses": metric_delta(before, after, "deadline_misses"),
        "skipped_sample_periods": metric_delta(before, after, "skipped_sample_periods"),
        "probe_calls": probe_calls,
        "probe_calls_per_sample": probe_calls / completed if completed else 0.0,
        "bytes_read": metric_delta(before, after, "bytes_read"),
        "average_probe_call_us": probe_read_ns / probe_calls / 1000.0 if probe_calls else 0.0,
        "average_probe_sample_us": probe_read_ns / completed / 1000.0 if completed else 0.0,
        "average_model_update_us": model_update_ns / completed / 1000.0 if completed else 0.0,
        "probe_lock_wait_ms": metric_delta(before, after, "probe_lock_wait_ns") / 1_000_000.0,
        "model_lock_wait_ms": metric_delta(before, after, "model_lock_wait_ns") / 1_000_000.0,
        "shared_buffer_lock_wait_ms": metric_delta(before, after, "shared_buffer_lock_wait_ns") / 1_000_000.0,
        "csv_format_ms": metric_delta(before, after, "csv_format_ns") / 1_000_000.0,
        "scheduler_sleep_ms": metric_delta(before, after, "scheduler_sleep_ns") / 1_000_000.0,
        "diagnosis": after.get("diagnosis"),
        "start_metrics": before,
        "end_metrics": after,
    }

    rendered = json.dumps(result, ensure_ascii=False, indent=2)
    print(rendered)

    if args.output:
        with open(args.output, "a", encoding="utf-8") as stream:
            stream.write(json.dumps(result, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    main()
