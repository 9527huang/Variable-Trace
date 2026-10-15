# Variable Viewer performance baseline

Date: 2026-10-13

This document defines the repeatable P0 baseline used before changing the
Variable Viewer acquisition path. It records measurements; it does not set an
expected probe speed.

## Instrumentation

`get_acquisition_state` keeps its existing `state` field and additionally
returns a `metrics` object for the current run. Counters reset after a probe is
successfully started and `run_id` advances. The snapshot includes:

- requested, completed and failed logical samples;
- sampled variable count, probe calls, successful calls and bytes read;
- deadline misses, skipped sample periods and maximum lateness;
- cumulative absolute-deadline scheduler sleep time;
- cumulative probe read and probe-lock wait time;
- cumulative model update, model-lock and shared-buffer-lock wait time;
- cumulative CSV formatting time and elapsed run time.

The acquisition thread only updates atomic integer counters. Formatting and
derived rates happen when a client requests a snapshot.

## Deterministic pre-optimization result

`ViewerDataHandlerPerformanceTest.OriginalPerVariablePathRemainsAvailable`
is the machine-independent P0 guard retained in P1. With merging disabled,
eight contiguous U32 variables in Normal mode perform exactly:

```text
probe calls = completed samples * 8
bytes read  = completed samples * 8 * 4
```

`ContiguousRamUsesOneBlockTransactionPerSample` checks the P1 path: the same
eight variables use one block call per sample and still transfer 32 bytes.
CI checks counts, decoded values and bytes, not
wall-clock throughput, because scheduler load would make a narrow timing limit
unreliable.

## Hardware matrix

Prepare the project and start acquisition, then capture a 40-second window:

```powershell
python tools/performance/variable_view_baseline.py `
  --endpoint http://127.0.0.1:7777/mcp `
  --seconds 40 `
  --label jlink-normal-8-contiguous `
  --output variable-view-baseline.jsonl
