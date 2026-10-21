# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$PythonArchive)
$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
foreach ($required in @('builds/launcher/Techmo.Launcher.exe', 'out/bb-probe.exe', 'out/bb-start.exe', 'out/native-build.json')) {
    if (!(Test-Path -LiteralPath (Join-Path $root $required))) { throw "Build missing: $required" }
}
$packageRoot = Join-Path $root ('builds/_internal/package-' + [Guid]::NewGuid().ToString('N'))
try {
$stage = Join-Path $packageRoot 'Techmo-experiment-v2'
foreach ($directory in @('builds', 'out', 'tools/python')) {
    New-Item -ItemType Directory -Force -Path (Join-Path $stage $directory) | Out-Null
}
Copy-Item -LiteralPath (Join-Path $root 'builds/launcher') -Destination (Join-Path $stage 'builds/launcher') -Recurse
foreach ($directory in @('scripts', 'licenses', 'docs', 'patches', 'data')) {
    $source = Join-Path $root $directory
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $stage $directory) -Recurse }
}
foreach ($name in @('run.bat', 'build.sh', 'engine.lock.json', 'LICENSE', 'README.md')) {
    Copy-Item -LiteralPath (Join-Path $root $name) -Destination $stage
}
$manifest = Get-Content -LiteralPath (Join-Path $root 'out/native-build.json') -Raw | ConvertFrom-Json
$runtimeFiles = @('bb-probe.exe', 'bb-start.exe', 'bb-gpu-capabilities.exe', 'bb-engine-assets.exe', 'bb-shader-lab.exe',
    'native-build.json') + @($manifest.runtime_dlls.PSObject.Properties.Name)
foreach ($name in $runtimeFiles) { Copy-Item -LiteralPath (Join-Path $root "out/$name") -Destination (Join-Path $stage 'out') }
foreach ($name in @('gpu-cache.id', 'gpu-cache.legacy.id')) {
    $source = Join-Path $root "out/$name"
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination (Join-Path $stage 'out') }
}
Copy-Item -LiteralPath (Join-Path $root 'tools/benchmark.ps1'),(Join-Path $root 'tools/benchmark.py') -Destination (Join-Path $stage 'tools')
if (!$PythonArchive) {
    $pythonCache = Join-Path $root 'builds/_internal/cache/python'
    New-Item -ItemType Directory -Force -Path $pythonCache | Out-Null
    $PythonArchive = Join-Path $pythonCache 'python-3.12.10-embed-amd64.zip'
    if (!(Test-Path -LiteralPath $PythonArchive)) {
        $download = Join-Path $pythonCache ([Guid]::NewGuid().ToString('N') + '.download')
        try {
            Invoke-WebRequest -Uri 'https://www.python.org/ftp/python/3.12.10/python-3.12.10-embed-amd64.zip' -OutFile $download
            Move-Item -LiteralPath $download -Destination $PythonArchive -Force
        } finally {
            if (Test-Path -LiteralPath $download) { Remove-Item -LiteralPath $download -Force }
        }
    }
}
Expand-Archive -LiteralPath $PythonArchive -DestinationPath (Join-Path $stage 'tools/python')
$pythonPathFile = Join-Path $stage 'tools/python/python312._pth'
Add-Content -LiteralPath $pythonPathFile -Value '..\..\scripts' -Encoding ascii
@'
@echo off
start "" /D "%~dp0" "%~dp0builds\launcher\Techmo.Launcher.exe"
'@ | Set-Content -LiteralPath (Join-Path $stage 'Techmo.cmd') -Encoding ascii
$python = Join-Path $stage 'tools/python/python.exe'
& $python -B -c 'import run_techmo, session_diagnostics, mods; print("Packaged Python imports OK")'
if ($LASTEXITCODE -ne 0) { throw 'Packaged Python module check failed.' }
$dist = Join-Path $root 'dist'
New-Item -ItemType Directory -Force -Path $dist | Out-Null
$zip = Join-Path $dist 'Techmo-experiment-v2-win-x64.zip'
Compress-Archive -LiteralPath $stage -DestinationPath $zip -CompressionLevel Optimal -Force
$sha = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash
Write-Host "Package: $zip"
Write-Host "SHA256: $sha"
} finally {
    $temporary = [IO.Path]::GetFullPath($packageRoot)
    $expectedParent = [IO.Path]::GetFullPath((Join-Path $root 'builds/_internal'))
    if ((Split-Path -Parent $temporary) -ne $expectedParent -or
        (Split-Path -Leaf $temporary) -notmatch '^package-[0-9a-f]{32}$') {
        throw 'Refusing to remove a package directory outside the staging location.'
    }
    if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Recurse -Force }
}
