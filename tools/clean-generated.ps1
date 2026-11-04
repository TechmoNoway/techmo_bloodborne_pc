# SPDX-License-Identifier: GPL-3.0-or-later
# Removes reproducible outputs, keeping runnable builds, SDKs, NuGet, saves and shader caches.
[CmdletBinding(SupportsShouldProcess, ConfirmImpact='Low')]
param(
    [string]$Root = (Join-Path $PSScriptRoot '..'),
    [switch]$IncludeDownloadCache
)
$ErrorActionPreference = 'Stop'
$rootPath = [IO.Path]::GetFullPath($Root).TrimEnd([char[]]'\/')
$gitRoot = & git -C $rootPath rev-parse --show-toplevel
if ($LASTEXITCODE -or [IO.Path]::GetFullPath($gitRoot.Trim()) -ne $rootPath -or
    !(Test-Path -LiteralPath (Join-Path $rootPath 'apps/launcher/Techmo.Launcher.csproj'))) {
    throw 'Choose a Techmo source checkout root.'
}
$paths = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
function Add-Output([string]$relative) {
    $path = [IO.Path]::GetFullPath((Join-Path $rootPath $relative))
    if (Test-Path -LiteralPath $path) { [void]$paths.Add($path) }
}
foreach ($project in @('apps/launcher', 'apps/engine-backend', 'tests/TechmoHosted',
                       'builds/_internal/ui-before-lantern-parity')) {
    foreach ($name in @('bin', 'obj')) { Add-Output "$project/$name" }
}
$internal = Join-Path $rootPath 'builds/_internal'
if (Test-Path -LiteralPath $internal) {
    Get-ChildItem -LiteralPath $internal -Directory | Where-Object Name -Match '^package-[0-9a-f]{32}$' |
        ForEach-Object { [void]$paths.Add($_.FullName) }
}
foreach ($build in @('out/gpu', 'engine/native/out/gpu')) {
    $directory = Join-Path $rootPath $build
    if (Test-Path -LiteralPath $directory) {
        Get-ChildItem -LiteralPath $directory -File -Filter '*.exe' | ForEach-Object { [void]$paths.Add($_.FullName) }
    }
}
$out = Join-Path $rootPath 'out'
if (Test-Path -LiteralPath $out) {
    Get-ChildItem -LiteralPath $out -File | Where-Object Name -Match '-test\.(exe|o)$' |
        ForEach-Object { [void]$paths.Add($_.FullName) }
}
if ($IncludeDownloadCache) {
    $packages = Join-Path $internal 'toolchain/packages'
    if (Test-Path -LiteralPath $packages) {
        Get-ChildItem -LiteralPath $packages -File | Where-Object Name -Match '\.pkg\.tar\.zst(\.sig)?$' |
            ForEach-Object { [void]$paths.Add($_.FullName) }
    }
}
# Validate every target before deleting anything. Never traverse a junction or delete tracked files.
$plan = @(
    foreach ($path in $paths) {
        if (!$path.StartsWith($rootPath + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Output escaped the checkout: $path"
        }
        $parent = Get-Item -LiteralPath $path -Force
        while ($null -ne $parent) {
            if ($parent.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Preserving linked output: $path" }
            if ($parent.FullName -eq $rootPath) { break }
            $parent = if ($parent.PSIsContainer) { $parent.Parent } else { $parent.Directory }
        }
        $relative = $path.Substring($rootPath.Length + 1)
        $tracked = @(& git -C $rootPath ls-files -- $relative)
        if ($LASTEXITCODE -or $tracked.Count) { throw "Preserving tracked output: $relative" }
        & git -C $rootPath check-ignore --quiet -- $relative
        if ($LASTEXITCODE) { throw "Preserving output not covered by .gitignore: $relative" }
        $item = Get-Item -LiteralPath $path -Force
        $bytes = $item.Length
        if ($item.PSIsContainer) {
            $children = @(Get-ChildItem -LiteralPath $path -Recurse -Force)
            if ($children | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }) {
                throw "Preserving output containing a link: $relative"
            }
            $bytes = ($children | Where-Object { !$_.PSIsContainer } | Measure-Object Length -Sum).Sum
        }
        [PSCustomObject]@{ Path=$path; Bytes=[long]$bytes }
    }
)
[long]$removed = 0
foreach ($entry in $plan) {
    if ($PSCmdlet.ShouldProcess($entry.Path, 'Remove reproducible build output')) {
        Remove-Item -LiteralPath $entry.Path -Recurse -Force
        $removed += $entry.Bytes
    }
}
[PSCustomObject]@{ Root=$rootPath; Targets=$plan.Count; PlannedGiB=[math]::Round(($plan | Measure-Object Bytes -Sum).Sum/1GB,3); FreedGiB=[math]::Round($removed/1GB,3) }
