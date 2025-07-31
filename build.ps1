param(
    [Parameter(Mandatory=$true)]
    [ValidateSet("debug", "release", "signed-release")]
    [string]$BuildConfig
)

# PowerShell equivalent of build.bat and build-arch.bat combined
# Usage: .\build.ps1 -BuildConfig release

$ErrorActionPreference = "Stop"

# Initialize Visual Studio environment
$VSPath = "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
$WorkspaceRoot = "c:\games\moonlight-qt"

# Set up environment variables
$env:OLDPATH = $env:PATH
$env:PATH = "$env:OLDPATH;C:\Qt\6.7.3\msvc2019_64\bin"

# Change to workspace directory
Set-Location $WorkspaceRoot

Write-Host "Building Moonlight Qt with configuration: $BuildConfig"

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
$QmakeCmd = $null
$QmakePath = $null

try {
    $QmakePath = Get-Command qmake.bat -ErrorAction SilentlyContinue
    if ($QmakePath) {
        $QmakeCmd = "qmake.bat"
        Write-Host "Found qmake.bat"
    } else {
        $QmakePath = Get-Command qmake.exe -ErrorAction SilentlyContinue
        if ($QmakePath) {
            $QmakeCmd = "qmake.exe"
            Write-Host "Found qmake.exe"
        } else {
            Write-Error "Unable to find QMake. Did you add Qt bins to your PATH?"
            exit 1
        }
    }
} catch {
    Write-Error "Unable to find QMake. Did you add Qt bins to your PATH?"
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

Write-Host "Cleaning output directories"
if (Test-Path $DeployFolder) { Remove-Item $DeployFolder -Recurse -Force }
if (Test-Path $BuildFolder) { Remove-Item $BuildFolder -Recurse -Force }
if (Test-Path $InstallerFolder) { Remove-Item $InstallerFolder -Recurse -Force }
if (Test-Path $SymbolsFolder) { Remove-Item $SymbolsFolder -Recurse -Force }

New-Item -ItemType Directory -Path $BuildRoot -Force | Out-Null
New-Item -ItemType Directory -Path $DeployFolder -Force | Out-Null
New-Item -ItemType Directory -Path $BuildFolder -Force | Out-Null
New-Item -ItemType Directory -Path $InstallerFolder -Force | Out-Null
New-Item -ItemType Directory -Path $SymbolsFolder -Force | Out-Null

Write-Host "Configuring the project"
Push-Location $BuildFolder
try {
    if ($QmakeCmd -eq "qmake.bat") {
        cmd /c "qmake.bat `"$SourceRoot\moonlight-qt.pro`""
    } else {
        & qmake.exe "$SourceRoot\moonlight-qt.pro"
    }
    if ($LASTEXITCODE -ne 0) {
        Write-Error "QMake configuration failed"
        exit $LASTEXITCODE
    }
} finally {
    Pop-Location
}

Write-Host "Compiling Moonlight in $BuildConfigLower configuration"
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

Write-Host "Saving PDBs"
Get-ChildItem -Path $BuildFolder -Recurse -Filter "*.pdb" | ForEach-Object {
    Copy-Item $_.FullName $SymbolsFolder
}
Copy-Item "$SourceRoot\libs\windows\lib\$Arch\*.pdb" $SymbolsFolder

# Handle symbol store publishing
if ($env:ML_SYMBOL_STORE) {
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

if ($env:ML_SYMBOL_ARCHIVE) {
    Write-Host "Copying PDB ZIP to symbol archive: $env:ML_SYMBOL_ARCHIVE"
    Copy-Item "$SymbolsFolder\MoonlightDebuggingSymbols-$Arch-$Version.zip" $env:ML_SYMBOL_ARCHIVE
} elseif ($MustDeploySymbols) {
    Write-Error "A symbol archive directory must be specified in ML_SYMBOL_ARCHIVE for signed release builds"
    exit 1
}

Write-Host "Copying DLL dependencies"
Copy-Item "$SourceRoot\libs\windows\lib\$Arch\*.dll" $DeployFolder

Write-Host "Copying AntiHooking.dll"
Copy-Item "$BuildFolder\AntiHooking\$BuildConfigLower\AntiHooking.dll" $DeployFolder

Write-Host "Copying GC mapping list"
Copy-Item "$SourceRoot\app\SDL_GameControllerDB\gamecontrollerdb.txt" $DeployFolder

# Handle Qt configuration
$WinDeployQtArgs = @()
if ($QtPath -match "\\5\.") {
    Write-Host "Copying qt.conf for Qt 5"
    Copy-Item "$SourceRoot\app\qt_qt5.conf" "$DeployFolder\qt.conf"
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

Write-Host "Deploying Qt dependencies"
$WinDeployQtAllArgs = @(
    "--dir", $DeployFolder,
    "--$BuildConfigLower",
    "--qmldir", "$SourceRoot\app\gui",
    "--no-opengl-sw",
    "--no-compiler-runtime",
    "--no-sql"
) + $WinDeployQtArgs + @("$BuildFolder\app\$BuildConfigLower\Moonlight.exe")

if ($WinDeployQtCmd -match "^.*\\windeployqt\.exe") {
    & $WinDeployQtCmd.Split()[0] @WinDeployQtAllArgs
} else {
    & windeployqt.exe @WinDeployQtAllArgs
}

if ($LASTEXITCODE -ne 0) {
    Write-Error "Qt deployment failed"
    exit $LASTEXITCODE
}

Write-Host "Deleting unused styles"
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

Write-Host "Copying application binary to deployment directory"
Copy-Item "$BuildFolder\app\$BuildConfigLower\Moonlight.exe" $DeployFolder

if ($Sign) {
    Write-Host "Signing deployed binaries"
    $FilesToSign = @("$BuildFolder\app\$BuildConfigLower\Moonlight.exe")
    $FilesToSign += Get-ChildItem -Path $DeployFolder -Recurse -Include "*.dll", "*.exe" | ForEach-Object { $_.FullName }
    
    & signtool $SignToolParams.Split() $FilesToSign
    if ($LASTEXITCODE -ne 0) {
        Write-Error "Code signing failed"
        exit $LASTEXITCODE
    }
}

Write-Host "build successful $BuildFolder\app\$BuildConfigLower\Moonlight.exe" -ForegroundColor Green
