# Small memory correctness fixes

This batch adapts upstream shadPS4 fixes to the existing Techmo backend. It does
not replace the memory model or change the FSR/DLSS SDKs.

Local validation on 2026-10-14: the eight targeted memory/renderer test programs,
125 Python tests, native runtime fixtures and rebuilt executable's Vulkan smoke
passed. GPU checks used an RTX 4060 Laptop GPU. Gameplay acceptance is pending.

- Completed nested CE, graphics and compute indirect-buffer tasks destroy their
  coroutine frames. Root submissions keep their existing queue ownership.
- Every sparse backing allocation requests device-address support, including
  demand allocation, asynchronous spare allocation and tight pressure fallback.
- `INDEX_BUFFER_SIZE` persists independently of direct draw counts and capacities.
  All three indexed indirect variants carry it into draw-pipeline register deltas.
- Index range arithmetic uses wide offsets, rejects a null base before mapping
  queries, clamps to mapped memory and aligns to complete 16/32-bit indices.
  Missing storage uses an immutable 16-byte zero buffer. Required
  `robustBufferAccess2` supplies zero for reads beyond its end.
- When existing maintenance5 support is available, the index binding includes its
  mapped size. Older devices retain the legacy binding command; its bounds cover
  the physical buffer rather than the explicit mapped subrange. Maintenance6 is
  not introduced as a new requirement.
- Index reads use `INDEX_INPUT`/`INDEX_READ` tracking. The remaining vertex,
  indirect-argument and count-buffer tracking group is a separate upgrade.

Upstream references and local adaptations are recorded in
[`engine/renderer/VENDOR.txt`](../../engine/renderer/VENDOR.txt).

## Automated checks

Run in the configured MSYS2 native shell so GUI-subsystem executables are waited
on and their real exit codes are captured:

```sh
ninja -C out/gpu -j2 index-buffer-range-test liverpool-tasks-test buffer-budget-test
out/gpu/index-buffer-range-test.exe
out/gpu/liverpool-tasks-test.exe
out/gpu/buffer-budget-test.exe
```

`liverpool-tasks-test` executes production PM4 coroutines: 1,000 submissions per
engine with 16 nested indirect buffers, balanced allocation/free counts, a child
that suspends and resumes, and capacity preservation across direct/indirect draws.

`buffer-budget-test` uses production caches and headless Vulkan. Guest mapping
services are fixture boundaries. A compute shader dereferences actual sparse
device addresses under demand, spare and tight allocation paths. Indexed vertex
shader draws verify zero fallback and out-of-bounds behavior for 16/32-bit indices,
as well as bounded partial storage when maintenance5 is available. Existing
eviction, GPU-owned data, alias and unmap/retirement checks remain active.

## Gameplay acceptance

Open `builds/TEST-LAUNCHER.cmd`. Use the same settings and route as the saved
comparison build. Enable frame statistics and capture a warmed 15–30 minute run,
including combat, stairs, deaths/reloads and several area transitions. Compare
frame times, process memory, VRAM/budget and crash reports. Repeat on an actual
RTX 3050 Ti; local RTX 4060 Laptop checks do not substitute for that hardware.

These checks establish memory correctness coverage, not a measured Bloodborne
FPS gain or a guarantee that total process RAM will decrease. Other allocations
and caches contribute to process memory. Headless tests do not exercise real
guest fault handlers or full gameplay.
