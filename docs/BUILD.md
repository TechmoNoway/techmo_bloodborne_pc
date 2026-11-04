# Build and verification

## GitHub Actions

**Build Techmo Windows** runs on pushes and pull requests targeting `main` and `experiment-v2`.
Manual runs are available from the Actions tab.

The launcher job runs Python checks and an isolated WPF fixture, then publishes a
self-contained .NET 8 Windows x64 frontend. The native job restores the pinned
source and builds with MSYS2 CLANG64, then combines the engine, frontend, runtime
DLLs and portable Python into **Techmo-Bloodborne-win-x64**. Extract and run
`Techmo.cmd`. **Techmo-launcher-win-x64** is a frontend-only artifact for an
existing runtime checkout. Artifacts retain license notices and build identity.
Failed runs upload available native build logs.

Game dumps, personal settings, saves and shader caches are never packaged.
Optional upscaler SDK assets are not included in the source-build package.

## Local build

Install .NET 8 SDK, Git and MSYS2 CLANG64. The workflow lists the required
CLANG64 packages. Python 3 is used by the session wrapper and checks.

```powershell
.\tools\bootstrap-engine.ps1
.\tools\build-native.ps1 -Toolchain C:\msys64\clang64 -Jobs 2 -SkipBootstrap
.\tools\build-launcher.ps1
.\builds\TEST-LAUNCHER.cmd
```

Set `BB_MSYS2` for a nonstandard MSYS2 installation. `build-launcher.ps1 -Offline`
reuses the local NuGet cache. Native output goes to `out/`, launcher output to
`builds/launcher/`. `out/native-build.json` records source pins and binary hashes.

The build also writes `package.json` with the Techmo engine identity. Both it and
the native provenance are included in the Windows ZIP; the engine tree must match
the pinned lock before the provenance file is written.

After both builds, `tools/package-windows.ps1` creates a clean ZIP in `dist/`.
It packages runtime files and notices, downloading the fixed Python embeddable
runtime from python.org. `run.bat` prefers `tools/python/python.exe` from the
package, then falls back to the local development toolchain.

## Source layout

- `apps/launcher`: Techmo WPF frontend and input editor.
- `apps/engine-backend`: configuration models and native option catalogs.
- `engine`: upstream engine-tree lock and separate Techmo compatibility patches.
- `scripts`: session reports and mod support.
- `tools`: builds, packaging and benchmarks.
- `tests`: isolated fixtures and regression checks.
- `licenses`: original acknowledgements and third-party notices.

The reference frontend remains under `launcher/windows/` for source comparison.
The active backend is a library; its original window is never launched.

## Verification

```powershell
python -B -m unittest discover -s tests -v
dotnet run --project tests/TechmoHosted/TechmoHosted.csproj -c Release -- .
dotnet run --project tests/TechmoUpdates/TechmoUpdates.csproj -c Release -- .
.\apps\launcher\Verify-Launcher.ps1
```

The hosted fixture checks page bounds, trophies and filters, engine settings,
recommendations, Apply/Undo and input persistence. It uses synthetic data under
`builds/_internal/`. The original interaction suite additionally needs CLANG64
Python; set `BB_MSYS2` if it is outside `C:\msys64`.
`tests/test_pc_input.cpp` checks native modifier and mouse handling against a
bootstrapped engine. CI does not run the game or claim gameplay performance.

The native CI job runs the upstream option-row builder, menu-memory and input fixtures,
plus the Techmo chord/capture and VRAM tests. The title row is now built natively:
no generated title GFX needs to be shipped. The fixture verifies the previous five
rows remain and Exit uses the game's YES/NO callback.

Bootstrap fetches the exact commit and verifies its engine tree hash before import.
Each Techmo patch is checked before the new source replaces the generated checkout.
The previous source remains in an ignored backup. Packaging removes temporary staging;
`tools/clean-generated.ps1 -WhatIf` previews other reproducible output cleanup.

## In-app updates

Settings checks the latest stable release in `TechmoNoway/techmo_bloodborne_pc`.
The full Windows ZIP is verified using GitHub's SHA256 digest or `SHA256SUMS.txt`.
Download and verification happen before any installed file changes. The user then
chooses Save settings, restart & install. An independent Windows PowerShell helper
waits for the launcher to exit, refuses running games/other launcher instances,
and backs up each runtime file before replacing it. A failed copy rolls back the
files already changed. The next launch shows the installation result.

Personal data is excluded from the install plan and independently rejected by the
helper: user/, saves, mods, logs, caches, bbport.ini and launcher-settings.json.
Only the latest successful runtime backup is kept under `.techmo-update/`.
In-place installation is for extracted release packages; Git source checkouts
remain build-managed. Existing 2.0 installations need the updater-enabled build
once before they can receive future updates in the app.

The update suite uses injected HTTP responses and runs the real installer with
locked-file, rollback, checksum, cancellation, path and data-preservation fixtures.
To validate a complete local ZIP too, append `--package <absolute-zip-path>`.

## Benchmark

Use Diagnostics > Start benchmark, or `builds/launcher/Benchmark.cmd`.
Development captures target the v2 game PID and write results under
`../benchmarks/results/<timestamp>-techmo-v2`. Match resolution, quality and
frame-generation settings when comparing builds.
