param([switch]$Bootstrap, [switch]$Test, [ValidateRange(1,64)][int]$Jobs = 2)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$msysRoot = if ($env:BB_MSYS2) { $env:BB_MSYS2 } else { 'C:\msys64' }
$bash = Join-Path $msysRoot 'usr/bin/bash.exe'
if (!(Test-Path -LiteralPath $bash)) { throw 'MSYS2 bash is missing. Install MSYS2 or set BB_MSYS2.' }
$savedMsystem = $env:MSYSTEM
$savedChere = $env:CHERE_INVOKING
Push-Location $projectRoot
try {
    $env:MSYSTEM = 'CLANG64'
    $env:CHERE_INVOKING = '1'
    if ($Bootstrap) {
        & $bash -lc 'bash tools/bootstrap-native.sh'
        if ($LASTEXITCODE) { throw "Toolchain bootstrap failed ($LASTEXITCODE)." }
    }
    $testArg = if ($Test) { ' --test' } else { '' }
    $command = 'local_bin="$PWD/builds/_internal/toolchain/clang64/bin"; if [[ -f "$local_bin/clang.exe" ]]; then export PATH="$local_bin:/usr/bin:/c/Program Files/Git/cmd:$PATH"; fi; export CMAKE_BUILD_PARALLEL_LEVEL=' + $Jobs + '; export BB_BUILD_JOBS=' + $Jobs + '; bash build.sh' + $testArg
    & $bash -lc $command
    if ($LASTEXITCODE) { throw "Native build failed ($LASTEXITCODE). See out/gpu-build.log." }
} finally {
    Pop-Location
    $env:MSYSTEM = $savedMsystem
    $env:CHERE_INVOKING = $savedChere
}
