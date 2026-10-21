param([ValidateRange(1,64)][int]$Jobs = 2)
$ErrorActionPreference = 'Stop'
$projectRoot = Split-Path $PSScriptRoot -Parent
$msysRoot = if ($env:BB_MSYS2) { $env:BB_MSYS2 } else { 'C:\msys64' }
$savedMsystem = $env:MSYSTEM
$savedChere = $env:CHERE_INVOKING
Push-Location $projectRoot
try {
    $env:MSYSTEM = 'CLANG64'
    $env:CHERE_INVOKING = '1'
    $command = 'export PATH="$PWD/builds/_internal/toolchain/clang64/bin:/usr/bin:/c/Program Files/Git/cmd:$PATH"; cmake -S engine/renderer -B out/gpu -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DBB_LTO=ON -DBB_PGO=off && ninja -C out/gpu -j ' + $Jobs + ' bb-pipeline-lab'
    & (Join-Path $msysRoot 'usr/bin/bash.exe') -lc $command
    if ($LASTEXITCODE) { throw "Pipeline lab build failed ($LASTEXITCODE)." }
} finally {
    Pop-Location
    $env:MSYSTEM = $savedMsystem
    $env:CHERE_INVOKING = $savedChere
}
