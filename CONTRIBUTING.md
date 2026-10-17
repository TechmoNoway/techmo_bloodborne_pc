# Contributing to Techmo

Keep changes focused on a behavior that can be checked. A useful change explains
the problem, what the user sees afterward, and how that result was verified.

## Work locally

Build the launcher with `./tools/build-launcher.ps1` and the native engine with
`./tools/build-native.ps1 -Test`. Use synthetic fixtures for launcher and preparation
tests. Keep game dumps, saves, credentials and captured game data outside version
control. Development output belongs in `builds` or `out`.

For localization changes, keep the English/Vietnamese catalog keys and format
placeholders identical. The launcher defaults to English. Preserve configuration
IDs and input key names when translating labels.

For renderer work, compare the same scene, resolution, driver and settings.
Report frame times and the test conditions with performance claims. UI changes
alone do not establish an FPS improvement.

## Preserve provenance

Keep file-level SPDX/copyright notices and dependency licenses. Record vendor
updates in `DEPENDENCIES.md`, including the source revision and local patches.
Avoid replacing upstream authorship when changing a file or renaming the project.

## Before sharing

Run the checks relevant to your changes, explain any skipped checks, and include
the corresponding source and notices when distributing a compiled build.
