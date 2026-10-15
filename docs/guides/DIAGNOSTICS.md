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
| `vram.csv` | Driver memory estimates and cache counters when frame statistics are enabled |
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

GPU queries now collect without waiting on the CPU. If a four-frame query slice
is still in flight, it remains intact and profiling skips that frame. The slice
is reset only after its owning scheduler submission completes. `skipped_frames`
in `gpu.csv` is a cumulative counter at the time of the sample; the report keeps
the latest observed value. Heavy frames can be missed, so GPU averages from a
capture with skips should not be treated as an unbiased workload average.
See the Vulkan rules for [query results](https://docs.vulkan.org/refpages/latest/refpages/source/vkGetQueryPoolResults.html)
and [host query reset](https://docs.vulkan.org/refpages/latest/refpages/source/vkResetQueryPool.html).

New GPU captures also separate `labelled_work_ms` and
`between_submissions_ms`. Their sum is the recorded interval. The second field
tracks the scheduler's existing gap label, which can include frame pacing and
waiting for CPU work. Neither field is a hardware utilization counter; labelled
work still includes barriers and transfers. Older captures remain readable.
Session environment snapshots include Vulkan recording thread/segment settings,
async submission, direct segments and the FSR 4 shader variant setting.

## VRAM cache management

Texture GC now reads usage and budget together through `VK_EXT_memory_budget` on
discrete and integrated GPUs, at most once every 250 ms. It starts idle-image
cleanup at the smaller of 512 MiB and 70% of the current budget, enters pressure at 85% and critical pressure
at 95%. Recovery marks are 80% and 90%, respectively, to avoid repeatedly crossing
one threshold. A temporarily zero budget retains the previous valid policy.
An unavailable budget query skips evictions for that pass. These are cache
thresholds, not hard allocation caps or guarantees against OOM.
The existing 20-second idle-image retention remains in effect below pressure;
GPU-written images still require the existing safe write-back checks, and Vulkan
destruction is deferred until the scheduler timeline completes.

Each pass is bounded; guest texture sizes no longer get added/subtracted from a
driver heap measurement. The batch uses host image allocation sizes to estimate
reclamation, then the next query observes actual usage. Shared allocator blocks
may retain slack even after an image is released. Budget/usage semantics are
documented by [Khronos](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_memory_budget.html).

With **Record frame statistics** enabled, a session also writes `vram.csv` at
most four times per second. The report's `memory` object and `summary.txt` contain
the peak driver usage estimate, minimum budget, budget fraction and pressure
sample counts. The CSV includes tracked Vulkan image bytes (scene targets and
other images, not just game textures), guest buffer allocated/unused bytes,
pending GPU-deferred image destruction and cumulative GC evictions/write-backs.
The driver sample is taken before that pass's evictions; the other counters are
read at the end of the pass. These categories overlap and cannot be added to the
driver estimate. Upscaler internal allocations and allocator slack are not
individually attributed. Samples can miss short peaks and remain backward
compatible with sessions that have no `vram.csv`.

For a real A/B comparison, use the same resolution, upscaler, patches and route;
visit several areas and return to the start. Compare peak usage **and** frame-time
p95/p99 and 1% lows. A lower VRAM number with frequent re-uploads or write-backs
can be a performance regression. `BB_GC_BUDGET_MB` is an optional cache-policy
override for controlled pressure testing; it does not reserve VRAM or cap driver
allocations. Session reports record it and the existing idle/dedicated-allocation
overrides.

`memory-pressure-test` runs without a GPU and exercises 0.5/1/4/6/8 GiB budgets,
shrinking budgets, exact transition boundaries, unavailable budgets and overflow.
`memory-residency-test` uses real headless Vulkan images to check memory accounting
and deferred release only after submitted work completes. It does not launch
Bloodborne or establish in-game VRAM savings:

```sh
ninja -C out/gpu memory-pressure-test memory-residency-test
out/gpu/memory-pressure-test.exe
out/gpu/memory-residency-test.exe
```

## Runtime clock measurements

The Windows runtime caches QPC frequency through a thread-safe initializer. When
the frequency divides one billion exactly, integer multiplication converts ticks
to nanoseconds; other frequencies retain the quotient/remainder conversion.
The final sleep spin compares QPC ticks to a deadline rounded up once, preserving
the no-early-wake behavior. QPC frequency is fixed for the system boot, as covered
by [Microsoft's timing guidance](https://learn.microsoft.com/en-us/windows/win32/sysinfo/acquiring-high-resolution-time-stamps).

`tests/test_host_clock.c` checks concurrent clock reads, conversion against native
QPC and sleep deadlines. `gpu-profiler-test` uses real headless Vulkan queries,
deliberately withholds submissions, and checks that pending slices are preserved
and later reused. Build the latter with `ninja -C out/gpu gpu-profiler-test` in the
native shell, then run `out/gpu/gpu-profiler-test.exe` with an optional CSV path.
It requires a Vulkan-capable GPU and is not part of GPU-less hosted CI.
`tools/bench-runtime-clock.c` measures clock-call cost only. Clock throughput and
synthetic query tests do not establish a Bloodborne FPS gain.

Compare the rebuilt native runtime against a saved baseline on the same warmed
route, with the same 60 FPS patch, output, upscaler and effects. Also keep a run
with GPU recording off to assess profiler overhead. Record average FPS, 1% low,
p99 and visible artifacts/crashes. Do not change thread counts, barriers and
shader settings together; each needs a separate comparison.

The repository contains the host runtime/renderer source. A PS4 game dump holds
the game's executable and assets, not the game's C++ source. The loaded image
check in `scripts/game_check.py` currently targets CUSA03173 v1.09; patches and
runtime hooks depend on that executable layout. Texture/mesh mods can change
memory and upload costs even when the executable stays identical. Keep the
original game dump and list enabled mods when comparing performance.

## Validate without the game

`image-buffer-sync-test` uses the production Vulkan runtime to clear an image,
copy it to a VRAM buffer at a nonzero offset, and upload a second image from that
buffer before any CPU wait. It verifies every pixel, untouched bytes around the
copy and four successive submissions. Build/run it in the native shell:

```sh
ninja -C out/gpu image-buffer-sync-test
out/gpu/image-buffer-sync-test.exe
```

The selective coherence backport from upstream
[3c57da4](https://github.com/DarkIzuku/bloodborne_pc/commit/3c57da4198ad62039f63f33bfd205b9916518acf)
publishes render-target downloads to their VRAM buffer immediately and prevents
completed VRAM-only writes from being treated as current guest RAM. Guest CPU
readback remains in place. `BB_GPU_DATA_CHECK=0` disables the latter check for
diagnostics only; it can reintroduce stale reads.

Those two fixes apply to `BB_GUEST_IN_PLACE`. This Windows fork currently rejects
that model in `bbport_windows_experiments.cpp`; the backport does not enable it or
import the Linux memory layer. The GPU test checks common transfer/barrier paths,
not guest-memory import, the guarded cache branches or the reported in-game
motion-blur bug. No Windows FPS or VRAM saving follows from this backport alone.

## Shared Windows cache budget

The legacy Windows renderer now passes the same driver usage, budget and pressure
state to both texture and buffer caches. The memory model remains unchanged.
Buffer allocation keeps its 64 MiB pool when there is headroom, suppresses the
next speculative pool near the pressure threshold and falls back to the requested
size if a larger pool allocation fails. Unknown budgets disable speculation and
pressure eviction; they still allow safely retired empty allocations to be freed.

Cold CPU-backed buffer copies are eligible after 60 seconds normally, 5 seconds
under pressure and 1 second at critical pressure. Work must have completed and no
GPU-only bytes may remain. Scans and eviction batches are bounded. Shader DMA
access conservatively keeps resident ranges active. Sparse slots retire only after
the unbind submission completes; empty whole allocations can then return to Vulkan.
Unbinds are submitted before the same guest range can be rebound, following the
[Vulkan sparse binding ordering rules](https://docs.vulkan.org/refpages/latest/refpages/source/vkQueueBindSparse.html).
Guest unmaps also retire fully unmapped legacy buffer blocks. Unlike pressure
eviction, they can include draws still being recorded, so legacy unmap maintenance
submits prior work before changing those mappings. GPU timeline waits chain sparse
batches and order unbinds after earlier reads, without a CPU wait for the entire
queue. These extra submissions occur only when cached ranges are retired; assess
their cost in the same-route gameplay comparison.

Texture pressure passes keep a 5-second grace period
(`BB_GC_PRESSURE_IDLE_SECONDS`, 1–63); critical passes use 1 second. If cold images
are insufficient above the critical threshold, a bounded emergency pass retains
the existing writeback protections. Image-to-buffer transfers now mark the legacy
buffer bytes (including GPU-filled HTile metadata) as GPU-owned, preventing stale
CPU uploads or premature reclamation.

`vram.csv` and session reports distinguish `pending_buffer_bytes` (awaiting GPU
retirement), cumulative `buffer_evicted_bytes` (discarded cached copies) and
`buffer_freed_bytes` (whole allocations and unused spare pools returned to Vulkan).
Evicting a slot alone does not lower allocated VRAM. Older captures remain readable.

`buffer-budget-test` exercises the actual legacy caches on headless Vulkan with a
small mapped guest fixture: fresh CPU data after eviction/rebinding, retained
GPU-written data, image/buffer alias ownership and whole-allocation release after
unbind completion. Guest protection services are simulated; it does not test real
game fault handlers or establish an FPS/VRAM gain. Run GPU tests in the MSYS2 native
shell, which waits for these executables and captures their output:

```sh
ninja -C out/gpu buffer-budget-test memory-pressure-test
out/gpu/buffer-budget-test.exe
out/gpu/memory-pressure-test.exe
```

Use the existing same-route A/B procedure to assess real gameplay before changing
the memory model. A forced 4 GiB cache budget on an 8 GiB GPU tests policy only;
it does not reproduce a real 4 GiB card's bandwidth, capacity or driver behavior.

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
