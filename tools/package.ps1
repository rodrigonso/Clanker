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
    Copy-Notice $Prefix 'pcre2' (Join-Path $pcre2Source 'LICENCE.md')
    Copy-Notice $Prefix 'clanker' (Join-Path $root 'LICENSE')
    foreach ($dependency in @('pi', 'jcode')) {
        Copy-Notice $Prefix $dependency (Join-Path $root "licenses\$dependency\LICENSE")
    }
    $notices = Join-Path $root 'THIRD_PARTY_NOTICES.md'
    Require-File $notices
    Copy-Item -LiteralPath $notices -Destination (Join-Path $Prefix 'share\clanker') -Force

    $version = [IO.File]::ReadAllText((Join-Path $root 'VERSION')).Trim()
    Write-Changed (Join-Path $Prefix 'share\clanker\DISTRIBUTION.txt') @"
Clanker $version - Windows x86_64

tools/install/install.ps1 installs this archive after verifying its checksum,
adds bin to your user PATH, and enables "clanker upgrade". You can instead
extract it anywhere and run bin\clanker.exe directly. License notices
accompany the executable. The core SDK is separate.

The grep tool searches in-process and requires no external search executable.
Normal use does not download or install helper tools, libraries, or runtimes.

libcurl $curlVersion is statically linked; its license is in licenses/curl.
PCRE2 $pcre2Version (8-bit) is statically linked; its license is in licenses/pcre2.
PCRE2 source archive SHA256: $pcre2Hash
HTTPS uses native Schannel and the Windows certificate store. Windows 10 or
later is required. Clanker statically links the release CRT and needs no
VC runtime DLLs. Debug runtimes are never packaged.
Existing ANSI filesystem/process operations require ASCII workspace paths.
No system trust store, user configuration, or session data is copied.

This is an unsigned preview. Windows may show a publisher/security warning.
The SHA256 file detects corruption, not publisher identity.
Local packaging does not sign or publish archives.

The project license is in licenses/clanker/LICENSE.
Source attribution and dependency scope are in THIRD_PARTY_NOTICES.md.
"@
    if ($Config -ne 'Debug') { Audit-Runtime $bin }
}

function Audit-Runtime([string]$Bin) {
    $dumpbin = Join-Path $toolBin 'dumpbin.exe'
    $clanker = Join-Path $Bin 'clanker.exe'
    $queue = [Collections.Generic.Queue[string]]::new()
    $queue.Enqueue($clanker)
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
            if ($binary -eq $clanker -and
                $name -match '^(msvcp|vcruntime|concrt|vcomp|msvcr|ucrtbase|api-ms-win-crt-)') {
                throw "Clanker must use the static CRT (/MT): unexpected dependency $name"
            }
            $local = Join-Path $Bin $name
            if (Test-Path -LiteralPath $local) { $queue.Enqueue($local); continue }
            if ($name -match '^(api-ms-|ext-ms-)') { continue }
            if (-not (Test-Path -LiteralPath (Join-Path $env:SystemRoot "System32\$name") -PathType Leaf)) {
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
