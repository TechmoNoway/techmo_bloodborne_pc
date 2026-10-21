param(
    [ValidateSet('lantern', 'techmo', 'techmo-v2')][string]$Target = 'techmo-v2',
    [ValidateRange(1,3600)][int]$Seconds = 180,
    [ValidateRange(0,120)][int]$Delay = 10,
    [switch]$AsAdministrator
)
$ErrorActionPreference = 'Stop'
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if ($AsAdministrator -and -not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -Target {1} -Seconds {2} -Delay {3}' -f $PSCommandPath,$Target,$Seconds,$Delay
        Start-Process -FilePath 'powershell.exe' -Verb RunAs -ArgumentList $arguments -Wait
        return
    }
    $projectRoot = Split-Path -Parent $PSScriptRoot
    $python = Join-Path $projectRoot 'tools\python\python.exe'
    if (!(Test-Path -LiteralPath $python)) {
        $python = Join-Path $projectRoot 'builds\_internal\toolchain\clang64\bin\python.exe'
    }
    if (!(Test-Path -LiteralPath $python)) {
        $python = Join-Path $projectRoot '..\techmo_launcher\builds\_internal\toolchain\clang64\bin\python.exe'
    }
    & $python -B (Join-Path $PSScriptRoot 'benchmark.py') --target $Target --seconds $Seconds --delay $Delay
    if ($LASTEXITCODE -ne 0) { throw "Benchmark exited with code $LASTEXITCODE" }
} catch {
    Write-Host $_ -ForegroundColor Red
} finally {
    Read-Host 'Press Enter to close'
}
