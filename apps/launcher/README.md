# Control Hub

The native WPF launcher starts on Play. Display owns output resolution, FPS,
presentation mode, borderless output, HDR output conversion and the FPS counter. Graphics separates
upscaling, effects and camera settings. Controls separates keyboard/mouse bindings
from controller selection. Diagnostics contains the latest session report,
recording switches and device/asset details. Settings owns installation, language,
appearance and shared game options.
Saves & mods provides separate save-management and mod tabs.

## Profiles

- The first launch creates **My setup** from the current configuration. Existing
  settings and the runtime INI remain authoritative at startup, including changes
  made through the in-game controls. A difference from the stored profile appears
  as unapplied changes instead of silently replacing the runtime settings.
- **Apply changes / Ctrl+S** saves the selected profile and runtime settings.
  **Save a copy / Ctrl+Shift+S** saves the current choices under a new name.
- **Undo**, next to Apply changes, discards all pending edits across tabs and
  restores the last successfully applied settings, input bindings and selected
  profile. Before the first Apply, it restores the configuration loaded on startup.
  It also restores pending launcher preferences and in-memory account fields.
  Undo does not write settings or modify save data, mods, or profile names/library
  membership. Successful Apply/Play/Save a copy replaces the checkpoint; a failed
  Apply leaves it intact. Settings loaded when the game exits also become the
  checkpoint, preserving adjustments already applied in-game. Undo is disabled
  when there are no pending edits.
- Selecting a profile stages its display, graphics, camera and input settings;
  Apply or Play writes them to the runtime configuration. Unsaved changes prompt
  before switching profiles or closing the launcher.
- The profile menu supports rename and delete. Names are unique without regard
  to case, use 1–60 characters and can contain Unicode. The library supports up to
  50 profiles and always retains at least one profile.
- Profiles include the active key bindings and the remembered Custom input
  preset. Side-specific modifier, mouse and wheel combinations are preserved.
- Installation paths, UI language/theme/motion, online addresses/accounts,
  diagnostic switches, mods, HDR display calibration and shared gameplay unlocks are excluded from profile
  data. Switching profiles preserves in-memory online passwords/tokens too.

The library is local at `user/launcher/profiles.json` and excluded from Git by
the existing `/user/` rule. Version 1 has an active profile ID, stable GUID IDs,
names, an explicit allowlist of settings and per-action bindings. Loading validates
the library, individual setting values and bindings before applying any profile.

Saves write and flush a temporary file in the same directory before atomically
replacing the destination, retaining `profiles.json.bak`. A malformed library can
recover from a valid backup; the damaged bytes are preserved as
`profiles.json.corrupt-<UTC timestamp>`. Unsupported future schema versions are
preserved without downgrade. If neither file is usable, profile controls are
disabled and an error is shown; the existing profile file is not overwritten.
Ordinary launcher settings remain usable.

## Presentation and appearance

Present mode is the sole presentation control. Mailbox, FIFO, FIFO Relaxed and
Immediate have descriptions, remain independent of FPS and retain the existing
runtime launch contract. Off and TAA disable upscaling quality while preserving
the previous selection for DLSS/FSR. The render-size diagram previews the requested
scene dimensions using the runtime's even-dimension rounding; it is not live GPU
telemetry.

Page and render-scale transitions honor both Reduce motion and the Windows
client-area animation setting. The Apply bar appears for pending edits and slides
down out of view after a successful save, returning its space to the settings page.
Save errors keep the bar and retry action visible; session notices can also reveal
the bar without marking settings dirty. Reduced motion collapses it immediately.
Scrolling pages or their scrollbars does not mark settings as changed. Wheel input
over a closed dropdown scrolls its page without changing the selected option;
opened dropdown lists retain their normal scrolling behavior.
Themes use native WPF resources. No browser runtime
or additional renderer dependency is required. The dark palette uses a near-black
background, white accents and translucent panel fills within an opaque window.
Popups remain opaque for readability; no desktop blur or WPF window transparency
is needed.

## Sharpening

Graphics → Upscaling exposes the existing RCAS switch and a **Sharpness** slider
from 0 to 2 (default 0.30). The readout and arrow-key steps use 0.01, Page Up/Down
uses 0.10. Apply writes `sharpen` and `sharpness` to the runtime INI, and each
profile retains its own strength. Old v1 profiles migrate to 0.30. Changes made
through the native menu are read back at startup. Launcher edits apply on the
next game start; the native menu supports live adjustment.

The strength is disabled when sharpening is unchecked or Upscaler is Off, and its
value is retained for TAA/DLSS/FSR. High values can emphasize noise and bright edges.

## Lighting and color

