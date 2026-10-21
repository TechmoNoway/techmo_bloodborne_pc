# Pipeline replay lab

Reconstruct recorded graphics/compute pipelines through Techmo's production
metadata loader and Vulkan pipeline constructors, without launching Bloodborne.
Use one captured case repeatedly after a pipeline change. The game is still
needed to validate pixels, gameplay, scheduling contention and frame pacing.

## Build once

From the repository root, in PowerShell:

```powershell
.\tools\build-pipeline-lab.ps1 -Jobs 2
```

This produces `out/gpu/bb-pipeline-lab.exe`. It does not replace the running
launcher or install a game runtime. The helper uses the same renderer library;
the normal game constructor retains its warm-up and workers. The offline
constructor skips those operations and uses a read-only cache database.

## Capture a case

Capture after the game exits so all cache writes have finished:

```powershell
.\tools\experiments\pipeline-replay.cmd capture user/cache/CUSA03173 out/pipeline-lab/my-case
```

For a single hitch, use the full key from `Pipeline timeline`, e.g.:

```powershell
.\tools\experiments\pipeline-replay.cmd capture user/cache/CUSA03173 out/pipeline-lab/one-hitch --key g_0x0123456789abcdef
```

Or use `--from-log path/to/gameplay.log`. This selects explicit graphics/compute
pipeline diagnostics, not shader hashes or memory addresses. Missing keys cause
a clear error: a hitch without a persisted record cannot yet be replayed.

The new bundle contains selected `.key` records, all shader dependencies, the
profile and compiler stamps, plus a SHA-256 manifest. Original GCN `.input`
records are deliberately excluded. Source files are checked for concurrent changes;
the bundle is verified before and after every run. Existing output directories
are not overwritten. These local records contain game-derived data; keep them
local rather than committing or publishing a shared cache.

## Measure repeatedly

```powershell
.\tools\experiments\pipeline-replay.cmd run out/pipeline-lab/my-case out/pipeline-lab/before --repeats 3 --mode empty
.\tools\experiments\pipeline-replay.cmd run out/pipeline-lab/my-case out/pipeline-lab/warm --repeats 3 --mode warm
```

- `empty`: a new, empty **application VkPipelineCache before every sample**.
- `warm`: one application cache retained across pipelines and repetitions.
  The first pass seeds it; subsequent passes can reuse it.
- Both modes destroy pipeline objects and shader modules between samples, so
  repeated timings do not measure a hit in Techmo's in-memory pipeline map.
- `reconstruct_ms` covers the production loader, metadata/SPIR-V reads,
  shader-module/layout creation and completed native pipeline construction.
  It excludes key auditing, JSON output, object cleanup and cache creation.
- Windows filesystem caching and the driver's internal/disk caches remain active.
  **Neither mode guarantees a cold driver compilation.**
- Native constants, GPU selection and other `BB_*` feature settings must match
  the captured profile. The report records the environment and executable hash.
  Device/profile mismatches and stale/missing compiler stamps fail explicitly.
  No automatic migration, shader rebuilding, cache deletion or gameplay fallback
  can hide a rejected record.

Outputs: `report.md`, `report.json`, flushed `events.jsonl`, and `process.log`.

## Verify production startup warm-up

```powershell
.\tools\experiments\pipeline-replay.cmd run out/pipeline-lab/my-case out/pipeline-lab/startup --mode startup
```

This separate helper process runs the production `WarmUp()` loader once, with an
empty application Vulkan cache and the captured database opened read-only. It
checks that every selected record is accepted and its native pipeline is present
in the actual graphics/compute map before gameplay. `startup_keys` records each
key's availability and rejection reason. Shader rebuilding/migration is skipped;
the player's cache is never opened. The bundle is verified before and after.

Startup mode requires one repetition (the default in this mode). Its total time
includes the complete startup loader, not per-key reconstruction samples. It
cannot be used with `compare`. Driver/OS caches remain active. A pass proves
availability for this bundle/GPU/settings; it does not establish whether those
records existed or were accepted in an earlier gameplay session.

## Trace first-use compute work during gameplay

First-use compute pipeline misses now build their layout on the GPU command
thread and submit owned native-create state to the existing compute worker.
Resource preparation can continue while the driver compiles. Binding still
waits for completion before dispatch; no dispatch is skipped. Required jobs use
the existing bounded queue, and shutdown/shader replacement drain accepted work
before retiring modules or merging native caches.

`BB_FIRST_USE_COMPUTE=0` selects the previous required-worker selection/wait path
for A/B testing. Already queued compute lookahead still waits at selection, and
first-time shader translation still runs on the command thread. This change
does not promise to hide the full duration of a genuinely cold compilation.

Runtime `Pipeline cache lookup` lines distinguish loaded, absent or rejected
startup records, the bounded rejection reason, shader state and preparation path.
`Pipeline timeline: compute_bind` separates queue, compile, lead and remaining
bind wait. Session diagnostics retain bounded examples and full event counts.

