#requires -Version 5.0
<#
.SYNOPSIS
    Configures and builds mini-agent with CMake + Ninja using the Visual Studio toolchain.

.DESCRIPTION
    Locates a Visual Studio installation (via vswhere), sets up the vcvars64
    environment, and runs `cmake` configure + build. libcurl is fetched
    automatically by CMake's FetchContent on first configure.

.PARAMETER Config
    CMake build type: Release (default), Debug, RelWithDebInfo, or MinSizeRel.

.PARAMETER BuildDir
    Build directory (default: build), relative to the repo root unless absolute.

.PARAMETER Target
    Optional specific target to build (e.g. mini-agent, ma_tests). Builds all by default.

.PARAMETER Clean
    Delete the build directory before configuring for a fresh build.

.PARAMETER Test
    Run the unit test suite (ma_tests) after a successful build.

.EXAMPLE
    .\build.ps1
.EXAMPLE
    .\build.ps1 -Config Debug -Test
.EXAMPLE
    .\build.ps1 -Target mini-agent -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build',
    [string]$Target,
    [switch]$Clean,
    [switch]$Test
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

$vsInstallerDir = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer'

function Find-VsInstall {
    $vswhere = Join-Path $vsInstallerDir 'vswhere.exe'
    if (-not (Test-Path $vswhere)) {
        throw "vswhere.exe not found at '$vswhere'. Install Visual Studio (with the C++ workload) or the Build Tools."
    }
    $vcTools = 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64'
    # Prefer a stable install; fall back to prerelease (Insiders/Preview).
    foreach ($pre in @(@(), @('-prerelease'))) {
        $path = & $vswhere -latest @pre -products * -requires $vcTools -property installationPath 2>$null |
            Select-Object -First 1
        if ($path -and (Test-Path $path)) { return $path }
    }
    throw "No Visual Studio install with the C++ toolchain (VC.Tools.x86.x64) was found."
}

$vsPath = Find-VsInstall
$vcvars = Join-Path $vsPath 'VC\Auxiliary\Build\vcvars64.bat'
if (-not (Test-Path $vcvars)) {
    throw "vcvars64.bat not found at '$vcvars'."
}
Write-Host "Visual Studio: $vsPath" -ForegroundColor Cyan

# Resolve the build directory (absolute paths are honored as-is).
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }

if ($Clean -and (Test-Path $buildPath)) {
    Write-Host "Cleaning $buildPath" -ForegroundColor Yellow
    Remove-Item -Recurse -Force $buildPath
}

# Directories to prepend to PATH *before* invoking vcvars64:
#   - the VS Installer dir, because vcvars64.bat calls `vswhere` unqualified
#   - the VS-bundled CMake/Ninja, so a standalone install isn't required
# All prepends must happen in ONE `set` before vcvars: cmd expands %PATH% at parse
# time for the whole `&&` chain, so a second `set ...%PATH%...` placed after vcvars
# would discard the compiler paths that vcvars adds.
$prepend = @($vsInstallerDir)
foreach ($p in @(
        (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin'),
        (Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja'))) {
    if (Test-Path $p) { $prepend += $p }
}

# Build a single cmd.exe invocation so the vcvars64 environment persists across steps.
$steps = @(
    "set `"PATH=$($prepend -join ';');%PATH%`"",
    "call `"$vcvars`" >nul",
    "cmake -S `"$root`" -B `"$buildPath`" -G Ninja -DCMAKE_BUILD_TYPE=$Config")
if ($Target) {
    $steps += "cmake --build `"$buildPath`" --target $Target"
} else {
    $steps += "cmake --build `"$buildPath`""
}

Write-Host "Configuring + building ($Config)..." -ForegroundColor Cyan
& $env:ComSpec /c ($steps -join ' && ')
if ($LASTEXITCODE -ne 0) {
    Write-Host "Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
    exit $LASTEXITCODE
}
Write-Host "Build succeeded -> $buildPath" -ForegroundColor Green

if ($Test) {
    $tests = Join-Path $buildPath 'ma_tests.exe'
    if (-not (Test-Path $tests)) {
        Write-Host "ma_tests.exe not found (was it built?). Run without -Target or build the ma_tests target." -ForegroundColor Red
        exit 1
    }
    Write-Host "Running tests..." -ForegroundColor Cyan
    & $tests
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Tests failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "Tests passed." -ForegroundColor Green
}
