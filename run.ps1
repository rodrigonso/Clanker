#requires -Version 5.0
<#
.SYNOPSIS
    Incrementally builds Clanker and runs the resulting executable.

.DESCRIPTION
    Delegates the build to build.ps1 (Visual Studio toolchain + CMake/Ninja),
    which is already incremental: it reconfigures only when the cache is
    missing or the build type changed, and skips the build entirely when Ninja
    reports no work to do. Only the `clanker` target is built, so the test
    harness is not compiled.

    The executable runs in your current working directory with stdin/stdout
    left attached, so the full-screen TUI gets a real console.

.PARAMETER Config
    CMake build type: Release (default), Debug, RelWithDebInfo, or MinSizeRel.

.PARAMETER BuildDir
    Build directory (default: build), relative to the repo root unless absolute.

.PARAMETER NoBuild
    Skip the build and run the executable already in the build directory.

.PARAMETER ClankerArgs
    Arguments forwarded verbatim to clanker.exe.

    PowerShell binds a leading-dash token to this script's own parameters when
    it is an unambiguous prefix, so `-c` would be read as `-Config`. Put `--`
    before Clanker's own short options to pass them through untouched.

.EXAMPLE
    .\run.ps1
    Build and launch the interactive TUI.

.EXAMPLE
    .\run.ps1 -- -c
    Continue the most recent session for this project.

.EXAMPLE
    .\run.ps1 -Config Debug -- -p "summarize this repo"

.EXAMPLE
    .\run.ps1 -NoBuild --help
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [string]$BuildDir = 'build',
    [switch]$NoBuild,
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)]
    [string[]]$ClankerArgs
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

# Resolve the build directory the same way build.ps1 does.
$buildPath = if ([System.IO.Path]::IsPathRooted($BuildDir)) { $BuildDir } else { Join-Path $root $BuildDir }
$exe = Join-Path $buildPath 'clanker.exe'

if (-not $NoBuild) {
    & (Join-Path $root 'build.ps1') -Config $Config -BuildDir $BuildDir -Target clanker
    # build.ps1 exits non-zero on configure/compile failure; throws otherwise.
    if ($null -ne $LASTEXITCODE -and $LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

if (-not (Test-Path $exe)) {
    $hint = if ($NoBuild) { "Drop -NoBuild to build it" } else { "Check -BuildDir and -Config" }
    throw "clanker.exe not found at '$exe'. $hint."
}

Write-Host "Running $exe" -ForegroundColor Cyan
if ($ClankerArgs) { & $exe @ClankerArgs } else { & $exe }
exit $LASTEXITCODE
