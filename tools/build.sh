#!/usr/bin/env bash
# Bash 3.2 compatible.
set -eo pipefail

root="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
config=Release
build_dir= target= install_prefix= curl_archive= openssl_prefix= rg_executable=
core=0 examples=0 run_tests=0 package=0 clean=0 static_openssl=0
test_args=() openssl_licenses=() rg_licenses=()
fail() { echo "error: $*" >&2; exit 2; }
while (($#)); do
  case "$1" in
    --core-only) core=1; shift ;;
    --examples) examples=1; shift ;;
    --test) run_tests=1; shift ;;
    --package) package=1; shift ;;
    --clean) clean=1; shift ;;
    --static-openssl) static_openssl=1; shift ;;
    --config|--build-dir|--target|--install-prefix|--curl-archive|--openssl-prefix|--openssl-license|--rg-executable|--rg-license)
      (($# >= 2)) && [[ -n "$2" && "$2" != --* ]] || fail "$1 requires a value"
      case "$1" in
        --config) config="$2" ;;
        --build-dir) build_dir="$2" ;;
        --target) target="$2" ;;
        --install-prefix) install_prefix="$2" ;;
        --curl-archive) curl_archive="$2" ;;
        --openssl-prefix) openssl_prefix="$2" ;;
        --openssl-license) openssl_licenses+=("$2") ;;
        --rg-executable) rg_executable="$2" ;;
        --rg-license) rg_licenses+=("$2") ;;
      esac
      shift 2 ;;
    --) shift; test_args=("$@"); run_tests=1; break ;;
    -h|--help)
      cat <<'HELP'
usage: bash tools/build.sh [--core-only] [--examples] [--test] [--clean]
       [--config Release|Debug|RelWithDebInfo|MinSizeRel] [--build-dir path]
       [--target clanker|clanker_core|clanker_agent|clanker_tests|clanker_core_example]
       [--install-prefix path] [--package] [--curl-archive path]
       [--openssl-prefix path] [--static-openssl] [--openssl-license absolute-file]...
       [--rg-executable absolute-file --rg-license absolute-file]... [-- suite...]

Requires CMake 3.24+, a C/C++17 toolchain and a CMake build tool (make or Ninja).
CLI builds need OpenSSL development files on Linux; macOS uses SecureTransport.
CC, CXX, CPPFLAGS, CFLAGS, CXXFLAGS, LDFLAGS and JOBS are honored. CMake caches
toolchain settings; use a new build directory when changing compilers.
Relative build/install paths are relative to the repository. Tests retain
the caller's working directory. --clean uses CMake's --clean-first without
deleting the build directory or downloaded dependencies.
Concurrent writers fail fast. An interrupted process can leave a *.lock.d
directory; remove it only after confirming that no builder still owns it.
Packages contain only the runtime. Linux packaging requires static OpenSSL
and its complete notices. No OpenSSL or ripgrep is downloaded automatically.
HELP
      exit 0 ;;
    *) fail "unknown argument: $1" ;;
  esac
done
case "$config" in
  Release|Debug|RelWithDebInfo|MinSizeRel) ;;
  *) fail "invalid build configuration: $config" ;;
