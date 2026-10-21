# Shader cache, memory and renderer experiments

Branch: `experiment/shader-cache-replay`. Gameplay acceptance is separate from fixtures.

## Recorded pipeline replay

See [pipeline-replay.md](pipeline-replay.md) for the standalone Vulkan replay lab:
immutable cache snapshots, repeated construction, per-key timing comparisons and
an audit of canonical aliases / possible redundant state variants. This is
independent of the CPU shader rebuild helper below.

## Shader rebuild

New shader permutations retain their original GCN, runtime/binding inputs, user
registers, flattened resource data and captured fetch/tessellation reads in local
`.input` files. They contain data from the user's game and must not be distributed
as a shared shader pack. Existing caches cannot invent these inputs: they acquire
them when a shader is translated on the new build. Benefits after compiler updates
start once those records have been collected.

The compiler identity follows translator sources, host compiler version and direct
memory mode, independently of UI commits. GPU/profile checks and shader metadata
versions still apply. Before
pipeline warm-up, `bb-shader-lab.exe` reconstructs compatible shader binaries in a
separate CPU-only process. Replay never follows guest pointers from a previous
session. A resource-walker layout change or missing captured read rejects the entry.
Inputs have a bounded size, schema/ABI identity and checksum; completed files replace
old files atomically. A per-shader attempt marker prevents a repeatedly crashing
shader from blocking all remaining entries for the same compiler. Startup has a
bounded helper retry/time budget. Failed or unavailable preparation leaves ordinary
first-use compilation available. `BB_SHADER_REBUILD=0` disables the helper.

A current directory-wide compiler stamp no longer hides missing/stale stamps on
individual captured shaders. Startup scans for those repair candidates; the helper
checks capture checksum/ABI, identity and metadata generation before rebuilding.
A capture already labelled current is only a warm hit when its stage stamp and
structurally valid binary are also present. New failure markers include capture
and metadata fingerprints, so unchanged failures stay suppressed while a coherent
replacement can retry during the same compiler build. Records without original
captured inputs still need ordinary first-use translation. Startup diagnostics
record repair results and counts of pipeline rejection reasons separately.

Individual damaged pipeline records no longer clear compatible records.
Lookup keys retain the requested runtime state. Compiler-added clip-distance
inputs stay in the translated shader and must not increment the input count in
the lookup key. Both live compilation and offline rebuilding follow this rule.
Missing legacy compiler stamps are reported through compatibility results rather
than one filesystem error per lookup. Validity/version checks remain active.
The older standard GPU profile migrates without clearing compatible records;
modules introduced only by a rejected warm-up record are rolled back.
Partial rollback invalidates the removed specialization without shifting surviving
permutation indices. Both render-thread and preparation-worker lookups require a
live shader module; a missing permutation is compiled again on the required path.
Graphics and compute constructors reject missing active modules before Vulkan
pipeline creation, with the pipeline key included in the diagnostic.
Driver pipeline compilation still occurs during normal warm-up; this feature does not
precompile shaders which have never been captured, and is not a shader catalog.

## Runtime and VRAM

Windows memory preflight retains the successful committed section in a small helper
process. Runtime duplicates that exact section handle and maps it instead of asking
Windows for a second pool. The launcher closes its helper on completion or launch
failure. Explicit pool requests remain strict; automatic expanded-pool failure still
coordinates the existing standard-heap/render fallback. This reserves Windows
commit capacity earlier; it does not reduce the game's required RAM or VRAM.

Queued VRAM demotions include the latest GPU reader tick, even if a buffer was bound
again after evacuation was requested. A read in the current command buffer defers
demotion; sparse rebinding waits for the updated submission before a slot is reused.
The existing age, GPU-written-data, mapping and budget guards remain in force.

## FSR preparation

FSR v07 and the FSR 4.1.1 replay can create model pipelines/resources on a worker.
FSR 3.1 runs during preparation; completion switches back with history reset. Only
the owning thread submits GPU work or retires the previous context. There is a
conservative budget check for the temporary fallback context; insufficient/unknown
headroom retains synchronous setup. `BB_FSR4_ASYNC=0` selects synchronous preparation
for comparisons. Source-v07 context flags and the 4.1.1 asset capture already use
ordinary depth, so no reversed-depth flag is added. Old externally captured 4.1.1
assets must be checked separately.

## First-use stalls and image refresh

