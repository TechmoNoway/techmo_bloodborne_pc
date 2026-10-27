param()
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lock = Get-Content -LiteralPath (Join-Path $root 'engine.lock.json') -Raw | ConvertFrom-Json
$native = Join-Path $root 'engine/native'
$patches = @('techmo-compat.patch','techmo-menu.patch','techmo-vram.patch','techmo-platform.patch')
$current = if (Test-Path -LiteralPath (Join-Path $native '.git')) { & git -C $native rev-parse 'HEAD^{tree}' 2>$null } else { '' }
if ($current -ne $lock.engine_tree) {
    $cache = Join-Path $root 'builds/_internal/cache/upstream-lantern'
    if (!(Test-Path -LiteralPath (Join-Path $cache 'HEAD')) -and !(Test-Path -LiteralPath (Join-Path $cache '.git'))) {
        New-Item -ItemType Directory -Force -Path $cache | Out-Null
        & git -C $cache init --bare -q
        if ($LASTEXITCODE) { throw 'Could not initialise upstream source cache.' }
    }
    & git -C $cache cat-file -e "$($lock.upstream_commit)^{commit}" 2>$null
    if ($LASTEXITCODE) {
        & git -C $cache fetch --depth 1 --no-tags $lock.upstream_repository $lock.upstream_commit
        if ($LASTEXITCODE) { throw 'Could not fetch the pinned upstream source.' }
    }
    $tree = (& git -C $cache rev-parse "$($lock.upstream_commit):engine").Trim()
    if ($tree -ne $lock.engine_tree) { throw 'Upstream engine tree does not match the lock file.' }
    $stage = Join-Path $root ('builds/_internal/engine-import-' + [Guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Force -Path $stage | Out-Null
    & git -C $stage init -q
    & git -C $stage config core.longpaths true
    & git -C $stage config core.autocrlf false
    & git -C $stage fetch --depth 1 --no-tags $cache $lock.upstream_commit
    if ($LASTEXITCODE) { throw 'Could not prepare engine source objects.' }
    $import = (& git -C $stage -c user.name='Techmo source importer' -c user.email='source-import@techmo.local' commit-tree $tree -m "Unmodified engine subtree from $($lock.upstream_repository) at $($lock.upstream_commit)").Trim()
    & git -C $stage reset --hard $import
    if ($LASTEXITCODE) { throw 'Could not materialise the pinned engine tree.' }
    foreach ($name in $patches) {
        & git -C $stage apply --check (Join-Path $root "engine/$name")
        if ($LASTEXITCODE) { throw "Techmo patch no longer matches upstream: $name" }
        & git -C $stage apply (Join-Path $root "engine/$name")
        if ($LASTEXITCODE) { throw "Could not apply $name" }
    }
    $backup = Join-Path $root ('builds/_internal/engine-before-update-' + [Guid]::NewGuid().ToString('N'))
    foreach ($target in @($native,$stage,$backup)) {
        if (![IO.Path]::GetFullPath($target).StartsWith($root + [IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase)) { throw 'Engine move escaped the checkout.' }
    }
    if (Test-Path -LiteralPath $native) { Move-Item -LiteralPath $native -Destination $backup }
    Move-Item -LiteralPath $stage -Destination $native
} else {
    foreach ($name in $patches) {
        & git -C $native apply --reverse --check (Join-Path $root "engine/$name") 2>$null
        if ($LASTEXITCODE) { throw "Existing engine has unexpected edits near $name; preserving it." }
    }
}
Copy-Item -LiteralPath (Join-Path $root 'engine/win_compat.h') -Destination (Join-Path $native 'gpu/win_compat.h')
Copy-Item -LiteralPath (Join-Path $root 'engine/overlay/vram_monitor.h') -Destination (Join-Path $native 'gpu/shim/vram_monitor.h')
Copy-Item -LiteralPath (Join-Path $root 'apps/launcher/Assets/bloodborne.ico') -Destination (Join-Path $native 'src/techmo.ico')
# Compare the working source with a temporary index containing exactly our patch chain.
# The real index and local edits stay intact if this validation fails.
$verifyIndex = Join-Path $native ('.git/techmo-verify-' + [Guid]::NewGuid().ToString('N'))
$previousIndex = $env:GIT_INDEX_FILE
try {
    $env:GIT_INDEX_FILE = $verifyIndex
    & git -C $native read-tree HEAD
    if ($LASTEXITCODE) { throw 'Could not prepare source verification index.' }
    foreach ($name in $patches) {
        & git -C $native apply --cached (Join-Path $root "engine/$name")
        if ($LASTEXITCODE) { throw "Source verification failed for $name" }
    }
    & git -C $native diff --exit-code --ignore-space-at-eol -- .
    if ($LASTEXITCODE) { throw 'Native source contains changes outside the pinned engine and Techmo patches; preserving them.' }
    $extra = @(& git -C $native ls-files --others --exclude-standard)
    if ($extra | Where-Object { $_ -notin @('gpu/shim/vram_monitor.h','src/techmo.ico') }) {
        throw 'Native source contains unexpected untracked files; preserving them.'
    }
} finally {
    $env:GIT_INDEX_FILE = $previousIndex
    if (Test-Path -LiteralPath $verifyIndex) { Remove-Item -LiteralPath $verifyIndex -Force }
}
Write-Host "Techmo engine ready: upstream $($lock.upstream_version), tree $($lock.engine_tree)"
