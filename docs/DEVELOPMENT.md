# Techmo launcher changes

The development copy lives under `techmo_launcher`. Published Windows launcher builds go to `builds/launcher`; double-click `builds/TEST-LAUNCHER.cmd` to test the latest version. Caches, backups, screenshots and diagnostics are grouped under `builds/_internal`. The existing launcher and game runtime in the parent folder are kept separate.

## Launcher languages

- English (`en`) is the default, regardless of the Windows UI language.
- Vietnamese (`vi`) is the only other supported launcher UI language.
- Select **Launcher language** in the window header, then **Save settings** to remember the choice.
- Language changes apply immediately to navigation, controls, key-binding action names, accessibility labels and status messages. Unsaved key bindings and graphics selections are preserved.
- The UI preference is stored as `UiLanguage` in `launcher-settings.json`. Unsupported or missing preferences fall back to English.
- The game's numeric `Language` setting is separate. Existing game-language settings are preserved; this change does not translate Bloodborne's dialogue or assets.

Translations live in `apps/launcher/Localization/en.json` and `vi.json`, embedded in the executable. Keep their keys and format placeholders identical. XAML uses `LocExtension`; runtime messages use `L10n`. Configuration IDs, SDL key names, network protocol values and executable paths remain stable.

## Next improvements, in order

1. **Startup checks and reliable saves.** Check that the game runtime and required files exist before enabling Play. Validate all settings before writing files; use temporary files and atomic replacement to avoid partially saved configurations. The current save path writes JSON before key-binding and online validation finishes.
2. **Input reliability.** Exercise native keyboard/mouse controls with W + Space, releases in both orders, focus changes and menus. Verify that a released movement key always produces a neutral movement axis. Test without a second keyboard-to-controller mapper active.
3. **GPU detection and useful presets.** Cache GPU detection and add a timeout to the capability helper. Filter graphics choices by actual device support, then provide measured Performance / Balanced / Quality presets for the RTX 4060 Laptop GPU.
4. **Performance measurements.** Add repeatable benchmarks with frame-time graphs, 1% lows and CPU/GPU timing. Compare the same route and settings before changing renderer threading, shader caches, upscaling or adding new GPU features. Launcher UI changes alone do not establish an FPS improvement.

## Verification

`apps/launcher/Verify-Launcher.ps1` uses synthetic game/runtime files in a temporary directory and renders an off-screen window. It checks language selection, persistence, English fallback, translation, preservation of user settings and existing launcher behavior. It does not start the real game.

The engine still needs its own Windows build before the isolated launcher can be used to play. See [the build guide](guides/BUILD.md).

## Windows-only source

The GTK4 launcher (`launcher/`), Nix/AppImage packaging (`packaging/`, `shell.nix`),
Linux entry point (`run.sh`), Proton capture scripts and Linux desktop profiling
tools have been removed. Five Linux-only dma-buf/SIGTRAP experiment implementations
were removed; their headers remain because the existing Win32 stubs implement
the same interfaces for the shared renderer.

`build.sh` remains the Windows MSYS2 CLANG64 engine build, and now rejects other
hosts or shells. GPU CMake rejects non-Windows builds, removes X11 dependencies
and builds a static library only. DLSS downloads now select the Windows DLL only.
The mod launch regression now exercises `scripts/run_windows.py` with synthetic
files and a mocked runtime, replacing its former `run.sh` dependency.

Shared runtime platform guards and third-party sources are preserved for
compatibility. Historical upstream documents remain reference material. The
FSR 4.1.1 replay provider is kept, but Linux/Proton capture automation is removed.

Removed files and the previous versions of edited files are recoverable from
`builds/_internal/backups/linux-removal/linux-removal-backup.zip`. The removal target list is recorded in
`builds/_internal/backups/linux-removal/linux-removal-manifest.txt`. Both files stay local and are ignored by Git.

Verification after removal: 74 Python tests passed; 28 tests requiring compiled
native binaries were skipped. The WPF verifier passed all 215 assertions. Bash
syntax checks passed for the retained engine/shader scripts, and the build
rejected a simulated Linux host and a non-CLANG64 Windows shell before writing
build output. The original launcher, runtime and configuration hashes matched
the baseline. A full Windows engine compilation has not been performed.

## Techmo project identity

The WPF project is now `apps/launcher/Techmo.Launcher.csproj` and publishes
`Techmo.Launcher.exe`. The source paths are `engine/runtime` and `engine/renderer`;
owned include references, CMake, build scripts and Windows CI follow these paths.
The native executable names and `BB_*`/`bbport.ini` interfaces remain compatible.

Dependencies are complete vendored snapshots with provenance in
`../DEPENDENCIES.md`. The old local Git metadata was absent, so a fresh `main`
repository was initialized without a remote or an initial commit. Historical
notes live in `archive/upstream`.

After restructuring, the verifier includes two Techmo identity checks (217 total).
The Python checks cover preparation and the Windows launch/mod lifecycle. Native
compilation and real game testing remain pending; launcher/UI results do not
establish a measured performance improvement.

## Hunter’s Desk interface

The warm dark palette, compact navigation and two-column home replace the
previous full-window artwork and ornamental frames. Home shows game version,
folder, runtime and the current input mode. Graphics configuration is one click
away, all five quick settings remain linked to their detailed controls, and GPU
and upscaler status is available in expandable system details.

The language selector is shared by every page. Home stacks its poster and launch
panel below 1050 DIPs; quick settings wrap to three or two columns as space
decreases. Navigation and page content scroll independently. The window still
supports moving, resizing, maximizing and keyboard focus indicators.

