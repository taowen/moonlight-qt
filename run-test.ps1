param(
    [ValidateSet("debug", "release", "signed-release")]
    [string]$BuildConfig = "release"
)

# PowerShell script for running tests
# Usage: .\run-test.ps1 -BuildConfig release

$ErrorActionPreference = "Stop"

$WorkspaceRoot = "c:\games\moonlight-qt"
Set-Location $WorkspaceRoot

Write-Host "Building Moonlight Qt for testing with configuration: $BuildConfig"

# Call build.ps1 to do the full build and deployment
& ".\build-incremental.ps1" -BuildConfig $BuildConfig
if ($LASTEXITCODE -ne 0) {
    Write-Error "Build failed"
    exit $LASTEXITCODE
}

Write-Host "Running integration tests"

# Determine architecture for finding the deployed executable
$QmakePath = Get-Command qmake.exe -ErrorAction SilentlyContinue
if (-not $QmakePath) {
    Write-Error "Unable to find qmake.exe to determine architecture"
    exit 1
}

$QtPath = Split-Path -Parent $QmakePath.Source
$Arch = $null

if ($QtPath -match "_arm64") {
    $Arch = "arm64"
} elseif ($QtPath -match "_64") {
    $Arch = "x64"
} elseif ($QtPath -match "msvc") {
    $Arch = "x86"
} else {
    Write-Error "Unable to determine Qt architecture"
    exit 1
}

$BuildConfigLower = $BuildConfig.ToLower()
if ($BuildConfig -eq "signed-release") {
    $BuildConfigLower = "release"
}

# Use the deployed executable from build.ps1
$DeployFolder = "$WorkspaceRoot\build\deploy-$Arch-$BuildConfigLower"
$MainExecutable = "$DeployFolder\Moonlight.exe"

if (Test-Path $MainExecutable) {
    & $MainExecutable "test"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Test execution failed"
        exit $LASTEXITCODE
    }
    Write-Host "Test completed successfully!" -ForegroundColor Green
} else {
    Write-Error "Moonlight executable not found at: $MainExecutable"
    exit 1
}