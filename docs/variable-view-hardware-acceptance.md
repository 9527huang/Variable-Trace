# Variable Viewer hardware acceptance

This checklist closes the tests that cannot be represented by the fake probe,
simulated disk delay, or headless GUI benchmark. Record raw MCP captures with
`tools/performance/variable_view_baseline.py`; do not replace missing hardware
with an estimate.

## Test record

| Field | Value |
|---|---|
| Date / tester | 2026-10-14 through 2026-10-15 / Codex-assisted P5.3-P5.6 acceptance |
| Application commit | `810a1a16f3f88cad2ab8f01da3b3e0580f7cf456` plus P0-P5.6 working-tree changes; final Release SHA-256 `1C92CCC464FC0AA7150F44AE6F7D8980C9CF5E6A85C742DD73C77773CCE14D28` |
| Host CPU / RAM / OS | AMD Ryzen AI MAX+ 392 / 31.1 GiB / Windows 11 Pro 10.0.28000 |
| Display resolution / scale / refresh | AMD Radeon physical display, 2560x1600 / 150% (144 DPI) / 165 Hz; 1920x1080 exact client viewport also measured |
| Probe model / firmware / serial | J-Link USB (model/firmware not exposed by the available host API), serial `000069730307`; host library 8.10k |
| Target / clock / firmware build | STM32F103C8; clock not independently verified; ELF SHA-256 `53EAE483CBD3029255525F3CB601F67DAE3AC6C88457DF8656487D96E7338D50` |
| SWD speed / connection | 4,000 kHz / USB, Normal mode, keep-connection enabled |
| CSV volume and file system | D: fixed NTFS, 1,242,625 bytes / 5,065 lines in the P5.4 run; slow/removable medium unavailable |

Before measuring, use a Release build, close other debuggers, program the same
firmware for every A/B pair, and warm acquisition for at least 10 seconds.
Capture each performance scenario for 40 seconds. Preserve the generated JSON,
application log, project file and a screenshot of the final indicator tooltip.

## Required ST-Link or J-Link Normal matrix

Use `u32` variables in ordinary RAM. For contiguous cases, place them at
adjacent four-byte addresses. Run both merged and fallback paths with otherwise
identical settings.

| Probe | Layout | Variables | Path | Requested Hz | Actual Hz | Transactions/sample | Deadline miss % | Result |
|---|---|---:|---|---:|---:|---:|---:|---|
| J-Link | contiguous RAM | 1 | fallback | 1,000 | 999.047 | 1.000 | 0.177 | Pass |
| J-Link | contiguous RAM | 1 | merged | 1,000 | 999.420 | 1.000 | 0.177 | Pass: 100.04% of fallback |
| J-Link | contiguous RAM | 8 | fallback | 5,000 | 510.122 | 8.000 | 89.802 | Pass (control) |
| J-Link | contiguous RAM | 8 | merged | 5,000 | 2,641.254 | 1.000 | 47.218 | Pass: 5.18x fallback |
| J-Link | contiguous RAM | 20 | fallback | 5,000 | 204.998 | 20.001 | 95.903 | Pass (control) |
| J-Link | contiguous RAM | 20 | merged | 5,000 | 1,672.671 | 1.000 | 66.576 | Pass: 8.16x fallback |
| J-Link | sparse RAM | 8 | automatic | 100 | 100.099 | 8.000 | 0.000 | Pass: 100.07% of fallback; 8 exact spans |
| J-Link | mixed RAM/peripheral | 8 | fallback | 100 | 100.016 | 8.000 | 0.000 | Pass (control) |
| J-Link | mixed RAM/peripheral | 8 | automatic | 100 | 100.016 | 6.000 | 0.000 | Pass: one RAM span plus five exact reads; 100.00% of fallback |

Deadline miss percentage above is `skipped periods / (completed samples +
skipped periods)`. The 8- and 20-variable ratio rows deliberately request
5,000 Hz so both paths are probe-limited; a 1,000 Hz check was also captured
but capped the merged path and therefore was not suitable for a throughput
ratio. Every recorded row had zero failed samples.

