"""Build a Debian bookworm compatible `clank` binary using Modal.

DeepSWE task images are Debian 12 (glibc 2.36); a binary built on Ubuntu 24.04
(glibc 2.39) fails at agent setup with ``version 'GLIBC_2.38' not found``. With
no local Docker available, the build runs in a Modal sandbox instead. Bookworm
carries libssl-dev, so the result also resolves libssl.so.3 from the image and
needs no ``clank_libs`` upload.

    python build_bookworm.py /path/to/clank-src [-o out/clank]
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import modal

BUILD_SCRIPT = r"""
set -eux
cd /src
rm -rf build-bookworm
cmake -S . -B build-bookworm -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-bookworm --target clank -j "$(nproc)"
/src/build-bookworm/clank --help >/dev/null
echo '--- ldd ---'
ldd /src/build-bookworm/clank
echo '--- glibc requirement ---'
objdump -T /src/build-bookworm/clank | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -3
"""


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path, help="clank checkout to build")
    parser.add_argument("-o", "--output", type=Path, default=Path("clank-bookworm"))
    args = parser.parse_args()

    source = args.source.expanduser().resolve()
    if not (source / "CMakeLists.txt").is_file():
        print(f"not a clank checkout: {source}", file=sys.stderr)
        return 1

    image = (
        modal.Image.from_registry("debian:bookworm", add_python=None)
        .apt_install(
            "build-essential",
            "cmake",
            "ninja-build",
            "git",
            "libssl-dev",
            "ca-certificates",
            "binutils",
        )
        # copy=True so the build layer sees the sources.
        .add_local_dir(
            str(source),
            "/src",
            copy=True,
            ignore=["build*", "out", ".git", "**/__pycache__"],
        )
    )

    app = modal.App.lookup("clank-bookworm-build", create_if_missing=True)

    print(f"building {source} on debian:bookworm ...")
    # Keep the sandbox alive past the build: a sandbox whose entrypoint has
    # exited is torn down before the artifact can be copied out.
    sandbox = modal.Sandbox.create(
        "sleep", "infinity", image=image, app=app, timeout=3600, cpu=4.0
    )
    try:
        process = sandbox.exec("bash", "-lc", BUILD_SCRIPT)
        process.wait()

        print(process.stdout.read())
        stderr = process.stderr.read()
        if process.returncode != 0:
            print(stderr, file=sys.stderr)
            print(f"build failed (exit {process.returncode})", file=sys.stderr)
            return 1
        # ldd/objdump output goes to stderr under `set -x`; keep it for the record.
        print(stderr[-1500:])

        output = args.output.expanduser().resolve()
        output.parent.mkdir(parents=True, exist_ok=True)
        sandbox.filesystem.copy_to_local("/src/build-bookworm/clank", str(output))
        output.chmod(0o755)
    finally:
        sandbox.terminate()

    print(f"\nwrote {output} ({output.stat().st_size} bytes)")
    print(f"run it with --ak clank_binary={output} and no --ak clank_libs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
