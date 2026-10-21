# Techmo Bloodborne

Windows x64 build on the `experiment-v2` branch, with the Techmo Control Hub,
keyboard/mouse controls and gameplay performance recording.

## Features

- English/Vietnamese interface, themes and profiles.
- Separate keyboard/mouse bindings, modifier combinations and wheel input.
- Display, upscaling, effects, camera and engine settings.
- Hardware-based graphics recommendations with preview and Apply/Undo.
- Trophy tracking, save backups, mods and online settings.
- Title-menu Exit Game row with the game's YES/NO confirmation.
- Session reports and PresentMon benchmark captures.

## Download

Open [Windows build](https://github.com/TechmoNoway/techmo_bloodborne_pc/actions/workflows/build-windows.yml?query=branch%3Aexperiment-v2),
select a successful run and download **Techmo-experiment-v2-win-x64** from Artifacts.
Extract the ZIP and run `Techmo.cmd`. The package includes the launcher, native
engine and Python runtime. **Techmo-launcher-win-x64** contains only the frontend
for updating an existing checkout.

No game files are included. Select your own decrypted Bloodborne game folder
with update 1.09, then press Play. Optional upscaler assets are provided separately;
FSR 3 is available in the native engine.

## Development

See [Build and verification](docs/BUILD.md) for local builds, GitHub Actions and
test commands. See [Control Hub](apps/launcher/README.md) for the interface.

## Licenses

Techmo additions retain their GPL-3.0 notices. The inherited frontend retains
[its GPL-2.0-or-later license](LICENSE). [Source acknowledgements](licenses/SOURCE-NOTICES.md)
and [third-party notices](licenses/Techmo-THIRD_PARTY-NOTICES.txt) identify original
projects, authors and dependencies. `engine.lock.json` records the pinned sources.