Graphics → Effects groups each effect switch with its label inside a separate
bordered tile. The layout uses two columns, switching to one below 900 logical
pixels so longer English and Vietnamese labels remain readable. The whole tile
is the checkbox hit area, with hover and keyboard-focus feedback.

Graphics → Effects has independent AO strength, shadow distance, bloom, saturation
and vignette controls. The four numeric values use horizontal sliders with a live
percentage readout, one-percent steps, and visible range endpoints. Click the track
or drag the thumb; arrow keys adjust by 1%, Page Up/Down by 10%. Narrow windows
stack them in a single column. Vignette remains an On/Off switch.
There is no separate enable switch. The four multipliers
default to **100% — Original**, and vignette defaults to On, preserving the game's
per-area values. Zero AO/bloom removes the effect; zero saturation is grayscale.
Shadow distance supports 100–300%. AO needs SSAO enabled; shadow distance needs
dynamic shadows enabled.

Apply or Play derives the internal `graphics_controls` startup flag from these
values: any non-original multiplier or vignette Off enables support. Returning
all values to original disables the extra hooks. The flag remains serialized for
v1 profile compatibility; a legacy flag cannot suppress the selected values.
Existing values, including intermediate in-game slider values, are retained.
When overrides enable the native graphics pages, their supported settings can
still be adjusted live in-game. Launcher edits take effect on the next game start.

**Restore defaults** resets only this group and stages the edit until Apply or
Play. It leaves AA/SSAO switches, other effects, upscaling, HDR, camera and input
settings intact.

## In-game performance overlay

Display → In-game overlay has independent FPS, CPU usage and GPU usage switches.
Selected values share the existing corner overlay; CPU/GPU can also be shown with
FPS hidden. The choices are part of each profile. Old v1 profiles default the two
new fields to Off and retain their existing settings. The native menu can toggle
the counters during play; its values are read from the runtime INI when the
launcher returns. Launcher edits take effect at the next game start.

CPU is total system busy time from Windows `Processor Information(_Total)`.
GPU is the busiest engine of the Vulkan adapter selected by its Windows LUID and
node mask. Per-process usage is combined for each engine; independent engines
are not added together. These are system/adapter metrics, not game-process-only
load, and polling intervals/CPU definitions can differ from other monitoring tools.

Windows PDH collection runs on a lazy worker at below-normal priority, roughly
every 500 ms. Drawing reads cached atomic values; it never enumerates counters or
waits for a sample. With CPU/GPU disabled no PDH query is collected, and the worker
is not created until needed. Missing, invalid, warming-up or stale samples display
`N/A`, distinct from a valid 0%. Counters close on disable and at normal shutdown.
No vendor monitoring SDK or external overlay program is required. Performance
impact has not been benchmarked in gameplay.

