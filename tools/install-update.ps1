# SPDX-License-Identifier: GPL-3.0-or-later
param([Parameter(Mandatory=$true)][string]$Plan, [switch]$NoRestart)
$ErrorActionPreference='Stop'
$applied=New-Object Collections.Generic.List[object]
$root=$null; $stage=$null; $resultPath=$null; $lock=$null; $success=$false; $parentClosed=$false
function Safe-Path([string]$base,[string]$relative) {
    if (!$relative -or $relative.Contains('\')) { throw 'Invalid relative update path.' }
    foreach ($part in $relative.Split('/')) {
        if (!$part -or $part -eq '.' -or $part -eq '..' -or $part.EndsWith('.') -or $part.EndsWith(' ') -or $part.IndexOfAny([IO.Path]::GetInvalidFileNameChars()) -ge 0) { throw 'Invalid relative update path.' }
    }
    $fence=[IO.Path]::GetFullPath($base).TrimEnd('\')+'\'
    $target=[IO.Path]::GetFullPath((Join-Path $base $relative.Replace('/','\')))
    if (!$target.StartsWith($fence,[StringComparison]::OrdinalIgnoreCase)) { throw 'Update path escaped its folder.' }
    $probe=$target
    while ($probe -and ($probe -eq $fence.TrimEnd('\') -or $probe.StartsWith($fence,[StringComparison]::OrdinalIgnoreCase))) {
        if (Test-Path -LiteralPath $probe) {
            if ((Get-Item -LiteralPath $probe -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) { throw 'Refusing to update a linked path.' }
        }
        $probe=Split-Path -Parent $probe
    }
    return $target
}
function Runtime-Path([string]$relative) {
    $parts=$relative.Split('/')
    if ($parts.Count -eq 1) { return $relative -in @('Techmo.cmd','run.bat','build.sh','engine.lock.json','package.json','LICENSE','README.md') }
    if ($parts[0] -in @('out','scripts','licenses','docs','patches','data')) { return $true }
    if ($parts[0] -eq 'builds') { return $parts.Count -gt 2 -and $parts[1] -eq 'launcher' }
    if ($parts[0] -eq 'tools') { return $parts[1] -eq 'python' -or ($parts.Count -eq 2 -and $parts[1] -in @('benchmark.ps1','benchmark.py','install-update.ps1')) }
    return $false
}
function Retry([scriptblock]$operation) {
    for ($attempt=0; ; $attempt++) {
        try { & $operation; return } catch { if ($attempt -ge 5) { throw }; Start-Sleep -Milliseconds 400 }
    }
}
function File-Sha256([string]$path) {
    $algorithm=[Security.Cryptography.SHA256]::Create()
    $stream=[IO.File]::OpenRead($path)
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-','').ToLowerInvariant() }
    finally { $stream.Dispose(); $algorithm.Dispose() }
}
function Write-Result([bool]$ok,[string]$message) {
    $result=@{success=$ok;message=$message;version=$configuration.version;stage=$stage;utc=[DateTime]::UtcNow.ToString('o')}|ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $stage 'result.json'),$result)
    $temporary=$resultPath+'.new'
    [IO.File]::WriteAllText($temporary,$result)
    Move-Item -LiteralPath $temporary -Destination $resultPath -Force
}
try {
    $configuration=Get-Content -LiteralPath $Plan -Raw -Encoding UTF8|ConvertFrom-Json
    $root=[IO.Path]::GetFullPath($configuration.root).TrimEnd('\')
    $stage=[IO.Path]::GetFullPath($configuration.stage)
    if ((Split-Path -Parent $stage) -ne (Join-Path $root '.techmo-update') -or (Split-Path -Leaf $stage) -notmatch '^[0-9a-f]{32}$') { throw 'Invalid update staging folder.' }
    if ([IO.Path]::GetFullPath($Plan) -ne (Join-Path $stage 'plan.json')) { throw 'Invalid update plan location.' }
    [void](Safe-Path $root ('.techmo-update/'+(Split-Path -Leaf $stage)))
    if (Test-Path -LiteralPath (Join-Path $root '.git')) { throw 'Source checkouts must be built separately.' }
    if (!(Test-Path -LiteralPath (Join-Path $root 'Techmo.cmd'))) { throw 'Destination is not a Techmo package.' }
    $resultPath=Safe-Path $root '.techmo-update/result.json'
    $lock=[IO.File]::Open((Join-Path $root '.techmo-update/install.lock'),[IO.FileMode]::OpenOrCreate,[IO.FileAccess]::ReadWrite,[IO.FileShare]::None)
    if ($configuration.parentId -gt 0) {
        $parent=Get-Process -Id $configuration.parentId -ErrorAction SilentlyContinue
        $parentStarted=[DateTimeOffset]::Parse([string]$configuration.parentStartedUtc).UtcDateTime
        if ($parent -and [Math]::Abs(($parent.StartTime.ToUniversalTime()-$parentStarted).TotalSeconds) -lt 1) {
            if (!$parent.WaitForExit(60000)) { throw 'The launcher did not close. No files were changed.' }
        }
    } elseif (!$NoRestart) { throw 'An update must wait for its launcher to close.' }
    $parentClosed=$true
    $launcher=Safe-Path $root 'builds/launcher/Techmo.Launcher.exe'
    $engine=Safe-Path $root 'out/bb-probe.exe'
    $running=@(Get-CimInstance Win32_Process -Filter "Name = 'bb-probe.exe' OR Name = 'bb-start.exe' OR Name = 'Techmo.Launcher.exe'" | Where-Object {
        $_.ExecutablePath -eq $launcher -or $_.ExecutablePath -eq $engine -or $_.ExecutablePath -eq (Join-Path $root 'out/bb-start.exe')
    })
    if ($running.Count) { throw 'Close the game and other Techmo windows before installing.' }
    $payload=Safe-Path $stage 'payload'
    $backup=Safe-Path $stage 'backup'
    $seen=New-Object 'Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
    # Validate every source and destination before changing any installed file.
    foreach ($file in $configuration.files) {
        if (!(Runtime-Path $file.path) -or !$seen.Add($file.path)) { throw 'Invalid or duplicate install entry.' }
        $source=Safe-Path $payload $file.path
        $target=Safe-Path $root $file.path
        if ([IO.Directory]::Exists($target)) { throw 'A destination file is a directory.' }
        [void](Safe-Path $backup $file.path)
        if ($file.sha256 -notmatch '^[0-9a-fA-F]{64}$' -or (File-Sha256 $source) -ne $file.sha256) { throw 'Staged update file verification failed.' }
    }
    foreach ($required in @('builds/launcher/Techmo.Launcher.exe','out/bb-probe.exe','out/bb-start.exe','package.json','Techmo.cmd')) {
        if (!$seen.Contains($required)) { throw "Update is missing $required" }
    }
    foreach ($file in $configuration.files) {
        $target=Safe-Path $root $file.path; $source=Safe-Path $payload $file.path; $saved=Safe-Path $backup $file.path
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $target) | Out-Null
        $existed=Test-Path -LiteralPath $target
        if ($existed) {
            New-Item -ItemType Directory -Force -Path (Split-Path -Parent $saved) | Out-Null
            Retry { Move-Item -LiteralPath $target -Destination $saved }
        }
        $applied.Add([PSCustomObject]@{path=$file.path;existed=$existed})
        Retry { Copy-Item -LiteralPath $source -Destination $target -Force }
    }
    Write-Result $true 'Update installed.'
    $success=$true
} catch {
    $failure=$_.Exception.Message
    $rollbackErrors=New-Object Collections.Generic.List[string]
    for ($i=$applied.Count-1; $i -ge 0; $i--) {
        $file=$applied[$i]
        try {
            $target=Safe-Path $root $file.path; $saved=Safe-Path (Join-Path $stage 'backup') $file.path
            if ($file.existed) {
                Retry { if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }; Move-Item -LiteralPath $saved -Destination $target }
            } elseif (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Force }
        } catch { $rollbackErrors.Add($_.Exception.Message) }
    }
    if ($rollbackErrors.Count) { $failure+=' Rollback needs attention: '+($rollbackErrors -join '; ') }
    if ($resultPath -and $lock) { Write-Result $false $failure }
    Write-Output $failure
} finally {
    if ($lock) { $lock.Dispose() }
}
if ($success) {
    # The backup is retained; remove only verified temporary payloads and older successful backups.
    $cleanup=@((Join-Path $stage 'payload'))
    foreach ($old in Get-ChildItem -LiteralPath (Join-Path $root '.techmo-update') -Directory) {
        if ($old.FullName -ne $stage -and $old.Name -match '^[0-9a-f]{32}$' -and (Test-Path -LiteralPath (Join-Path $old.FullName 'result.json'))) {
            $previous=Get-Content -LiteralPath (Join-Path $old.FullName 'result.json') -Raw -Encoding UTF8|ConvertFrom-Json
            if ($previous.success -and $previous.stage -eq $old.FullName) { $cleanup+=$old.FullName }
        }
    }
    foreach ($candidate in $cleanup) {
        try {
            $parent=[IO.Path]::GetFullPath((Join-Path $root '.techmo-update')).TrimEnd('\')+'\'
            $absolute=[IO.Path]::GetFullPath($candidate)
            if (!$absolute.StartsWith($parent,[StringComparison]::OrdinalIgnoreCase)) { throw 'Cleanup escaped the update folder.' }
            $links=@(Get-ChildItem -LiteralPath $absolute -Recurse -Force|Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint })
            if (((Get-Item -LiteralPath $absolute -Force).Attributes -band [IO.FileAttributes]::ReparsePoint) -or $links.Count) { throw 'Preserving linked update folder.' }
            Remove-Item -LiteralPath $absolute -Recurse -Force
        } catch { # Cleanup failure must not invalidate a successful install.
        }
    }
}
if (!$NoRestart -and $parentClosed -and $root -and $resultPath) {
    $launcher=Safe-Path $root 'builds/launcher/Techmo.Launcher.exe'
    if (Test-Path -LiteralPath $launcher) { Start-Process -FilePath $launcher -WorkingDirectory $root -WindowStyle Normal }
}
if (!$success) { exit 1 }
