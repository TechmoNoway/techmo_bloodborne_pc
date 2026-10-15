# Techmo Renderer experiment

Status: design proposal, 2026-10-12. Branch: `experiment/techmo-renderer`,
starting at `4f7541e`. No new renderer backend or performance improvement has
been implemented or measured by this proposal.

The checkout already contained uncommitted memory/cache and diagnostics work
when the branch was created. Those changes remain in the working tree and are
part of any local build unless explicitly separated. A branch does not snapshot
uncommitted files; preserve the exact source fingerprint of each benchmark build.

## Direction

Build a Bloodborne-specific **Techmo Renderer** by replacing measured bottlenecks
in stages. Keep the current Vulkan renderer as the comparison and recovery path.
A complete GPU decoder, resource system and GCN shader compiler rewrite would
make correctness difficult to establish before producing useful measurements.

The intended advantage is lower CPU translation cost, fewer redundant uploads
and barriers, predictable VRAM use, better temporal inputs and smoother frame
delivery for this game. A higher displayed FPS with interpolation alone does
not demonstrate faster native rendering or better input response.

Use Techmo Renderer as the working name for the owned layer. Preserve existing
upstream source paths, authorship and notices during the experiment. Rename
owned targets and directories after their boundaries are real; mass renaming
`shadps4/`, namespaces or `BB_*` interfaces is not a performance experiment.

## What the current checkout already has

| Area | Evidence in source | Remaining experiment |
| --- | --- | --- |
| Guest GPU translation | `engine/renderer/shadps4/video_core/`, shader recompiler | Profile decoding, recording, resource lookup and synchronization separately |
| Temporal reconstruction | `vk_temporal_upscaler.{h,cpp}` | Extract provider-neutral frame inputs; verify both native-AA and scaled paths |
| DLSS Super Resolution | `vk_dlss.{h,cpp}`, dynamic NGX Vulkan calls | Validate current integration with official headers/ABI and controlled captures |
| FSR | `vk_fsr4.{h,cpp}`, `fsr411/`, vendored FSR-Vulkan | Track provider/model/assets/device capabilities independently |
| Motion | `vk_camera_motion.*`, `vk_object_motion.*`, `motion_history.h` | Measure coverage and correctness for skinned/deforming objects, effects and cuts |
| Frame generation | FSR-Vulkan framegen implementations and presenter policy helpers | Connect to the game presenter; no framegen calls were found in the game renderer/shim |
| Inspection | `vk_frame_capture.*`, raw upscaler dumps, `tools/dump_view.py` | Add consecutive-frame metadata and complete replay inputs |
| Measurements | `docs/guides/DIAGNOSTICS.md`, session analyzer | Distinguish simulation, native rendering and actual presentation with FG enabled |

These are source observations, not evidence that every path works on the current
Windows build. A vendored test or helper is not an end-to-end game integration.

## Proposed source boundaries

```text
engine/renderer/
  shadps4/                 existing guest decoder and shader translation
  techmo/                  proposed owned layer (not created yet)
    frame/                 semantic frame data, jitter and history invalidation
    resources/             residency, upload planning and resource lifetimes
    temporal/              DLSS / FSR / TAA provider adapters
    presentation/          HUD composition, pacing and frame generation
    capture/               versioned temporal captures and validation
```

Keep the Vulkan backend first. Select a second graphics API only if a measured
benefit or required SDK feature justifies its shader, synchronization and
maintenance costs. Cross-API copies for upscaling can erase the expected benefit.

First extract the frame input contract currently embedded in
`Fsr4Upscaler::Frame`. DLSS already consumes this FSR-named type. Its successor
should describe the resources and their meaning without choosing a provider:
scene color stage and color space, depth convention, motion units/direction,
render/output sizes and viewport, current/previous camera transforms, jitter,
exposure/pre-exposure, frame ID, simulation timestamp, delta time, reset reason,
optional masks, and resource layout/ownership/completion token.

SR and FG need separate adapters: SR reconstructs the current rendered scene;
FG consumes consecutive scenes and owns additional presentation requirements.
Resolve optional inputs and unsupported formats inside each provider adapter.

## Dump structure: two different needs

For the **game dump**, keep the supported source game layout. Add a derived,
versioned preparation cache before considering a new archive format:

```text
out/prepared/<content-and-recipe-key>/
  manifest.json
  image/                   derived executable and link metadata
  patches/                 applied patch recipe and hashes
  assets/                  derived asset indexes, if measurements justify them
```

The key must cover the game version/content hashes, preparation tool revision,
link/runtime ABI version, patch selection and mod overlay identity. The manifest
records compatibility and hashes; changed inputs invalidate the cache. Preserve
the original dump and saves, write through a staging directory, and finalize
atomically. Shader caches additionally depend on translator revision, GPU,
driver and enabled Vulkan features. Existing preparation already validates the
supported game and computes hashes; extend that machinery.

A custom game container primarily targets preparation, loading and I/O. It
does not repair motion vectors or automatically improve steady-state FPS.
Measure those costs before repacking assets or adding compression.

For a **renderer capture**, a versioned directory is a useful first format:

```text
out/captures/<capture-id>/
  manifest.json            schema/build/GPU/driver/settings and frame range
  frames/<frame-id>/
    frame.json             conventions, matrices, jitter, timing, reset reasons
    resources.json         formats, extents, strides, stages, hashes and file names
    scene-color.raw
    depth.raw
    motion.raw
    hudless-display.raw    display-size scene at the FG input stage
    ui.raw                 only when separable; record alpha/composition semantics
    reconstructed.raw
    presented.raw
```

