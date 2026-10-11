# Build Techmo on Windows

Run these commands from the repository root. Keep development output separate
from any installation you are currently using to play.

## Launcher

Install a .NET SDK that supports `net8.0-windows`, then run PowerShell:

```powershell
./tools/build-launcher.ps1
```

Output: `builds/launcher/Techmo.Launcher.exe`, a self-contained Windows
x64 executable. It needs the native engine under the same project root to play.
Double-click `builds/TEST-LAUNCHER.cmd` to open the latest build.
The helper restores packages to `builds/_internal/cache/nuget` and keeps its
.NET working files in `builds/_internal/cache/dotnet-home`. Local backups,
screenshots and diagnostics are grouped under `builds/_internal`; the entire
`builds` directory is ignored by Git.

## Native engine

Install MSYS2 at `C:\msys64`, or set `BB_MSYS2` to its location. Use the
**CLANG64** shell, with Clang/libc++/lld rather than another MSYS2 toolchain.

Alternatively, use the existing MSYS2 shell to prepare an isolated CLANG64
toolchain and build from PowerShell:

```powershell
./tools/build-native.ps1 -Bootstrap -Test
```

Packages stay under `builds/_internal/toolchain`; system MSYS2 packages are not
changed. Next time, use `./tools/build-native.ps1 -Test`. See the
[diagnostics guide](DIAGNOSTICS.md) for session capture and baseline comparison.

```bash
pacman -S --needed git curl mingw-w64-clang-x86_64-{clang,lld,libc++,cmake,ninja,pkgconf,python,sdl3,boost,fmt,glslang,spirv-cross,spirv-headers,vulkan-headers,vulkan-loader,vulkan-memory-allocator,xxhash,zydis,robin-map,ffmpeg,protobuf,nlohmann-json,openssl,miniupnpc}
bash build.sh
```

The script uses `engine/runtime`, configures `engine/renderer`, applies the
retained FSR-Vulkan patches and produces:

```text
out/bb-probe.exe              Native runtime
out/bb-gpu-capabilities.exe   GPU capability helper
out/bb-engine-assets.exe      Local asset preparation helper
out/bb-crash-reporter.exe     Out-of-process Windows minidump writer
out/native-build.json        Compiler, source and binary provenance
out/gpu/libbbgpu.a            Renderer static library
```

These executable/target names remain stable for the inherited runtime interfaces.
The project and launcher name is Techmo Bloodborne. `build.sh` rejects Linux and
non-CLANG64 Windows shells. Dependencies are vendored, so submodule initialization
is not required. CMake may fetch its pinned helper libraries on the first build.

Optional upscaler assets, from the same CLANG64 shell:

```bash
bash tools/fetch_dlss.sh out
bash tools/fetch_fsr4_assets.sh
```

DLSS needs a compatible NVIDIA RTX GPU. The Linux/Proton FSR 4.1.1 asset capture
workflow is removed; the replay provider needs an existing asset set.

To reuse the optional assets from a separate local installation on Windows:

```powershell
./tools/install-upscalers.ps1 -SourceDirectory C:\Games\BloodBorne
```

This copies `out/nvngx_dlss.dll` and the complete 189-file `fsr4_shaders` set
into Techmo, leaving the source installation unchanged. It checks the NVIDIA
signature, FSR shader manifest hashes and SPIR-V headers, preserves licenses,
and records copied file hashes in `out/upscaler-assets.json`. Different existing
destination files are preserved and cause an error. The DLSS license is fetched
from NVIDIA's pinned SDK if the source does not include it.

Select **DLSS** or **FSR 4 v07 (experimental)** in Graphics, then choose Quality
and an output resolution. These are alternative Super Resolution paths; they
do not add frame generation. FSR 4 v07 is the experimental INT8/DOT4 provider,
separate from FSR 4.1.1. Installed files do not guarantee device compatibility:
the renderer checks required Vulkan features or NGX when it starts. When the
selected upscaler cannot run, inspect the session log for the reported reason.
The existing `tools/fsr4_optimize.sh` generates corrected/faster v07 shaders
under `fsr4_shaders/opt`; run it with the native toolchain before testing v07.
Downloaded models and shaders remain local and are excluded from Git.

## Run

Open `builds/TEST-LAUNCHER.cmd` or `builds/launcher/Techmo.Launcher.exe`, select your own CUSA03173
v1.09 game dump, configure settings and save. Alternatively, from Command Prompt:

```bat
run.bat --game-dir D:\Games\CUSA03173
```

`run.bat` uses `scripts/run_windows.py`. `BB_PREBUILT=1` skips building the engine
for a package with existing native binaries. The portable package supplies Python
under `tools/python`; a source run uses MSYS2 Python.

## Verify

The UI verifier uses synthetic game/runtime files outside the project and does
not start the real game:

```powershell
./apps/launcher/Verify-Launcher.ps1 -PreviewDirectory builds/_internal/previews/controls -PackageCache builds/_internal/cache/nuget
python -m unittest discover -s tests -p "test_*.py"
```

`dotnet` must be on PATH for the verifier. Native tests are skipped when their
compiled binaries do not exist. The Windows workflow builds and runs renderer
and input test targets before packaging the launcher.

To additionally validate every captured keyboard name against an existing SDL3
library, pass `-SdlDll PATH_TO_SDL3_DLL` to the launcher verifier. This loads the
library in the isolated test process and reads its key-name lookup; it does not
start the game.
