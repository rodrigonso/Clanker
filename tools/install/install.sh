#!/bin/sh
# Installs or upgrades Clanker on macOS from GitHub Releases:
#
#   curl -fsSL https://raw.githubusercontent.com/rodrigonso/Clanker/main/tools/install/install.sh | sh
#
# Settings come from the environment:
#   CLANKER_INSTALL_VERSION  X.Y.Z to install (default: newest release)
#   CLANKER_INSTALL_DIR      install prefix (default: ~/.local/share/clanker)
#   CLANKER_INSTALL_ARCHIVE  local clanker-X.Y.Z-Darwin-<arch>.tar.gz with its
#                            .sha256 alongside, instead of downloading
# The executable is linked as ~/.local/bin/clanker. Shell profiles are never
# edited; a missing PATH entry is reported instead.
set -eu

repo=rodrigonso/Clanker
fail() { printf 'error: %s\n' "$1" >&2; exit 1; }

[ "$(uname -s)" = Darwin ] || fail "this installer supports macOS; use install.ps1 on Windows"
arch="$(uname -m)"
# A shell running under Rosetta reports x86_64 on Apple Silicon.
if [ "$arch" = x86_64 ] && [ "$(sysctl -n sysctl.proc_translated 2>/dev/null || echo 0)" = 1 ]; then
  arch=arm64
fi
case "$arch" in arm64|x86_64) ;; *) fail "unsupported architecture: $arch" ;; esac
platform="Darwin-$arch"

install_dir="${CLANKER_INSTALL_DIR:-$HOME/.local/share/clanker}"
bin_dir="$HOME/.local/bin"
version="${CLANKER_INSTALL_VERSION:-}"
version="${version#v}"
archive="${CLANKER_INSTALL_ARCHIVE:-}"

if [ -n "$archive" ]; then
  [ -f "$archive" ] || fail "archive not found: $archive"
  name="$(basename "$archive")"
  version="$(printf '%s\n' "$name" | sed -n "s/^clanker-\([0-9]*\.[0-9]*\.[0-9]*\)-$platform\.tar\.gz\$/\1/p")"
  [ -n "$version" ] || fail "unexpected archive name: $name"
elif [ -z "$version" ]; then
  version="$(curl -fsSL -H 'Accept: application/vnd.github+json' \
    "https://api.github.com/repos/$repo/releases?per_page=1" |
    sed -n 's/.*"tag_name"[[:space:]]*:[[:space:]]*"v\{0,1\}\([^"]*\)".*/\1/p' | head -n 1)"
  [ -n "$version" ] || fail "no Clanker release has been published yet"
fi
printf '%s\n' "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+$' || fail "invalid version: $version"

if [ -e "$install_dir" ] && [ ! -f "$install_dir/share/clanker/DISTRIBUTION.txt" ]; then
  fail "refusing to replace $install_dir: it is not a Clanker install"
fi
if [ -x "$install_dir/bin/clanker" ] &&
   [ "$("$install_dir/bin/clanker" --version 2>/dev/null || true)" = "clanker $version" ]; then
  printf 'Clanker %s is already installed in %s.\n' "$version" "$install_dir"
  exit 0
fi

work="$(mktemp -d "${TMPDIR:-/tmp}/clanker-install.XXXXXX")"
trap 'rm -rf "$work"' EXIT
name="clanker-$version-$platform.tar.gz"
if [ -z "$archive" ]; then
  base="https://github.com/$repo/releases/download/v$version"
  archive="$work/$name"
  printf 'Downloading Clanker %s...\n' "$version"
  curl -fsSL -o "$archive" "$base/$name"
  curl -fsSL -o "$archive.sha256" "$base/$name.sha256"
fi

expected="$(awk -v name="$name" 'NF == 2 && $2 == name && $1 ~ /^[0-9a-f]+$/ && length($1) == 64 { print $1 }' "$archive.sha256")"
[ -n "$expected" ] || fail "invalid checksum file for $name"
actual="$(shasum -a 256 "$archive" | awk '{ print $1 }')"
[ "$actual" = "$expected" ] || fail "checksum mismatch for $name"

tar -xzf "$archive" -C "$work"
staged="$work/clanker-$version-$platform"
[ -x "$staged/bin/clanker" ] || fail "$name does not contain bin/clanker"

# Stage beside the destination so the final swap is a same-volume rename; a
# running clanker keeps its open inode either way.
mkdir -p "$(dirname "$install_dir")"
rm -rf "$install_dir.new" "$install_dir.old"
mv "$staged" "$install_dir.new"
if [ -e "$install_dir" ]; then mv "$install_dir" "$install_dir.old"; fi
mv "$install_dir.new" "$install_dir"
rm -rf "$install_dir.old"

mkdir -p "$bin_dir"
ln -sf "$install_dir/bin/clanker" "$bin_dir/clanker"
printf 'Installed Clanker %s to %s.\n' "$version" "$install_dir"
case ":$PATH:" in
  *":$bin_dir:"*) printf "Run 'clanker' from your project directory.\n" ;;
  *) printf '%s is not on your PATH. Add this line to your shell profile (e.g. ~/.zshrc):\n  export PATH="%s:$PATH"\n' "$bin_dir" "$bin_dir" ;;
esac
