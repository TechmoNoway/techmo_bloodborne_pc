param([ValidateRange(1,16)][int]$Jobs=2, [string]$Toolchain=$env:TECHMO_TOOLCHAIN, [switch]$SkipBootstrap)
$ErrorActionPreference='Stop'
$root=Split-Path -Parent $PSScriptRoot
if (!$Toolchain) {
    $candidates=@((Join-Path $root 'builds\_internal\toolchain\clang64'), (Join-Path $root '..\techmo_launcher\builds\_internal\toolchain\clang64'), 'C:\msys64\clang64')
    $Toolchain=$candidates | Where-Object { Test-Path -LiteralPath (Join-Path $_ 'bin\clang.exe') } | Select-Object -First 1
}
if (!$Toolchain) { throw 'Set -Toolchain to an MSYS2 CLANG64 toolchain directory.' }
if (!$SkipBootstrap) { & (Join-Path $PSScriptRoot 'bootstrap-engine.ps1') }
$bash=Join-Path $(if($env:BB_MSYS2){$env:BB_MSYS2}else{'C:\msys64'}) 'usr\bin\bash.exe'
$savedSystem=$env:MSYSTEM; $savedChere=$env:CHERE_INVOKING; $savedToolchain=$env:TECHMO_TOOLCHAIN; $savedJobs=$env:BB_JOBS
Push-Location $root
try {
    $env:MSYSTEM='CLANG64'; $env:CHERE_INVOKING='1'; $env:TECHMO_TOOLCHAIN=[IO.Path]::GetFullPath($Toolchain); $env:BB_JOBS=[string]$Jobs
    & $bash -lc 'set -e; export PATH="$(cygpath -u "$TECHMO_TOOLCHAIN")/bin:/usr/bin:/c/Program Files/Git/cmd:$PATH"; bash build.sh'
    if ($LASTEXITCODE) { throw 'Native build failed. See engine/native/out/native-build.log and gpu-build.log.' }
} finally {
    Pop-Location
    $env:MSYSTEM=$savedSystem; $env:CHERE_INVOKING=$savedChere; $env:TECHMO_TOOLCHAIN=$savedToolchain; $env:BB_JOBS=$savedJobs
}
