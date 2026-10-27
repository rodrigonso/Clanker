# Native CI and draft previews

`workflows/native.yml` runs on branch pushes, `v*` tag pushes, pull requests
(including forks), and manual dispatch. Only a matching version-tag **push**
can prepare a release draft. Manual dispatch tests its selected ref but does
not create a draft; rerun the successful tag-push workflow to repair a draft.

## Native runners and action pins

Verified against the official
[runner image table](https://github.com/actions/runner-images#available-images)
and GitHub REST git refs on 2026-09-15:

| Target | Explicit runner label | Image inventory |
| --- | --- | --- |
| Windows x86_64 | `windows-2025-vs2026` | [VS 2026 image](https://github.com/actions/runner-images/blob/main/images/windows/Windows2025-VS2026-Readme.md) |
| macOS arm64 | `macos-15` | [ARM image](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-arm64-Readme.md) |
| macOS x86_64 | `macos-15-intel` | [Intel image](https://github.com/actions/runner-images/blob/main/images/macos/macos-15-Readme.md) |

The Windows label explicitly selects Visual Studio 2026 for C++23 mode.
The listed images supply CMake newer than the required 3.24. No dependency
installation actions, package-manager installs, caches, or Linux jobs are
configured. macOS jobs set `MACOSX_DEPLOYMENT_TARGET=15.0`.

| Official action | Release | Pinned commit | Official reference |
| --- | --- | --- | --- |
| `actions/checkout` | v7.0.1 | `3d3c42e5aac5ba805825da76410c181273ba90b1` | [git ref](https://api.github.com/repos/actions/checkout/git/ref/tags/v7.0.1) |
| `actions/upload-artifact` | v7.0.1 | `043fb46d1a93c77aae656e7c1c64a875d1fc6a0a` | [git ref](https://api.github.com/repos/actions/upload-artifact/git/ref/tags/v7.0.1) |
| `actions/download-artifact` | v8.0.1 | `3e5f45b2cfb9172054b4087a40e8e0b5a5461e7c` | [git ref](https://api.github.com/repos/actions/download-artifact/git/ref/tags/v8.0.1) |
| `actions/github-script` | v9.0.0 | `3a2844b7e9c422d3c10d287c895573f7108da1b3` | [annotated tag object](https://api.github.com/repos/actions/github-script/git/tags/d746ffe35508b1917358783b479e04febd2b8f71) |

Runner labels select maintained images, not immutable machine snapshots.
Action refs were resolved to commits, including dereferencing the annotated
github-script tag; do not substitute a tag-object SHA for its commit.

## Gates and trust boundary

- Each native job builds/tests the core with examples and the CLI in separate
  directories through the repository wrappers. Packaging retains their
  architecture and static-runtime dependency audits.
- Checkout goes into `source/`. A fresh extraction is created under the
  workspace's sibling `out/`, outside that checkout. Its executable runs
  `--help` and `--version` from an empty working directory with an isolated
  profile and OS-only `PATH`. No developer toolchain or user profile is
  inherited. The platform install script then installs that archive (through
  `CLANKER_INSTALL_ARCHIVE`, without network access or `PATH` edits) into an
  isolated home, checks the installed version and notices, and reruns as a
  no-op upgrade.
- `tools/ci/package.cjs` verifies archive names, SHA256 sidecars, exact runtime
  contents, required nonempty notices, source notice equality, platform/version
  metadata, static curl 8.11.1 / PCRE2 10.48, native TLS, and unsigned labeling.
  Keep these assertions aligned when intentionally updating dependency policy.
- Build jobs have only `contents: read`; checkout never persists credentials.
  Only the dependent tag-push draft job has `contents: write`. Fork PRs cannot
  reach it. The official github-script action's provided client is used without
  configuring credential material.
- The draft job downloads only the current run's artifacts, requires exactly
  three platform archives and three sidecars, rechecks every checksum and
  `vVERSION`, and requires the source license/notices before any release write.
  The live tag must still resolve to the tested commit (annotated tags work).
  It creates or updates **draft prereleases only**, refuses published releases
  and unexpected existing assets, and rechecks draft state before each write.
  An interrupted upload can leave an incomplete draft; rerun the tag workflow.
  Do not manually edit or publish a draft while that workflow is running.
- No signing, notarization, automatic publication, history audit, or secret
  scanning clearance is provided. Checksums offer integrity, not authenticity.
  Follow [the manual release checklist](../docs/RELEASING.md).

These definitions must first run on the configured GitHub repository before
claiming native CI success. In particular, YAML validation, Windows tests and
mocked API tests do not qualify either Mac architecture. Hosted-runner smoke
checks also do not qualify interactive TUI behavior, HTTPS, first-run OS
security prompts, or the minimum supported OS release.

## Helper validation

Node is preinstalled on the CI runners and used only for CI automation; it is
not shipped or required by Clanker users. The helper tests use Node's built-in
test runner and no npm dependencies:

```powershell
node --test tools\ci\ci.test.cjs
$env:CLANKER_CI_PACKAGE_DIR = 'out\release\packages'
node --test tools\ci\ci.test.cjs
```

The optional environment variable adds a real native archive smoke using
metadata copied into an isolated fixture under ignored `out/`. No network or
GitHub API requests are made by these tests. Test and smoke fixtures are
removed after use.