Acceptance:

- One-variable merged rate is at least 95% of fallback.
- Eight-variable contiguous merged rate is at least 3x fallback.
- Twenty-variable contiguous merged rate is at least 5x fallback.
- Sparse layout is at least 90% of fallback and reports the expected read plan.
- Peripheral addresses are never widened beyond the declared bytes.
- Values, timestamps and CSV columns match between merged and fallback paths.

## Functional regression while sampling

| Operation | Expected | Result |
|---|---|---|
| Start, stop and restart 20 times | No hang; each requested state settles | Pass: 20/20, every RUN produced samples and every STOP settled |
| Add/remove a plotted variable | Read plan changes without stale Series access | Pass: sampled count changed 1 -> 8 -> 1 with controlled restart and zero failures |
| Switch active sampling group | Only the new group's variables update | Pass: sampled count changed 1 -> 47 -> 1 with zero failures |
| Change 1/8/16/32-bit and float types | Values decode identically to fallback | Pass: live U8/I8/U16/I16/U32/I32/F32 values were nonzero; fixed signed and float fixtures matched |
| Use unaligned and overlapping RAM variables | Correct values; no extra unsafe bytes | Pass: packed struct members at offsets 2, 6, 8 and 12 decoded as `-85`, `-5234`, `-2141248`, `37.21` |
| Write a variable inside/outside limits | Valid write succeeds; guarded write is rejected | Pass: `petalsNumber` 8 -> 9 -> 8; 99 rejected against [1, 12]; global API writes restored disabled |
| Keep connection, stop and restart | Link state and next run remain valid | Pass: 20 cycles reused the held J-Link connection |
| Refresh symbols during a run | Configured stop/restart behavior is preserved | Pass: 67/67 addresses resolved, state remained RUN and completed samples advanced |
| Run recorder through the shared probe | No interleaved probe frames or starvation | Pass: MCP start/download/stop, one full 8,184-byte capture and five repeated cycles; Viewer stayed RUN, advanced every cycle and recorded zero failed samples |
| Force a target read failure | Current sample is invalid and acquisition stops with an error | Pass: physical J-Link USB removal stopped on the first failed sample, exposed a specific connection-loss error, and recovered after USB re-enumeration without restarting the application |

### P5.3.2 functional safety and recovery

P5.3.2 used the final Release build for 28 read/recovery checks and six
controlled write checks. It exposed a hot-path regression: raw target bits were
stored, but `Variable::value` was not converted unless CSV logging was enabled,
so Plot/MCP values stayed at zero. Conversion now runs once for the bound
sampling variables before Plot and CSV updates. A deterministic CSV-off
nonzero-value test was added; the complete suite passes 547/547.

Raw evidence is in
`analysis/variable-view-p5.3.2-jlink-functional-2026-10-14.json` and
`analysis/variable-view-p5.3.2-jlink-write-2026-10-14.json`. Physical mid-run
link loss remains Pending because there is no production fault injector for one
live hardware read. It is not inferred from the connection-start failure test.

### P5.3.3 Recorder shared-probe acceptance

The Release build added `start_recorder`, `download_recorder` and
`stop_recorder`. Target control obeys the global API-write guard; each hardware
operation holds the same probe mutex as Variable Viewer, and capture JSON is
built only after that mutex is released. A saved project now refreshes the ELF
once when its internal Recorder structure symbols are not yet cached.

On J-Link Normal, Recorder 8.3 was detected at 20 kHz with an 8,192-byte
buffer and six-variable limit. Four mixed-width variables produced 11-byte
samples. The first and all five repeated downloads returned 744 samples / 8,184
bytes. Float, U8, I16 and U32 channels changed in the capture; every series and
timestamp count matched. Recorder stop left Viewer RUN and Viewer completed
samples advanced after every start/download/stop cycle with zero failed reads.

