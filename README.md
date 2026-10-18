<div align="center">

# Techmo Bloodborne

**A Windows home for Bloodborne. Built to be understood, tuned and improved.**

Windows x64 · Vulkan · C# / WPF · English + Tiếng Việt

[Build launcher](tools/build-launcher.ps1) · [Build engine](tools/build-native.ps1) · [Contribute](CONTRIBUTING.md)

</div>

Techmo brings the launcher, native runtime and renderer into one Windows-focused
workspace. Pick your game folder, configure controls and graphics, and keep your
settings in one place. The interface starts in English and can switch to Vietnamese.

The **Hunter’s Desk** interface pairs game artwork with launch controls, quick
settings and expandable system details. The language selector stays available
on every page, and smaller windows use a scrolling layout.

## What you can configure

- Click-to-bind keyboard/mouse controls with Shift/Ctrl/Alt combinations, Default / Dark Souls 3 / Custom presets
  and controller selection.
- Render/output resolution, 30 / 60 / 90 FPS presets and available upscalers.
- Mods, community patches and optional shadNet connection settings.
- English or Vietnamese launcher UI, independent of the game's language.
- Session exit reports, optional frame/GPU CSV recordings and native minidumps.
- **Quit Game** on the title screen, with the game's own YES/NO confirmation.
  It uses the window-close path and saves completed pipeline-cache work before exiting.

Recorded whole-session statistics
include loading screens and menus; use the same route for comparisons.

In **Controls**, click a keyboard or mouse slot and press the desired input.
Esc cancels, **×** clears one slot, and **Save settings** keeps the changes.
Custom bindings are retained when switching presets. For `Left Shift + Mouse1`,
click the mouse slot, hold Left Shift and click the left mouse button. The same
capture supports multiple side-specific modifiers, keyboard keys and the wheel.
The new combination syntax requires an engine rebuilt from this source.

Available graphics features depend on the GPU, driver and installed assets.
Restructuring the project or changing the launcher does not by itself raise FPS.

## Renderer measurements

Frame recordings separate image hashing, snapshot copies, detile/upload command
setup and unchanged mip bytes. These CPU timings can overlap; they are not GPU
transfer times. Known compute selections are queued during command scanning;
first-time shader translation can still require a wait.

Graphics lookahead now attempts up to 16 ordinary direct draws per scanned buffer
before publishing it to preparation workers. `BB_GRAPHICS_SCAN=0` disables that
pass for comparisons. Only known shader modules/permutations are eligible; new
shader translation and required draws keep their correctness fallback. Pipeline
log timelines separate queue delay, native compilation and lead time; these overlap
foreground waits and must not be added together. Startup/shutdown log the compatible
native cache seed, warm-up record counts and saved payload size.

Retired CPU-backed texture allocations may be reused for up to one second, with
exact image-create compatibility and fresh layout/hash/guest state. The recycler
keeps at most 16 MiB by default (also capped to 1/256 of the reported driver budget),
is disabled above 75% budget usage or without a usable budget, and drains under
memory pressure. `BB_IMAGE_RECYCLE_MB=0` disables retention; explicit values are
capped at 64 MiB. GPU-modified surfaces and video/scene proxies are excluded.
The retained bytes remain part of driver/image usage, not extra free memory.
Frame logs include allocation, image-view and format-query CPU scopes and reuse
counts; VRAM logs expose retained bytes. Bounded texture samples identify repeated
refreshes and slow creations. They do not weaken dirty tracking or enable parallel
hashing automatically. Gameplay p99 improvements require matched cold/warm captures.

`BB_DETILE_SCRATCH_REUSE=0`, `BB_KEEP_MIP_HASHES=0` and `BB_COMPUTE_SCAN=0`
disable individual changes for comparisons. `BB_PARALLEL_IMAGE_HASH=1` enables
the optional owned-snapshot hash experiment; it is off by default because its
copy/task cost can outweigh parallel hashing. Run `tools/test-tiled-writeback.sh`
in MSYS2 CLANG64 for GPU upload/lifetime and mip-hash regressions.

## License and credits

