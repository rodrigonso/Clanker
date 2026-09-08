# Runtime installation and release packaging, using build.ps1's selected toolchain.
function Write-Changed([string]$Path, [string]$Text) {
    if (-not (Test-Path -LiteralPath $Path) -or [IO.File]::ReadAllText($Path) -cne $Text) {
        [IO.File]::WriteAllText($Path, $Text, [Text.UTF8Encoding]::new($false))
    }
}
function Hash-File([string]$Path) {
    $sha = [Security.Cryptography.SHA256]::Create()
    $stream = [IO.File]::OpenRead($Path)
    try { return ([BitConverter]::ToString($sha.ComputeHash($stream))).Replace('-', '').ToLowerInvariant() }
    finally { $stream.Dispose(); $sha.Dispose() }
}
function Require-File([string]$Path) {
    if (-not [IO.Path]::IsPathRooted($Path) -or
        -not (Test-Path -LiteralPath $Path -PathType Leaf) -or
        (Get-Item -LiteralPath $Path).Length -eq 0) {
        throw "Expected a nonempty absolute file path: $Path"
    }
}
function Assert-DistributionOptions {
    if ([bool]$RgExecutable -ne [bool]$RgLicense) { throw '-RgExecutable and -RgLicense must be supplied together.' }
    if ($RgExecutable) {
        Require-File $RgExecutable
        $names = @{}
        foreach ($path in $RgLicense) {
            Require-File $path
            $name = [IO.Path]::GetFileName($path)
            if ($names.ContainsKey($name)) { throw "Duplicate ripgrep notice filename: $name" }
            $names[$name] = $true
        }
    }
}

function Copy-Notice([string]$Prefix, [string]$Dependency, [string]$Path) {
    Require-File $Path
    $destination = Join-Path $Prefix "share\clanker\licenses\$Dependency"
    New-Item -ItemType Directory -Force $destination | Out-Null
    Copy-Item -LiteralPath $Path -Destination $destination -Force
}

function Install-Runtime([string]$Prefix) {
    $bin = Join-Path $Prefix 'bin'
    New-Item -ItemType Directory -Force $bin | Out-Null
    Copy-Item -LiteralPath (Join-Path $build 'clanker.exe') -Destination $bin -Force
    Copy-Notice $Prefix 'curl' (Join-Path $curlSource 'COPYING')
    $rgNotice = 'ripgrep is not bundled. Supply rg on PATH or use the existing runtime download policy. CLANKER_OFFLINE=1 disables that download.'
    if ($RgExecutable) {
        Copy-Item -LiteralPath $RgExecutable -Destination (Join-Path $bin 'rg.exe') -Force
        foreach ($path in $RgLicense) { Copy-Notice $Prefix 'ripgrep' $path }
        $rgNotice = 'ripgrep is bundled beside clanker. CLANKER_RG_PATH overrides it. Its matching upstream notices are in licenses/ripgrep.'
    }
    $license = Join-Path $root 'LICENSE'
    $licenseNotice = 'PUBLIC DISTRIBUTION BLOCKER: no project LICENSE exists. This archive is for local use until a maintainer-approved license is provided. No license grant or attribution has been invented.'
    if (Test-Path -LiteralPath $license -PathType Leaf) {
        Copy-Notice $Prefix 'clanker' $license
        $licenseNotice = 'The project license is in licenses/clanker/LICENSE.'
    } else { Write-Warning $licenseNotice }

    $version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
    Write-Changed (Join-Path $Prefix 'share\clanker\DISTRIBUTION.txt') @"
Clanker $version - Windows x86_64

Extract the complete archive and add its bin directory to PATH. Keep the
directory tree intact when relocating. The core SDK is installed separately.

$rgNotice

libcurl $curlVersion is statically linked; its license is in licenses/curl.
HTTPS uses native Schannel and the Windows certificate store. Windows 10 or
later is required, including the OS Universal CRT. Release VC runtime DLLs
come only from Visual Studio's redistributable directory and remain subject
to Microsoft's redistribution terms. Debug runtimes are never packaged.
Existing ANSI filesystem/process operations require ASCII workspace paths.
No system trust store, user configuration, or session data is copied.

The SHA256 file detects corruption, not publisher identity. These scripts
do not sign or publish archives. The packager must supply a target-matching
ripgrep binary with its complete notices, including optional dependencies.

$licenseNotice
"@
    if ($Config -ne 'Debug') { Audit-Runtime $bin }
}

