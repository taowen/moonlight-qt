param(
    [ValidateSet("x64")]
    [string] $Arch = "x64",
    [ValidateSet("release", "debug")]
    [string] $Config = "release",
    [string] $DeployDir,
    [ValidateSet("flat", "stereo", "depth")]
    [string] $Mode = "stereo",
    [ValidateSet("weak", "medium", "strong")]
    [string] $Strength = "weak",
    [string] $DepthModel
)

$ErrorActionPreference = "Stop"

$sourceRoot = Resolve-Path (Join-Path $PSScriptRoot "..")
if (-not $DeployDir) {
    $DeployDir = Join-Path $sourceRoot "build\deploy-$Arch-$Config"
}

$exe = Join-Path $DeployDir "Moonlight.exe"
if (-not (Test-Path $exe)) {
    throw "Moonlight.exe not found at $exe. Run scripts\build-arch.bat $Config $Arch first."
}

$sidecar = Join-Path $DeployDir "moonlight-acer-sr.dll"
if (-not (Test-Path $sidecar)) {
    throw "moonlight-acer-sr.dll not found at $sidecar. Rebuild with Acer SR sidecar enabled."
}

$env:MOONLIGHT_ACER_SR = "1"
$env:MOONLIGHT_ACER_SR_MODE = $Mode
$env:MOONLIGHT_ACER_SR_STRENGTH = $Strength
if (-not $DepthModel) {
    $DepthModel = Join-Path $sourceRoot "models\distill_any_depth_small_588x336_nncf_int8.onnx"
}
$resolvedDepthModel = Resolve-Path $DepthModel
if ([System.IO.Path]::GetFileName($resolvedDepthModel.Path) -ne "distill_any_depth_small_588x336_nncf_int8.onnx") {
    throw "Only the fastest depth path is supported: distill_any_depth_small_588x336_nncf_int8.onnx"
}
$env:MOONLIGHT_ACER_SR_DEPTH_MODEL = $resolvedDepthModel.Path
Push-Location $DeployDir
try {
    & $exe @args
} finally {
    Pop-Location
}
