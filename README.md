<div align="center">

# Techmo Bloodborne

**A Windows home for Bloodborne. Built to be understood, tuned and improved.**

Windows x64 · Vulkan · C# / WPF · English + Tiếng Việt

[Build launcher](tools/build-launcher.ps1) · [Build engine](tools/build-native.ps1) · [Contribute](CONTRIBUTING.md)

</div>

Techmo brings the launcher, native runtime and renderer into one Windows-focused
workspace. Pick your game folder, configure controls and graphics, and keep your
settings in one place. The interface starts in English and can switch to Vietnamese.

The **Control Hub** interface separates Play, Display, Graphics, Controls, Saves & mods,
Online, Diagnostics and Settings. Play focuses on launching the game; graphics
settings live in Upscaling, Effects and Camera tabs. Choose English or Vietnamese,
a dark/light/system theme and reduced motion in Settings.

## What you can configure

- Click-to-bind keyboard/mouse controls with Shift/Ctrl/Alt combinations, Default / Dark Souls 3 / Custom presets
  and controller selection.
- Output resolution, 30 / 60 / 90 FPS or experimental Unlimited, present mode and available upscalers.
- Named display/graphics/input profiles with Save a copy, rename and delete.
- Save backups, restore with a safety backup, and Techmo backup ZIP import/export.
- Experimental SDR-to-HDR10 output conversion with display brightness controls.
- Mods, community patches and optional shadNet connection settings.
- English or Vietnamese launcher UI, independent of the game's language.
- Session exit reports, optional frame/GPU CSV recordings and native minidumps.
- Optional in-game FPS, CPU usage and active GPU usage overlay counters.
- **Quit Game** on the title screen, with the game's own YES/NO confirmation.
  It uses the window-close path and saves completed pipeline-cache work before exiting.

Recorded whole-session statistics
include loading screens and menus; use the same route for comparisons.

In **Controls**, click a keyboard or mouse slot and press the desired input.
Esc cancels, **×** clears one slot, and **Apply changes** keeps the changes.
Custom bindings are retained when switching presets. For `Left Shift + Mouse1`,
click the mouse slot, hold Left Shift and click the left mouse button. The same
capture supports multiple side-specific modifiers, keyboard keys and the wheel.
The new combination syntax requires an engine rebuilt from this source.

The profile selector is available on every page. **Apply changes** (`Ctrl+S`)
saves the current setup to the selected profile and runtime configuration.
**Save a copy** (`Ctrl+Shift+S`) creates a new named profile. Switching profiles
stages their settings until Apply or Play; unsaved changes prompt before switching
or closing. The first run imports the current configuration as **My setup**.
Profiles stay local in `user/launcher/profiles.json`, with atomic replacement and
a previous-file backup. Game paths, launcher preferences and online accounts are
shared and excluded from profile data. See [launcher details](apps/launcher/README.md).

**Display → Present mode** is the single presentation choice: Mailbox, FIFO,
FIFO Relaxed or Immediate. There is no separate VSync checkbox. Each mode has
an explanation; the selection stays independent of the FPS limit.

Available graphics features depend on the GPU, driver and installed assets.
Restructuring the project or changing the launcher does not by itself raise FPS.

## Renderer measurements

Frame recordings separate image hashing, snapshot copies, detile/upload command
setup and unchanged mip bytes. These CPU timings can overlap; they are not GPU
transfer times. Known compute selections are queued during command scanning;
first-time shader translation can still require a wait.

Graphics lookahead now attempts up to 16 ordinary direct draws per scanned buffer
before publishing it to preparation workers. `BB_GRAPHICS_SCAN=0` disables that
pass for comparisons. Only known shader modules/permutations are eligible; new
shader translation and required draws keep their correctness fallback. Pipeline
log timelines separate queue delay, native compilation and lead time; these overlap
foreground waits and must not be added together. Startup/shutdown log the compatible
native cache seed, warm-up record counts and saved payload size.

Retired CPU-backed texture allocations may be reused for up to one second, with
exact image-create compatibility and fresh layout/hash/guest state. The recycler
keeps at most 16 MiB by default (also capped to 1/256 of the reported driver budget),
is disabled above 75% budget usage or without a usable budget, and drains under
memory pressure. `BB_IMAGE_RECYCLE_MB=0` disables retention; explicit values are
capped at 64 MiB. GPU-modified surfaces and video/scene proxies are excluded.
The retained bytes remain part of driver/image usage, not extra free memory.
Frame logs include allocation, image-view and format-query CPU scopes and reuse
counts; VRAM logs expose retained bytes. Bounded texture samples identify repeated
refreshes and slow creations. They do not weaken dirty tracking or enable parallel
hashing automatically. Gameplay p99 improvements require matched cold/warm captures.

`BB_DETILE_SCRATCH_REUSE=0`, `BB_KEEP_MIP_HASHES=0` and `BB_COMPUTE_SCAN=0`
disable individual changes for comparisons. `BB_PARALLEL_IMAGE_HASH=1` enables
the optional owned-snapshot hash experiment; it is off by default because its
copy/task cost can outweigh parallel hashing. Run `tools/test-tiled-writeback.sh`
in MSYS2 CLANG64 for GPU upload/lifetime and mip-hash regressions.

## License and credits

Techmo's combined distribution uses **GPL-3.0-or-later**, consistent with the
inherited bbhost ports. Original source notices and dependency licenses remain
in place; GPL-2.0-or-later components retain their provenance.
Read [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and [NOTICE.md](NOTICE.md).

Bloodborne is a game by FromSoftware and Sony. Techmo is an independent project
and is not affiliated with or endorsed by either company.