function Audit-Runtime([string]$Bin) {
    $dumpbin = Join-Path $toolBin 'dumpbin.exe'
    $redists = @(Get-ChildItem -LiteralPath (Join-Path $vs 'VC\Redist\MSVC') -Directory |
        Sort-Object Name -Descending |
        ForEach-Object {
            $x64 = Join-Path $_.FullName 'x64'
            if (Test-Path -LiteralPath $x64) {
                Get-ChildItem -LiteralPath $x64 -Directory -Filter 'Microsoft.VC*.CRT'
            }
        })
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue((Join-Path $Bin 'clanker.exe'))
    if ($RgExecutable) { $queue.Enqueue((Join-Path $Bin 'rg.exe')) }
    $seen = @{}
    while ($queue.Count) {
        $binary = $queue.Dequeue()
        if ($seen.ContainsKey($binary)) { continue }
        $seen[$binary] = $true
        $headers = & $dumpbin /nologo /headers $binary
        if ($LASTEXITCODE -ne 0 -or -not ($headers -match '^\s*8664\s')) {
            throw "Package requires an x86_64 PE binary: $binary"
        }
        $imports = & $dumpbin /nologo /dependents $binary
        if ($LASTEXITCODE -ne 0) { throw "Cannot inspect dependencies of $binary" }
        foreach ($line in $imports) {
            if ($line -notmatch '^\s+([A-Za-z0-9_.+-]+\.dll)\s*$') { continue }
            $name = $Matches[1]
            $local = Join-Path $Bin $name
            if (Test-Path -LiteralPath $local) { $queue.Enqueue($local); continue }
            if ($name -match '^(api-ms-|ext-ms-)') { continue }
            if ($name -match '^(msvcp|vcruntime|concrt|vcomp|msvcr)[0-9]') {
                $runtime = @($redists | ForEach-Object { Join-Path $_.FullName $name } |
                    Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1)
                if (-not $runtime.Count) { throw "No redistributable release runtime found for $name" }
                Copy-Item -LiteralPath $runtime[0] -Destination $local -Force
                $queue.Enqueue($local)
            } elseif (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$name") -PathType Leaf)) {
                throw "A non-system dependency would escape the archive: $name (from $binary)"
            }
        }
    }
}

function Package-Clanker {
    $version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
    $name = "clanker-$version-Windows-x86_64"
    $packages = Join-Path $build 'packages'
    New-Item -ItemType Directory -Force $packages | Out-Null
    # A fresh, owned staging directory prevents stale optional files leaking in.
    $stage = Join-Path $packages ("stage-" + [Guid]::NewGuid().ToString('N'))
    $prefix = Join-Path $stage $name
    $archive = Join-Path $packages "$name.zip"
    $temporary = "$archive.$PID.tmp.zip"
    New-Item -ItemType Directory -Force $prefix | Out-Null
    try {
        Install-Runtime $prefix
        Invoke-Checked 'tar.exe' @('-a', '-cf', $temporary, '-C', $stage, $name)
        Move-Item -LiteralPath $temporary -Destination $archive -Force
        Write-Changed "$archive.sha256" ((Hash-File $archive) + "  $name.zip`n")
        Write-Host "Archive: $archive"
    } finally {
        if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary -Force }
        Remove-Item -LiteralPath $stage -Recurse -Force
    }
}
