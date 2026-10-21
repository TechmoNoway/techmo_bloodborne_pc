# Techmo experiment-v2

This checkout is on `experiment-v2`, based on Awakenchan/LanternEngine
`5321f86245567b4ae3c37d5ab4c6f362ef46a63b` (0.9.34 stable).
The base engine and upstream patch are pinned in `engine.lock.json`.

## The Techmo interface

`apps/launcher` is the original Techmo Control Hub: its hero artwork, sidebar,
profiles, navigation, graphics cards, themes, English/Vietnamese interface,
Apply/Undo workflow and keyboard/mouse editor. The 29 action bindings retain
separate keyboard and mouse slots, action groups, search, modifier capture,
clear buttons, wheel input and the original capture overlay.

The source of InputKeyNames, MainWindow.Input and MainWindow.InputCapture is
unchanged from the current Techmo app. Its binding defaults and the user's
current local input choices are carried into the experiment by value.

Additional engine settings are presented in Techmo cards under the relevant
pages. Trophies and Engine switches use the Techmo sidebar and card templates.
The upstream launcher window is never constructed. Upstream layouts, window
buttons and themes do not replace the Techmo interface.

`apps/lantern-backend` supplies the upstream setting models and choice catalogs
as a library. Engine choices, defaults, INI handling and environment generation
remain upstream's, apart from namespace isolation and an embedding hook that
routes pending edits through Techmo Apply/Undo. This library has no runnable app.
The original upstream sources stay in `launcher/windows/BloodborneLauncher`.

## Input compatibility

Lantern numbers mouse buttons as SDL does: 1 left, 2 middle, 3 right. Techmo's UI
uses 1 left, 2 right, 3 middle. `MainWindow.InputBoundary.cs` converts only while
reading/writing bind.* INI values; physical button names and capture behavior in
the Techmo UI stay unchanged. It handles both alternatives and modifier chords.

`engine/techmo-compat.patch` adds Techmo's tested modifier-chord handling to the
native input adapter, including priority and release handling. It also honors
the existing Techmo legacy/root mod enable flags in the native starter. Only
input and mod loading are patched. The upstream renderer is not replaced or
mixed with the previous Techmo renderer experiments.

## Local data and builds

Techmo UI settings: `launcher-settings.json`.
Additional native settings: `user/launcher/settings.json`.
Shared game settings and bindings: `bbport.ini` (SDL mouse numbering).
Default saves/cache and session logs: this checkout's `user` and `logs` folders.
Explicit player-profile/save-folder choices are passed to the session wrapper.
The game dump is referenced and never bundled or edited.

- Native build: `tools/build-native.ps1 -Jobs 2` (MSYS2 CLANG64).
- UI build: `tools/build-launcher.ps1`.
- Offline UI: `tools/build-launcher.ps1 -Offline` with the local NuGet cache.
- Launch: `builds/TEST-LAUNCHER.cmd` or `builds/launcher/Techmo.Launcher.exe`.

The sibling Techmo toolchain may supply compiler dependencies and Python. Its
renderer binary and saves are not reused. Native provenance and binary hashes
are recorded in `out/native-build.json`, including the compatibility patch hash.
Build output, tool caches, personal settings, game files and saves are ignored.
Installed-package rename/cleanup migrations and upstream auto-install do not run
through this custom frontend. The source version stays pinned for experiments.

## Measurement

Graphics has a Techmo-styled Recommended graphics card. Recommend uses the pinned
Lantern Hardware.Recommend rules, comparing against pending UI selections. The
preview lists current and proposed values without editing settings. Use
recommendation stages the changes for the normal Apply/Undo flow. This is a
hardware-based recommendation, not an automatic gameplay benchmark. Native output
resolution choices, including Display (auto), persist through Techmo profiles.

Diagnostics has Start benchmark and Benchmark results. Alternatively use
`builds/launcher/Benchmark.cmd`. The pinned PresentMon 2.6.0 collector targets the
exact v2 executable path and visible game PID, excluding the memory helper.
Results, frame times and RAM/VRAM samples go to
`../benchmarks/results/<timestamp>-techmo-v2`.

Turn off frame generation and match resolution, quality, effects, FPS cap,
present mode and profiling state for comparisons. Startup/menu checks only
verify integration and are not gameplay performance comparisons.
Structured session reports remain in `logs/<session>/`.

## Verification

- `apps/launcher/Verify-Launcher.ps1`: original Techmo interaction and capture regression checks.
- `tests/TechmoHosted`: Techmo shell, supplemental settings, recommendations, Apply/Undo, INI mouse conversion, launch environment and screenshots. Standalone pages must occupy the full content row below the header; engine-switch persistence and the trophy list/filter are exercised.
- `tests/test_pc_input.cpp`: native chord parsing, priority, release, capture and mouse-adapter behavior.
- `python -B -m unittest discover -s tests -v`: benchmark analysis, source semantics and input provenance.

Preview images are saved under ignored `builds/_internal/techmo-hosted-previews`.
The superseded upstream-style frontend verification is archived with local UI
backups under `builds/_internal/ui-before-lantern-parity/superseded`.

The upstream GPL-2.0-or-later launcher authorship and license are retained.
Techmo additions and the native engine are GPL-3.0-or-later. Native, WPF-UI and
PresentMon notices are preserved in source and staged with local builds.
