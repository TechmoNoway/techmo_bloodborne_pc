param()
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$lock = Get-Content -LiteralPath (Join-Path $root 'engine.lock.json') -Raw | ConvertFrom-Json
$native = Join-Path $root 'engine\native'
if (!(Test-Path -LiteralPath (Join-Path $native '.git'))) {
    if (Test-Path -LiteralPath $native) { throw 'engine/native already exists without Git metadata; preserve it and choose a clean directory.' }
    git clone --depth 1 --branch $lock.base_tag --no-recurse-submodules $lock.base_repository $native
    if ($LASTEXITCODE) { throw 'Native engine clone failed.' }
}
$head = (git -C $native rev-parse HEAD).Trim()
if ($head -ne $lock.base_commit) { throw "Wrong native base: $head" }
$patch = Join-Path $root $lock.patch
$compatibility = Join-Path $root 'engine\techmo-compat.patch'
$menu = Join-Path $root 'engine\techmo-menu.patch'
$vram = Join-Path $root 'engine\techmo-vram.patch'
git -C $native apply --reverse --check $vram 2>$null
if ($LASTEXITCODE -eq 0) {
    git -C $native apply --reverse $vram
    if ($LASTEXITCODE) { throw 'Could not validate the Techmo VRAM patch.' }
}
git -C $native apply --reverse --check $menu 2>$null
if ($LASTEXITCODE -eq 0) {
    git -C $native apply --reverse $menu
    if ($LASTEXITCODE) { throw 'Could not validate the Techmo title menu patch.' }
}
git -C $native apply --reverse --check $compatibility 2>$null
if ($LASTEXITCODE -eq 0) {
    git -C $native apply --reverse $compatibility
    if ($LASTEXITCODE) { throw 'Could not validate the Techmo input compatibility patch.' }
}
try {
git -C $native apply --reverse --check $patch 2>$null
if ($LASTEXITCODE) {
    git -C $native apply --check $patch
    if ($LASTEXITCODE) { throw 'Native source differs from the pinned base; refusing to overwrite local changes.' }
    git -C $native apply $patch
    if ($LASTEXITCODE) { throw 'Native engine patch failed.' }
}
} finally {
    git -C $native apply --check $compatibility
    if ($LASTEXITCODE) { throw 'Native source differs from the expected Techmo compatibility patch.' }
    git -C $native apply $compatibility
    if ($LASTEXITCODE) { throw 'Techmo input compatibility patch failed.' }
    git -C $native apply --check $menu
    if ($LASTEXITCODE) { throw 'Native source differs from the expected Techmo title menu patch.' }
    git -C $native apply $menu
    if ($LASTEXITCODE) { throw 'Techmo title menu patch failed.' }
    git -C $native apply --check $vram
    if ($LASTEXITCODE) { throw 'Native source differs from the expected Techmo VRAM patch.' }
    git -C $native apply $vram
    if ($LASTEXITCODE) { throw 'Techmo VRAM patch failed.' }
}
Copy-Item -LiteralPath (Join-Path $root 'engine\win_compat.h') -Destination (Join-Path $native 'gpu\win_compat.h')
Copy-Item -LiteralPath (Join-Path $root 'engine\overlay\vram_monitor.h') -Destination (Join-Path $native 'gpu\shim\vram_monitor.h')
git -C $native submodule update --init --recursive
if ($LASTEXITCODE) { throw 'Pinned native submodules could not be restored.' }
Write-Host "Techmo engine ready: pinned engine $($lock.upstream_version), base $head"