esac
os="$(uname -s)"; arch="$(uname -m)"
case "$os" in Linux|Darwin) ;; *) fail "unsupported platform: $os" ;; esac
if [[ "$os" != Linux && ( -n "$openssl_prefix" || "$static_openssl" == 1 || ${#openssl_licenses[@]} != 0 ) ]]; then
  fail "OpenSSL options are Linux-only; macOS uses SecureTransport"
fi
if [[ -z "$build_dir" ]]; then
  if ((core)); then build_dir=out/core; else build_dir=build; fi
fi
[[ "$build_dir" == /* ]] || build_dir="$root/$build_dir"
[[ -z "$install_prefix" || "$install_prefix" == /* ]] || install_prefix="$root/$install_prefix"
if [[ -z "$target" ]]; then
  if ((core)); then target=clanker_core; else target=clanker; fi
fi
want_tests=$run_tests; want_example=$examples
case "$target" in
  clanker) ((core == 0)) || fail "clanker is unavailable with --core-only" ;;
  clanker_agent) target=clanker_core ;;
  clanker_core) ;;
  clanker_tests) want_tests=1 ;;
  clanker_core_example) want_example=1 ;;
  *) fail "unknown target: $target" ;;
esac
if ((package)); then
  ((core == 0)) || fail "--package requires the CLI; omit --core-only"
  [[ "$config" != Debug ]] || fail "Debug runtimes cannot be packaged"
fi
[[ -z "${JOBS:-}" || "$JOBS" =~ ^[1-9][0-9]*$ ]] || fail "JOBS must be a positive integer"
source "$root/tools/package.sh"
validate_distribution
command -v cmake >/dev/null || fail "required tool not found: cmake"

mkdir -p "$build_dir"
build_dir="$(cd "$build_dir" && pwd)"
lock_dir="$build_dir/build.lock.d"
mkdir "$lock_dir" 2>/dev/null || fail "lock busy or not writable: $lock_dir (retry after the other build exits; remove an abandoned lock only after confirming no builder is running)"
owned_dirs=() pending_files=()
cleanup() {
  local status=$? path
  set +e
  for path in "${pending_files[@]}"; do rm -f -- "$path"; done
  for path in "${owned_dirs[@]}"; do rm -rf -- "$path"; done
  rm -f "$lock_dir/owner"
  rmdir "$lock_dir"
  return "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
printf '%s\n' "$$" > "$lock_dir/owner"

cli_option=ON; ((core == 0)) || cli_option=OFF
tests_option=OFF; ((want_tests == 0)) || tests_option=ON
example_option=OFF; ((want_example == 0)) || example_option=ON
configure_args=(-S "$root" -B "$build_dir"
  "-DCMAKE_BUILD_TYPE=$config"
  "-DCLANKER_BUILD_CLI=$cli_option"
  "-DCLANKER_BUILD_TESTS=$tests_option"
  "-DCLANKER_BUILD_EXAMPLES=$example_option")
if ((core == 0)); then
  if [[ -n "$curl_archive" ]]; then
    [[ "$curl_archive" == /* ]] || curl_archive="$PWD/$curl_archive"
    configure_args+=("-DCLANKER_CURL_ARCHIVE:FILEPATH=$curl_archive")
  fi
  if [[ "$os" == Linux ]]; then
    [[ -z "$openssl_prefix" || "$openssl_prefix" == /* ]] || openssl_prefix="$PWD/$openssl_prefix"
    static_option=OFF; ((static_openssl == 0)) || static_option=ON
    configure_args+=("-DOPENSSL_ROOT_DIR:PATH=$openssl_prefix" "-DOPENSSL_USE_STATIC_LIBS=$static_option")
  fi
fi
# CMake initializes language flags from CFLAGS/CXXFLAGS, not CPPFLAGS.
CFLAGS="${CPPFLAGS:-} ${CFLAGS:-}" CXXFLAGS="${CPPFLAGS:-} ${CXXFLAGS:-}" cmake "${configure_args[@]}"
targets=("$target")
((want_tests == 0)) || [[ "$target" == clanker_tests ]] || targets+=(clanker_tests)
((want_example == 0)) || [[ "$target" == clanker_core_example ]] || targets+=(clanker_core_example)
if [[ -n "$install_prefix" && "$target" != clanker_core ]]; then targets+=(clanker_core); fi
if ((core == 0)) && { ((package)) || [[ -n "$install_prefix" ]]; } && [[ "$target" != clanker ]]; then
  targets+=(clanker)
fi
jobs="${JOBS:-$(getconf _NPROCESSORS_ONLN)}"
build_args=(--build "$build_dir" --config "$config" --parallel "$jobs")
build_args+=(--target "${targets[@]}")
((clean == 0)) || build_args+=(--clean-first)
cmake "${build_args[@]}"

if ((run_tests)); then "$build_dir/clanker_tests" "${test_args[@]}"; fi
if ((examples)); then "$build_dir/examples/core/clanker_core_example"; fi
if ((core == 0)) && { [[ -n "$install_prefix" ]] || ((package)); }; then
  curl_info=()
  [[ -f "$build_dir/clanker-curl.txt" ]] || fail "CMake curl metadata is missing"
  while IFS= read -r line || [[ -n "$line" ]]; do curl_info+=("$line"); done < "$build_dir/clanker-curl.txt"
  [[ ${#curl_info[@]} == 3 && "${curl_info[0]}" == /* && -n "${curl_info[1]}" && "${curl_info[2]}" =~ ^[0-9a-fA-F]{64}$ ]] ||
    fail "invalid CMake curl metadata"
  curl_source="${curl_info[0]}"; curl_version="${curl_info[1]}"; curl_hash="${curl_info[2]}"
fi
if [[ -n "$install_prefix" ]]; then install_clanker "$install_prefix"; fi
if ((package)); then package_clanker; fi
echo "Build complete: $build_dir ($config)"