The short, deliberately download-heavy run finished at 94.38 Hz for a 100 Hz
Viewer request. The first start/download/stop target I/O times were 8.612 ms,
39.586 ms and 0.290 ms. The 38-41 ms bulk downloads explain the thirteen skipped
Viewer periods and are bounded probe-lock pauses, not interleaved frames or a
deadlock. Long-run stability remains a separate Pending gate.

Raw evidence is in
`analysis/variable-view-p5.3.3-jlink-recorder-2026-10-14.json` and
`analysis/tmp/p533_release_final4_stdout.log`. The automated suite passes
552/552 tests. The user's saved global configuration remained
`writesEnabled=false`; API writes were enabled only in an isolated acceptance
configuration.

### P5.3.4 physical disconnect and recovery

The first physical mid-run USB removal established the failure mode: Viewer
stopped after one failed sample and Recorder operations failed closed, but a
direct Viewer restart after reconnect reused the stale keep-connection J-Link
session and produced no valid samples. The pre-fix evidence is preserved in
`analysis/variable-view-p5.3.4-jlink-disconnect-pre-fix-2026-10-14.json`.

J-Link memory I/O failures now close and invalidate the DLL session even when
keep-connection is enabled. The next Start therefore opens the re-enumerated
probe. A failed Viewer sample is counted but is no longer distributed to
Variables, Plot buffers or CSV. A deterministic regression test verifies that
the Plot sample count remains equal to the completed-sample count after a read
failure.

The final Release run physically removed the same J-Link while Viewer and
Recorder were active. At the first post-removal observation Viewer was STOP,
with exactly one failed sample and the explicit message `J-Link connection
lost while reading target memory`. Recorder download and stop returned clear
stopped-acquisition errors, while the application and MCP endpoint remained
responsive. After reconnect, without restarting the application, a new run
started with zero failed samples. Recorder then started, downloaded 744 samples
/ 8,184 bytes, stopped successfully, and Viewer remained healthy.

Final evidence is in
`analysis/variable-view-p5.3.4-jlink-disconnect-2026-10-14.json` and
`analysis/tmp/p534_release_final_stdout.log`. The complete suite passes
553/553 tests. Both the normal user configuration and isolated acceptance
configuration were restored to `writesEnabled=false` after the run.

### P5.3.5 reviewed side-effect-free peripheral reads

The mixed eight-variable row used three adjacent RAM floats, the dynamic RAM
`traceLoopCount`, and four explicitly reviewed 32-bit registers:

| Register | Address | Review decision | Observed value |
|---|---:|---|---:|
| `DBGMCU_IDCODE` | `0xE0042000` | Read-only device/revision identity | `0x20036410` |
| `SCB_CPUID` | `0xE000ED00` | Read-only ARM core identity | `0x411FC231` |
| `RCC_CFGR` | `0x40021004` | Configuration/status; no clear-on-read fields | `0x001D040A` |
| `FLASH_ACR` | `0x40022000` | Configuration/status; no clear-on-read fields | `0x00000032` |

USART/SPI status-data sequences, ADC data, DMA/EXTI/timer pending/status,
peripheral FIFOs and write-only command registers were excluded because their
read semantics can consume data, clear flags or depend on an ordered sequence.

Both paths used a 10-second warm-up followed by a 40-second capture at 100 Hz.
Automatic reported one merged span and exactly six transactions per sample:
one read for the three adjacent RAM floats, one exact read for
`traceLoopCount`, and four exact four-byte register reads. Fallback reported
eight transactions. Both attained 100.016 Hz with zero skipped periods and
zero failed samples. Register and fixed RAM values matched exactly, and
`traceLoopCount` advanced throughout both runs.

Raw evidence is in
`analysis/variable-view-p5.3.5-jlink-peripheral-2026-10-14.json` and
`analysis/tmp/p535_release_stdout.log`. The isolated project is
`analysis/tmp/p535_jlink_peripheral_acceptance.mcuvproj`; the original P1
project was not modified. API target writes remained disabled for the entire
run.