References: [Windows PDH counters](https://learn.microsoft.com/en-us/windows/win32/api/pdh/nf-pdh-pdhaddenglishcounterw)
and [Microsoft GPU utilization definition](https://devblogs.microsoft.com/directx/gpus-in-the-task-manager/).

## Mods

Saves & mods → Mods lists loose-file mods with individual switches, file counts,
load order and shared-file previews. Select a named mod and use **Move up/down**;
the lower enabled mod wins an exact file overlap. Conflicts compare paths without
regard to case, as the Windows loader does. The preview lists up to 200 shared
paths and names each winner; it does not merge mod contents or establish gameplay
compatibility. Master Off suppresses all mod layers and effective conflicts.

Use **Open Mods Folder**, extract each mod into its own subfolder, and Refresh.
The accepted layout is `mods/<name>/dvdroot_ps4/...`; `app0/`, `CUSA03173/`, one
archive wrapper, and bare game folders such as `chr/` or `parts/` are recognized by
the same `scripts/mods.py` code used at game startup. Archives and unsupported file
types appear with instructions instead of silently looking installed. ZIP/RAR/7z
are not extracted automatically; DLL injectors and XML executable patches are not
loose-file mods. Executable/system replacements and mod links/junctions are rejected.

The legacy `<game>-mods` sibling folder and direct `mods/dvdroot_ps4` layout remain
supported, with individual switches and fixed priority before named mods. When a
direct root overlay is present, named subfolders are not separately loaded, matching
the existing runtime convention. Configuring named mods in separate folders is
recommended when independent load order is needed.

Changes are staged until **Apply/Play**, and **Undo** restores their last applied
state. Mod selection is global and stays outside gameplay profiles. `mods.json`
stores `order`, `disabled`, `legacy_enabled` and `direct_enabled`; old configurations
default both overlay switches to On. Unknown configuration fields are preserved.
Writes are atomic with a `.bak` copy. Invalid config bytes are preserved and editing
is disabled; the master switch can turn mods off to launch without reading that
config. Active invalid mods must be switched off before Apply.

Scanning is read-only, runs through the bundled Python in a background process,
and does not hash/read asset contents. It caps the catalog at 200,000 files/10,000
entries, output at 32 MiB, and scanner execution at 60 seconds. Refresh retains
pending toggles/order and newly discovered folders are enabled by default. Selecting,
scrolling and localizing rows do not stage edits. The launcher explicitly passes
`BB_MODS_DIR` and `BB_MODS_CONFIG` to startup so the displayed paths and runtime agree.

At game startup the loader constructs a temporary linked overlay and mounts it as
`/app0`. It removes that view at exit without replacing the source game or mod files.
Tests cover synthetic assets and real loader configuration round trips; individual
mods still need gameplay testing with the target game version.

## Save data

The save tab discovers `user/savedata/<user>/<CUSA title>` (or `BB_USER_DIR` when
configured), including the paired `<title>.memory` folder and `.sce_sys` metadata.
Backups are local ZIPs under `user/save-backups`, outside the live save tree.
Create backup, restore, import/export and folder shortcuts are explicit actions;
opening the tab only reads directory information.

Backup and restore require the game to be closed. Restore checks the manifest,
entry names, sizes and SHA-256 hashes before changing live files, then backs up
existing data and replaces the title and memory directories using staged renames.
An ordinary I/O failure rolls those renames back. A power loss is not an atomic
transaction across both directories: the separate safety ZIP remains available
for recovery. Failed rollback retains its recovery folder and reports its path.
The launcher blocks Play and closing while an operation is in progress.

Only Techmo backup ZIPs are supported for import; this is not a save editor or a
converter for encrypted console saves. Imports reject traversal, Windows device
names, links/junctions, duplicate or extra entries and inconsistent data. Limits:
4,096 files, 2 GiB uncompressed total and a 1 MiB manifest. Backups are never pruned
automatically. All tests use isolated synthetic saves.

## HDR output (experimental)

Display offers **SDR → HDR10 output**, reference white (80–400 nits) and optional
highlight expansion with a 400–2,000 nit target. These settings are shared across
profiles because they calibrate the display. They take effect at game startup;
HDR defaults to Off and expansion defaults to Off.

This maps the final SDR display image into Rec.2020 / ST.2084; it does not restore
scene information already clipped by the game's SDR rendering. Expansion is an
artistic luminance curve above 75% linear luminance. The game's baked-in HUD is
part of that image; the Techmo overlay is separately mapped to reference white
with no highlight expansion. Overlay alpha blending still occurs in PQ space.

Enable Windows HDR on a compatible display before launching. The renderer selects
10-bit HDR10 only when Vulkan advertises the required surface format/color space;
otherwise it retains SDR and logs the fallback. The conversion uses the existing
post-process pass with no new full-screen image allocation. It has not yet been
visually validated on an HDR display. There is no claim of native game HDR or a
measured performance gain.

Launch variables: `BB_HDR_OUTPUT=sdr|off`, `BB_HDR_WHITE=203`,
`BB_HDR_PEAK=1000`, `BB_HDR_EXPAND=0`. The launcher explicitly overrides inherited
activation flags. The existing guest-native `BB_HDR` path is separate.
Encoding references: [Vulkan color spaces](https://docs.vulkan.org/refpages/latest/refpages/source/VkColorSpaceKHR.html)
and [Microsoft Advanced Color](https://learn.microsoft.com/en-us/windows/win32/direct3darticles/high-dynamic-range).

## Build and verify

From the project root:

```powershell
.\tools\build-launcher.ps1
.\apps\launcher\Verify-Launcher.ps1 -PreviewDirectory launcher-previews
```

The launcher is published to `builds/launcher/Techmo.Launcher.exe`. Verification
uses a separate temporary application with synthetic game/runtime files; it
checks profiles, corruption recovery, failed atomic replacements, input capture,
English/Vietnamese, appearance, navigation, launch arguments and session reports.
It also renders native screenshots for layout review. It does not run the user's
game or read/write their saved settings.

The native build runs CPU HDR transfer tests via `tools/test-shader-correctness.sh`.
`ninja -C out/gpu hdr-shader-test` builds a headless GPU readback fixture; run
`out/gpu/hdr-shader-test.exe` locally on a Vulkan GPU. It exercises the actual
post-process pass with SDR, 10-bit HDR, primary colors, manual sRGB decoding and
highlight expansion. This does not validate a monitor's HDR presentation.

`performance-monitor-test` covers counter names, adapter filtering, engine
aggregation, missing values and overlay combinations. Its optional `--live` mode
samples Windows CPU counters. `performance-monitor-gpu-test` is a local WDDM/Vulkan
smoke test using a small synthetic transfer workload; it requires working Windows
GPU counters and is compiled, but not run, on hosted CI.