```

Run each scenario three times. Keep the middle 40 seconds after at least 10
seconds of warm-up. Do not change the ELF, target firmware, probe firmware,
SWD speed, USB topology, window size, viewport, max points or CSV directory
between comparisons.

| Variables | Layout | GUI | CSV | Required modes |
|---:|---|---|---|---|
| 1 | contiguous | off/on | off | Normal |
| 8 | contiguous | off/on | off/on | Normal, HSS/Serial when available |
| 20 | contiguous | off/on | off/on | Normal, HSS/Serial when available |
| 100 | contiguous | on | off | Normal, HSS when available |
| 8 | sparse | off/on | off | Normal |
| 20 | mixed width and unaligned | off/on | off | Normal |

Record the target board, firmware commit, ELF hash, probe model and firmware,
connection type, configured link speed, requested sample rate, host CPU/OS and
application build commit beside the JSONL result.

## Current hardware result

A live 40-second baseline was captured with the Release build and an existing
project. This is a representative high-load project result, not a substitute
for the controlled 1/8/20-variable matrix above.

| Setting | Value |
|---|---|
| Target | STM32F103C8 |
| Probe | J-Link USB, serial 000069730307 |
| J-Link host library | 8.10k |
| Transport | SWD, configured at 4000 kHz |
| Acquisition mode | Normal |
| Requested sampling rate | 100 Hz |
| GUI series | 46 visible series |
| Actual sampled variables | 47 (including XY-axis dependencies) |
| CSV | Off |
| Host | Windows 11, AMD Ryzen AI MAX+ 392 w/ Radeon 8060S |
| ELF SHA-256 | `53EAE483CBD3029255525F3CB601F67DAE3AC6C88457DF8656487D96E7338D50` |
| Application base commit | `810a1a16f3f88cad2ab8f01da3b3e0580f7cf456` plus the P0 working-tree changes |
| Firmware commit | Unavailable; the firmware directory is not a Git repository |

The captured window produced:

```text
completed samples          1693
actual sampling rate       42.325 Hz
failed samples             0
deadline misses            1693
probe calls                79587
probe calls per sample     47.009
bytes read                 259076
average probe call         502.425 us
average model update       29.477 us
probe lock wait            0.0633 ms
model lock wait            1.0217 ms
shared-buffer lock wait    0.8613 ms
CSV formatting             0 ms
```

Every completed sample missed the requested 100 Hz schedule. Probe reads
dominated the acquisition time, while model updates and lock waits were minor.
The result also confirms that the current scheduler accumulates lateness when
the requested rate exceeds the probe-limited rate. This makes per-variable
probe round trips the first P1 optimization target.

### Controlled contiguous-variable baseline

The controlled Normal-mode baseline used `waveTable[0..19]`, a contiguous
array of `f32` values resolved from the same ELF. The requested rate was 100
Hz, the GUI was running, CSV was off, and each cell below is the median of
three independent 40-second windows after a 10-second warm-up.

| Variables | Samples/window | Actual Hz | Deadline misses | Probe calls/sample | Probe call (us) | Model update (us/sample) | Failures |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 4002 | 100.050 | 2 | 1.000 | 524.332 | 3.408 | 0 |
| 8 | 4003 | 100.075 | 3 | 8.001 | 507.421 | 6.709 | 0 |
| 20 | 3963 | 99.075 | 3963 | 20.001 | 504.116 | 13.679 | 0 |

The slight non-integer calls-per-sample values are snapshot-boundary effects:
a metrics request can land after a sample's first probe call and before that
sample is completed. Across complete samples, the deterministic test confirms
the current path performs exactly one transaction per variable.

At 20 variables, the median probe cost alone is approximately 10.08 ms per
sample, already beyond the requested 10 ms period. Every completed sample in
all three windows missed its deadline, while model update work remained about
13.7 us per sample. The controlled result independently confirms that probe
round-trip count is the dominant pre-optimization bottleneck.

The raw harness output is stored locally at
`analysis/variable-view-baseline-2026-10-13.jsonl`. The `analysis` directory is
ignored by Git because it can contain machine- and project-specific data.

The controlled GUI-on, CSV-off 1/8/20 baseline is complete. GUI-off, CSV-on,
sparse, mixed-width, HSS and Serial rows remain comparison scenarios for the
P1 acceptance campaign rather than blockers for starting implementation.
Historical engineering notes reporting approximately 170-187 us per
transaction and about 4.3 kHz for a single busy-polled variable remain
references only; they were not produced by this baseline harness.

P1 must not begin on a specific hardware path until its corresponding Normal
mode baseline has been captured. Missing HSS, Serial or GDB Server hardware is
recorded as unverified rather than inferred from another probe.

## P1: contiguous RAM reads and local decoding

Normal-mode acquisition now builds a read plan when the sampled address list
is built or refreshed. Only overlapping or strictly adjacent ranges in the ARM
SRAM address region (`0x20000000` through `0x3fffffff`) are merged. No holes are
filled and no extra bytes outside the selected ranges are requested. This is
an architecture-level address heuristic, not a device-specific memory map.
Peripheral and unknown regions keep exact reads. A merged span is fetched via
`readBlock`, then copied into the existing per-address raw-value representation;
type conversion, fractional variables, XY dependencies, plotting and CSV still
use the existing model update path.

Serial retains its configured sweep protocol and HSS retains its streaming
protocol. ST-Link spans whose address or length is not four-byte aligned keep
per-variable reads because its mem32 block transport requires alignment.
Single-variable spans use `readMemory`; exact-read buffers reserve at least
four bytes for legacy probe implementations that write a word for a narrow type.

The old path can be selected through MCP, while acquisition is stopped:

```text
stop_acquisition {}
set_sampling {"merge_contiguous_reads": false}
start_acquisition {}
```

Set the flag to `true` to re-enable merging. The flag defaults to enabled and
is a runtime setting; it is not persisted in project files. Reloading a project
restores the default. A failed block read stops acquisition and counts a failed
sample; it does not automatically reread the target through the old path in
the same sample. The user can select the old path and restart acquisition.

`planned_probe_calls_per_sample` and `merged_read_spans` describe the current
plan, not cumulative counters. The latter must not be subtracted across a
measurement window. Probe calls count interface invocations; a driver may split
a block into several transport transactions. A block read is not an atomic
snapshot of a running MCU. Timestamps retain the existing read-completion
semantics; midpoint timestamps are outside this P1 change.

Deterministic coverage includes contiguous, overlapping and sparse ranges,
peripheral exclusion, mixed U8/I16/F32/U32 decoding with unaligned offsets,
the old path, Serial isolation, ST-Link alignment fallback, failure handling,
run counter reset and MCP metrics. Hardware measurements below cover J-Link
Normal only; other probes, HSS, Serial and CSV-on remain hardware-unverified.

### P0/P1 hardware comparison

P1 used a fresh, unsaved minimal project with only the selected
`waveTable[0..N-1]` variables and one visible curve. The target, ELF hash,
J-Link, SWD speed and acquisition mode match P0 above. Both runs used the
Release configuration (`-O3 -DNDEBUG`), GUI on, CSV off, 100 Hz requested,
2,000 retained points and a 50,000 ms viewport. P1 includes the P0/P1
working-tree changes on the same base commit. Each reported number is the
median of three 40-second windows following a 10-second warm-up.

| Variables | P0 calls/sample | P1 calls/sample | P0 probe us/sample | P1 probe us/sample | Probe time reduction | P0 Hz | P1 Hz | P0/P1 median deadline misses |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1.000 | 1.000 | 524.332 | 542.460 | -3.5% | 100.050 | 100.075 | 2 / 0 |
| 8 | 8.001 | 1.000 | 4059.747 | 706.361 | 82.6% | 100.075 | 100.125 | 3 / 0 |
| 20 | 20.001 | 1.000 | 10082.072 | 963.029 | 90.4% | 99.075 | 100.125 | 3963 / 0 |

All nine P1 windows recorded zero failures and zero deadline misses. P1 model
update medians were 2.081, 6.098 and 10.356 us/sample respectively. One variable
has no merging benefit; its small probe-time increase should not be described
as a performance gain. At eight and twenty variables the optimization reduces
probe time and restores schedule headroom, not an eightfold or twentyfold
observed sampling rate: the rate remains capped by the 100 Hz setting.
Window rates slightly above 100 Hz include the MCP snapshot-boundary effect.

Probe time **per sample** is computed from the delta of `probe_read_ns` divided
by the delta of `completed_samples`. It must not be confused with probe time
per call, since merging changes the number of calls. P0/P1 model-update timing
also depends on the project contents, so probe cost and transaction count are
the primary comparison metrics.

Final raw snapshots and returned values are stored locally in
`analysis/variable-view-p1-final-2026-10-13.jsonl`. Earlier P1 exploratory
captures using the larger existing project are supplemental only and are not
used in this table. The original P0 JSONL remains unchanged.

### Same-session old-path control

After the P1 matrix, the same minimal twenty-variable project was recreated
with `merge_contiguous_reads=false` and measured for another three 40-second
windows. Its medians were 99.900 Hz, 20.002 calls/sample and 10004.524 us of
probe time/sample. Deadline misses were 3975, 3996 and 4004; failures were zero.
Relative to this same-build old-path control, merging reduced probe time by
90.4%. This also confirms the original path remains usable through the switch.

All twenty returned values, including the nonzero and small floating-point
values, matched exactly between merged and old-path captures, and every result
reported `ok=true`. This is evidence of agreement with the old reader on this
target; it does not independently validate the firmware's intended signal
contents or guarantee simultaneous values on a changing target. Deterministic
mixed-width tests separately validate the local byte extraction and conversions.

### Final validation

The final source, including ST-Link alignment fallback and HSS plan reporting,
built successfully in Debug and Release. The complete test executable passed
515/515 tests across 56 suites, including all ten acquisition performance
tests. `git diff --check` passed. The J-Link hardware timing used the P1 Release
binary before the final ST-Link alignment and HSS reporting guards; those guards
do not alter the measured J-Link Normal contiguous path. The later ST-Link
matrix used the rebuilt binary. The self-launched measurement processes were
stopped after capture.

### ST-Link hardware comparison

On 2026-10-14 the same minimal project was measured with an ST-Link USB probe
connected to an STM32F103C8. The ELF, target firmware, 4,000 kHz SWD speed,
Normal mode, GUI, CSV setting, requested 100 Hz and window settings matched the
J-Link protocol. Each row is the median of three 40-second windows after a
10-second warm-up.

| Variables | Read strategy | Calls/sample | Probe us/sample | Actual Hz | Deadline misses | Failures |
|---:|---|---:|---:|---:|---:|---:|
| 1 | merged enabled (one exact span) | 1.000 | 217.823 | 100.100 | 0 | 0 |
| 8 | contiguous block | 1.000 | 410.478 | 100.075 | 0 | 0 |
| 20 | contiguous block | 1.000 | 686.880 | 100.100 | 0 | 0 |
| 20 | old per-variable fallback | 20.002 | 3507.399 | 100.075 | 0 | 0 |

The merged ST-Link path reduced probe time for 20 variables by 80.4% compared
with the same-session old-path control. ST-Link is faster per transaction than
the J-Link used for the earlier baseline, so the old 20-variable path remained
within the 100 Hz schedule and recorded no deadline misses. The comparison is
therefore about transaction cost and headroom, not a claim that the sample rate
must increase above the configured 100 Hz.

All 20 returned values matched exactly between the merged and fallback paths,
with all results marked `ok=true`. The raw ST-Link capture is stored locally at
`analysis/variable-view-p1-stlink-2026-10-14.jsonl`.

### P1 non-hardware close-out

On 2026-10-14 the final test, Debug and Release targets built successfully.
The complete automated suite passed 515/515 tests across 56 suites with no
disabled tests; the `ViewerDataHandlerPerformanceTest` suite passed 10/10.
`git diff --check` also passed. Release and Debug output directories both carry
the 63-file ST-Link chip database required for target identification.

P1 is therefore **code complete with non-hardware close-out passed**. It is not
marked unconditionally complete while ST-Link hardware is unavailable. The
remaining hardware acceptance list is: unaligned and mixed-width reads, sparse
RAM/peripheral layouts, CSV enabled, runtime variable/group changes, stop/start,
disconnect/reconnect and a 30-minute stability run. Serial, HSS and GDB Server
hardware throughput matrices also remain unverified. These deferred items are
acceptance work, not evidence of a known functional failure.

## P2.1 absolute-deadline scheduler baseline

P2.1 replaces the Normal-mode floating-point elapsed-time polling loop with a
`steady_clock` absolute deadline. A completed attempt advances the deadline by
one interval. If work crosses one or more complete intervals, those expired
periods are counted and skipped before the next attempt; the reader never runs
a burst to manufacture missing historical samples. A runtime frequency change
reanchors the schedule at the new frequency.

Waiting is interruptible on platforms with precise standard timed waits. On
Windows, the standard wait measured only about 65-70 Hz for a requested 100 Hz
in the test process because of timer granularity, so the implementation uses a
high-resolution waitable timer. Waits are capped at 50 ms to bound stop and
restart response when the requested sampling rate is low. HSS remains driven by
its streaming reader and is not rate-limited by this Normal-mode scheduler.

The deterministic baseline uses the zero-delay `PerformanceDebugProbe`, merged
contiguous U32 variables, 100 Hz, and at least 201 completed samples. It records
properties in the GoogleTest JSON result rather than imposing a narrow
wall-clock microbenchmark on CI.

| Variables | Completed | Actual Hz | Scheduler sleep/run time | Skipped periods |
|---:|---:|---:|---:|---:|
| 1 | 201 | 100.479 | 99.759% | 0 |
| 8 | 201 | 100.483 | 99.725% | 0 |
| 20 | 202 | 100.474 | 99.598% | 0 |

The ratio is an idle-time indicator for the acquisition thread, not a process
CPU measurement. It shows that the low-cost 100 Hz case waits instead of busy
polling. The slight rate above 100 Hz is a short-window boundary effect because
the first sample is due immediately.

The overload guard uses an eight-variable merged read at 1,000 Hz with a fake
probe delay. On this Windows host the fake probe's standard 5 ms sleep rounded
to the system timer granularity: 21 samples completed at 62.628 Hz while 300
expired periods were skipped. Probe calls and requested samples remained equal
to completed samples, confirming there was no catch-up burst. The 1 Hz stop
response test completed in 44 ms. A 10 Hz to 100 Hz runtime frequency change
also reanchored and delivered 15 new samples within the 400 ms acceptance bound.

P2.1 added `scheduler_sleep_ns`/`scheduler_sleep_ms` and
`skipped_sample_periods` to `get_acquisition_state.metrics`. From P2.1 onward,
`deadline_misses` counts expired scheduled periods and equals
`skipped_sample_periods` for Normal mode. Debug, Release and test targets built
successfully, the scheduler suite passed 16/16, and the complete suite passed
521/521 tests across 56 suites.

## P2.2 fixed sample layout baseline

P2.2.1 removes the per-sample Normal-mode raw-value hash map. When the sample
list is rebuilt, the handler now creates a stable address-to-slot index and
resizes reusable value and validity arrays to the sample-list size. Merged
reads and exact fallback reads decode into those arrays. The HSS and Serial
paths still use their existing timestamp/map input because their probe-side
protocols have different sample ownership and timing semantics.

The following non-hardware baseline was captured on 2026-10-14 with the
deterministic zero-delay `PerformanceDebugProbe`, merged contiguous U32
variables, one visible plot, CSV disabled, and 100 Hz. Values are averages over
the completed samples in each test. `model_update` includes the existing model
submission and Plot update work; it is not a claim that P2.2.1 already removes
the full-variable or full-Plot scans.

| Variables | Completed | Actual Hz | Model update us/sample | Model lock wait us/sample | Shared buffer wait us/sample | Probe read us/sample | Skipped periods |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 202 | 100.490 | 9.176 | 0.182 | 0.119 | 4.211 | 0 |
| 8 | 202 | 100.495 | 15.348 | 0.200 | 0.130 | 6.624 | 0 |
| 20 | 202 | 100.492 | 21.379 | 0.370 | 0.110 | 8.164 | 0 |

The raw GoogleTest JSON is generated as `build_test/p22-baseline.json` during
verification; it is a build artifact and is not treated as a hardware result.
The P2.2.1 correctness evidence is the 16-test performance suite plus the
complete 521-test suite. No ST-Link measurement was possible in this run, so
probe transaction time and hardware throughput remain covered only by the P1
records.

## P2.2.2 bound model baseline

P2.2.2 extends the fixed sample layout with bindings rebuilt under the model
lock. Each binding stores a `shared_ptr<Variable>` and its sample slot; the
active Plot list stores only visible entries of the active group. Normal,
HSS, and Serial model submission now iterate these bindings. A restart caused
by a variable, Series, group, or visibility change rebuilds the bindings before
the next sample, so no raw variable pointer is retained across model edits.

The same deterministic 100 Hz, zero-delay probe matrix was rerun after the
change. Values are per completed sample and include the existing Plot update
work:

| Variables | Completed | Actual Hz | Model update us/sample | Model lock wait us/sample | Shared buffer wait us/sample | Probe read us/sample | Skipped periods |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 201 | 100.484 | 12.109 | 0.487 | 0.219 | 8.411 | 0 |
| 8 | 202 | 100.481 | 15.244 | 0.264 | 0.245 | 7.738 | 0 |
| 20 | 202 | 100.487 | 19.468 | 0.224 | 0.191 | 8.350 | 0 |

The 20-variable model-update time is lower than the P2.2.1 capture (21.379 us)
on this run, while the one-variable result is noisier because the absolute
cost is very small; these are CI baselines, not statistically controlled
microbenchmark claims. The raw result is `build_test/p222-baseline.json`.

## P2.2.3 Series and CSV hot-path baseline

P2.2.3 binds visible Series and XY horizontal-axis updates when the active
sample plan is rebuilt. `Plot::updateSeries()` receives only the visible Series
for that active Plot, while the existing full-update overload remains for
other callers. Viewer CSV columns are also bound in header order and written
through a vector overload, avoiding per-sample string-key construction and
hash lookups. The original map overload remains for TraceDataHandler.

The deterministic zero-delay probe was rerun at 100 Hz with one visible Plot
and CSV disabled. The model-update duration includes the bound Series update
and time-point append:

| Variables | Completed | Actual Hz | Model update us/sample | Model lock wait us/sample | Shared buffer wait us/sample | Probe read us/sample | Skipped periods |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 201 | 100.479 | 2.541 | 0.224 | 0.126 | 5.217 | 0 |
| 8 | 201 | 100.490 | 3.709 | 0.179 | 0.164 | 5.996 | 0 |
| 20 | 201 | 100.495 | 6.834 | 0.180 | 0.159 | 10.328 | 0 |

Relative to the P2.2.2 capture, the 20-variable model-update duration fell from
19.468 us to 6.834 us on this host. These are repeatable CI acceptance baselines,
not controlled laboratory microbenchmarks. The raw result is
`build_test/p223-baseline.json`. CSV file and header behavior remains covered
by the existing CSVStreamer tests; no ST-Link hardware was available for a
hardware CSV throughput measurement.

## P2.3 lock-boundary and close-out baseline

P2.3 keeps the variable update, bound Series append, time-point append, and CSV
numeric snapshot under the model lock. The lock is released before CSV text
formatting, buffer exchange, and asynchronous file scheduling. This preserves
one internally consistent sample while removing file-format work from the
model critical section.

Two non-hardware acceptance scenarios were added:

| Scenario | Result |
|---|---:|
| 10,000 imported variables, 8 sampled, CSV off | 2.125 us model update/sample |
| 20 sampled variables, CSV on | 24.304 us total update/sample |
| CSV formatting portion of the CSV-on case | 21.168 us/sample, outside model lock |
| 50 add/remove/rebuild cycles while sampling | continued sampling, zero failures |

The difference between total update and CSV formatting in the CSV-on capture
is about 3.137 us/sample; it includes the locked submission plus measurement
overhead and is not exposed as a separate production metric. The raw scale and
CSV result is `build_test/p23-baseline.json`.

The full suite now passes 524/524 tests across 56 suites. P2 is code complete
with non-hardware close-out passed. ThreadSanitizer is not available in the
current Windows/MinGW environment. Hardware CSV throughput, probe/model lock
contention, and long-run stability remain deferred until a probe is available.

## P3 curve snapshot and display-decimation baseline

P3.1 gives each `ScrollingBuffer` snapshot its own valid count, ring offset and
generation. A snapshot copies only the valid prefix before the ring fills and
reuses the previous copy when no sample has changed. Once the ring wraps, the
copy retains the physical ring layout and consumers traverse it from the saved
snapshot offset. Plot drawing, cursor lookup, Plot CSV export and MCP data reads
use snapshot metadata rather than combining copied data with live metadata.

P3.2 adds visible-window min/max aggregation for time-axis curves. Variable
Viewer CURVE plots and SWO stairs plots generate at most two extrema per pixel
column plus the visible endpoints. The display cache is invalidated by either
axis generation, the visible X range, or pixel width. Statistics, CSV and MCP
continue to consume the complete snapshot. XY is deliberately unchanged
because a time-column algorithm does not preserve a two-dimensional path;
Bar and Table keep their latest-value behavior.

The deterministic non-hardware checks establish these properties:

| Scenario | Result |
|---|---:|
| 3 valid samples in a 1,000-sample buffer | 3 slots copied |
| Repeated copy without a write | snapshot generation reused, no copy |
| Wrapped five-slot buffer after seven writes | logical values remain 3..7 |
| 1,000 points over 10 pixel columns | no more than 22 output points |
| Adjacent +100/-100 spikes | both extrema retained |
| Requested X window 40..59 | only points from 40..59 returned |

This is an algorithm and regression baseline, not the complete P3 performance
acceptance. The 1/8/20 acquisition matrix does not measure GUI rendering cost.
The planned 1x8 and 4x20 plot matrices, maximum-history case, 1080p/high-DPI
windows, follow/pan/zoom interaction, frame P50/P95/P99, copied bytes and model
lock wait still require an instrumented GUI run. Hardware sampling stability
also remains deferred until a probe is available.

### P3.3 GUI CPU measurement and close-out

The production frame loop now keeps a bounded 2,048-frame measurement window.
`get_acquisition_state.metrics.gui` reports CPU frame-build and total frame
(including swap/vsync) P50/P95/P99, variable-plot time, bytes actually copied
into snapshots, raw and displayed point counts, decimation time and cache
misses. Counters reset when the acquisition run id changes. A snapshot byte is
counted only when `copyData()` performs a copy, not when a generation is reused.

The repeatable Release benchmark executes real ImGui/ImPlot CPU plot creation
without an OpenGL backend. Every result below contains a 30-frame warm-up and
240 measured frames, except the deliberately large maximum-history case, which
uses 3 warm-up and 10 measured frames. Decimation is recomputed every frame to
model a live data generation; its output vector capacity is reused as it is in
the application. GPU drawing, buffer swap and display latency are not included.

| Scenario | Width | Path | Points/frame | P50 us | P95 us | P99 us |
|---|---:|---|---:|---:|---:|---:|
| 1 plot x 8 curves x 10,000 | 1920 | raw | 80,000 | 424.9 | 655.5 | 845.6 |
| 1 plot x 8 curves x 10,000 | 1920 | P3 | 30,080 | 363.8 | 459.6 | 648.8 |
| 4 plots x 20 curves x 10,000 | 1920 | raw | 800,000 | 5,099.3 | 5,822.7 | 6,106.6 |
| 4 plots x 20 curves x 10,000 | 1920 | P3 | 300,800 | 3,966.3 | 4,503.2 | 4,793.7 |
| 1 plot x 8 curves x 10,000 | 3840 | raw | 80,000 | 426.8 | 558.3 | 666.3 |
| 1 plot x 8 curves x 10,000 | 3840 | P3 policy | 80,000 | 430.1 | 703.5 | 714.5 |
| 4 plots x 20 curves x 10,000 | 3840 | raw | 800,000 | 5,072.4 | 5,873.8 | 6,291.9 |
| 4 plots x 20 curves x 10,000 | 3840 | P3 policy | 800,000 | 5,083.7 | 5,638.7 | 5,873.9 |
| 1 plot x 20 curves x 1,597,830, latest 10,000 visible | 1920 | raw | 31,956,600 | 78,698.2 | 81,101.5 | 81,101.5 |
| 1 plot x 20 curves x 1,597,830, latest 10,000 visible | 1920 | P3 | 75,200 | 1,016.7 | 1,059.0 | 1,059.0 |

At 1920 logical pixels the ordinary scenarios reduce submitted points by 62.4%.
The 1x8 P50/P95 improve by 14.4%/29.9%; the 4x20 P50/P95 improve by
22.2%/22.7%. The maximum-history P50 improves by 98.7%. Visible-window lookup
uses binary search over logical ring order, so viewing the newest end does not
linearly scan the hidden history for every curve.

An exploratory run showed that forcing aggregation at 3840 pixels made the
10,000-point case slower because the output was already close to the input.
The production policy now requires more than three samples per horizontal
pixel. Therefore the 3840 rows above intentionally take the raw path; timing
differences between their two identical drawing paths are run-order noise, not
an optimization claim.

The benchmark can be reproduced with:

```powershell
ninja -C build_rel P3-Gui-Benchmark
.\build_rel\P3-Gui-Benchmark.exe --output analysis\variable-view-p3-gui-cpu-2026-10-14.json
```

The raw JSON is `analysis/variable-view-p3-gui-cpu-2026-10-14.json`. P3 is code
complete with non-hardware CPU close-out passed. A real GPU/display run is still
required for total-frame P50/P95/P99, 1080p/high-DPI presentation, pan/zoom
interaction and subjective input responsiveness. A probe is still required to
correlate those GUI metrics with sampling rate and shared-buffer lock wait.

The P3 source builds successfully in Debug and Release. The complete automated
suite passes 532/532 tests across 59 suites, including snapshot, decimator,
performance-tracker and Plot CSV regression tests. The Release build retains
the previously noted Statistics optimizer, TraceDataHandler strict-aliasing and
RecorderTools unused function warnings; no new P3 warning was introduced.

## P4.1 CSV binary queue baseline

The production CSV path now places only a timestamp, header generation and an
ordered vector of binary `double` values into a bounded 1,024-entry queue. A
persistent writer thread formats rows and writes batches of up to 64 queue
items. `finishLogging()` drains all accepted rows before closing the file.
Changing the directory drains the old file first, and a changed header remains
ordered between the rows belonging to its two column generations.

If the queue fills, the first P4 policy is `stop logging`: no new CSV rows are
accepted, the writer drains and closes those already accepted, and acquisition
continues. Rows are never overwritten. The synchronous implementation remains
available as an explicit fallback and supplies the A/B reference, but the
application uses the asynchronous path by default.

The Release benchmark submits 20,000 rows per case to a temporary local file.
Its queue is deliberately sized for the complete burst so it measures producer
cost rather than overload policy. Timings include the binary vector copy and
queue lock. End-to-end includes the final drain and close.

| Variables | Mode | Submit P50/P95/P99 us | Submit total ms | End-to-end ms |
|---:|---|---:|---:|---:|
| 1 | synchronous fallback | 0.7 / 0.9 / 1.0 | 16.8 | 17.0 |
| 1 | asynchronous | 0.1 / 0.3 / 0.8 | 4.5 | 14.7 |
| 8 | synchronous fallback | 2.6 / 3.3 / 12.2 | 61.2 | 61.3 |
| 8 | asynchronous | 0.1 / 0.3 / 0.3 | 4.4 | 58.1 |
| 20 | synchronous fallback | 6.0 / 14.2 / 17.5 | 134.7 | 134.9 |
| 20 | asynchronous | 0.2 / 0.3 / 0.5 | 5.3 | 128.7 |

For 20 variables the producer-side total falls by 96.0%; the remaining format
and file time is paid by the writer. All six cases wrote all 20,000 accepted
rows with zero rejection. A deterministic slow-writer test uses a two-entry
queue and verifies overload, bounded high-water mark, accepted equals written,
queue drain and exact output line count. Existing restart, directory change,
header generation and per-run file tests continue to cover compatibility.

`get_acquisition_state.metrics` now separates `csv_submit_ns` from background
`csv_format_ns` and `csv_write_ns`, and reports queue depth/high-water mark,
enqueued/written/rejected rows, maximum writer lag, overload state and I/O
errors. The raw benchmark is
`analysis/variable-view-p4-csv-cpu-2026-10-14.json` and can be reproduced with:

```powershell
ninja -C build_rel P4-Csv-Benchmark
.\build_rel\P4-Csv-Benchmark.exe --output analysis\variable-view-p4-csv-cpu-2026-10-14.json
```

This is a host CPU and lifecycle baseline. Real slow/removable/network disks,
process termination during drain, and CSV-on sampling-rate correlation with an
ST-Link/J-Link remain hardware or system integration work for P4.2/P5.

## P4.2 CSV fault visibility and slow-writer baseline

CSV failures are now durable snapshots rather than log-only events. The main
menu shows an orange `CSV stopped` badge for queue overload and a red badge for
open/write failures. The acquisition settings show active/idle/stopped state,
reason, local failure time, file path, queue depth/capacity/peak and
written/failed/rejected sample counts. These failures stop only CSV logging;
target acquisition continues. `get_acquisition_state.metrics` exposes the same
reason, timestamp, error, file and accounting fields for automation.

The fault benchmark uses deterministic delays and injected stream failures. It
does not require a probe and writes only to a temporary local directory.

| Scenario | Submit ms | Finish/drain ms | Accounting |
|---|---:|---:|---|
| 50 ms writer delay, capacity 256 | 0.096 | 187.470 | 128 written, 0 failed/rejected |
| 50 ms writer delay, capacity 2 | 0.010 to overload | 61.076 | 2 accepted/written, 1 rejected |
| Injected first-batch write failure | 0.023 | 29.543 | 100 accepted/failed, 1 I/O error |
| Injected file-open failure | 0.337 | 0.000 | 0 accepted, 1 I/O error |

Every scenario ended with queue depth zero. The overload high-water mark stayed
at the configured capacity, and every accepted row satisfied
`written + failed == enqueued`. A separate restart test verifies that a new CSV
run clears the previous overload and writes normally. The complete automated
suite passes 539/539 tests across 59 suites.

The raw result is
`analysis/variable-view-p4.2-csv-faults-2026-10-14.json` and can be reproduced
with:

```powershell
ninja -C build_rel P42-Csv-Fault-Benchmark
.\build_rel\P42-Csv-Fault-Benchmark.exe --output analysis\variable-view-p4.2-csv-faults-2026-10-14.json
```

P4 is non-hardware complete. A real slow/removable/network drive and CSV-on
sampling-rate correlation with ST-Link/J-Link remain deferred because no probe
is currently available. The Release application and benchmark build
successfully; only the previously recorded Statistics, TraceDataHandler and
RecorderTools warnings remain.

## P5.1 automatic bottleneck diagnosis baseline

The Variable Viewer now derives one diagnosis from cumulative acquisition,
CSV and GUI metrics. It waits for at least 100 completed samples and one second
before classifying ordinary acquisition behavior. A definite CSV failure is
reported immediately. This prevents startup noise and individual slow frames
from changing the visible diagnosis.

Classification uses the following ordered evidence:

| Diagnosis | Evidence |
|---|---|
| `csv_backlog` | CSV failed, or an active queue is at least 75% full |
| `render_contention` | at least 30 GUI frames with build P95 >= 33.3 ms, or shared-buffer wait >= 5% of run time |
| `address_fragmentation` | rate/deadline pressure with more than two transactions and at least one transaction per two sampled variables |
| `probe_latency` | rate/deadline pressure with probe reads consuming at least 50% of run time |
| `deadline_limited` | rate/deadline pressure without a dominant consumer above |

Rate/deadline pressure means less than 90% of the requested sampling rate or a
deadline-miss ratio of at least 1%. If neither condition applies, the result is
`none` even when probe time is the largest component. The status bar shows an
orange diagnosis badge; its tooltip reports requested/actual rate, attained
percentage, deadline misses, probe time, shared-buffer wait and CSV fill. MCP
returns the same values in `metrics.diagnosis`.

The non-hardware 100 Hz contiguous-RAM baseline uses the deterministic fake
probe and one merged read per sample:

| Variables | Actual Hz | Attainment | Deadline misses | Probe/read ratio | Diagnosis |
|---:|---:|---:|---:|---:|---|
| 1 | 100.48 | 100.48% | 0.00% | 0.078% | `none` |
| 8 | 100.50 | 100.50% | 0.00% | 0.071% | `none` |
| 20 | 100.49 | 100.49% | 0.00% | 0.081% | `none` |

Pure deterministic tests cover warm-up, healthy operation and all five active
diagnoses. An MCP integration check injects a 40 ms GUI frame-build P95 and
verifies `render_contention`. Raw 1/8/20 results are in
`analysis/variable-view-p5.1-diagnosis-2026-10-14.json`. The complete suite
passes 546/546 tests across 60 suites, and the Release application builds with
only the previously recorded warnings.

## P5.2 non-hardware delivery close-out

The final non-hardware regression passes 546/546 tests across 60 suites. Both
the complete Debug and Release GUI applications build, and all three P3/P4
benchmark executables run from the Release tree. Project compatibility tests
cover round-trip settings and defaults for fields absent from older project
files; P1-P5 add no required persisted setting.

A staged install contains the executable, current user guide, `imgui.ini`, 63
ST-Link chip descriptions, GDB and all Windows runtime DLLs. The source,
runtime and staged guide SHA-256 values match. A 19.4 MB ZIP package was
generated and inspected successfully. Native NSIS generation remains an
environment item because `makensis` is not installed on this host.

The delivery check found and corrected stale incremental runtime assets: the
guide, layout and chip database now participate in the executable link
dependencies, so changing one triggers the existing post-build copy commands.
The user guide now describes merged reads, fallback behavior, CSV failure
visibility and bottleneck diagnosis instead of the old one-transaction-per-
variable performance model.

The detailed report is
`analysis/variable-view-p5.2-delivery-2026-10-14.md`. Hardware, real display and
physical-disk sign-off remains explicitly Pending in
`docs/variable-view-hardware-acceptance.md`; those rows must not be marked Pass
without their raw captures.

## P5.3 J-Link Normal hardware acceptance

On 2026-10-14 the current P0-P5.2 Release build was exercised with J-Link USB
serial `000069730307`, host library 8.10k, an STM32F103C8 and 4,000 kHz SWD.
Each result followed a 10-second warm-up with a 40-second capture. The 8- and
20-variable ratio rows requested 5,000 Hz so that both paths were probe-limited
rather than capping the merged path at the configured rate.

| Variables/layout | Merged actual Hz / calls | Fallback actual Hz / calls | Rate ratio | Failures |
|---|---:|---:|---:|---:|
| 1 contiguous, 1,000 Hz requested | 999.420 / 1.000 | 999.047 / 1.000 | 1.0004x | 0 / 0 |
| 8 contiguous, 5,000 Hz requested | 2,641.254 / 1.000 | 510.122 / 8.000 | 5.18x | 0 / 0 |
| 20 contiguous, 5,000 Hz requested | 1,672.671 / 1.000 | 204.998 / 20.001 | 8.16x | 0 / 0 |
| 8 sparse/mixed width, 100 Hz requested | 100.099 / 8.000 | 100.024 / 8.000 | 1.0007x | 0 / 0 |

The contiguous matrix passes the 0.95x, 3x and 5x acceptance thresholds. The
sparse planner reported eight exact calls and zero merged spans, and its rate
was 100.07% of fallback. Twenty returned values and the eight sparse mixed-width
values had zero merged/fallback differences. Because the connected firmware
returned zero for every selected variable, non-zero numeric semantics remain
Pending rather than being inferred from path equality.

Twenty start/stop cycles, live Plot changes (`1 -> 8 -> 1` sampled variables),
active-group switching (`1 -> 47 -> 1`) and J-Link keep-connection reuse passed
without an acquisition failure. Raw output is
`analysis/variable-view-p5.3-jlink-2026-10-14.jsonl`; the detailed decision is
`analysis/variable-view-p5.3-jlink-acceptance-2026-10-14.md`.

### P5.3.2 J-Link functional safety and recovery

The final Release executable SHA-256 is
`9F1BA95373004FDA330DBBACF7B794EA75E58AA49E031075C62D6C7764AB1182`.
It passed 28 automated Normal-mode read/recovery checks and six controlled
write checks on the same J-Link, target and 4,000 kHz SWD connection.

Nonzero U8/I8/U16/I16/U32/I32/F32 values were observed. Packed fixtures decoded
to `-85`, `-5234`, `-2141248` and approximately `37.21`; contiguous float
fixtures decoded exactly to `0.125, -0.25, 0.5, -0.75, 1.0, -1.25, 1.5,
-1.75`. A RUN-time symbol refresh resolved 67/67 addresses and did not stop or
stall acquisition. An invalid target name failed closed with an explicit error,
then the corrected target recovered to RUN with zero failed samples.

The controlled write check changed `petalsNumber` from 8 to 9, rejected 99
against limits [1, 12], and restored 8. API writes were enabled only for that
run and the saved global setting was verified disabled afterward.

The run found and fixed a P2 hot-path regression: raw values were updated, but
conversion to the value consumed by Plot and MCP happened only when CSV was
enabled. Bound sampled variables are now converted once per sample before
Plot/CSV distribution. The added CSV-off nonzero-value test brings the full
suite to 547/547 passing tests. Evidence is stored in
`analysis/variable-view-p5.3.2-jlink-functional-2026-10-14.json` and
`analysis/variable-view-p5.3.2-jlink-write-2026-10-14.json`.

### P5.3.3 J-Link Recorder shared-probe acceptance

The MCP surface now includes explicit Recorder start, download and stop tools.
Start rebuilds the exact variable set from the active Recorder group and
refreshes internal ELF symbols for a saved project when necessary. Download
uses that frozen start configuration, converts raw target values with the same
Variable semantics as the GUI/CSV path, and releases the shared probe lock
before serializing the result. Start and stop obey the global API-write guard.

The final Release SHA-256 is
`F66C2022B569DFDC76FB1E339104B9AFF02A1F7A1667DB44D0865F297C19F68F`.
J-Link serial `000069730307`, STM32F103C8 and 4,000 kHz SWD were unchanged.
Recorder 8.3 reported a 50 us timestep, 8,192-byte buffer and six-variable
limit. The active Recorder group contained `fastSine`, `fastSquare`,
`fastTriangle` and `traceLoopCount`, for 11 bytes per sample.

| Operation | Result | Target I/O | Viewer result |
|---|---:|---:|---|
| First start | 4 variables, RUN | 8.612 ms | Viewer remained RUN |
| First download | 744 samples, 8,184 bytes, overrun correctly reported | 39.586 ms | Zero failed samples |
| First stop | Recorder stopped | 0.290 ms | Viewer continued sampling |
| Five repeated start/download/stop cycles | 744 samples each | Download 37.782-40.804 ms | Completed samples advanced each cycle; zero failures |

All four channels had changing values, including decoded float and signed I16
data. The download-heavy 2.16-second run attained 94.38 Hz of the requested
100 Hz, with thirteen skipped periods and no failed probe read. Probe-lock wait
was bounded by the bulk downloads; no frame interleaving, disconnect, deadlock
or Recorder/Viewer state coupling was observed.

Raw evidence:
`analysis/variable-view-p5.3.3-jlink-recorder-2026-10-14.json` and
`analysis/tmp/p533_release_final4_stdout.log`. The complete deterministic suite
passes 552/552 tests across 61 suites. Physical mid-run link loss, reviewed
peripheral-register access, HSS, real-disk/high-DPI integration and the
60-minute joint run remain Pending.

### P5.3.4 J-Link physical disconnect and recovery

Physical USB removal while Viewer and Recorder were active exposed a stale
keep-connection session: the first implementation stopped safely, but a direct
Start after USB re-enumeration reused the dead DLL handle. J-Link memory I/O
failures now invalidate and close that session, and failed Viewer samples are
not distributed to Plot or CSV.

The final Release SHA-256 is
`1C92CCC464FC0AA7150F44AE6F7D8980C9CF5E6A85C742DD73C77773CCE14D28`.
On removal, the first observation found Viewer STOP with exactly one failed
sample and a specific connection-loss error. Recorder calls failed closed and
MCP remained responsive. After reconnect, the same application process opened
a new J-Link session, Viewer ran with zero failures, and Recorder completed a
744-sample / 8,184-byte start-download-stop cycle. The deterministic regression
suite passes 553/553 tests across 61 suites, including the assertion that a
failed read does not append a Plot sample.

Final evidence:
`analysis/variable-view-p5.3.4-jlink-disconnect-2026-10-14.json` and
`analysis/tmp/p534_release_final_stdout.log`. The original failed recovery is
kept in
`analysis/variable-view-p5.3.4-jlink-disconnect-pre-fix-2026-10-14.json`.
At the P5.3.4 checkpoint, reviewed peripheral-register access, HSS,
real-disk/high-DPI integration and the 60-minute joint run remained Pending;
P5.3.5 below closes the reviewed peripheral item.

### P5.3.5 reviewed peripheral-register reads

The reviewed mixed set combined `waveTable[0..2]`, `traceLoopCount`,
`DBGMCU_IDCODE`, `SCB_CPUID`, `RCC_CFGR` and `FLASH_ACR`. The four registers
are identity or configuration/status registers without read-clear, FIFO or
ordered-read behavior. USART/SPI/ADC data paths, pending/status registers and
write-only commands were explicitly excluded.

Each path used a 10-second warm-up plus a 40-second capture at 100 Hz:

| Path | Actual Hz | Calls/sample | Merged spans | Skipped periods | Failed samples |
|---|---:|---:|---:|---:|---:|
| Automatic | 100.016 | 6.000 | 1 | 0 | 0 |
| Fallback | 100.016 | 8.000 | 0 | 0 | 0 |

The automatic plan merged only the three adjacent RAM floats. The dynamic RAM
variable and all four non-RAM registers remained exact four-byte reads. Values
matched fallback exactly: `DBGMCU_IDCODE=0x20036410`,
`SCB_CPUID=0x411FC231`, `RCC_CFGR=0x001D040A`, and
`FLASH_ACR=0x00000032`. The identity values match an STM32F1 medium-density
device with an ARM Cortex-M3 core, and `traceLoopCount` advanced in both runs.

Raw evidence:
`analysis/variable-view-p5.3.5-jlink-peripheral-2026-10-14.json` and
`analysis/tmp/p535_release_stdout.log`. API target writes remained disabled.
HSS, real-disk/high-DPI integration and the 60-minute joint run remain Pending.

## P5.4 real-disk CSV and high-DPI GUI acceptance

The unchanged Release executable (SHA-256
`1C92CCC464FC0AA7150F44AE6F7D8980C9CF5E6A85C742DD73C77773CCE14D28`)
was exercised through J-Link Normal at 100 Hz. Windows reported one active
physical display: 2560x1600 at 144 DPI (150% scale). The 1080p rows used an
exact 1920x1080 client area, and the high-DPI rows used a maximized 2560x1494
client area. Each normal scenario had a 10-second warm-up and 40-second
capture.

| Scenario | Client / DPI | Actual Hz | Frame total P95 | Build P95 | Result |
|---|---:|---:|---:|---:|---|
| 1 Plot / 8 curves, follow | 1920x1080 / 144 | 100.017 | 6.945 ms | 1.691 ms | Pass |
| 4 Plots / 20 curves, follow | 1920x1080 / 144 | 100.014 | 11.390 ms | 3.226 ms | Pass |
| 4 Plots / 20 curves, pan and zoom | 2560x1494 / 144 | 100.013 | 11.453 ms | 3.232 ms | Pass |
| 986,895-point capacity, newest 10 s | 2560x1494 / 144 | 100.014 | 11.273 ms | 3.066 ms | Pass |
| CSV off | 2560x1494 / 144 | 100.01437 | 11.421 ms | 3.262 ms | Baseline |
| CSV on, D: fixed NTFS | 2560x1494 / 144 | 100.01396 | 11.398 ms | 3.312 ms | Pass |

All normal rows had zero failed samples, zero skipped periods and diagnosis
`none`. Manual pan/zoom left Plot follow mode (the screenshot shows `Back to
live`) while MCP full-buffer statistics retained the `fastSine` extrema at
approximately -1 and +1. The display submitted 140,223,540 points from
1,228,618,390 raw points in that run, demonstrating active display decimation
without changing the full-resolution data surface.

CSV reduced the measured rate by only 0.0004%. The real file on D: was
1,242,625 bytes and 5,065 lines: one header plus all 5,064 enqueued samples.
Writer queue high-water was one, the queue drained to zero on stop, and failed
and rejected samples remained zero.

A nonexistent parent directory exercised the production open-failure path.
CSV became inactive with reason `open_failed`; the visible status showed `CSV
stopped` and `CSV backlog`, while acquisition remained RUN and advanced 502
more samples at 100.143 Hz. A real slow/removable disk was not available and
remains Pending rather than being inferred from P4.2 fault injection.

Evidence:

- `analysis/variable-view-p5.4-gui-csv-2026-10-14.json`
- `analysis/variable-view-p5.4-gui-csv-acceptance-2026-10-14.md`
- `analysis/tmp/p54_gui_csv_acceptance.mcuvproj`
- `analysis/tmp/p54_csv/viewer_20261014-231001.csv`
- `analysis/variable-view-p5.4-*-2026-10-14.png`

## P5.5 60-minute J-Link joint stability

The available J-Link Normal path ran for exactly 3,600 seconds with 20
variables in four Plots, a maximized 2560x1494 client area at 144 DPI/150%,
100 Hz sampling and asynchronous CSV on D: fixed NTFS. Five manual pan/zoom
and return-to-live interactions ran during the window.

| Metric | Result |
|---|---:|
| Completed samples / actual rate | 361,013 / 100.0002 Hz |
| Failed samples / skipped periods | 0 / 0 |
| GUI frame-total P95 | 10.929 ms |
| CSV enqueued / written | 361,063 / 361,063 |
| CSV file | 89,278,048 bytes, 361,064 lines including header |
| Queue depth / high-water | 0 / 1 |
| Working-set span after warm-up | 142.8 MiB |

All twelve five-minute observations plus the final snapshot were recorded.
Sampling stayed at the requested rate, GUI P95 stayed below 33.3 ms, and the
diagnosis remained `none`. CSV failures, rejections, I/O errors and overload
stayed at zero. The queue drained on stop, and a subsequent start/run/stop
cycle remained functional with zero failed samples.

Evidence:

- `analysis/variable-view-p5.5-jlink-long-run-2026-10-14.json`
- `analysis/tmp/p55_long_run_acceptance.py`
- `analysis/tmp/p55_csv/viewer_20261014-232142.csv`
- `analysis/variable-view-p5.4-p5.5-final-2026-10-14.png`

## P5.6 final delivery close-out

The final delivery gate was repeated on 2026-10-15 without changing the
Release executable. J-Link Normal serial `000069730307` completed a fresh
20-variable, 100 Hz smoke run at 4,000 kHz: 1,007 samples in 10.063 seconds
(`100.066 Hz`), zero failed samples, zero skipped periods and a clean stop.
The automatic diagnosis remained `none` and API target writes were disabled.

The complete deterministic suite passes 553/553 tests across 61 suites.
Debug, Release and test builds are current, all three P3/P4/P4.2 Release
benchmarks pass, and `git diff --check` reports no whitespace error. A fresh
staged install contains the application, guide, required runtime DLLs and 63
ST-Link chip descriptions. The fresh ZIP is
`build_rel/p56-package-smoke/Variable-Trace-1.1.0-win64.zip`.

| Artifact | SHA-256 |
|---|---|
| `build_rel/Variable-Trace.exe` | `1C92CCC464FC0AA7150F44AE6F7D8980C9CF5E6A85C742DD73C77773CCE14D28` |
| P5.6 ZIP package | `34635CD08AA7C49099FC2763678C0431AA4C0E189C982C07017BF7FAADCE05D4` |

HSS is not claimed: no HSS-capable probe is available. The unsuccessful
temporary-probe attempt failed before either a Normal control or HSS session
could connect to the target and is therefore inconclusive. HSS, Serial, GDB
Server, SWO and a physical slow/removable disk remain named supplemental
hardware tests, not failures and not release claims. Native NSIS generation
also remains unavailable because `makensis` is not installed; the verified ZIP
is the deliverable package on this host.

Evidence:

- `analysis/variable-view-p5.6-jlink-000069730307-normal-4000khz-smoke-2026-10-15.json`
- `analysis/variable-view-p5.6-delivery-closeout-2026-10-15.md`
- `build_rel/p56-p3-smoke.json`
- `build_rel/p56-p4-smoke.json`
- `build_rel/p56-p42-smoke.json`
