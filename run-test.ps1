param(
    [ValidateSet("debug", "release", "signed-release")]
    [string]$BuildConfig = "release"
)

# PowerShell equivalent for running tests
# Usage: .\run-test.ps1 -BuildConfig release

$ErrorActionPreference = "Stop"

function Initialize-BuildEnvironment {
    param(
        [string]$QtBinPath = "C:\Qt\6.7.3\msvc2019_64\bin"
    )
    
    Write-Host "`n=== Initializing Build Environment ==="
    
    # Remove all Git paths from PATH to avoid conflicts
    $env:OLDPATH = $env:PATH
    $CleanPath = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'Git' }) -join ';'
    $env:PATH = "$CleanPath;$QtBinPath"
    $env:COMSPEC = "$env:SystemRoot\system32\cmd.exe"
    
    Write-Host "Environment initialized successfully"
}

# Initialize Visual Studio environment
$VSPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$WorkspaceRoot = "c:\games\moonlight-qt"

# Initialize build environment with clean PATH
Initialize-BuildEnvironment -QtBinPath "C:\Qt\6.7.3\msvc2019_64\bin"

# Change to workspace directory
Set-Location $WorkspaceRoot

Write-Host "Building Moonlight Qt Test with configuration: $BuildConfig"

# Convert build config to lowercase and set additional variables
$BuildConfigLower = $BuildConfig.ToLower()

switch ($BuildConfig) {
    "debug" {
        $BuildConfigLower = "debug"
    }
    "release" {
        $BuildConfigLower = "release"
    }
    "signed-release" {
        $BuildConfigLower = "release"
        
        # Check for unstaged changes
        $gitStatus = git diff-index --quiet HEAD --
        if ($LASTEXITCODE -ne 0) {
            Write-Error "Signed release builds must not have unstaged changes!"
            exit 1
        }
    }
}

# Locate qmake
$QmakeCmd = "qmake.exe"
$QmakePath = $null

try {
    $QmakePath = Get-Command qmake.exe -ErrorAction SilentlyContinue
    if ($QmakePath) {
        Write-Host "Found qmake.exe at: $($QmakePath.Source)"
    } else {
        Write-Error "Unable to find qmake.exe. Did you add Qt bins to your PATH?"
        exit 1
    }
} catch {
    Write-Error "Unable to find qmake.exe. Did you add Qt bins to your PATH?"
    exit 1
}

# Determine Qt path and architecture
$QtPath = Split-Path -Parent $QmakePath.Source
Write-Host "QT_PATH=$QtPath"

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

Write-Host "Detected target architecture: $Arch"

# Set build paths
$BuildRoot = "$WorkspaceRoot\build"
$SourceRoot = $WorkspaceRoot
$BuildFolder = "$BuildRoot\build-test-$Arch-$BuildConfigLower"

# Determine VC architecture
$VcArch = $null
if ($Arch -eq "x64") {
    $VcArch = "AMD64"
} else {
    $VcArch = $Arch
}

# If cross-compiling, adjust VC architecture
if ($VcArch -ne $env:PROCESSOR_ARCHITECTURE) {
    $VcArch = "$env:PROCESSOR_ARCHITECTURE" + "_" + "$VcArch"
}

# Find Visual Studio and run vcvarsall.bat
$VSWhere = "$SourceRoot\scripts\vswhere.exe"
$VSInstallPath = & $VSWhere -latest -property installationPath
if (-not $VSInstallPath) {
    Write-Error "Could not find Visual Studio installation"
    exit 1
}

Write-Host "Setting up Visual Studio environment for $VcArch"
$VCVarsScript = "$VSInstallPath\VC\Auxiliary\Build\vcvarsall.bat"
cmd /c "`"$VCVarsScript`" $VcArch && set" | ForEach-Object {
    if ($_ -match "^([^=]+)=(.*)$") {
        [System.Environment]::SetEnvironmentVariable($matches[1], $matches[2])
    }
}

Write-Host "Cleaning output directories"
if (Test-Path $BuildFolder) { Remove-Item $BuildFolder -Recurse -Force }

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $BuildFolder -Force | Out-Null

Write-Host "Configuring the test project"
Push-Location $BuildFolder
try {
    & qmake.exe "$SourceRoot\moonlight-qt.pro" "CONFIG+=test"
    if ($LASTEXITCODE -ne 0) {
        Write-Error "QMake configuration failed"
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "Compiling Test in $BuildConfigLower configuration"
Push-Location $BuildFolder
try {
    & "$SourceRoot\scripts\jom.exe" $BuildConfigLower
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Compilation failed"
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "Running test executable"
$TestExecutable = "$BuildFolder\app\$BuildConfigLower\MoonlightTest.exe"
if (Test-Path $TestExecutable) {
    & $TestExecutable
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Test execution failed"
        exit $LASTEXITCODE
    }
    Write-Host "Test completed successfully!" -ForegroundColor Green
} else {
    Write-Error "Test executable not found at: $TestExecutable"
    exit 1
}