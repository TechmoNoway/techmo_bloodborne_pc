# Windows guest red-zone protection

Implemented on 2026-10-14. Enabled by default when Techmo prepares the game.

Bloodborne uses the System V x86-64 ABI, including temporary data in the 128
bytes below RSP. Windows does not reserve that area across exceptions. A resumed
write fault on a GPU-tracked page can therefore damage guest locals on affected
CPU/Windows configurations. The tester's two access-violation dumps read a null
pointer at guest offset `0x28cf0d6`, after loading it from `[rsp-0x60]`. The dumps
support investigating red-zone corruption; they do not prove which earlier
write fault caused it.

## Implementation

`scripts/red_zone.py` builds trampolines for the pinned Bloodborne 1.09 store
sites in `scripts/red_zone_sites.json`. Each protected instruction executes
between `lea rsp,[rsp-128]` and `lea rsp,[rsp+128]`. LEA preserves CPU flags;
no extra general-purpose register is consumed. Copied stack loads execute with
the original RSP. The detour returns immediately after the replaced span.

`link_modules.py` places the trampolines in an additional read/execute segment
of the generated `boot-linked.bin`. It checks the table hash, instruction span
structure, original bytes, executable boundaries and overlaps with relocations
and selected startup patches before changing the private image. Original game
files are never patched on disk. `run_windows.py` compiles the selected patches
before linking so external patches can be included in these overlap checks.

The exact table is credited in [the provenance notice](../../licenses/WINDOWS-RED-ZONE.md).
It analyzes 153 leaf functions: 607 spans cover stores in 151 of them; 42 short
stores remain uncovered. Functions with unknown jump-table destinations or
community patches are excluded. This is store-fault protection for reviewed
sites, not complete instrumentation of every guest read, write or exception.
Modified game code or custom patches can reduce the effective coverage.

## Diagnostics and comparison

The startup log reports applied sites, protected stores and conflicts. The
generated `out/link.json` records skipped addresses and changed ranges. Each
new `logs/<session>/session.json` also records its own `red_zone` summary
after a successful linker step, including the table hash and coverage.

For a controlled comparison, close Techmo and run from PowerShell:

```powershell
$env:BB_RED_ZONE = '0'
& .\builds\TEST-LAUNCHER.cmd
```

Restore the default for subsequent launches:

```powershell
Remove-Item Env:BB_RED_ZONE -ErrorAction SilentlyContinue
& .\builds\TEST-LAUNCHER.cmd
```

Keep resolution, upscaler, frame cap, mods and route identical. Changing this
switch rebuilds the private guest image on the next launch, including portable
launches using `BB_PREBUILT=1`. The native executable itself does not change for
this feature: the preparation scripts emit different guest code.

## Validation and remaining acceptance

- All 607 original byte spans match the local unmodified 1.09 executable.
  A developer Capstone check decoded every instruction and regenerated all
  151 functions containing emitted spans, checking branch boundaries,
  stack-pointer changes, relative operands and store classification.
- All 3,516 write ranges in the bundled community XML were checked for overlap.
  The selected 90 FPS and graphics patches produced zero detour conflicts.
- The prepared image protected 1,736 stores. It added a 47,680-byte executable
  segment and preserved module metadata, TLS, imports, relocations and every
  existing image byte outside the detour spans.
- The real native loader executed a synthetic protected guest image. A Windows
  VEH test resumed 100 protected write faults with live AVX state and preserved
  the sentinel local, RSP, flags and the destination value.
- The 143 Python regression tests passed. The local AMD control did not
  reproduce corruption, so these checks cannot establish that the tester's
  Intel-specific gameplay crash is fixed.
- Native crash reporter, input, clocks, runtime, mod-file access, semaphore and
  content fixtures passed. Vulkan startup and the four-binary/146-DLL package
  manifest were verified.

Gameplay acceptance still requires the tester's Intel/RTX 3050 Ti machine:
repeat the Blood-starved Beast death/effect sequence and combat after reloading,
then retain the entire new session logs and any dump. Compare frame times on
the same warmed route. This correctness change does not resolve the separately
observed shader-compilation hitch and does not establish an FPS improvement.
