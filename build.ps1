#requires -Version 5.0
<#
.SYNOPSIS
    Configures and builds Clank with CMake + Ninja using the Visual Studio toolchain.

.DESCRIPTION
    Locates a Visual Studio installation (via vswhere), sets up the vcvars64
    environment, and runs `cmake` configure + build. libcurl is fetched
    automatically by CMake's FetchContent on first configure.

.PARAMETER Config
    CMake build type: Release (default), Debug, RelWithDebInfo, or MinSizeRel.

.PARAMETER BuildDir
    Build directory (default: build), relative to the repo root unless absolute.

.PARAMETER Target
    Optional specific target to build (e.g. clank, clank_tests). Builds clank by default.

.PARAMETER Clean
    Delete the build directory before configuring for a fresh build.

.PARAMETER Test
    Build and run the test harness (clank_tests) after a successful build.

.PARAMETER TestArgs
    Arguments forwarded to clank_tests, e.g. suite names, --list, or --filter.
    Implies -Test. With no arguments the full offline sweep runs.

.EXAMPLE
    .\build.ps1
.EXAMPLE
    .\build.ps1 -Config Debug -Test
.EXAMPLE
    .\build.ps1 -Test -TestArgs json,session
.EXAMPLE
    .\build.ps1 -Target clank -Clean
#>
[CmdletBinding()]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build',
    [ValidatePattern('^[A-Za-z0-9_.:+-]+$')]
    [string]$Target,
    [switch]$Clean,
    [switch]$Test,
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$TestArgs
)

# Passing -TestArgs alone implies -Test.
if ($TestArgs -and -not $Test) { $Test = $true }

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

function Get-CMakeCacheValue {
    param(
        [string]$CachePath,
        [string]$Name
    )
    $match = Select-String -Path $CachePath -Pattern "^$([regex]::Escape($Name)):[^=]+=(.*)$" |
        Select-Object -First 1
    if ($match -and $match.Matches.Count -gt 0) {
        return $match.Matches[0].Groups[1].Value
    }
    return $null
}

$cachePath = Join-Path $buildPath 'CMakeCache.txt'
$ninjaPath = Join-Path $buildPath 'build.ninja'
$needsConfigure = -not (Test-Path $cachePath) -or -not (Test-Path $ninjaPath)
if (-not $needsConfigure) {
    $cachedSource = Get-CMakeCacheValue $cachePath 'CMAKE_HOME_DIRECTORY'
    if ($cachedSource -and
        -not [System.IO.Path]::GetFullPath($cachedSource).Equals(
            [System.IO.Path]::GetFullPath($root),
            [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Build directory '$buildPath' belongs to '$cachedSource'. Choose another -BuildDir or use -Clean."
    }

    $cachedGenerator = Get-CMakeCacheValue $cachePath 'CMAKE_GENERATOR'
    if ($cachedGenerator -ne 'Ninja') {
        throw "Build directory '$buildPath' uses generator '$cachedGenerator'. Use -Clean to recreate it with Ninja."
    }

    $cachedConfig = Get-CMakeCacheValue $cachePath 'CMAKE_BUILD_TYPE'
    $needsConfigure = $cachedConfig -ne $Config
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

$targets = [System.Collections.Generic.List[string]]::new()
$targets.Add($(if ($Target) { $Target } else { 'clank' }))
if ($Test -and -not $targets.Contains('clank_tests')) {
    $targets.Add('clank_tests')
}

$cmakeExe = Join-Path $vsPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $cmakeExe)) {
    $cmakeExe = (Get-Command cmake -ErrorAction Stop).Source
}

$hasBuildWork = $true
if (-not $needsConfigure) {
    $dryRunArgs = @('--build', $buildPath, '--target') + $targets + @('--', '-n')
    $dryRunOutput = & $cmakeExe @dryRunArgs 2>&1
    $hasBuildWork = $LASTEXITCODE -ne 0 -or
        -not ($dryRunOutput -match '^ninja: no work to do\.$')
}

# Build a single cmd.exe invocation so the vcvars64 environment persists across steps.
$steps = @(
    "set `"PATH=$($prepend -join ';');%PATH%`"",
    "call `"$vcvars`" >nul")
if ($needsConfigure) {
    $steps += "cmake -S `"$root`" -B `"$buildPath`" -G Ninja -DCMAKE_BUILD_TYPE=$Config"
}
$steps += "cmake --build `"$buildPath`" --target $($targets -join ' ')"

$action = if ($needsConfigure) { 'Configuring + building' } else { 'Building' }
Write-Host "$action ($Config): $($targets -join ', ')" -ForegroundColor Cyan
if ($hasBuildWork) {
    & $env:ComSpec /c ($steps -join ' && ')
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Build failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit $LASTEXITCODE
    }
} else {
    $dryRunOutput | Write-Output
}
Write-Host "Build succeeded -> $buildPath" -ForegroundColor Green

if ($Test) {
    $tests = Join-Path $buildPath 'clank_tests.exe'
    $suiteLabel = if ($TestArgs) { $TestArgs -join ' ' } else { 'all offline suites' }
    Write-Host "Running tests ($suiteLabel)..." -ForegroundColor Cyan
    if ($TestArgs) { & $tests @TestArgs } else { & $tests }
    if ($LASTEXITCODE -ne 0) {
        Write-Host "Tests failed (exit $LASTEXITCODE)." -ForegroundColor Red
        exit $LASTEXITCODE
    }
    Write-Host "Tests passed." -ForegroundColor Green
}
