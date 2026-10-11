# Project map

Techmo separates the Windows interface from the native engine and preparation
pipeline. The launcher owns user interaction; the runtime and renderer own game
execution and GPU work.

```text
apps/launcher
  └─ run.bat → scripts/run_windows.py
       ├─ validate / prepare / link / patch / mount mods
       └─ out/bb-probe.exe
            ├─ engine/runtime   (loader, HLE, Win32 memory/threads)
            └─ engine/renderer  (Vulkan, shaders, input, engine hooks)
```

## Source boundaries

| Area | Responsibility |
| --- | --- |
| `apps/launcher` | WPF views, settings, online credentials, input table and localization |
| `engine/runtime` | Game image loading, runtime services, guest ABI and Win32 platform code |
| `engine/renderer` | Vulkan renderer, shader recompiler, SDL input, engine hooks and online bridge |
| `scripts` | Game preparation, patches, mods and the Windows process launch sequence |
| `tools` | Development builds, setup, diagnostics and shader/asset utilities |
| `tests` | Synthetic fixtures and native regression executables |
| `third_party` | Shared dependencies used outside the renderer |
| `docs/archive/upstream` | Historical upstream technical notes; not current build instructions |

The renderer's internal directories remain close to their upstream snapshots.
That keeps vendor updates and patches easier to compare. Top-level source paths,
build definitions, CI and owned source includes follow the new Techmo layout.

## State and output

`builds` holds ignored local launcher builds, previews and caches. `out` holds
native binaries and preparation output. Runtime settings stay in `bbport.ini`;
launcher preferences stay in `launcher-settings.json`. Saves and protected account
data live under `user`. Those paths are ignored by Git.

The launcher locates the root using `run.bat` and `build.sh`, so a published build
inside this repository keeps its configuration separate from another installation.
The internal `BB_*` variables, `bbport.ini`, source namespaces and native target
names are compatibility interfaces, rather than public project branding.
