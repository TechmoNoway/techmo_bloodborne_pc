# SPDX-License-Identifier: GPL-3.0-or-later
[CmdletBinding()]
param([string]$PackageCache, [switch]$Offline)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$dotnetCommand = Get-Command dotnet -ErrorAction SilentlyContinue
$dotnetPath = if ($dotnetCommand) { $dotnetCommand.Source } else { Join-Path $env:ProgramFiles 'dotnet\dotnet.exe' }
if (!(Test-Path -LiteralPath $dotnetPath)) { throw 'Install a .NET SDK with Windows desktop support before building the launcher.' }
$buildRoot = Join-Path $projectRoot 'builds'
$env:DOTNET_CLI_HOME = Join-Path $buildRoot '_internal\cache\dotnet-home'
$env:DOTNET_NOLOGO = '1'
$outputPath = Join-Path $buildRoot 'launcher'
if (!$PackageCache) { $PackageCache = Join-Path $buildRoot '_internal\cache\nuget' }
$restoreOptions = @("-p:RestorePackagesPath=$([IO.Path]::GetFullPath($PackageCache))")
if ($Offline) { $restoreOptions += @('-p:RestoreIgnoreFailedSources=true', '-p:NuGetAudit=false') }
& $dotnetPath publish (Join-Path $projectRoot 'apps\launcher\Techmo.Launcher.csproj') -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:DebugType=None -p:DebugSymbols=false @restoreOptions -o $outputPath
if ($LASTEXITCODE -ne 0) { throw 'Techmo launcher publish failed.' }
$retiredBackendConfig = Join-Path $outputPath 'Techmo.LanternBackend.runtimeconfig.json'
if (Test-Path -LiteralPath $retiredBackendConfig) { Remove-Item -LiteralPath $retiredBackendConfig }
$noticePath = Join-Path $outputPath 'licenses'
New-Item -ItemType Directory -Force -Path $noticePath | Out-Null
Copy-Item -LiteralPath (Join-Path $projectRoot 'LICENSE') -Destination (Join-Path $noticePath 'Lantern-GPL-2.0.txt')
Copy-Item -LiteralPath (Join-Path $projectRoot 'licenses\Techmo-GPL-3.0.txt') -Destination $noticePath
Copy-Item -Path (Join-Path $projectRoot 'licenses\launcher\*') -Destination $noticePath
$executable = Get-Item -LiteralPath (Join-Path $outputPath 'Techmo.Launcher.exe')
$sha256 = (Get-FileHash -LiteralPath $executable.FullName -Algorithm SHA256).Hash
@"
# Techmo Bloodborne - local build

Executable: Techmo.Launcher.exe
Source project: apps/launcher/Techmo.Launcher.csproj
Output: builds/launcher
Target: Windows x64, self-contained .NET 8, single-file, Release.
Published: $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss zzz')
Size: $($executable.Length) bytes
SHA256: $sha256

Open builds/TEST-LAUNCHER.cmd to test this launcher.
Building the launcher does not compile the native engine. Playing requires the
engine under the project's out/ folder; build it with tools/build-native.ps1.
Local backups and verification screenshots are under builds/_internal/.
"@ | Set-Content -LiteralPath (Join-Path $outputPath 'BUILD-INFO.md') -Encoding utf8
@'
@echo off
if not exist "%~dp0launcher\Techmo.Launcher.exe" (
    echo Launcher missing. Run tools\build-launcher.ps1 from the project folder first.
    pause
    exit /b 1
)
start "" /D "%~dp0.." "%~dp0launcher\Techmo.Launcher.exe"
'@ | Set-Content -LiteralPath (Join-Path $buildRoot 'TEST-LAUNCHER.cmd') -Encoding ascii
@'
@echo off
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0..\..\tools\benchmark.ps1" -Target techmo-v2 %*
'@ | Set-Content -LiteralPath (Join-Path $outputPath 'Benchmark.cmd') -Encoding ascii
@'
Techmo Bloodborne - local test builds

Double-click TEST-LAUNCHER.cmd to open the latest launcher.
You can also open launcher\Techmo.Launcher.exe directly.
launcher\Benchmark.cmd records FPS, frame times and RAM/VRAM for experiment-v2.
Load a test location in the game before starting a capture.

launcher\   Latest Windows launcher and build information.
_internal\  Build caches, backups, verification screenshots and diagnostics.

Build again from the project root: .\tools\build-launcher.ps1
All builds stay local and are excluded from Git.
Playing requires the native engine in the project's out\ folder.
'@ | Set-Content -LiteralPath (Join-Path $buildRoot 'README.txt') -Encoding ascii
Write-Host "Launcher: $(Join-Path $outputPath 'Techmo.Launcher.exe')"
Write-Host "Quick test: $(Join-Path $buildRoot 'TEST-LAUNCHER.cmd')"
