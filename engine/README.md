# Techmo engine integration

`engine.lock.json` pins the native base and imported engine patch.
`tools/bootstrap-engine.ps1` reproduces the native source in `engine/native/`;
`tools/build-native.ps1` compiles and stages it in `out/`.

`bbhost-changes.patch` contains imported engine work. `techmo-compat.patch` adds
modifier-chord input, mouse-state integration and existing mod-flag compatibility.
It does not replace the renderer.

`techmo-menu.patch` restores the Techmo title-menu Exit Game row, native YES/NO
confirmation, all installed title variants and verified 1.09 hook guards.

`techmo-vram.patch` connects the shared `overlay/vram_monitor.h` sampler to the
native overlay. It matches the renderer's Vulkan LUID to DXGI and samples this
process's local GPU memory against dedicated physical capacity on a worker.
Shared RAM and the changing WDDM budget are not reported as VRAM capacity.

Generated sources, binaries, caches and game assets stay outside Git.
See [Build and verification](../docs/BUILD.md) and
[source acknowledgements](../licenses/SOURCE-NOTICES.md).
