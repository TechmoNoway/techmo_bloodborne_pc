# Techmo engine integration

`engine.lock.json` pins the native base and imported engine patch.
`tools/bootstrap-engine.ps1` reproduces the native source in `engine/native/`;
`tools/build-native.ps1` compiles and stages it in `out/`.

`bbhost-changes.patch` contains imported engine work. `techmo-compat.patch` adds
modifier-chord input, mouse-state integration and existing mod-flag compatibility.
It does not replace the renderer.

Generated sources, binaries, caches and game assets stay outside Git.
See [Build and verification](../docs/BUILD.md) and
[source acknowledgements](../licenses/SOURCE-NOTICES.md).
