param(
    [string]$Server = 'byond://127.0.0.1:1337',
    [string]$ByondBin = 'C:\Program Files (x86)\BYOND\bin',
    [switch]$CheckOnly
)
$ErrorActionPreference = 'Stop'
$compatibility = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'compatibility.json') -Raw | ConvertFrom-Json
foreach ($entry in $compatibility.sha256.PSObject.Properties) {
    $actual = (Get-FileHash -LiteralPath (Join-Path $ByondBin $entry.Name) -Algorithm SHA256).Hash
    if ($actual -ne $entry.Value) {
        throw "Unsupported BYOND binary: $($entry.Name). Expected $($compatibility.byondVersion) x86; do not bypass this check."
    }
}
if ($CheckOnly) { Write-Host "BYOND $($compatibility.byondVersion) binary hashes verified."; return }
if ($Server -notmatch '^byond://[a-zA-Z0-9.:-]+$') { throw 'Expected byond://host:port' }
foreach ($name in @('launch.exe', 'scene_probe.dll')) {
    if (-not (Test-Path -LiteralPath (Join-Path $PSScriptRoot "bin\$name"))) { throw "Missing bin\$name. Run build.cmd first." }
}
$env:BS220_NATIVE_LOG = Join-Path $PSScriptRoot 'bin\scene-probe.log'
$env:BS220_NATIVE_SNAPSHOT = Join-Path $PSScriptRoot 'bin\scene.bin'
$env:BS220_NATIVE_TEXTURE = Join-Path $PSScriptRoot 'bin\native-texture.bmp'
$env:BS220_NATIVE_FRAME = Join-Path $PSScriptRoot 'bin\native-frame.bmp'
$env:BS220_VOLUME_LOG = Join-Path $PSScriptRoot 'bin\volume.log'
& (Join-Path $PSScriptRoot 'bin\launch.exe') (Join-Path $ByondBin 'dreamseeker.exe') (Join-Path $PSScriptRoot 'bin\scene_probe.dll') $Server
if ($LASTEXITCODE -ne 0) { throw "Native launcher failed: $LASTEXITCODE" }
Write-Host "Probe log: $env:BS220_NATIVE_LOG"
