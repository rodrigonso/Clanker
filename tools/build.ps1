#requires -Version 5.0
<#
.SYNOPSIS
    Configures and builds Clanker with CMake and the Visual Studio toolchain.
.DESCRIPTION
    build.bat accepts the same options. Core-only builds never fetch networking
    or regex dependencies. Tests remain offline unless explicitly selected otherwise.
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Config = 'Release',
    [string]$BuildDir,
    [ValidateSet('clanker', 'clanker_core', 'clanker_agent', 'clanker_tests', 'clanker_core_example')]
    [string]$Target,
    [switch]$Clean,
    [switch]$Test,
    [switch]$CoreOnly,
    [switch]$Examples,
    [string]$InstallPrefix,
    [switch]$Package,
    [string]$CurlArchive,
    [string]$Pcre2Archive,
    [switch]$Help,
    [string[]]$TestArgs
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if ($Help) {
    Write-Output @'
usage: build.bat [-Config Release|Debug|RelWithDebInfo|MinSizeRel]
       [-CoreOnly] [-Examples] [-Test] [-TestArgs session,tools]
       [-BuildDir path] [-Target name] [-Clean] [-InstallPrefix path]
       [-Package] [-CurlArchive path] [-Pcre2Archive path]
Requires Visual Studio C++ tools, CMake 3.24+ and Ninja (included with
Visual Studio's C++ CMake tools). -Clean rebuilds without deleting the cache.
'@
    return
}
function Invoke-Checked([string]$Exe, [string[]]$Arguments) {
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Exe failed (exit $LASTEXITCODE)." }
}
if ($TestArgs) {
    $TestArgs = @($TestArgs | ForEach-Object { $_ -split ',' } | Where-Object { $_ -ne '--' })
    $Test = $true
}
if ($Package -and ($CoreOnly -or $Config -eq 'Debug')) {
    throw '-Package requires a non-Debug CLI build.'
}
if ($CoreOnly -and $Target -eq 'clanker') { throw '-CoreOnly cannot build the CLI.' }
if (-not $Target) { $Target = if ($CoreOnly) { 'clanker_core' } else { 'clanker' } }
if ($Target -eq 'clanker_agent') { $Target = 'clanker_core' }
if ($Package -or ($InstallPrefix -and -not $CoreOnly)) { $Target = 'clanker' }
if (-not $BuildDir) { $BuildDir = if ($CoreOnly) { 'out\core' } else { 'build' } }
$root = Split-Path -Parent $PSScriptRoot
$build = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($BuildDir)) {
    $BuildDir
} else { Join-Path $root $BuildDir }))
. (Join-Path $PSScriptRoot 'package.ps1')

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (-not (Test-Path -LiteralPath $vswhere)) { throw 'Install the Visual Studio Desktop development with C++ workload.' }
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    $vs = & $vswhere -latest -prerelease -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) { throw 'Visual Studio C++ build tools were not found.' }
$cmakeTools = Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake'
$env:PATH = "$(Join-Path $cmakeTools 'CMake\bin');$(Join-Path $cmakeTools 'Ninja');$(Split-Path -Parent $vswhere);$env:PATH"
$cmake = (Get-Command cmake.exe -ErrorAction Stop).Source
Get-Command ninja.exe -ErrorAction Stop | Out-Null
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$environment = & $env:ComSpec /d /c "call `"$vcvars`" >nul && set"
if ($LASTEXITCODE -ne 0) { throw 'Visual Studio toolchain initialization failed.' }
foreach ($line in $environment) {
    if ($line -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2], 'Process') }
}
$toolBin = Join-Path $env:VCToolsInstallDir 'bin\Hostx64\x64'

New-Item -ItemType Directory -Force $build | Out-Null
$lock = [IO.File]::Open((Join-Path $build '.clanker-build.lock'), 'OpenOrCreate', 'ReadWrite', 'None')
try {
    $buildTests = $Test -or $Target -eq 'clanker_tests'
    $buildExample = $Examples -or $Target -eq 'clanker_core_example'
    $configure = @('-S', $root, '-B', $build, '-G', 'Ninja',
        "-DCMAKE_BUILD_TYPE=$Config",
        "-DCLANKER_BUILD_CLI=$(-not $CoreOnly)",
        "-DCLANKER_BUILD_TESTS=$buildTests",
        "-DCLANKER_BUILD_EXAMPLES=$buildExample")
    if (-not $CoreOnly -and $PSBoundParameters.ContainsKey('CurlArchive')) {
        if ($CurlArchive) { $CurlArchive = [IO.Path]::GetFullPath($CurlArchive) }
        $configure += "-DCLANKER_CURL_ARCHIVE:FILEPATH=$CurlArchive"
    }
    if (-not $CoreOnly -and $PSBoundParameters.ContainsKey('Pcre2Archive')) {
        if ($Pcre2Archive) { $Pcre2Archive = [IO.Path]::GetFullPath($Pcre2Archive) }
        $configure += "-DCLANKER_PCRE2_ARCHIVE:FILEPATH=$Pcre2Archive"
    }
    Invoke-Checked $cmake $configure
    $targets = @($Target)
    if ($buildTests) { $targets += 'clanker_tests' }
    if ($buildExample) { $targets += 'clanker_core_example' }
    if ($InstallPrefix) { $targets += 'clanker_core' }
    $arguments = @('--build', $build, '--config', $Config, '--parallel',
        '--target') + @($targets | Select-Object -Unique)
    if ($Clean) { $arguments += '--clean-first' }
    Invoke-Checked $cmake $arguments

    if ($Test) {
        Invoke-Checked (Join-Path $build 'clanker_tests.exe') $TestArgs
        if (-not $TestArgs) {
            Invoke-Checked $cmake @('-P', (Join-Path $build 'generated\test_platform.cmake'))
        }
    }
    if ($Examples) { Invoke-Checked (Join-Path $build 'examples\core\clanker_core_example.exe') @() }
    if ($InstallPrefix -or $Package) {
        if (-not $CoreOnly) {
            $curlInfo = [IO.File]::ReadAllLines((Join-Path $build 'clanker-curl.txt'))
            $curlSource = $curlInfo[0]
            $curlVersion = $curlInfo[1]
            $pcre2Info = [IO.File]::ReadAllLines((Join-Path $build 'clanker-pcre2.txt'))
            if ($pcre2Info.Count -ne 3 -or -not [IO.Path]::IsPathRooted($pcre2Info[0]) -or
                [string]::IsNullOrWhiteSpace($pcre2Info[1]) -or $pcre2Info[2] -notmatch '^[0-9a-fA-F]{64}$') {
                throw 'Invalid CMake PCRE2 metadata.'
            }
            $pcre2Source = $pcre2Info[0]
            $pcre2Version = $pcre2Info[1]
            $pcre2Hash = $pcre2Info[2]
        }
        if ($InstallPrefix) {
            $prefix = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($InstallPrefix)) {
                $InstallPrefix
            } else { Join-Path $root $InstallPrefix }))
            Invoke-Checked $cmake @('--install', $build, '--config', $Config, '--prefix', $prefix, '--component', 'sdk')
            if (-not $CoreOnly) { Install-Runtime $prefix }
        }
        if ($Package) { Package-Clanker }
    }
    Write-Host "Ready: $build"
} finally { $lock.Dispose() }