The initial Hunter's Desk verifier passed 239 assertions, including global language access,
input-summary updates, graphics navigation, capability details and access to
Play/quick controls at 1000×680, 900×520 with 200% rendering, and 640×480 in
Vietnamese. A synthetic launch also verifies that closing the game returns the
launcher and re-enables Play; the exit handler refreshes status after restoring
settings. No real game is launched by these checks.

The pre-redesign XAML, theme, presentation code, verifier and language catalogs
are saved locally in `builds/_internal/backups/ui`. Preview images are in
`builds/_internal/previews/hunters-desk`. Both directories are ignored by Git.

## Click-to-bind Controls

The Controls page groups all 29 supported actions into Movement, Combat, Camera
and Items & menus. Its read-only table has independent keyboard and mouse slots:
click a slot, then press a key, one of five mouse buttons, or turn the wheel.
The window consumes captured input, Esc cancels, and losing focus cancels without
altering the binding. An explicit Assign Esc command allows rebinding menu keys.
Clearing one slot preserves the other. Labels and prompts switch between English
and Vietnamese without rewriting the serialized input names.

Default restores the inherited engine bindings. The Dark Souls 3 adaptation adds
middle-click lock-on and wheel-down item cycling, keeping C for weapon transform.
Custom remembers personal edits across preset switches and restarts, stored in
the local launcher JSON. Active bindings remain in `bbport.ini` for compatibility
with the engine's own key-binding menu. Shared inputs produce an informational
warning without silently removing another action's input.

Each action has a keyboard alternative and a mouse alternative. Each alternative
can prefix its primary input with left/right Shift, Ctrl and Alt modifiers.
Strong attacks also support the separate Strong attack modifier action. Comma cannot be captured because
the engine uses it as its binding separator; Windows shortcut keys are not offered.
Keyboard token mapping follows [SDL's scancode names](https://github.com/libsdl-org/SDL/blob/main/src/events/SDL_keymap.c).

Current verification passed 485 assertions with `-SdlDll`, including actual WPF
button handlers, cancellation, side buttons, both wheel directions, keypad Enter,
slot preservation, preset persistence, duplicate warnings and small/high-DPI
dialogs. English/Vietnamese catalogs contain 308 matching keys. These checks use
synthetic fixtures and read SDL's name lookup; real game behavior still requires
the native engine build and in-game testing.

Backups are in `builds/_internal/backups/controls`, including the previous launcher executable.
Screenshots are in `builds/_internal/previews/controls`. The build remains isolated from the
existing installation in the parent directory.

## Modifier combinations

To bind `Left Shift + Mouse1`, click the mouse slot, hold Left Shift, then click
the left mouse button. Keyboard capture also accepts combinations such as
`Left Ctrl + V`. Press and release a modifier in the keyboard slot to assign it
alone. Esc cancels; the explicit Assign Esc button can include held modifiers.
Releasing modifiers while waiting for a mouse input removes them from capture.
Cancellation and focus loss clear the capture's modifier state.

INI values use ` + ` between required components and commas between alternatives:
`bind.strong_attack=V, Left Shift + Mouse1`. The spaces around the separator
distinguish a combination from SDL's `Keypad +` key name. Modifiers retain their
left/right identity; multiple modifiers are supported. Custom profiles and both
language displays preserve the complete combination.

The engine evaluates keyboard and mouse in the same pad sample. The most specific
matching alternative wins among bindings of the same primary input; Shift+Mouse1
can trigger Strong attack while unmodified Mouse1 still triggers Attack. Equally
specific matching alternatives remain shared. Additional held modifiers are
allowed. A modifier assigned separately to another action still triggers that
action: for example Left Ctrl retains its default Left stick click binding.
Clear or rebind that standalone slot if the extra action is unwanted.

Both required components must be down. Releasing either removes that alternative;
any remaining simpler binding becomes active from the current state. Mouse wheel
combinations remain short pulses, and focus loss clears mouse buttons and pending
edges. The existing strong-attack modifier behavior remains available.

The native `pc-input-test` and `pc-input-bridge-test` pass locally against SDL3.
The bridge test runs the actual SDL event/pad adapter with synthetic states and
stubbed game/renderer hooks, covering release order, W+Space, walk transitions,
focus, wheel pulses, menu handling, gamepad preservation and capture blocking.
Windows CI builds and runs both targets. The launcher verifier covers capture,
cancel/focus behavior, persistence, preset switching, keypad plus and 640×480
Vietnamese dialogs. A full Techmo engine build and real game testing remain
pending. Existing native binaries must be rebuilt to understand the new syntax;
the original installation has not been modified.

## Native baseline and diagnostics

The local Windows native engine has now been compiled with the isolated
CLANG64 toolchain under `builds/_internal/toolchain`. Native executables and
their DLL import closure are in `out/`; `native-build.json` identifies the
compiler, dependencies and exact binaries. Real game stability and performance
on the target GPUs still need in-game measurements.

Sessions now have separate logs, outcome reports and optional frame/GPU CSVs.
The launcher reads the native outcome after process exit, distinguishes startup
errors from crashes and offers a report-folder button in English/Vietnamese.
Missing native binaries disable Play. A prestarted external crash reporter writes
minidumps for fatal native exceptions; its failure status is also recorded.

The current WPF verifier passes 503 assertions. The full Python suite passes 114
tests with no skips using the compiled native binaries. Native runtime fixtures
and 13 renderer regression targets pass; Vulkan submission/readback passes on
the local RTX 4060 Laptop GPU. These results supersede the older native-build
pending notes above. See [the diagnostics guide](guides/DIAGNOSTICS.md) for
recording instructions, definitions and limits of the performance measurements.