## Interpret reconstruction timings

The report now breaks successful reconstruction down into **exclusive** wall-time
phases: record decode, metadata read/decode, SPIR-V read/validation, shader-module
creation, descriptor-layout creation, pipeline-layout creation, pipeline state
assembly, the native Vulkan pipeline call, and remaining bookkeeping. Nested
phases are subtracted from their enclosing phase. Their sum must match the sample
duration within 0.1 ms, and a successful timed sample must include a native call.
Per-key medians, per-round distributions, the 20 slowest samples and phase totals
for samples over 33 ms are retained in `report.json`.

Timing is opt-in on the replay thread. Inactive scopes do not read the clock;
ordinary cache hits do not enter the instrumented construction paths. The lab
records shader-specialization evidence **after** timing, outside the measured
interval. This evidence compares the production lookup relation in both directions
and records binding starts and an Info fingerprint. It never creates a merged
shader, changes a key or reuses a pipeline object to make a sample appear faster.

The last `begin` without a matching sample identifies a crash/timeout candidate.
A driver crash stops that run; capture that key alone to isolate it. A rejected
record is reported as a failure and excluded from timing aggregates. The command
returns nonzero for any rejection, crash, timeout or incomplete run.

## Compare a change

Rebuild the helper, then run the **same bundle and mode** into a new directory:

```powershell
.\tools\experiments\pipeline-replay.cmd run out/pipeline-lab/my-case out/pipeline-lab/after --repeats 3 --mode empty
.\tools\experiments\pipeline-replay.cmd compare out/pipeline-lab/before/report.json out/pipeline-lab/after/report.json --threshold-ms 5
```

Comparison requires successful complete runs, identical input manifest, GPU,
driver, cache mode and repetition count. It reports per-key median changes and returns nonzero if
any increase exceeds the requested absolute threshold. It is a local regression
signal, not proof of significance: use more repetitions and alternating A/B runs
when differences are small. Keep repeat counts and external GPU load consistent.

This first version replays **captured SPIR-V and pipeline state**, not the GCN
translator or a frame's work queue. For translator experiments, rebuild a
separate copy with `bb-shader-lab` first and capture a new case. The immutable-case
comparison intentionally refuses to compare different shader inputs.

## Audit variant keys

`report.json` separates:

1. **Canonical aliases:** different record names have the same production
   canonical key and identical saved auxiliary state. The canonicalizer removes
   padding, using exactly the implementation used by live key hashing.
2. **Dynamic-state candidates:** different canonical keys become equal after
   removing vertex formats and/or color write masks only when that GPU supports
   the corresponding dynamic state. Stage identities and auxiliary state must
   also match. This is **not permission to merge**: masks can affect derived
   static blend state, and metadata/descriptors still need review.
3. **Same-stage groups:** useful for investigating state-driven variant growth;
   sharing shader stages is normal and does not establish redundancy.
4. **Identical SPIR-V candidates:** byte-identical modules under different names.
   Their specializations/resource layouts may still differ. This dependency audit
   includes unselected shaders copied into the bundle.
5. **Name/hash mismatches:** a record filename disagrees with the current
   canonical key hash. This can expose records retained from an older hash policy;
   it does not prove an extra pipeline was compiled during the current session.

No key is normalized for execution and no shader/pipeline is merged by this tool.
Review a candidate, add a targeted correctness test, then measure the change.

`spirv_classification` separates identical-byte groups belonging to one program
from those crossing program hashes. Each member records its permutation, metadata
header identity, compiler stamp state, selected-key references and whether its
metadata was actually accepted during replay. Only current-stamped, header-matching
permutations of the **same program** enter pairwise classification. If both were
observed, the report distinguishes different lookup specializations, different
binding starts, different static Info fingerprints, different descriptor types/counts,
and mutually matching evidence. The full captured Info fingerprint is reported
separately, since changing captured resource values alone need not create a
different compiled shader.
Fingerprints come directly from the persisted metadata bytes, after parsing the
specialization with the production decoder. They do not hash a reconstructed C++
copy, whose unused vector slots/padding may vary between runs. The static prefix
fingerprint is conservative: different persisted padding may prevent a match;
unequal fingerprints do not establish that the meaningful static fields differ.
Old/missing stamps and unobserved records remain visible rather than being counted
as proven redundant permutations. Header consistency alone does not establish ABI
compatibility. Fingerprints and lookup equality are review evidence; the full
Info also contains captured resource values which can legitimately differ.
Native comparison work is bounded to the first 256 observed variants of a program.
The report marks pairs outside that coverage as `not_compared_by_native_audit`,
never as unequal. Pair counts are complete for the classified group; JSON retains
up to ten examples per classification per group, plus exact aggregate counts.
Raw comparison coverage is retained in `shader_variant` events.
