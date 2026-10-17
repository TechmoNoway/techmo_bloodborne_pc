# Deferred recording and partial unmap

This batch adapts two focused fixes from DarkIzuku's Windows stability branch.
It leaves the renderer's memory model, upscaler settings and frame limits intact.
Source identities and license notices are recorded in
[the port notice](../../licenses/RECORDING-MEMORY-PORTS.md).

Local validation on 2026-10-16 passed: native build/runtime fixtures, CPU ownership
checks, Windows remapping, Vulkan readback with asynchronous/three-worker and
synchronous/one-worker recording, buffer-budget/image-buffer regressions and the
rebuilt executable's Vulkan smoke. GPU checks used an RTX 4060 Laptop GPU.
The pre-change runtime reproduces both queue overflow and dropped invalidations
with the same fixture. Gameplay acceptance on the RTX 3050 Ti remains pending.

Deferred commands retain the existing 128 KiB inline storage. Larger payloads
use separately owned spill allocations, retained by the recording chunk until
Vulkan has recorded its commands. Aggregate reservations keep multiple arrays
with the command that uses them. Bounds, alignment, arithmetic and capture cleanup
are checked, including chunks discarded before execution. This addresses the
previous assertion on payloads larger than the fixed recording chunk; it is not
a change to shader compilation or texture resolution.

The runtime memory notification queue grows past 64 entries. During partial
unmapping on Windows, surviving pieces of a section may be mapped again, losing
their GPU tracking protection. The early UNMAP callback is still sent once;
subsequent INVALIDATE callbacks for those surviving pieces are retained and
flushed outside the region-table write lock. The renderer can then restore its
tracking instead of treating those remapped pages as already protected.

## Checks

Run in the configured MSYS2 native shell:

```sh
ninja -C out/gpu -j2 record-data-test memory-notifications-test
out/gpu/record-data-test.exe --cpu-only
out/gpu/memory-notifications-test.exe
BB_ASYNC_SUBMIT=1 BB_VK_RECORD_THREAD=1 BB_VK_RECORD_THREADS=3 BB_VK_SEGMENT_KB=1 out/gpu/record-data-test.exe
BB_ASYNC_SUBMIT=0 BB_VK_RECORD_THREAD=1 BB_VK_RECORD_THREADS=1 BB_VK_SEGMENT_KB=1 out/gpu/record-data-test.exe
```

The CPU fixture covers oversized and over-aligned storage, arithmetic rejection,
chunk recycling and capture destruction after cancellation or an exception.
The GPU fixture copies more regions than fit in the old chunk, overwrites and
releases the original descriptor arrays before recording, and compares every
output byte over repeated chunks and submissions. It also checks direct-mode
borrowed spans and re-entry into deferred recording with producer diagnostics.

The mapping fixture exercises the production Windows section API and runtime
queue: 8,194 ordered notifications, then unmapping the middle of a three-page
view. It checks exactly one early UNMAP and two surviving-view invalidations,
preserved data, restored read-only tracking and rejection of an unaligned unmap.
Both device-free fixtures run in Windows CI; Vulkan readback runs locally.

## Gameplay acceptance

Start `builds/TEST-LAUNCHER.cmd` and keep the same route/settings as the saved
comparison build. Capture combat, deaths/reloads and several area transitions,
including the reported Blood-starved Beast encounter. Compare crashes, p99 frame
time and memory growth on the actual RTX 3050 Ti. These correctness checks do not
establish a gameplay FPS improvement or resolve every cause of that boss crash.
