<div align="center">

# Techmo Bloodborne

**A Windows home for Bloodborne. Built to be understood, tuned and improved.**

Windows x64 · Vulkan · C# / WPF · English + Tiếng Việt

[Build](docs/guides/BUILD.md) · [Project map](docs/ARCHITECTURE.md) · [Contribute](CONTRIBUTING.md) · [Tiếng Việt](README.vi.md)

</div>

Techmo brings the launcher, native runtime and renderer into one Windows-focused
workspace. Pick your game folder, configure controls and graphics, and keep your
settings in one place. The interface starts in English and can switch to Vietnamese.

The **Hunter’s Desk** interface pairs game artwork with launch controls, quick
settings and expandable system details. The language selector stays available
on every page, and smaller windows use a scrolling layout.

![Techmo Bloodborne launcher](docs/assets/launcher.png)

## What you can configure

- Click-to-bind keyboard/mouse controls with Shift/Ctrl/Alt combinations, Default / Dark Souls 3 / Custom presets
  and controller selection.
- Render/output resolution, 30 / 60 / 90 FPS presets and available upscalers.
- Mods, community patches and optional shadNet connection settings.
- English or Vietnamese launcher UI, independent of the game's language.
- Session exit reports, optional frame/GPU CSV recordings and native minidumps.
- **Quit Game** on the title screen, with the game's own YES/NO confirmation.
  It uses the window-close path and saves completed pipeline-cache work before exiting.

For native builds and repeatable measurements, see the
[diagnostics guide](docs/guides/DIAGNOSTICS.md). Recorded whole-session statistics
include loading screens and menus; use the same route for comparisons.

In **Controls**, click a keyboard or mouse slot and press the desired input.
Esc cancels, **×** clears one slot, and **Save settings** keeps the changes.
Custom bindings are retained when switching presets. For `Left Shift + Mouse1`,
click the mouse slot, hold Left Shift and click the left mouse button. The same
capture supports multiple side-specific modifiers, keyboard keys and the wheel.
The new combination syntax requires an engine rebuilt from this source.

<details>
<summary>Preview the Controls page</summary>

![Click-to-bind controls](docs/assets/controls.png)

![Keyboard and mouse combinations](docs/assets/controls-chords.png)

</details>

Available graphics features depend on the GPU, driver and installed assets.
Restructuring the project or changing the launcher does not by itself raise FPS.

## License and credits

Techmo's combined distribution uses **GPL-3.0-or-later**, consistent with the
inherited bbhost ports. Original source notices and dependency licenses remain
in place; GPL-2.0-or-later components retain their provenance.
Read [LICENSE](LICENSE), [LICENSING.md](LICENSING.md) and [NOTICE.md](NOTICE.md).

Bloodborne is a game by FromSoftware and Sony. Techmo is an independent project
and is not affiliated with or endorsed by either company.
