# Stability, shader correctness and tiled VRAM collection

The default native build includes bounded audio concatenation, checked savedata
search growth, null/incomplete descriptor validation and renderer-assertion dumps.
Assertions retain exit code 23 and now produce an exception record with code
`0xe0424223`; session reports identify `renderer_assertion`. The external reporter
handles main/worker failures and waits for a concurrent first report before exit.
The full assertion and source location remain in the GPU log.

The shader translator implements undefined U64 and preserves EXEC masking after
an empty closing/opening scope. Shader binary versions are now 9 (regular) and
10 (motion). Existing binaries from versions 7/8 are rejected, so a first run may
compile shaders again. The cache is not deleted from disk by this change.

## Tiled writeback experiment

`BB_IMAGE_WRITEBACK_GC=1` enables the experimental collector path. Its default is
off. It also requires CPU write tracking. Existing dynamic memory-budget thresholds,
frame caps, presentation choices and the staging upload reuse policy remain in use.

Only fully tracked, completed plain-color tiled images are candidates. The path
rejects depth/stencil, compressed metadata, MSAA, block/volume/thick layouts,
scene proxies, unsupported mip layouts, GPU-written buffer aliases and other
GPU-written image aliases. It checks the driver's actual headroom and caps a
surface at 16 MiB and collection at one attempt per 250 ms sample. Failed
candidates back off. Unsupported images remain resident with their GPU contents.

The GPU detiles the existing backing, overlays the image's logical texels and
tiles the result back, preserving row/layer/mip padding. An owned temporary linear
buffer stays alive until GPU completion, then is released; this path does not add
a persistent 16 MiB block to the upload staging rings. A store validates every
mapping under the runtime read lock and refuses the whole result if the mapping
generation changed. A stale download cannot overwrite a newly recycled mapping.
Only a successful store permits collection and invalidates clean aliases.

### Compare gameplay on the tester's GPU

Save the desired settings in the launcher first. From the project root run:

```bat
tools\experiments\test-vram-writeback.cmd off
tools\experiments\test-vram-writeback.cmd on
```

These launch the native runtime with the last saved game path and settings,
using the existing prebuilt engine. Both runs enable the same CPU write tracking
and frame/session logs. They do not modify launcher preferences. Run the same
route/settings, warm shader caches in both, and repeat lamp travel and combat.
Keep any save backup policy already used for ordinary testing.

Normal launcher runs use the default-off path unless the environment explicitly
requests the experiment. The local `builds/TEST-VRAM-WRITEBACK.cmd` forwards to the
entry above and accepts `off`/`on` (default `on`). Use `builds/TEST-LAUNCHER.cmd`
for the normal launcher.

Each recorded session has `frames.csv`, `vram.csv` and `session.json`. New memory
columns are cumulative `gc_tiled_attempts`, `gc_tiled_writebacks`, `gc_tiled_bytes`,
`gc_tiled_kept`, `gc_tiled_ms`, `gc_tiled_max_ms`. Reports retain their maxima,
not the sum of repeated samples. Older CSV headers remain supported. Compare
memory plateaus, p99 frame time and crash evidence. Live image allocation bytes
are different from driver heap usage: allocator/staging reserves can remain after
individual resources are collected. Microbenchmark times are not gameplay FPS.

## Verification

In the configured MSYS2 CLANG64 shell, after the native build:

```sh
bash tools/test-runtime-guards.sh
bash tools/test-shader-correctness.sh
bash tools/test-tiled-writeback.sh
python -m unittest discover -s tests -p 'test_*.py'
```

The first two scripts are also called by the native CI test path and need no GPU.
The third uses real Vulkan with synthetic mapped guest memory. It covers
8/16/32/64/128-bit micro/macro surfaces, padded rows, mips/layers and repeated
buffer reuse; real cache guards; and default/off/on collection under async/three
worker and sync/one worker recording. Game files and real saves are not loaded.

See [the port notice](../../licenses/STABILITY-SHADER-GC-PORTS.md) for provenance.
