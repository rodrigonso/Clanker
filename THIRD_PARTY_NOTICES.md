# Third-party notices

Clanker's original contributions are licensed under the root `LICENSE` (MIT).
Upstream material retains its own copyright and license. These notices do not
claim ownership of upstream projects or their contributors' work.

## Source-derived material

| Project | Material and scope | License notice |
|---|---|---|
| [pi](https://github.com/earendil-works/pi) | Behavioral reference for the independent C++ implementation, including reused prompt-visible strings and tool guidance. No JavaScript runtime or pi package is embedded. | [MIT, copyright 2025 Mario Zechner](licenses/pi/LICENSE) |
| [jcode](https://github.com/1jehuang/jcode) | `benchmarks/agentbench` originated from `scripts/bench_memory_cli.py`; Clanker also references jcode's terminal behavior. The benchmark harness is source tooling, not part of the executable or SDK. | [MIT, copyright 2025 Jeremy Huang](licenses/jcode/LICENSE) |

The retained notices were checked against pi's
[`LICENSE` at a74c5da](https://github.com/earendil-works/pi/blob/a74c5da112c29466f182a03108337a488c785d76/LICENSE)
and jcode's
[`LICENSE` at 9004466](https://github.com/1jehuang/jcode/blob/9004466b1151a404291dfec7e49fc73bd0ede59e/LICENSE).
These identify the license texts, not a claim that all compatibility work
targets those old source revisions.

## Statically linked CLI dependencies

| Component | Pin | License | Notice in runtime archives |
|---|---|---|---|
| [libcurl](https://curl.se/) | 8.11.1; archive SHA256 in `cmake/curl.cmake` | curl license (MIT/X-derived) | `licenses/curl/COPYING` |
| [PCRE2](https://github.com/PCRE2Project/pcre2) | 10.48; archive SHA256 in `cmake/pcre2.cmake` | BSD-3-Clause WITH PCRE2-exception | `licenses/pcre2/LICENCE.md` |

The build copies these notices from the exact hash-pinned dependency sources,
not from a separate, potentially mismatched download. PCRE2's JIT is disabled;
the separately licensed SLJIT compiler is not compiled into Clanker.

Schannel, SecureTransport, and the operating system's certificate stores are
provided by Windows or macOS, not redistributed in the archives. Release
Windows binaries use the static Microsoft C runtime; building and distributing
that runtime remains subject to the applicable Microsoft toolchain license.

## Distribution

CLI archives and installed SDKs carry this file under `share/clanker`, the
project license under `share/clanker/licenses/clanker`, and the retained pi and
jcode notices under `share/clanker/licenses`. The jcode notice is included
conservatively even though the benchmark scripts are not shipped in either.
Only CLI distributions contain libcurl and PCRE2; the core SDK has neither.

External benchmark datasets, reference results, and harnesses remain separate
projects. Links and comparisons do not imply endorsement or redistribute their
software, datasets, or trademarks under Clanker's license.
