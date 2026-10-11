# Install optional assets from a local installation without modifying its files.
[CmdletBinding()]
param([Parameter(Mandatory)][string]$SourceDirectory)
$ErrorActionPreference = 'Stop'
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sourceRoot = (Resolve-Path -LiteralPath $SourceDirectory).Path
if ($sourceRoot.TrimEnd('\') -eq $projectRoot.TrimEnd('\')) { throw 'Choose a separate source installation.' }
$dll = Join-Path $sourceRoot 'out/nvngx_dlss.dll'
$signature = Get-AuthenticodeSignature -LiteralPath $dll
if ($signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch 'O=NVIDIA Corporation') {
    throw 'The source DLSS model must have a valid NVIDIA signature.'
}
$version = (Get-Item -LiteralPath $dll).VersionInfo.FileVersion
$assets = Join-Path $sourceRoot 'fsr4_shaders'
$names = [Collections.Generic.List[string]]::new()
foreach ($name in @('LICENSE-FSR4-v07.txt','rcas.spv','spd_auto_exposure.spv')) { $names.Add($name) }
foreach ($model in @('native','quality','balanced','performance','ultraperf','drs')) {
    $prefix = "fsr4_model_v07_i8_$model"
    foreach ($suffix in @('initializers.bin','pre_weights.bin','shader_manifest.json')) { $names.Add("${prefix}_$suffix") }
    foreach ($tier in @('1080','2160')) {
        $names.Add("${prefix}_${tier}_pre.spv"); $names.Add("${prefix}_${tier}_post.spv")
        foreach ($pass in 1..12) { $names.Add("${prefix}_${tier}_pass$pass.spv") }
    }
    $manifest = Get-Content -Raw -LiteralPath (Join-Path $assets "${prefix}_shader_manifest.json") | ConvertFrom-Json
    foreach ($item in $manifest.artifacts.PSObject.Properties) {
        # Manifest can list other permutations; validate every file in this asset set.
        if (!$names.Contains($item.Name)) { continue }
        $path = Join-Path $assets $item.Name
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $item.Value.sha256) {
            throw "FSR 4 manifest hash mismatch: $($item.Name)"
        }
    }
}
foreach ($name in $names) {
    $file = Get-Item -LiteralPath (Join-Path $assets $name)
    if ($file.Length -eq 0) { throw "Empty asset: $name" }
    if ($file.Extension -eq '.spv') {
        $stream = [IO.File]::OpenRead($file.FullName)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            if ($stream.Length -lt 20 -or $stream.Length % 4 -ne 0 -or $reader.ReadUInt32() -ne 0x07230203) {
                throw "Invalid SPIR-V: $name"
            }
        } finally { $reader.Dispose() }
    }
}
$destination = Join-Path $projectRoot 'fsr4_shaders'
$out = Join-Path $projectRoot 'out'
New-Item -ItemType Directory -Force -Path $destination,$out | Out-Null
# Preserve the SDK license alongside the separately supplied DLSS binary.
$license = Join-Path $sourceRoot 'out/LICENSE-DLSS-SDK.txt'
$licenseTarget = Join-Path $out 'LICENSE-DLSS-SDK.txt'
if (Test-Path -LiteralPath $license) { Copy-Item -LiteralPath $license -Destination $licenseTarget }
elseif (!(Test-Path -LiteralPath $licenseTarget)) {
    Invoke-WebRequest -Uri 'https://raw.githubusercontent.com/NVIDIA/DLSS/v310.9.1/LICENSE.txt' -OutFile $licenseTarget
}
$records = [Collections.Generic.List[object]]::new()
function Install-Asset([string]$source, [string]$target) {
    $hash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    if (Test-Path -LiteralPath $target) {
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) {
            throw "Different asset already installed; preserve it before replacing: $target"
        }
    } else { Copy-Item -LiteralPath $source -Destination $target }
    if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $hash) { throw "Copy verification failed: $target" }
    $records.Add([pscustomobject]@{path=[IO.Path]::GetRelativePath($projectRoot,$target); sha256=$hash; bytes=(Get-Item -LiteralPath $target).Length})
}
Install-Asset $dll (Join-Path $out 'nvngx_dlss.dll')
foreach ($name in $names) { Install-Asset (Join-Path $assets $name) (Join-Path $destination $name) }
[pscustomobject]@{
    schema=1; installed_utc=[DateTime]::UtcNow.ToString('o'); dlss_version=$version
    fsr4_version='v07 INT8/DOT4 (experimental)'; files=$records
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $out 'upscaler-assets.json') -Encoding utf8
Write-Host "Installed NVIDIA DLSS $version and $($names.Count) FSR 4 v07 assets in $projectRoot."
Write-Host 'Select DLSS or FSR 4 v07 in Graphics. GPU/driver support is checked by the renderer.'