Capture exposure textures, masks and other resources when the selected provider
needs them. Record missing inputs explicitly. Retain lossless numeric inputs;
PNG previews are unsuitable as the only HDR, depth or motion replay data.
Capture consecutive frames plus provider-specific warm-up history and resets.
The current pass log is useful for analysis but is not a complete GPU replay.
Offline temporal replay and full guest command-stream replay are separate scopes.
For temporal replay, record model/version/assets and deterministic frame order.
For full GPU replay, command packets, resource contents and synchronization would
also be required. Bound capture memory and use asynchronous readback outside
performance runs. Keep captures local and outside Git.

## Data quality and integration requirements

| Requirement | Implementation and validation target |
| --- | --- |
| Motion vectors | Canonical current-to-previous motion without jitter, with explicit units; provider adapters convert as required. Validate camera, rigid, skinned and deforming motion, new objects and history identity. Camera-only reconstruction is insufficient for combat. |
| Depth | Same scene/viewport as color and motion; declare reversed/finite/infinite depth, projection and near/far parameters. Inspect silhouettes, occlusion and newly visible surfaces. |
| Color and exposure | Identify linear/HDR/tonemapped input and pre-exposure. Supply exposure or a supported auto-exposure mode. Validate dark interiors and abrupt lighting changes. |
| Jitter and resets | Apply consistent subpixel jitter to the scene, keep UI stable, and reset on camera cuts, teleport, load, incompatible size/provider changes and invalid history. Avoid duplicate temporal AA. |
| Effects and UI | Keep output-resolution HUD separable for interpolation. Use masks where the selected provider expects them; FSR 4 ML does not share the analytical FSR reactive-mask requirement. Test smoke, sparks, hair, foliage, fog and UI transitions. |
| Resource lifetime | Keep SR/FG resources alive until every consuming queue completes. Record barriers, layouts and queue ownership explicitly; avoid overwriting history still in flight. |
| Presentation | Match frame ID and camera data to the presented scene; handle resize, minimize, acquire failure, device loss, mode switches, refresh limits and fallback. Measure interpolation cost and queue depth. |
| Latency | Pace input/simulation/render work and use supported latency APIs where available. Generated frames must not advance the simulation. Instrument actual displayed cadence and input latency separately. |
| Capability checks | Probe SDK/driver/device features and formats at runtime, include asset/model version, explain unsupported choices and fall back to a tested provider. SR support does not imply FG support. |

The table describes the proposed shared contract and quality checks. Consult
each pinned provider's documentation for its actual mandatory inputs.

AMD's current official FSR Upscaling page lists 4.1.1 support for RX 9000 and
RX 7000 discrete GPUs and DirectX 12. This checkout's Vulkan INT8/replay providers
are separate implementation paths with their own feature and asset requirements;
do not label them official Vulkan SDK support or promise identical compatibility.

NVIDIA documents Vulkan DLSS-G integration through Streamline, including
resource synchronization and Reflex markers. Adding `nvngx_dlss.dll` for SR does
not integrate FG. Its current guide limits Dynamic MFG and FG with VSync to
D3D12; query capabilities for the exact SDK/driver rather than assuming all
DLSS features share Vulkan support.

## Experiment order and acceptance

1. Record a repeatable baseline with FG off. Use the same area/camera/combat
   route, build fingerprint, driver, power mode, resolution, upscaler and mods.
   Run at least three warmed repeats and keep cold-cache runs separate.
2. Add provider-neutral frame metadata and temporal capture validation. Use
   camera pan, stationary-camera animation, combat, particles and teleport/load
   cases to establish correct input direction, history and HUD separation.
3. Optimize one measured cost at a time: uploads/residency, descriptor lookup,
   command recording, shader/pipeline compilation or redundant synchronization.
   Preserve GPU-visible guest writes and completion ordering. Record a local
   toggle or comparable builds for each experiment.
4. Integrate one FG provider end-to-end with conservative resource ownership,
   reliable fallback and mode/resize handling. Compare FG off/on at the same
   native workload; only then investigate useful queue overlap or other providers.
5. Move proven owned modules into the proposed Techmo layout. Consider larger
   decoder/compiler replacements only when profiling still identifies them.

Suggested initial target: stable 60 native FPS (16.67 ms budget), followed by
90 native FPS (11.11 ms) where hardware permits. These are targets, not measured
results or a universal minimum for FG. Begin FG quality evaluation near a stable
60 native FPS; evaluate lower rates explicitly rather than hiding them behind
displayed FPS. Lower resolution helps only when saved GPU work exceeds SR cost
and the workload is not already CPU-limited.

Report native frame time p50/p95/p99, 1% lows, CPU recording/translation, GPU
scene/SR/FG time, uploads, VRAM, compilation stalls, displayed cadence, latency
and visual failures. Existing flip CSVs are not sufficient for generated-frame
statistics; add presentation instrumentation before making such claims.

Accept a change only when its intended cost improves beyond repeated-run noise,
tail latency and correctness remain acceptable, and no new crashes or resource
growth appear across traversal. Cross-GPU conclusions need runs on those GPUs.
Compare against the current Techmo baseline first; any claim about outperforming
shadPS4 also needs a separately controlled shadPS4 build and run.

## References

- [Current project architecture](ARCHITECTURE.md)
- [Build and measurement workflow](guides/DIAGNOSTICS.md)
- [AMD FSR Upscaling: supported platforms and versions](https://gpuopen.com/amd-fsr-upscaling/)
- [AMD temporal integration guide](https://gpuopen.com/manuals/fsr_sdk/techniques/super-resolution-temporal/)
- [NVIDIA DLSS Super Resolution guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS.md)
- [NVIDIA DLSS Frame Generation guide](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md)

Vendor links were checked on 2026-10-12. Pin SDK revisions when implementation
starts; upstream `main` and supported-device lists can change.
