# Techmo upgrade order

Planning date: 2026-10-13. Status: staged upgrade in progress.
The small correctness/lifetime batch is now implemented; see
[memory safety checks](guides/MEMORY_SAFETY.md). Gameplay acceptance remains a
separate gate. A first pass at compilation lookahead, staging reuse and exact-base
image lookup is also implemented; see [compile/upload checks](guides/COMPILE_UPLOAD.md).
Pipeline preparation now retries unavailable draws after shader publication,
includes compute and generated rect/quad helpers, and submits required native
pipeline misses to prioritized compiler workers. First-use waits and cold shader
translation still require gameplay measurements.
Further work in the stages below remains subject to measurement.

A focused recording/mapping follow-up is implemented: deferred payloads may
exceed 128 KiB with owned storage, the runtime notification queue grows past
64 entries, and partial Windows unmaps retain surviving-view invalidations.
See [recording and mapping checks](guides/RECORDING_MEMORY.md).

The immediate crash follow-up is Windows guest red-zone store protection; see
[implementation, coverage and tester acceptance](guides/RED_ZONE.md). It is
enabled in guest preparation. Intel/RTX 3050 Ti gameplay verification remains
pending, independent of the compilation and upload performance work.

The immediate goal is to explain and recover the reported FSR 3 performance
regression in the package built from `4f7541e`, while preserving image correctness
and resource ownership. Stable 60 native FPS is a target to validate on each GPU,
not a guaranteed result. Frame generation and a complete renderer rewrite follow
only after the native renderer has a measured, stable baseline.

This plan complements [the renderer experiment](RENDERER_EXPERIMENT.md) and
[the measurement guide](guides/DIAGNOSTICS.md).

## Working rules

- Keep the original launcher, game, save data and configuration intact.
- Preserve the current uncommitted cache/coherence work and existing user changes.
  A branch alone does not isolate that work. Use explicit source snapshots or
  separate checkouts and record the fingerprint of each built binary.
- Make one reviewable change per experiment, with a saved comparison build.
  Keep experimental output under `builds/_internal`; keep the normal test entry
  point at `builds/TEST-LAUNCHER.cmd`.
- Changes fixing invalid Vulkan use or stale GPU data need correctness tests;
  they must not be disabled to manufacture an FPS improvement.
- A performance change advances only after repeated gameplay measurements show
  an improvement beyond run-to-run variation, without worse tail latency,
  resource growth, crashes or visible artifacts. Headless tests cannot establish
  Bloodborne performance.

## 0. Freeze comparisons and reproduce the FSR 3 regression

Preserve three distinct references:

1. The original, unmodified launcher used by the tester as their comparison.
2. The `4f7541e` package from the RTX 3050 Ti report, native hash beginning
   `9a2b8c649e56`. Its logs have no frame/GPU/memory CSVs. If that exact package
   is unavailable, label a fresh build as a reconstruction, not the same binary.
3. The current local cache/coherence build, native hash beginning `9454ef9f6bdb`.
   It includes work absent from the reported package.

Record GPU, VRAM capacity, driver, CPU/RAM, power mode, native/source hashes,
provider/model/assets, settings, mods, FPS cap, actual input/output dimensions
and fallback behavior. Do not attribute a result to a commit name alone.

Use the same area, camera/combat route, effects and resolution. Capture at least
three warmed repeats of a fixed 3–5 minute route; retain a separate cold-start
run. Start with FSR 3 and an upscaler-off control. Enable frame statistics for
frame/VRAM CSVs; disable GPU profiling in comparative FPS runs. Run profiling
separately with the same instrumentation on both builds. Use the same supported
frame cap: 60 for pacing validation, optionally 90 for additional headroom data.

Track average FPS, 1% low, p95/p99, hitches, CPU cache/recording cost, VRAM usage
versus driver budget, uploads, evictions and whole allocations actually freed.
Exclude loading using explicit route boundaries, not an arbitrary time cut.

**Exit:** establish whether the cost is shared renderer/runtime work, provider
work, memory pressure, differing settings or packaging. No claim of recovery
without this comparison. Investigate one earlier change group at a time.

## 1. Transfer small correctness and lifetime fixes

Start with independent, narrowly scoped upstream changes:

- [#5258](https://github.com/shadps4-emu/shadPS4/pull/5258): destroy completed
  nested indirect-buffer coroutine frames. Validate graphics, compute and CE
  task lifetime without double destruction or loss of queued work. This fixes
  host heap/RAM ownership, not VRAM.
- [#5233](https://github.com/shadps4-emu/shadPS4/pull/5233): request device-address
  support for sparse backing allocations. Cover demand, asynchronous spare pool
  and small-allocation fallback paths. Verify shader access through a device
  address, not only transfer copies.
- The null-index-buffer group
  [#5327](https://github.com/shadps4-emu/shadPS4/pull/5327) /
  [#5358](https://github.com/shadps4-emu/shadPS4/pull/5358): adapt the full fallback
  to Techmo's index-resolution and draw-preparation paths. Check zero base,
  offset/size arithmetic, partial mappings and indirect-draw capacity.

**Exit:** targeted regression checks and real gameplay show correct data,
no new validation failures/crashes and no unexpected latency increase.

## 2. Complete access tracking and shader correctness

Adapt [#5262](https://github.com/shadps4-emu/shadPS4/pull/5262) for vertex, index,
indirect and count buffers, with correct access/stage masks. Include cached
bindings and recording/preparation workers. Preserve read/write ordering.

Review [#5285](https://github.com/shadps4-emu/shadPS4/pull/5285) for shared-memory
ordering and [#5342](https://github.com/shadps4-emu/shadPS4/pull/5342) for FP64
detection as separate changes. Update shader cache keys/versions where necessary
so old translations cannot silently bypass the fix.

Do not merge open raw-image-buffer synchronization
[#5280](https://github.com/shadps4-emu/shadPS4/pull/5280) as though it were a
released fix. Review alias coverage and required cases separately.

**Exit:** GPU-produced data survives vertex/index/indirect consumption; affected
shaders validate; images remain correct across movement, combat and area changes.

## 3. Reduce measured CPU texture-cache cost

The first performance experiment is
[#5240](https://github.com/shadps4-emu/shadPS4/pull/5240): restrict exact-address
image lookup to the first page's bucket. Adapt it to the current bucket format;
do not require the entire upstream cache refactor. Retain address, size and
download-eligibility checks. Test overlapping images, equal addresses with
different sizes, page-spanning images and registration/removal.

Then split [#5219](https://github.com/shadps4-emu/shadPS4/pull/5219) into experiments:

1. LRU and lookup storage overhead.
2. Image/mip/layout calculations and reusable metadata.
3. Sampler and descriptor lookup costs.
4. Locking changes only after a thread-ownership audit.

Techmo's helper/readback/write-tracking threads differ from upstream. Removing
a lock because upstream removed it is not sufficient justification.

**Exit:** measured lookup/CPU cost improves without new watcher assertions,
stale images, races or frame-time regressions. Upstream results from other games
are not a prediction of Bloodborne gains.

## 4. Make reclaimed VRAM useful without cache churn

The local build has shared buffer/texture pressure policy and GPU retirement
ordering. It evicted about 92.3 MiB of buffer copies in the latest local capture,
but returned no whole buffer allocations to Vulkan. Investigate this distinction.

Add bounded attribution for chunk occupancy, pending retirement, GPU-owned
blocks, upload/rebind activity and allocation growth. Determine whether live
blocks, fragmentation or conservative retention prevent chunk release.

Experiment with tighter spare-allocation policy, reuse of compatible temporary
targets, and safe draining/compaction of eligible chunks. Any relocation needs
copy, alias/device-address updates and retirement of every GPU consumer.
Do not discard GPU-only data or repeatedly force texture eviction merely to
lower a reported memory number.

Run a 20–30 minute area traversal and return to the same scene. Compare actual
driver usage, pressure/critical samples, upload cost and p99, not category sums.
Measure host RAM separately from VRAM.

**Exit:** use under constrained budgets stays controlled, freed-allocation
counters agree with behavior, and streaming/1% lows do not deteriorate. Forced
budgets on an 8 GiB GPU do not replace real RTX 3050 Ti testing.

## 5. Improve scene work and compilation stalls

Evaluate [#5264](https://github.com/shadps4-emu/shadPS4/pull/5264) only where the
guest requests early depth testing. Keep depth-export and side-effect semantics,
and invalidate incompatible shader cache entries. Measure applicable draw
coverage and GPU scene time rather than enabling early depth globally.

Separate cold shader compilation, image creation/upload bursts and CPU write
tracking in hitch analysis. Improve valid cache reuse and preparation where
measured. A shortcut that skips required draws is not an accepted optimization.

**Exit:** repeated scenes compile less or process fewer redundant resources,
with improved tails and correct depth, shadows, effects and geometry.

## 6. Recover FSR 3, then tune DLSS

Keep provider work distinct from the common renderer. Measure preparation of
color/depth/motion, history handling, provider dispatch, composition/sharpening
and presentation. Verify actual reduced-resolution scene coverage, format
conversions, jitter, exposure, masks, resets and output-resolution UI.

Optimize FSR 3 first against the captured regression. Then use the same frame
inputs and methodology for DLSS. Remove copies, resource recreation or waits only
when ownership and pass measurements justify doing so. Validate stationary and
moving cameras, character animation, particles, combat and loading transitions.

**Exit:** lower-resolution rendering saves more GPU work than the provider and
integration add, image quality remains acceptable, and the original-regression
comparison improves. CPU-limited runs may not gain FPS from lower resolution.

## 7. Keep FSR 4 experimental until it wins a measured comparison

Measure the v07 model and surrounding passes on each actual GPU. Its 12 model
shaders and other processing do not imply better FPS than DLSS; the log's
"2 passes from opt/" counts optimized assets, not the full model's pass count.

Check provider slot waits, working-set size, model/assets versions and actual
fallback. Preserve the working FSR 3/DLSS options. Test capability and performance
independently; do not choose a provider solely by GPU series or its version name.

**Exit:** retain recommendations only for GPU/preset combinations with a measured
quality/performance benefit; report unsupported cases and fallback accurately.

## 8. Expand the Techmo Renderer experiment after the above gates

Extract proven owned boundaries for resource residency, frame inputs, provider
adapters and diagnostics. Change command scheduling or recording parallelism only
where profiles still identify a bottleneck. Keep the native comparison path.

Frame generation requires a separate presentation, synchronization, UI and
latency project. A complete decoder/compiler/backend replacement is considered
only if incremental improvements leave a documented architectural bottleneck.

## GPU coverage and release gate

Begin with the reported RTX 3050 Ti regression and the local RTX 4060 Laptop
comparison. Extend to RTX 3050 and GTX 1650 Ti with providers that their actual
runtime capabilities support. Do not copy NVIDIA-only settings across the matrix.

Each accepted step gets its own commit, native manifest, saved comparison build,
test evidence and benchmark notes. Run targeted tests for the changed subsystem,
appropriate runtime regressions, Vulkan smoke and gameplay visual checks. Avoid
bundling unrelated UI changes, SDK upgrades and scheduling tweaks into the same
performance comparison.

The first implementation batch should cover stage 0 and the independent fixes in
stage 1. It should not rewrite the memory model or replace the upscaler SDKs.