Techmo's combined distribution uses **GPL-3.0-or-later**, consistent with the
inherited bbhost ports. Original source notices and dependency licenses remain
in place; GPL-2.0-or-later components retain their provenance.
Read [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and [NOTICE.md](NOTICE.md).

Bloodborne is a game by FromSoftware and Sony. Techmo is an independent project
and is not affiliated with or endorsed by either company.

### GPU buffer/image synchronization and first-use pipelines

GPU writes now invalidate every overlapping image, including writes inside a
mip/alias, while neighbours outside the byte range remain clean. A matching CPU
hash cannot suppress a pending GPU refresh. The image upload still reads the
canonical GPU buffer and retains the existing barriers and detiling commands.

For GPU-only dirty refreshes, the CPU hash reference can be retained only with
continuous full write tracking, no CPU dirty hints, matching uploaded
backing/layout, unchanged guest mappings and no host backing write since the
reference was recorded. Mapping changes, any backing write (even unrelated),
tracking gaps and unwatched guest modes conservatively force hashing. This skips
CPU hashing, not GPU copies. `BB_REUSE_GPU_SOURCE_HASH=0` disables the reuse.
Sampled texture logs include GPU source/write range and reused hash bytes.

First-use graphics pipelines can expose their resource metadata/layout before
the native driver create finishes. An owned copy of every Vulkan create-info
pointee is queued on the existing graphics compiler; at most four queued/running
first-use jobs are accepted, with backpressure thereafter. Resource preparation
and later draw selection can overlap the native create. The recording thread
waits at the first native handle use, preserving all draws and shader quality.
Generated helper modules and pipeline layouts remain alive through completion;
shutdown and shader replacement drain pending native creates. Lookahead and
compute paths keep their previous policies. `BB_FIRST_USE_PIPELINE=0` restores
the prior required wait path. `gfx_bind` timelines report the remaining wait.

Cold guest-shader translation still runs on the requesting thread: it follows
live resource tables and cannot safely use the native create-info snapshot.
These changes do not guarantee elimination of first-use stalls or a measured
FPS improvement; compare the same cold/warm route and settings in gameplay.

### Pipeline lookahead and hash reuse diagnostics

Graphics key equality/hash now ignore unnamed padding in register/bitfield
structures. Canonical bytes preserve the current v6 serialization layout and
hashes of old zero-padded keys. Lambda captures and cache lookups use the same
semantic identity; the existing pipeline/shader/driver caches are retained.
The CPU-only pipeline-key-test also runs in the Windows workflow.

The scanner keeps four early draws and spreads its remaining selections through
the buffer, including the final direct draw, within the same 16-selection budget.
`BB_GRAPHICS_SCAN_SPREAD=0` restores the previous head-only policy for comparisons;
`BB_GRAPHICS_SCAN=0` disables scanner selections as before. Full draw preparation
and required fallback still handle every actual draw.

Known-module lookahead can now publish immutable pipeline frontend state before
running its native creation continuation on the same compiler thread. The GPU
command thread can consume layout/resource metadata; the recording thread waits
for the exact native handle when needed. No draw or compilation is skipped.
`BB_PIPELINE_EARLY_FRONTEND=0` restores whole-result lookahead waits. Setting
`BB_FIRST_USE_PIPELINE=0` also disables early lookahead frontend publication.
Compiler priority accounts for simultaneous command/recording consumers; older
required native jobs retain queue priority. Limits and worker count are retained.

Timeline `gfx_frontend` records metadata readiness; `gfx_bind` records native
completion consumption, including lifecycle drains of an unused pipeline.
These scopes and CPU thread wait counters overlap. `capacity_ms` records time
waiting for a first-use job slot; admission_ms separates registry-lock admission.
Early-lookahead native queue time starts when the owned native inputs are complete;
its earlier metadata queue/readiness has a separate gfx_frontend timeline.
Slow frontend samples split descriptor layout,
pipeline layout, state/helper construction, owned snapshot and enqueue time.

With BB_FRAME_STATS=1, texture samples include a hash-blocker mask and per-image
candidate/hit counts. The five-second GPU hash reuse line reports cumulative
multi-cause counters: disabled/tracking off, absent GPU source, backing/mip/CPU
state, tracking gaps, mapping changes/busy state and host write changes/busy or
invalid references. Several reasons can apply to one attempt; do not sum reasons
or repeated snapshots. This adds evidence without relaxing reuse safeguards or
changing the global host-write epoch. It does not claim to have reduced hashing
in the reported gameplay scenes. Cold guest shader translation remains on the
requesting thread; useful preparation lead and native driver costs still matter.

### Buffer consumers and shader ordering

Vertex, index, indirect draw/dispatch and indirect-count bindings retain their
actual Vulkan consumer stage and access type until the draw/dispatch is recorded.
Shader descriptors retain graphics/compute read/write access, including GDS.
The existing guest-address hazard domain still orders overlapping sparse aliases.
Indirect-command reads participate in read-after-write/write-after-read checks;
the binding list can spill beyond its inline shader-descriptor capacity.

Shared-memory dependencies now emit subgroup memory barriers where appropriate,
while the existing workgroup execution barriers preserve divergent if/loop safety.
Shader collection detects FP64 arithmetic, comparisons and conversions as well as
packing operations. Both changes require fresh translations: binary versions are
13 (regular) and 14 (motion), with metadata schema 8. Old shader binaries are rejected without deleting
the player's cache files, so the first visit to a scene can compile again.
These correctness changes are not a measured FPS improvement.

Headless checks: `buffer-access-test` queues actual vertex/index/indirect/count
and indirect-dispatch consumers followed by immediate overwrites, then verifies
GPU results. `shader-memory-test <existing-output-directory>` checks the production
IR passes and writes SPIR-V fixtures for `spirv-val --target-env vulkan1.3`.

### Depth, indirect IDs and texture cache lifetime

Fragment shaders use early depth/stencil tests only when requested by the game.
Shaders exporting depth keep the existing depth-export path. The early-test flag
and direct/indirect draw distinction participate in shader specialization.
Indirect vertex/instance IDs account for Vulkan base offsets even when the game
does not copy those offsets into SGPRs; fetch shaders retain ownership of offsets
they already handle. Instance step rates use the corrected ID as well.

The LRU uses stable indices into contiguous storage and honors boolean callback
termination. A precomputed BCn mip table covers equivalent, complete square
layouts; other layouts retain Techmo's existing size/alignment calculations.
Sampler keys compare descriptors, captured custom border colors, comparison mode
and LOD bias. A bounded lookup memo is invalidated when the map moves values, and
collection waits for GPU retirement without adding a synchronous GPU wait.

CMask/HTile/FMask metadata belongs to a live image generation. Rebinding or
deleting an older image cannot clear a newer owner's metadata. CMask reads expose
the expanded 1/2/4/8-sample patterns through the existing GPU buffer coherency
path; unsupported sample counts and misaligned CMask fills are rejected.

`render-semantics-test`, `image-layout-cache-test` and `lru-cache-test` run without
a game or Vulkan device. The `metadata` and `samplers` modes of `tiled-gc-test`
exercise actual Vulkan fills/readbacks and cache retirement. These changes still
require gameplay validation; no FPS or 4 GiB stability improvement is established.
