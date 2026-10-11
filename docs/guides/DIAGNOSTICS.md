# Native baseline and session diagnostics

Build in the isolated Techmo checkout. The helper defaults to two build workers:

```powershell
./tools/build-native.ps1 -Bootstrap -Test
./tools/build-launcher.ps1
```

Bootstrap downloads signed CLANG64 packages into `builds/_internal/toolchain`
using the existing MSYS2 shell. It does not install them into the system MSYS2
prefix. Keep `packages.sha256` to identify the toolchain used for a local build.
Subsequent builds omit `-Bootstrap`. An existing system CLANG64 toolchain also
works. Native output and its required toolchain DLLs live under `out/`.

`out/native-build.json` records the compiler, selected SDK versions, source
fingerprint and binary hashes. Keep the exact native executables used by a
crashing session: they retain DWARF symbols. A minidump does not supply matching
symbols by itself. Do not strip those baseline binaries.

## Record a session

Open `builds/TEST-LAUNCHER.cmd`, select your own game dump and save settings.
In **Advanced**, enable **Record frame statistics (CSV)**. For deeper GPU work,
also enable **Record GPU timings**. These switches work without Developer Mode.
Leave Detailed logs off for ordinary comparisons: that option also changes an
inherited renderer barrier setting. Profilers themselves add measurement cost.

Every launch gets a separate `logs/<timestamp>-<id>/` directory:

| File | Contents |
| --- | --- |
| `session.log` | Preparation, native output and existing per-pass GPU statistics |
| `session.json` | Stage, exit status, selected settings, build provenance and measurements |
| `summary.txt` | Short result and references to supporting files |
| `frames.csv` | VideoOut flip intervals, Stage A thread CPU time, waits, uploads and compile counts |
| `gpu.csv` | GPU timestamp intervals when GPU profiling is enabled |
| `fault.json` | Native exception code, instruction address, thread and access operation |
| `crash.dmp` | Windows minidump for captured fatal native exceptions |
| `minidump.json` | Whether the external dump writer succeeded, with its Windows error code |

Optional files only exist when that recording path runs. A minidump is not
expected for preparation errors, ordinary exits, every Vulkan error, or an
external process kill. If the reporting process is killed, `session.json` can
remain marked `running`; the UI labels that report incomplete.

The Home page shows the last session result and an **Open session report** button.
The preview below uses synthetic UI test data; its FPS values are not Bloodborne
performance measurements.

![Session report with synthetic test data](../assets/session-report.png)

It distinguishes preparation failures, known native exceptions and abnormal
exits whose cause is unknown. Device lost identifies a Vulkan failure; it does
not establish that VRAM exhaustion caused it. Reports and dumps stay local.
The structured settings snapshot omits account credentials; logs and minidumps
can still contain private paths or process memory. Inspect them before sharing.

## Compare the same route

Use the same source build, driver, power mode, game area, camera route and
graphics settings. Keep separate cold-cache and warmed-cache runs. Record at
least a few repeated routes; include combat and traversal. Stop at the game's
menu/close command so the buffered CSV tail is flushed on a normal exit.

Whole-session statistics include loading and menus. Extract a repeatable range:

```powershell
./builds/_internal/toolchain/clang64/bin/python.exe tools/analyze-session.py logs/SESSION --from-seconds 10 --to-seconds 70
```

This writes `benchmark.json` without replacing the original session report.
Average FPS is `1000 / mean(frame_ms)`. The 1% low is `1000 / mean` of the slowest
`ceil(N * 0.01)` intervals. p95/p99 use nearest-rank frame-time percentiles.
Zero/nonfinite/truncated rows are excluded; the skipped count is recorded.

Flip interval is frame pacing, not CPU execution time. `stage_a_cpu_ms` measures
one renderer thread's CPU consumption, not the whole CPU frame. GPU samples
cover recorded timestamp intervals with intervening copies/barriers/waits; they
are not paired one-to-one with flip CSV rows. Use Nsight for a complete CPU/GPU
timeline before attributing a bottleneck. A small CSV tail can be lost on crash
(up to 120 flip rows and 60 GPU rows are buffered between flushes).

## Validate without the game

`bb-probe.exe --vulkan-only` submits a tiny buffer fill and reads it back; it
does not load Bloodborne. Native regression fixtures cover the runtime and input.
The crash fixture raises a synthetic Windows exception and checks the original
exception stream inside the resulting remote minidump. WPF checks exercise
nonzero process exits and English/Vietnamese report presentation with synthetic
files. Passing these checks establishes a testable baseline; game stability and
performance on the GTX 1650 Ti / RTX 3050 / RTX 3050 Ti need real game runs on
those devices.

The dump writer runs outside the faulting process, following
[Microsoft's MiniDumpWriteDump guidance](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump).