## GUI and CSV joint acceptance

Run 1 plot x 8 curves and 4 plots x 20 curves at 1080p and at the available
high-DPI scale. Exercise follow, pause, pan and zoom, then use the maximum
configured history. Capture `metrics.gui` and `metrics.diagnosis`.

| Scenario | CSV | Frame total P95 | Sampling-rate change | Diagnosis / behavior | Result |
|---|---|---:|---:|---|---|
| 1x8, 1080p follow | off | 6.945 ms | 100.017 Hz baseline | `none`; zero failures/skips | Pass |
| 4x20, 1080p follow | off | 11.390 ms | 100.014 Hz | `none`; zero failures/skips | Pass |
| 4x20, high DPI pan/zoom | off | 11.453 ms | 100.013 Hz | `none`; manual view, full-resolution extrema retained | Pass |
| Maximum history, newest window | off | 11.273 ms | 100.014 Hz | `none`; 986,895-point capacity, latest 10 s visible | Pass |
| 20 variables, normal local disk | on | 11.398 ms | 0.0004% reduction | 5,064/5,064 rows, queue high-water 1 | Pass |
| 20 variables, slow/removable disk | on | Not run | Not run | No slow/removable medium available | Not verified (equipment unavailable) |
| CSV directory unavailable or disk full | on | 12.394 ms | Acquisition continues at 100.143 Hz | `CSV stopped` visible; MCP `open_failed` | Pass (unavailable directory) |

Acceptance:

- GUI frame P95 is at most 33.3 ms for the default plotting scenario.
- Enabling ordinary CSV reduces actual sampling rate by no more than 10%.
- GUI plotting reduces actual sampling rate by no more than 5%.
- CSV overload/failure stops only logging, drains accepted work, exposes the
  same reason in the GUI and MCP, and does not freeze stop or exit.
- Pan/zoom preserves visible extrema; statistics, cursor, MCP and CSV continue
  to use full-resolution data.

### P5.4 real-disk CSV and high-DPI GUI

The Release build ran on the physical 2560x1600 AMD display at 144 DPI (150%
scale). The two 1080p cases used an exact 1920x1080 application client area on
that display; the high-DPI cases used the maximized 2560x1494 client area. All
normal scenarios used a 10-second warm-up and 40-second capture at 100 Hz with
J-Link serial `000069730307` and the STM32F103C8.

Every normal run stayed at 100.013-100.017 Hz with zero failed samples and zero
skipped periods. Frame-total P95 was 6.945 ms for one Plot/eight curves and
11.390 ms for four Plots/twenty curves at 1080p. At 150% DPI, manual zoom and
pan produced the visible `Back to live` state and a frame-total P95 of 11.453
ms. Full-resolution MCP statistics retained the sine extrema at approximately
`[-1, 1]`; the display path submitted 140,223,540 of 1,228,618,390 raw points,
confirming that view decimation remained separate from retained data.

Maximum history was configured to the application-reported memory-budget
ceiling of 986,895 points per buffer with only the newest 10 seconds visible.
Its frame-total P95 was 11.273 ms. The 40-second hardware window populated
5,018 samples; a fully populated maximum buffer remains represented by the P3
deterministic CPU baseline rather than being claimed from this short run.

CSV-off and CSV-on runs at the maximized high-DPI viewport attained 100.01437
and 100.01396 Hz, a 0.0004% reduction. The D: fixed NTFS file contained 5,065
lines and 1,242,625 bytes; all 5,064 accepted samples were written, the queue
drained to zero, high-water was one, and no row failed or was rejected.

Opening a CSV file below a nonexistent parent failed with `open_failed` and an
actionable path. The GUI displayed `CSV stopped` and `CSV backlog`, MCP exposed
the same reason, and acquisition remained RUN for another 502 samples at
100.143 Hz with no target-read failure. No slow or removable filesystem was
present, so that row remains explicitly not verified and is not passed by
analogy with the deterministic P4.2 delay tests.

