param(
    [ValidateSet("debug", "release", "signed-release")]
    [string]$BuildConfig = "release",
    [switch]$Clean = $false,
    [switch]$Force = $false
)

# Fast incremental build script for Moonlight Qt
# Usage: .\build-incremental.ps1 -BuildConfig release [-Clean] [-Force]

$ErrorActionPreference = "Stop"

function Initialize-BuildEnvironment {
    param(
        [string]$QtBinPath = "C:\Qt\6.7.3\msvc2019_64\bin"
    )
    
    Write-Host "`n=== Initializing Build Environment ===" -ForegroundColor Cyan
    
    # Remove all Git paths from PATH to avoid conflicts
    $env:OLDPATH = $env:PATH
    $CleanPath = ($env:PATH -split ';' | Where-Object { $_ -notmatch 'Git' }) -join ';'
    $env:PATH = "$CleanPath;$QtBinPath"
    $env:COMSPEC = "$env:SystemRoot\system32\cmd.exe"
    
    Write-Host "Environment initialized successfully" -ForegroundColor Green
}

function Test-FileNewer {
    param(
        [string]$SourceFile,
        [string]$TargetFile
    )
    
    if (-not (Test-Path $TargetFile)) {
        return $true
    }
    
    if (-not (Test-Path $SourceFile)) {
        return $false
    }
    
    return (Get-Item $SourceFile).LastWriteTime -gt (Get-Item $TargetFile).LastWriteTime
}

function Copy-IfNewer {
    param(
        [string]$Source,
        [string]$Destination,
        [switch]$Force = $false
    )
    
    if ($Force -or (Test-FileNewer -SourceFile $Source -TargetFile $Destination)) {
        Write-Host "Updating: $(Split-Path -Leaf $Destination)" -ForegroundColor Yellow
        Copy-Item $Source $Destination -Force
        return $true
    }
    return $false
}

# Initialize Visual Studio environment
$VSPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$WorkspaceRoot = "c:\games\moonlight-qt"

# Initialize build environment with clean PATH
Initialize-BuildEnvironment -QtBinPath "C:\Qt\6.7.3\msvc2019_64\bin"

# Change to workspace directory
Set-Location $WorkspaceRoot

Write-Host "Building Moonlight Qt with configuration: $BuildConfig (incremental)" -ForegroundColor Cyan

# Convert build config to lowercase and set additional variables
$BuildConfigLower = $BuildConfig.ToLower()
$WixMums = 10
$Sign = $false
$MustDeploySymbols = $false

