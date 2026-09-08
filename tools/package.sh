# Sourced by build.sh; no dependency downloads or SDK files in runtime archives.
validate_notices() {
  local path name seen='|'
  for path in "$@"; do
    [[ "$path" == /* && -f "$path" && -s "$path" ]] || fail "notice must be a nonempty absolute file: $path"
    name="$(basename "$path" | tr '[:upper:]' '[:lower:]')"
    [[ "$seen" != *"|$name|"* ]] || fail "duplicate notice basename: $name"
    seen="$seen$name|"
  done
}
validate_distribution() {
  validate_notices "${openssl_licenses[@]}"
  validate_notices "${rg_licenses[@]}"
  if [[ -n "$rg_executable" ]]; then
    [[ "$rg_executable" == /* && -f "$rg_executable" && -s "$rg_executable" && -x "$rg_executable" ]] ||
      fail "--rg-executable must be a nonempty absolute executable file"
    ((${#rg_licenses[@]})) || fail "--rg-executable requires matching --rg-license files"
  else
    ((${#rg_licenses[@]} == 0)) || fail "--rg-license requires --rg-executable"
  fi
  if ((package)) && [[ "$os" == Linux ]]; then
    ((static_openssl)) || fail "Linux packaging requires --static-openssl"
    ((${#openssl_licenses[@]})) || fail "Linux packaging requires complete --openssl-license notices"
  fi
}
copy_notice() {
  local destination="$1/share/clanker/licenses/$2"
  mkdir -p "$destination"
  [[ -s "$3" ]] || fail "required notice missing or empty: $3"
  cp "$3" "$destination/$(basename "$3")"
}
audit_runtime() {
  local binary="$1" metadata needed dependency resolved machine expected paths path
  if [[ "$os" == Darwin ]]; then
    command -v otool >/dev/null || fail "otool is required to audit packages"
    lipo -verify_arch "$arch" "$binary" || fail "binary does not match package architecture: $binary"
    metadata="$(otool -arch "$arch" -L "$binary")"
    while IFS= read -r dependency; do
      case "$dependency" in /System/Library/*|/usr/lib/*) ;; *) fail "non-system dependency escapes package: $dependency ($binary)" ;; esac
    done < <(printf '%s\n' "$metadata" | sed '1d; s/^[[:space:]]*//; s/ (compatibility.*//')
  else
    command -v readelf >/dev/null || fail "readelf is required to audit packages"
    metadata="$(readelf -h "$binary")"
    machine="$(printf '%s\n' "$metadata" | sed -n 's/^[[:space:]]*Machine:[[:space:]]*//p')"
    case "$arch" in
      x86_64) expected='Advanced Micro Devices X86-64' ;;
      aarch64|arm64) expected=AArch64 ;;
      *) fail "package architecture audit is not implemented for $arch" ;;
    esac
    [[ "$machine" == "$expected" ]] || fail "binary does not match $arch: $binary ($machine)"
    metadata="$(readelf -l "$binary")"
    path="$(printf '%s\n' "$metadata" | sed -n 's/.*Requesting program interpreter: \(.*\)\].*/\1/p')"
    case "$path" in ''|/lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
      *) fail "non-system program interpreter escapes package: $path ($binary)" ;;
    esac
    metadata="$(readelf -d "$binary")"
    paths="$(printf '%s\n' "$metadata" | sed -n '/(RPATH)\|(RUNPATH)/s/.*\[\(.*\)\].*/\1/p')"
    paths="${paths//:/$'\n'}"
    while IFS= read -r path; do
      case "$path" in ''|/lib|/lib64|/usr/lib|/usr/lib64|/lib/*|/usr/lib/*) ;;
        *) fail "non-system loader search path escapes package: $path ($binary)" ;;
      esac
    done <<< "$paths"
    needed="$(printf '%s\n' "$metadata" | sed -n '/(NEEDED)/s/.*\[\(.*\)\].*/\1/p')"
    [[ -n "$needed" ]] || return 0
    while IFS= read -r dependency; do
      case "$dependency" in
        libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libresolv.so.*|libutil.so.*|libgcc_s.so.*|ld-linux*.so.*|ld-musl-*.so.*) ;;
        *) fail "non-system dependency escapes package: $dependency ($binary)" ;;
      esac
    done <<< "$needed"
    resolved="$(ldd "$binary")" || fail "cannot resolve packaged binary dependencies: $binary"
    [[ "$resolved" != *"not found"* ]] || fail "unresolved packaged binary dependency: $binary"
    while IFS= read -r path; do
      case "$path" in /lib/*|/lib64/*|/usr/lib/*|/usr/lib64/*) ;;
        *) fail "dependency resolves outside system libraries: $path ($binary)" ;;
      esac
    done < <(printf '%s\n' "$resolved" | awk '/=> \// {print $3} /^[[:space:]]*\// {print $1}')
  fi
}
install_runtime() {
  local prefix="$1" notice version rg_notice license_notice tls_notice compiler
  mkdir -p "$prefix/bin" "$prefix/share/clanker"
  cp "$build_dir/clanker" "$prefix/bin/clanker"
  copy_notice "$prefix" curl "$curl_source/COPYING"
  rg_notice='ripgrep is not bundled. Supply rg on PATH or use the existing runtime download policy. CLANKER_OFFLINE=1 disables that download.'
  if [[ -n "$rg_executable" ]]; then
    cp "$rg_executable" "$prefix/bin/rg"
    for notice in "${rg_licenses[@]}"; do copy_notice "$prefix" ripgrep "$notice"; done
    rg_notice='ripgrep is bundled beside clanker. CLANKER_RG_PATH overrides it. Matching upstream notices are in licenses/ripgrep.'
  fi
  for notice in "${openssl_licenses[@]}"; do copy_notice "$prefix" openssl "$notice"; done
  license_notice='PUBLIC DISTRIBUTION BLOCKER: no project LICENSE exists. This archive is for local use until a maintainer-approved license is provided. No license grant or attribution has been invented.'
  if [[ -s "$root/LICENSE" ]]; then
    copy_notice "$prefix" clanker "$root/LICENSE"
    license_notice='The project license is in licenses/clanker/LICENSE.'
  else
    echo "warning: $license_notice" >&2
  fi
  version="$(tr -d '\r\n' < "$root/VERSION")"
  compiler="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")"
  [[ -n "$compiler" ]] || fail "CMAKE_C_COMPILER is missing from CMakeCache.txt"
  compiler="$("$compiler" --version)"
  compiler="${compiler%%$'\n'*}"
  if [[ "$os" == Darwin ]]; then
    tls_notice='HTTPS uses native SecureTransport and the host certificate store. A compatible macOS release is required.'
  else
    tls_notice="HTTPS uses OpenSSL (static=$static_openssl). Linux requires a compatible host libc and CA certificates; this is not a universal distribution build. Static OpenSSL notices are in licenses/openssl when bundled."
  fi
  cat > "$prefix/share/clanker/DISTRIBUTION.txt" <<EOF
Clanker $version - $os $arch
Built with: $compiler
Build host: $(uname -sr)

Extract the complete archive and add its bin directory to PATH. Keep the
directory tree intact when relocating. The core SDK is installed separately.

$rg_notice

libcurl $curl_version is statically linked; its license is in licenses/curl.
Source archive SHA256: $curl_hash
$tls_notice
No system trust store, user configuration, or session data is copied.

The SHA256 file detects corruption, not publisher identity. These scripts
do not sign or publish archives. The packager must supply a target-matching
ripgrep binary with its complete notices, including optional dependencies.

$license_notice
EOF
}
install_clanker() {
  local prefix="$1"
  cmake --install "$build_dir" --config "$config" --prefix "$prefix" --component sdk
  if ((core == 0)); then install_runtime "$prefix"; fi
  echo "Installed: $prefix"
}
package_clanker() {
  local version name stage prefix archive sha hash
  if command -v sha256sum >/dev/null; then sha=(sha256sum); else sha=(shasum -a 256); fi
  version="$(tr -d '\r\n' < "$root/VERSION")"
  [[ "$version" =~ ^[0-9A-Za-z.+-]+$ ]] || fail "invalid VERSION for package filename"
  name="clanker-$version-$os-$arch"
  stage="$build_dir/package-$$"
  mkdir "$stage"; owned_dirs+=("$stage")
  prefix="$stage/$name"
  install_runtime "$prefix"
  audit_runtime "$prefix/bin/clanker"
  [[ -z "$rg_executable" ]] || audit_runtime "$prefix/bin/rg"
  mkdir -p "$build_dir/packages"
  archive="$build_dir/packages/$name.tar.gz"
  pending_files+=("$archive.$$.new" "$archive.sha256.$$.new")
  tar -czf "$archive.$$.new" -C "$stage" "$name"
  mv -f "$archive.$$.new" "$archive"
  hash="$("${sha[@]}" "$archive" | awk '{print $1}')"
  printf '%s  %s.tar.gz\n' "$hash" "$name" > "$archive.sha256.$$.new"
  mv -f "$archive.sha256.$$.new" "$archive.sha256"
  echo "Archive: $archive"
}
