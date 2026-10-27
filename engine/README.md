# Techmo engine integration

`engine.lock.json` pins upstream 0.9.37 stable at commit
`3860021d6550f95397ba74d046fb14498816a534` and its exact engine subtree.
`tools/bootstrap-engine.ps1` imports that complete tree into `engine/native/`;
no moving upstream branch or old 0.9.34 monolithic patch is used.
The old generated checkout is retained under `builds/_internal/` on upgrade.

The renderer, shader catalogs, frame pacing, game patches and upscalers come from
that pinned source. Techmo changes are separate:

- `techmo-compat.patch`: side-specific key chords, current keyboard/mouse state,
  Techmo capture spelling and mod-manager flags. Upstream multi-input bindings remain supported.
- `techmo-menu.patch`: title Quit/Exit remains available with optional PC settings off;
  the upstream native row builder and YES/NO dialog are tested with guest-call mocks.
- `techmo-vram.patch` and `overlay/vram_monitor.h`: process VRAM / physical dedicated
  capacity on the renderer's adapter, sampled off the render thread.
- `techmo-platform.patch`: Techmo executable icon/name, explicit isolated INI and a
  512 MiB ThinLTO build cache (gameplay shader caches are unaffected).

The hosted settings backend follows the same release. Techmo retains its own UI,
input capture, profiles, Apply/Undo and session reporting. Save safety copies stay
in this checkout's ignored `save-vault/`; periodic backups follow the upstream setting.

This is a modified integration of Lantern Engine by Awaken
(https://github.com/Awakenchan/LanternEngine), not an official release.
See [source acknowledgements](../licenses/SOURCE-NOTICES.md) and
[upstream terms](../licenses/Lantern-Engine-NOTICE.txt).