switch ($BuildConfig) {
    "debug" {
        $BuildConfigLower = "debug"
    }
    "release" {
        $BuildConfigLower = "release"
    }
    "signed-release" {
        $BuildConfigLower = "release"
        $Sign = $true
        $MustDeploySymbols = $true
        
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
$HostBinPath = $null
$WinDeployQtCmd = $null

if ($QtPath -match "_arm64") {
    $Arch = "arm64"
    $HostBinPath = $QtPath -replace "_arm64", "_64"
    Write-Host "HOSTBIN_PATH=$HostBinPath"
    
    if (Test-Path "$QtPath\windeployqt.exe") {
        Write-Host "Using windeployqt.exe from QT_PATH"
        $WinDeployQtCmd = "windeployqt.exe"
    } else {
        Write-Host "Using windeployqt.exe from HOSTBIN_PATH"
        $WinDeployQtCmd = "$HostBinPath\windeployqt.exe --qtpaths $QtPath\qtpaths.bat"
    }
} elseif ($QtPath -match "_64") {
    $Arch = "x64"
    $WinDeployQtCmd = "windeployqt.exe"
} elseif ($QtPath -match "msvc") {
    $Arch = "x86"
    $WinDeployQtCmd = "windeployqt.exe"
} else {
    Write-Error "Unable to determine Qt architecture"
    exit 1
}

Write-Host "Detected target architecture: $Arch"

# Set signing parameters
$SignToolParams = "sign /tr http://timestamp.digicert.com /td sha256 /fd sha256 /sha1 8b9d0d682ad9459e54f05a79694bc10f9876e297 /v"

# Set build paths
$BuildRoot = "$WorkspaceRoot\build"
$SourceRoot = $WorkspaceRoot
$BuildFolder = "$BuildRoot\build-$Arch-$BuildConfigLower"
$DeployFolder = "$BuildRoot\deploy-$Arch-$BuildConfigLower"
$InstallerFolder = "$BuildRoot\installer-$Arch-$BuildConfigLower"
$SymbolsFolder = "$BuildRoot\symbols-$Arch-$BuildConfigLower"
$Version = Get-Content "$SourceRoot\app\version.txt" -Raw
$Version = $Version.Trim()

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

# Find VC redistributable DLLs
$VcRedistDllPath = & $VSWhere -latest -find "VC\Redist\MSVC\*\$Arch\Microsoft.VC*.CRT" | Select-Object -First 1

# Create output directories if they don't exist
if (-not (Test-Path $BuildRoot)) { New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null }
if (-not (Test-Path $DeployFolder)) { New-Item -ItemType Directory -Path $DeployFolder -Force | Out-Null }
if (-not (Test-Path $BuildFolder)) { New-Item -ItemType Directory -Path $BuildFolder -Force | Out-Null }
if (-not (Test-Path $InstallerFolder)) { New-Item -ItemType Directory -Path $InstallerFolder -Force | Out-Null }
if (-not (Test-Path $SymbolsFolder)) { New-Item -ItemType Directory -Path $SymbolsFolder -Force | Out-Null }

# Clean directories only if requested
if ($Clean) {
    Write-Host "Cleaning output directories (--Clean specified)" -ForegroundColor Yellow
    if (Test-Path $DeployFolder) { Remove-Item $DeployFolder -Recurse -Force }
    if (Test-Path $BuildFolder) { Remove-Item $BuildFolder -Recurse -Force }
    if (Test-Path $InstallerFolder) { Remove-Item $InstallerFolder -Recurse -Force }
    if (Test-Path $SymbolsFolder) { Remove-Item $SymbolsFolder -Recurse -Force }
    
    New-Item -ItemType Directory -Path $DeployFolder -Force | Out-Null
    New-Item -ItemType Directory -Path $BuildFolder -Force | Out-Null
    New-Item -ItemType Directory -Path $InstallerFolder -Force | Out-Null
    New-Item -ItemType Directory -Path $SymbolsFolder -Force | Out-Null
}

# Check if project needs reconfiguration
$MakefileExists = Test-Path "$BuildFolder\Makefile"
$ProFileTime = (Get-Item "$SourceRoot\moonlight-qt.pro").LastWriteTime
$MakefileTime = if ($MakefileExists) { (Get-Item "$BuildFolder\Makefile").LastWriteTime } else { [DateTime]::MinValue }
$NeedsReconfigure = (-not $MakefileExists) -or ($ProFileTime -gt $MakefileTime) -or $Clean

if ($NeedsReconfigure) {
    Write-Host "Configuring the project" -ForegroundColor Cyan
    Push-Location $BuildFolder
    try {
        & qmake.exe "$SourceRoot\moonlight-qt.pro"
        if ($LASTEXITCODE -ne 0) {
            Write-Error "QMake configuration failed"
            exit $LASTEXITCODE
        }
    } finally {
        Pop-Location
    }
} else {
    Write-Host "Project configuration is up to date" -ForegroundColor Green
}

Write-Host "Compiling Moonlight in $BuildConfigLower configuration" -ForegroundColor Cyan
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

# Check if main executable was built
$MainExePath = "$BuildFolder\app\$BuildConfigLower\Moonlight.exe"
if (-not (Test-Path $MainExePath)) {
    Write-Error "Main executable not found: $MainExePath"
    exit 1
}

Write-Host "Saving PDBs" -ForegroundColor Cyan
$PdbUpdated = $false
Get-ChildItem -Path $BuildFolder -Recurse -Filter "*.pdb" | ForEach-Object {
    $TargetPdb = Join-Path $SymbolsFolder $_.Name
    if (Copy-IfNewer -Source $_.FullName -Destination $TargetPdb -Force:$Force) {
        $PdbUpdated = $true
    }
}

# Copy external PDBs
Get-ChildItem -Path "$SourceRoot\libs\windows\lib\$Arch\*.pdb" | ForEach-Object {
    $TargetPdb = Join-Path $SymbolsFolder $_.Name
    if (Copy-IfNewer -Source $_.FullName -Destination $TargetPdb -Force:$Force) {
        $PdbUpdated = $true
    }
}

# Handle symbol store publishing
if ($env:ML_SYMBOL_STORE -and $PdbUpdated) {
    Write-Host "Publishing PDBs to symbol store: $env:ML_SYMBOL_STORE"
    & symstore add /f "$SymbolsFolder\*.pdb" /s $env:ML_SYMBOL_STORE /t Moonlight
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Symbol store publishing failed"
        exit $LASTEXITCODE
    }
} elseif ($MustDeploySymbols) {
    Write-Error "A symbol server must be specified in ML_SYMBOL_STORE for signed release builds"
    exit 1
}

if ($env:ML_SYMBOL_ARCHIVE -and $PdbUpdated) {
    Write-Host "Copying PDB ZIP to symbol archive: $env:ML_SYMBOL_ARCHIVE"
    Copy-Item "$SymbolsFolder\MoonlightDebuggingSymbols-$Arch-$Version.zip" $env:ML_SYMBOL_ARCHIVE
} elseif ($MustDeploySymbols) {
    Write-Error "A symbol archive directory must be specified in ML_SYMBOL_ARCHIVE for signed release builds"
    exit 1
}

# Incremental dependency copying
$DependenciesUpdated = $false

Write-Host "Checking DLL dependencies" -ForegroundColor Cyan
Get-ChildItem -Path "$SourceRoot\libs\windows\lib\$Arch\*.dll" | ForEach-Object {
    $TargetDll = Join-Path $DeployFolder $_.Name
    if (Copy-IfNewer -Source $_.FullName -Destination $TargetDll -Force:$Force) {
        $DependenciesUpdated = $true
    }
}

# Check AntiHooking.dll
$AntiHookingSource = "$BuildFolder\AntiHooking\$BuildConfigLower\AntiHooking.dll"
$AntiHookingTarget = "$DeployFolder\AntiHooking.dll"
if (Test-Path $AntiHookingSource) {
    if (Copy-IfNewer -Source $AntiHookingSource -Destination $AntiHookingTarget -Force:$Force) {
        $DependenciesUpdated = $true
    }
}

# Check GC mapping list
$GcMapSource = "$SourceRoot\app\SDL_GameControllerDB\gamecontrollerdb.txt"
$GcMapTarget = "$DeployFolder\gamecontrollerdb.txt"
if (Copy-IfNewer -Source $GcMapSource -Destination $GcMapTarget -Force:$Force) {
    $DependenciesUpdated = $true
}

# Handle Qt configuration
$QtConfigUpdated = $false
if ($QtPath -match "\\5\.") {
    $QtConfSource = "$SourceRoot\app\qt_qt5.conf"
    $QtConfTarget = "$DeployFolder\qt.conf"
    if (Copy-IfNewer -Source $QtConfSource -Destination $QtConfTarget -Force:$Force) {
        $QtConfigUpdated = $true
    }
}

# Check if we need to run windeployqt
$MainExeTarget = "$DeployFolder\Moonlight.exe"

# Check if Qt translation files exist (sample check for common translations)
$QtTranslationsExist = (Test-Path "$DeployFolder\translations\qt_en.qm") -and 
                       (Test-Path "$DeployFolder\translations\qt_zh_CN.qm") -and
                       (Test-Path "$DeployFolder\translations\qt_de.qm")

$NeedsQtDeploy = $Force -or $Clean -or $QtConfigUpdated -or 
                 (Test-FileNewer -SourceFile $MainExePath -TargetFile $MainExeTarget) -or 
                 (-not (Test-Path "$DeployFolder\Qt6Core.dll")) -or
                 (-not (Test-Path "$DeployFolder\platforms")) -or
                 (-not $QtTranslationsExist)

if ($NeedsQtDeploy) {
    Write-Host "Deploying Qt dependencies" -ForegroundColor Cyan
    
    # Prepare windeployqt arguments
    $WinDeployQtArgs = @()
    if ($QtPath -match "\\5\.") {
        $WinDeployQtArgs = @("--no-qmltooling", "--no-virtualkeyboard")
    } else {
        # Qt 6.5+
        $WinDeployQtArgs = @(
            "--no-system-d3d-compiler",
            "--no-system-dxc-compiler", 
            "--skip-plugin-types", "qmltooling,generic",
            "--no-ffmpeg",
            "--no-quickcontrols2fusion",
            "--no-quickcontrols2imagine", 
            "--no-quickcontrols2universal",
            "--no-quickcontrols2fusionstyleimpl",
            "--no-quickcontrols2imaginestyleimpl",
            "--no-quickcontrols2universalstyleimpl",
            "--no-quickcontrols2windowsstyleimpl"
        )
    }

    $WinDeployQtAllArgs = @(
        "--dir", $DeployFolder,
        "--$BuildConfigLower",
        "--qmldir", "$SourceRoot\app\gui",
        "--no-opengl-sw",
        "--no-compiler-runtime",
        "--no-sql"
    ) + $WinDeployQtArgs + @($MainExePath)

    if ($WinDeployQtCmd -match "^.*\\windeployqt\.exe") {
        & $WinDeployQtCmd.Split()[0] @WinDeployQtAllArgs
    } else {
        & windeployqt.exe @WinDeployQtAllArgs
    }

    if ($LASTEXITCODE -ne 0) {
        Write-Error "Qt deployment failed"
        exit $LASTEXITCODE
    }

    Write-Host "Deleting unused styles" -ForegroundColor Cyan
    # Qt 5.x directories
    $StyleDirs = @(
        "$DeployFolder\QtQuick\Controls.2\Fusion",
        "$DeployFolder\QtQuick\Controls.2\Imagine", 
        "$DeployFolder\QtQuick\Controls.2\Universal",
        # Qt 6.5+ directories
        "$DeployFolder\qml\QtQuick\Controls\Fusion",
        "$DeployFolder\qml\QtQuick\Controls\Imagine",
        "$DeployFolder\qml\QtQuick\Controls\Universal",
        "$DeployFolder\qml\QtQuick\Controls\Windows",
        "$DeployFolder\qml\QtQuick\NativeStyle"
    )

    foreach ($StyleDir in $StyleDirs) {
        if (Test-Path $StyleDir) {
            Remove-Item $StyleDir -Recurse -Force
        }
    }
} else {
    Write-Host "Qt dependencies are up to date (skipping translation file regeneration)" -ForegroundColor Green
}

# Copy main executable if newer
if (Copy-IfNewer -Source $MainExePath -Destination $MainExeTarget -Force:$Force) {
    Write-Host "Updated main executable" -ForegroundColor Yellow
}

if ($Sign) {
    Write-Host "Signing deployed binaries" -ForegroundColor Cyan
    $FilesToSign = @($MainExePath)
    $FilesToSign += Get-ChildItem -Path $DeployFolder -Recurse -Include "*.dll", "*.exe" | ForEach-Object { $_.FullName }
    
    & signtool $SignToolParams.Split() $FilesToSign
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Code signing failed"
        exit $LASTEXITCODE
    }
}

$EndTime = Get-Date
Write-Host "`nBuild completed successfully!" -ForegroundColor Green
Write-Host "Executable: $MainExeTarget" -ForegroundColor Green
Write-Host "Deploy folder: $DeployFolder" -ForegroundColor Green