Required first-use native graphics jobs receive normal CPU priority from their
start, including the interval before the recording thread reaches the bind.
Speculative jobs retain idle priority. The existing reference-counted priority
callback restores priority after success/error without demoting another waiter.
`BB_REQUIRED_PIPELINE_PRIORITY=0` selects the previous bind-wait-only policy.
This removes scheduling delay the runtime can avoid; a genuinely new pipeline
still needs native driver compilation.

Early graphics preparation now yields between frontend publication and native
creation. The continuation uses the same worker/cache, behind already required
jobs and ahead of other speculation. Adopting a frontend promotes its pending
continuation; accepted continuations still drain during shutdown. An in-flight
driver call cannot be preempted. No extra compiler threads or speculative result
slots are added. `BB_PIPELINE_EARLY_FRONTEND=0` selects the older combined path
(it disables early publication as well, so this is not an isolated yield benchmark).

First-use compute misses also defer native creation to the compute worker while
the command thread can prepare resources. `BB_FIRST_USE_COMPUTE=0` restores the
previous required-worker wait at selection. The owned native inputs survive
constructor return; bind, shutdown and shader replacement wait before retiring
the associated objects. Runtime provenance distinguishes absent/rejected startup
records from later preparation. See [pipeline replay](pipeline-replay.md) for
the read-only production startup check and its limits.

Host backing writes track bounded ranges in 1 MiB stripes. A fully protected
GPU-source image can retain its CPU hash reference across unrelated host writes.
Overlapping writers, stripe aliases, unknown/oversized writes, mapping changes,
CPU dirtiness and lost protection still invalidate reuse. The stripes use
monotonic counters rather than hash equality; overflow disables reuse. All
required GPU copies, detiling and image uploads remain in place.
`BB_RANGE_BACKING_EPOCH=0` restores the global backing-write epoch for comparison.

Session summaries report per-flip pipeline blocking, including native bind waits,
separately from thresholded legacy events and per-stage timelines. These counters
overlap and must not be added. Slow image allocations log separate requirement
query and VMA call durations so loading spikes can be attributed before changing
allocation policy. The parser retains bounded allocation evidence in
`texture_activity.slow_allocations`.

## Native constants experiment

`BB_NATIVE_CONSTANTS=1` selects a std140 uniform-buffer path for host-owned flattened
constants up to 16 KiB. Writable, mixed-type and larger resources keep the storage
path. Scalar addressing uses uvec4/component indexing; descriptor types, offset
alignment and padded ranges agree with the shader interface. This is a focused
experiment, not a replacement shader compiler. It is off by default and uses
separate `-constants` shader/driver caches.

Run `tools/experiments/test-native-constants.cmd on` and `off` with the same route,
resolution, upscaler and FPS/present settings. Compare both cold and repeated runs.
Report frames above 33/50/100 ms, 1% low, foreground compilation, GPU time, peak
VRAM and visual differences. Synthetic readback is not a gameplay speedup claim.

## Verification

- `tools/test-shader-correctness.sh`: production translator/backend and cache replay.
- `tools/test-runtime-guards.sh`: small real Windows section/handle fixtures.
- `tools/test-renderer-upgrades.sh`: production storage/uniform compute layouts and
  exact GPU values, SPIR-V validation, FSR worker/error publication and memory policy.

Do not use player saves or the original launcher for fixtures. Long-session stability,
boss transitions, teleporting and comparative gameplay captures remain required.

## Image allocation size queries

