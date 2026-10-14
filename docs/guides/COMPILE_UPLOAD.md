# Compilation and resource upload work

Implemented on 2026-10-14 after the small memory safety batch. This is a first
pass at the measured hitches, with gameplay acceptance still pending.

The reviewed capture contains a 517 ms frame near 512 ms of foreground compile
work. Other long frames have no compilations and large image/buffer uploads.
These need separate fixes and comparisons; average FPS alone misses both.

## Native pipeline lookahead

Draw preparation can now queue native Vulkan graphics pipeline creation when
the required shader modules and permutations already exist. One background
compiler retains at most 32 outstanding results, including completed results
that have not been consumed. Duplicate keys and foreground reservations prevent
the same pipeline being built twice. Shader metadata, register user data,
runtime state and fetch information are copied into owned snapshots.

The worker uses its own driver cache, seeded from the foreground cache. It does
not hold the foreground cache mutex during creation. Only the GPU thread
publishes pipelines into the graphics map, reconnects their canonical runtime
stage pointers and saves their recipes. Shutdown drains accepted work before
merging the driver caches and saving. Existing disk caches remain compatible:
shader translation and recipe formats have not changed.

A required draw promotes its unstarted pipeline ahead of speculative queued
jobs, then waits for completion. An already running driver call cannot be
preempted. A preparation
failure falls back to required foreground compilation. No draws or resources
are skipped. The lookahead is limited to triangle lists/strips and point lists
with known vertex and fragment modules and dynamic vertex input. Debug shader
replacement disables it. Cold shader translation, unseen permutations,
compute pipelines and other primitive paths retain foreground compilation.
Lookahead can hide work only when the preparation thread gets far enough ahead;
driver contention and extra work may limit the gain.

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
waits >=2 ms. The shutdown summary records queued/adopted pipelines and all
foreground handoff wait time. These counters have different scopes.

`session.json` adds `compilation` and `resource_uploads`; `summary.txt` includes
their main figures. Reports retain at most 20 large events in each list and
stream the log. Upload/frame-time co-occurrence does not establish a cause.

Use these environment switches before starting Techmo for controlled comparisons:

| Override | Behavior |
| --- | --- |
| `BB_PIPELINE_PREP=0` | Existing foreground native pipeline creation |
| `BB_STAGING_REUSE_SLACK=0` | Existing staging reuse size threshold |

Both default to enabled and explicit overrides appear in session provenance.
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
The GPU fixture builds real production pipelines both ways
and checks every rendered pixel after releasing the preparation snapshot.
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
