# Sourced by the macOS build.sh; no dependency downloads or SDK files in archives.
copy_notice() {
  local destination="$1/share/clanker/licenses/$2"
  mkdir -p "$destination"
  [[ -s "$3" ]] || fail "required notice missing or empty: $3"
  cp "$3" "$destination/$(basename "$3")"
}
audit_runtime() {
  local binary="$1" metadata dependency
  command -v otool >/dev/null || fail "otool is required to audit packages"
  lipo -verify_arch "$arch" "$binary" || fail "binary does not match package architecture: $binary"
  metadata="$(otool -arch "$arch" -L "$binary")"
  while IFS= read -r dependency; do
    case "$dependency" in /System/Library/*|/usr/lib/*) ;; *) fail "non-system dependency escapes package: $dependency ($binary)" ;; esac
  done < <(printf '%s\n' "$metadata" | sed '1d; s/^[[:space:]]*//; s/ (compatibility.*//')
}
install_runtime() {
  local prefix="$1" version compiler dependency
  mkdir -p "$prefix/bin" "$prefix/share/clanker"
  cp "$build_dir/clanker" "$prefix/bin/clanker"
  copy_notice "$prefix" curl "$curl_source/COPYING"
  copy_notice "$prefix" pcre2 "$pcre2_source/LICENCE.md"
  copy_notice "$prefix" clanker "$root/LICENSE"
  for dependency in pi jcode; do
    copy_notice "$prefix" "$dependency" "$root/licenses/$dependency/LICENSE"
  done
  [[ -s "$root/THIRD_PARTY_NOTICES.md" ]] || fail "required third-party notices missing or empty"
  cp "$root/THIRD_PARTY_NOTICES.md" "$prefix/share/clanker/"
  version="$(tr -d '\r\n' < "$root/VERSION")"
  compiler="$(sed -n 's/^CMAKE_C_COMPILER:[^=]*=//p' "$build_dir/CMakeCache.txt")"
  [[ -n "$compiler" ]] || fail "CMAKE_C_COMPILER is missing from CMakeCache.txt"
  compiler="$("$compiler" --version)"
  compiler="${compiler%%$'\n'*}"
  cat > "$prefix/share/clanker/DISTRIBUTION.txt" <<EOF
Clanker $version - $os $arch
Built with: $compiler
Build host: $(uname -sr)

tools/install/install.sh installs this archive after verifying its checksum,
links bin/clanker into ~/.local/bin, and enables "clanker upgrade". You can
instead extract it anywhere and run bin/clanker directly. License notices
accompany the executable. The core SDK is separate.

The grep tool searches in-process and requires no external search executable.
Normal use does not download or install helper tools, libraries, or runtimes.

libcurl $curl_version is statically linked; its license is in licenses/curl.
Source archive SHA256: $curl_hash
PCRE2 $pcre2_version (8-bit) is statically linked; its license is in licenses/pcre2.
PCRE2 source archive SHA256: $pcre2_hash
HTTPS uses native SecureTransport and the host certificate store.
A compatible macOS release is required.
No system trust store, user configuration, or session data is copied.

This is an unsigned, unnotarized preview. macOS may block its first launch.
The SHA256 file detects corruption, not publisher identity.
Local packaging does not sign or publish archives.

The project license is in licenses/clanker/LICENSE.
Source attribution and dependency scope are in THIRD_PARTY_NOTICES.md.
EOF
}
install_clanker() {
  local prefix="$1"
  cmake --install "$build_dir" --config "$config" --prefix "$prefix" --component sdk
  if ((core == 0)); then install_runtime "$prefix"; fi
  echo "Installed: $prefix"
}
package_clanker() {
  local version name stage prefix archive hash
  version="$(tr -d '\r\n' < "$root/VERSION")"
  [[ "$version" =~ ^[0-9A-Za-z.+-]+$ ]] || fail "invalid VERSION for package filename"
  name="clanker-$version-$os-$arch"
  stage="$build_dir/package-$$"
  mkdir "$stage"; owned_dirs+=("$stage")
  prefix="$stage/$name"
  install_runtime "$prefix"
  audit_runtime "$prefix/bin/clanker"
  mkdir -p "$build_dir/packages"
  archive="$build_dir/packages/$name.tar.gz"
  pending_files+=("$archive.$$.new" "$archive.sha256.$$.new")
  tar -czf "$archive.$$.new" -C "$stage" "$name"
  mv -f "$archive.$$.new" "$archive"
  hash="$(shasum -a 256 "$archive" | awk '{print $1}')"
  printf '%s  %s.tar.gz\n' "$hash" "$name" > "$archive.sha256.$$.new"
  mv -f "$archive.sha256.$$.new" "$archive.sha256"
  echo "Archive: $archive"
}