The dedicated-allocation size hint now uses a per-logical-device CPU cache with
512 LRU entries. Keys include all scalar image create parameters in the supported
domain. Extension chains, concurrent sharing, explicit queue lists, sparse and
disjoint images bypass it. Driver calls run outside the cache mutex. Nothing in
this cache retains an image, allocation or VRAM; the existing image recycler keeps
its budget/retirement rules. VMA still creates, queries and binds every new image.
See the [Vulkan resource requirements specification](https://docs.vulkan.org/spec/latest/chapters/resources.html#resources-association).

`BB_IMAGE_REQUIREMENTS_CACHE=0` restores uncached size queries for comparison.
`frames.csv` adds `image_query_ms`, `image_vma_ms`, `image_query_hits`,
`image_query_misses` and `image_query_bypasses`; older captures remain readable.
The query timer includes cache lookup; the VMA timer includes image creation,
memory allocation and binding. These are nested inside `image_alloc_ms`, not
additional frame time to sum. Slow allocation records also show `query_cache`.

Build/run `image-allocation-cache-test` and `memory-residency-test`, then run the
existing `tiled-gc-test recycle` fixture with retention enabled and disabled.
Compare gameplay over the same route/settings on both cache settings, especially
loading/teleporting: query hit rate, query/VMA spikes, frames >33/50/100 ms, 1% low,
peak VRAM and pixels. First-time shapes and time spent inside VMA remain possible
sources of stalls; synthetic tests do not establish an FPS improvement.

## Shader compiler reproducibility

`tools/shader_repro.py` rebuilds local `.input` captures using the production
CPU-only helper in fresh processes, alternating input insertion order. It requires
`out/bb-shader-lab.exe`, `out/gpu/shader-cache-input-test.exe` and `spirv-val.exe`.
Build the native runtime and the `shader-cache-input-test` Ninja target first.

```powershell
python tools/shader_repro.py --cache out/my-captured-cache --output out/shader-repro/my-run --runs 8
```

Add repeated `--stage 0x0123456789abcdef` arguments to focus on recorded stages.
Use a new output directory each time. The tool reads the source cache into a
snapshot and writes only isolated result directories, retaining logs and a JSON
report. It refuses overlapping source/output paths and validates capture identity,
native lookup compatibility, typed persistent metadata and Vulkan 1.3 SPIR-V.
Exit 0 means byte-identical GPU shaders under the same compiler across the selected
runs; 1 means a reproducibility difference; 2 means an incomplete/invalid experiment.

Raw metadata/capture hashes are reported separately: the current serialization
includes unused static-vector slots, padding and host walker addresses. Their
byte equality is not the reproducibility criterion. Native comparison checks the
active persistent resource fields, lookup state, flattened data and walker code.
No SPIR-V canonicalization or optimizer rewrite is applied to hide differences.
This detects compiler instability; it does not prove pixel equivalence, faster
gameplay or coverage of shaders not present in the selected captures.

## Crash evidence and runtime identity

Launch sessions hash the selected `BB_PROBE` executable, using the manifest beside
that executable rather than assuming the data directory contains its runtime.
`runtime_identity.manifest_match` links recorded build information to the actual
EXE; a missing/mismatched manifest leaves version claims unverified. New manifests
also record the launcher version and Git commit/description/dirty state when
available. Folder names are not used to identify builds.

`diagnostics.json` records crash-reporter startup, handshake errors, fault-file
open/write failures and the bounded dump wait. The textual fault record includes
the original registers and instruction bytes when readable. `minidump.json`
records the external writer's result. An access-violation exit with no fault
record remains an unknown fault, not proof of GPU OOM or a previously seen RIP.
External termination and failure paths bypassing the unhandled handler can still
leave no exception context; the session summary makes missing evidence explicit.

Build the native runtime before `tools/test-runtime-guards.sh`, so its fixtures and
the reporter use the same crash-request protocol. The tests use synthetic faults,
including worker/concurrent assertions and reporter/file failures, without a game.

## Prepared vertex inputs and synchronous copy faults

Prepared vertex descriptors are used only when they match the draw packet's owned
snapshot. A mismatch takes the ordinary vertex-binding path using the packet;
the motion identity and merged buffer ranges are rebuilt from those descriptors.
Indirect index sync covers the larger of NUM_INDICES and a valid declared capacity,
with the existing null, alignment, overflow and mapped-range guards preserved.

GPU/draw callers explicitly opt watched guest copies into a synchronous fault
mailbox. A worker publishes its original fault context and waits; the waiting
caller handles it on its own thread, serially, before the worker retries. Workers
do not borrow cache ownership or submit cache work back to the blocked GPU thread.
The mailbox lives until all indices finish; async/host-only jobs never inherit it.
Parallel image hashing continues to use an owned readable snapshot and stays off
by default. This does not change which GPU-written data needs a readback.

`copy-fault-relay-test` uses actual Windows protected-page reads/writes, nested
loops, concurrent callers, declined faults and async isolation. Run with no
arguments, `--serial` and `--disabled`. `prepared-vertex-inputs-test` and
`index-buffer-range-test` cover descriptor rewrites and index range edge cases.
With frame diagnostics on, `Copy fault relay:` logs serviced requests and declines.
Fixtures do not establish that the tester's 1.3 crash is fixed or replace long
gameplay/visual checks.
