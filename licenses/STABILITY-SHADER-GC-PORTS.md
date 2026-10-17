# Runtime guards, shader correctness and experimental tiled collection

Adapted into Techmo on 2026-10-17. Existing authorship and notices remain in place.

DarkIzuku/bloodborne_pc sources:

- `4ee4e9ed39056d016bd882237edacb2f695a2d03`: bounded AJM input concatenation and
  savedata search allocation handling. Local failure-injection fixtures exercise
  the production functions; null audio inputs and empty spans are also checked.
- `dceac3f4789c0125bc7df446c56b135590c8d146`: null/incomplete descriptor table
  checks (upstream attribution: Supermedo `2c4ddeba`), tessellation pointer checks,
  tiled writeback/overlay, layout guards and GPU overlay fixture. Deferred user-data
  snapshots remain authoritative. The full upstream GC/budget policy is not imported.

shadps4-emu/shadPS4 sources, GPL-2.0-or-later:

- `ab1dd384dc8a20c7faa3766324a27763768c59a3` (#5366): emit undefined U64.
- `c34ca21f12153b6b61c30dcae38e638348841b61` (#5220): retain EXEC scope after an
  empty closing/opening instruction. The original source copyright notices remain.

Local adaptations include the renderer assertion bridge to Techmo's prestarted
Windows minidump reporter, concurrent-failure coordination, mapping-generation
guarded backing stores, exact-lifetime temporary linear buffers, bounded opt-in
collection, metrics, entry scripts and regression fixtures. The shader binary
versions are advanced for both regular and motion modules.

The tiled writeback/overlay and associated layout helpers/tests retain their
upstream GPL-3.0-or-later terms. Existing GPL-2.0-or-later code can be combined
under GPL-3.0-or-later, as the current distribution already does for other ports.
See `GPL-3.0.txt`; third-party SDK/model terms are unchanged. Game data, generated
game assets, native dumps and test binaries are not distributed as source changes.

The shadPS4 GC part 2 commit `e6faa2e3b6794a2fa79a709352ddfccfaa817ed1` was
reviewed, but its full dirty-state/edge-page collector replacement is not imported.