Raw evidence is
`analysis/variable-view-p5.4-gui-csv-2026-10-14.json`; the isolated project is
`analysis/tmp/p54_gui_csv_acceptance.mcuvproj`, and screenshots are named
`analysis/variable-view-p5.4-*-2026-10-14.png`. API target writes remained
disabled, the original P1 project was unchanged, and the application and MCP
listener were stopped after the run.

## Other probe modes

Mark unavailable equipment explicitly rather than passing it by analogy.

| Mode | Start/stop | Live edit | Values/timestamps | CSV | Result |
|---|---|---|---|---|---|
| J-Link HSS | Not run | Not run | Not run | Not run | Not verified (HSS-capable probe unavailable) |
| Serial sweep | Not run | Not run | Not run | Not run | Not verified (target transport unavailable) |
| GDB Server | Not run | Not run | Not run | Not run | Not verified (server/target environment unavailable) |
| SWO Trace Viewer regression | Not run | Not run | Not run | Not run | Not verified (trace environment unavailable) |

## Long-run exit gate

Run the available Normal probe for at least 60 minutes with 20 contiguous
variables, plots visible and CSV enabled. Pass only when acquisition remains
responsive, memory and queue sizes remain bounded, accepted CSV rows are fully
accounted, files close cleanly, and stop/restart still works at the end.

Hardware acceptance is complete only when every available row has evidence and
every unavailable row names the missing device or environment. Link the raw
captures from `docs/variable-view-performance-baseline.md` and update the
development plan status; never convert a Pending row to Pass without a run.

### P5.5 60-minute J-Link joint stability

The available J-Link Normal path ran for exactly 3,600 seconds with 20
variables across four Plots, a maximized 2560x1494 client area at 144 DPI/150%,
100 Hz acquisition and asynchronous CSV on D: fixed NTFS. Five pan/zoom and
return-to-live interactions were completed at ten-minute intervals.

| Gate | Result |
|---|---|
| Continuous duration | 3,600.000 s |
| Samples / actual rate | 361,013 / 100.0002 Hz |
| Failed samples / skipped periods | 0 / 0 |
| GUI frame-total P95 | 10.929 ms |
| CSV rows / file size | 361,064 / 89,278,048 bytes |
| CSV failed / rejected | 0 / 0 |
| CSV queue depth / high-water | 0 / 1 |
| Working-set span after warm-up | 142.8 MiB |
| Stop, restart, stop | Pass; restart had zero failed samples |

All twelve scheduled observation points were captured. The CSV file contains
one header plus all 361,063 accepted samples, the writer never accumulated
backlog, and the GUI stayed below the 33.3 ms P95 gate. After the formal
window, the queue drained, a new acquisition run started and produced samples,
and the second stop settled cleanly.

Raw evidence is `analysis/variable-view-p5.5-jlink-long-run-2026-10-14.json`;
the driver and logs are `analysis/tmp/p55_long_run_acceptance.py`,
`analysis/tmp/p55_release_stdout.log` and `analysis/tmp/p55_release_stderr.log`.

### P5.6 final delivery close-out

After the temporary probe was removed, J-Link serial `000069730307` was
reconnected and a fresh 20-variable Normal-mode control ran at 4,000 kHz. It
completed 1,007 samples in 10.063 seconds (`100.066 Hz`) with zero failed
samples, zero skipped periods and all 20 values available. Stop settled and
API target writes remained disabled. The raw capture is
`analysis/variable-view-p5.6-jlink-000069730307-normal-4000khz-smoke-2026-10-15.json`.

No HSS-capable J-Link is currently available. The earlier serial
`000175070332` attempt did not establish even the Normal control connection,
so it is inconclusive and is not evidence that HSS is supported or unsupported.
HSS is deferred as an independent supplemental matrix; it does not invalidate
the completed Normal acceptance. Serial, GDB Server, SWO and slow/removable
disk rows are likewise explicitly unverified because their required equipment
or environment is absent.
