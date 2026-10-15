# Compilation and resource upload work

Implemented on 2026-10-14 after the small memory safety batch. This is a first
pass at the measured hitches, with gameplay acceptance still pending.

The reviewed capture contains a 517 ms frame near 512 ms of foreground compile
work. Other long frames have no compilations and large image/buffer uploads.
These need separate fixes and comparisons; average FPS alone misses both.

## Native pipeline lookahead

Draw preparation queues native Vulkan graphics and compute pipeline creation
when the shader modules and permutations exist. The graphics compiler retains
at most 32 speculative results and the compute compiler at most eight, including
completed results awaiting use. Each compiler can accept one additional required
miss while the single GPU consumer waits. Duplicate keys prevent the same
pipeline being built twice. Shader metadata, register user data, runtime state
and fetch information are copied into owned snapshots.

Each compiler uses its own driver cache, seeded from the foreground cache. It
does not hold the foreground cache mutex during creation. Only the GPU thread
publishes pipelines into the graphics map, reconnects their canonical runtime
stage pointers and saves their recipes. Shutdown drains accepted work before
merging the driver caches and saving. Existing disk caches remain compatible:
shader translation and recipe formats have not changed.

A required miss is submitted to the native compiler even if no speculative job
was queued. Required work overtakes queued jobs, and Windows temporarily raises
that compiler from idle to normal thread priority while the GPU consumer waits.
An already running driver call cannot be preempted. Failed background work falls
back to foreground compilation. Draws and resources are preserved.

Graphics lookahead now includes lines, depth-only draws, and generated rectangle
and quad tessellation helpers. Those generated shader modules have scoped
ownership through pipeline creation, including error paths. Compute lookahead
replays the graphics queue's private compute-register state and direct dispatch
packets; selection on the GPU thread still verifies the actual shader and
specialization key before adopting a result. Async compute submissions and
indirect dispatches use the required path when first needed.

Preparation previously discarded a buffer after encountering an unknown shader
or permutation. It now retries unavailable draws after a program publication,
up to four passes per buffer. Already published draw data and its backing arenas
remain immutable. Programs first used after disk-cache warmup also publish their
new preparation templates. Speculative shader-header reads return unavailable
for incomplete headers and use the existing guest-read fault recovery.

Cold GCN-to-SPIR-V translation and unseen permutations still run on the GPU
command thread, where live guest resource tables and compiler state are owned.
Once those modules exist, native pipeline creation uses the compiler worker.
Guest tessellation selection also remains on the required path. Debug shader
replacement disables native preparation. Lookahead hides work only when it
gets far enough ahead: moving a required compile onto a worker still leaves a
first-use wait. Gameplay frame times, rather than a lower foreground compile
count alone, determine whether the change reduced hitches.

## Resource upload costs

Large staging requests reuse the smallest compatible retired allocation within
bounded slack. For example, an available 32 MiB buffer can serve a 20 MiB upload
instead of allocating and touching new mapped storage. The original memory-type,
GPU completion and deferred-hold checks remain. Requested bytes, flush ranges,
copy commands, pool trimming and upload scheduling are unchanged. Reuse may
retain a larger allocation; it is intended to reduce allocation/first-touch
cost rather than guarantee lower VRAM usage.

Exact-base image lookup scans the first registration page rather than every
page of a large range. All exact-base images are present there. Size selection,
GPU eligibility and alias behavior remain, with tests for multi-page images and
unregistration. This adapts [shadPS4 #5240](https://github.com/shadps4-emu/shadPS4/pull/5240);
region-manager synchronization remains in place.

Image upload statistics now count only source mip ranges actually scheduled,
excluding unchanged GPU-owned mips and no-op refreshes. This is a measurement
correction, not a reduction of those transfer commands. Buffer and image byte
counters can describe the same data at different stages and must not be added.
No texture resolution, mip availability, quality setting or DLSS model changes.

## Reports and comparisons

`BB_FRAME_STATS=1` records `pipeline_wait_ms` per flip, alongside foreground
compile counts and upload/allocation counters. The session log records foreground
compile operations >=8 ms, background pipeline operations >=8 ms and preparation
waits >=2 ms. The shutdown summary records queued/adopted graphics and compute
pipelines, required submissions, retries and all foreground handoff wait time.
Required misses record `cold_shader`, `new_permutation` or `not_prepared`.
These counters have different scopes; background time can overlap a foreground wait.

`session.json` adds `compilation` and `resource_uploads`; `summary.txt` includes
their main figures. Reports retain at most 20 large events in each list and
stream the log. Upload/frame-time co-occurrence does not establish a cause.

Use these environment switches before starting Techmo for controlled comparisons:

| Override | Behavior |
| --- | --- |
| `BB_PIPELINE_PREP=0` | Existing foreground native pipeline creation |
| `BB_COMPUTE_PREP=0` | Disable compute lookahead and required compiler handoff |
| `BB_PIPELINE_RETRY=0` | Process each preparation buffer only once |
| `BB_STAGING_REUSE_SLACK=0` | Existing staging reuse size threshold |

These features default to enabled and explicit overrides appear in session provenance.
For a comparison of the whole pipeline batch, disable both `BB_PIPELINE_PREP`
and `BB_PIPELINE_RETRY`; the master preparation switch also disables compute.
Keep red-zone protection enabled in both runs.
Keep FPS cap, GPU profiling, graphics/upscaler settings and the route identical.
Compare first traversal separately from subsequent cached traversals. Do not
delete the player's cache just to make a run cold. Record p99, 1% low, frames
over 33/50 ms, compile kinds/waits, upload/allocation peaks and VRAM pressure.
Test Quality and native rendering separately before testing lower resolutions.

The pre-change native executable and manifest are preserved under
`builds/_internal/backups/compile-upload-2026-10-14/` (native SHA-256 starts
`040ba399391fc8bd`). The test entry remains `builds/TEST-LAUNCHER.cmd` and uses
`out/bb-probe.exe`; the original parent launcher is separate.

## Automated validation

The new queue fixture checks concurrent duplicate prevention, bounded snapshots
and results, foreground waits/reservations, snapshot failures, failed compilation
and shutdown drain, plus required-job priority over speculative queued work.
The GPU fixture builds production pipelines both ways and checks every rendered
pixel after releasing the preparation snapshot, including generated rect/quad
helpers. It also checks compute register replay, incomplete shader headers and
prepared/required compute selection with exact storage-buffer readback.
The buffer budget fixture checks retired large-buffer reuse, held/in-flight and
undersized exclusions, correct GPU readback bytes, and exact-base image lookup.

Run from the configured MSYS2 native shell, capturing real exit codes:

```sh
ninja -C out/gpu -j2 pipeline-precompiler-test pipeline-precompile-gpu-test buffer-budget-test
out/gpu/pipeline-precompiler-test.exe
out/gpu/pipeline-precompile-gpu-test.exe
out/gpu/buffer-budget-test.exe
```

Headless correctness tests do not establish a Bloodborne FPS or hitch reduction.
The next gameplay comparison determines whether these changes are useful on
the RTX 4060 Laptop and the 4 GiB RTX 3050 Ti. Remaining large upload bursts may
need more work on range copies, detiling, residency and scheduling, supported
by the new reports.
