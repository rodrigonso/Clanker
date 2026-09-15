# Public-alpha releases

Clanker ships standalone Windows/macOS executables, not an installer. libcurl
and PCRE2 are statically linked; TLS and trust stores come from the OS. The
Windows release CRT is static. The core SDK is distributed separately from
runtime archives.

## Native build matrix

The [native workflow](../.github/workflows/native.yml) builds and exercises Windows x86_64, macOS Apple Silicon,
and macOS Intel on their native runners. Each platform builds the CLI and
dependency-free core in separate directories, runs the offline suites, and
packages the CLI through the repository wrappers. macOS previews set
`MACOSX_DEPLOYMENT_TARGET=15.0`; source builds are not automatically qualified
for older systems just because they compile there.

Runtime archives are extracted into a fresh location and started outside the
source tree. Their checksums, required notices, executable, and distribution
metadata are checked before being accepted as release artifacts. The native
packaging policies reject unexpected runtime dependencies and architecture
mismatches. These gates do not replace interactive terminal or minimum-OS
qualification.

Workflow changes in a local checkout do not run on GitHub until pushed to a
configured remote with Actions enabled. Do not label macOS as qualified based
only on Windows results or mocked macOS tooling.

## Version tags and drafts

Keep the root `VERSION` in sync with the intended release, using numeric
`X.Y.Z` compatible with CMake's project version. Preview status is a GitHub
prerelease flag, not a suffix in `VERSION`. A version tag must be exactly `v`
followed by that file's value. Tagging the wrong version is a
release error, not a reason to rename an existing binary.

Branch pushes, pull requests, and manual workflow dispatch only build and
exercise the project. **Only a matching version-tag push prepares a draft.**
Manually dispatching on a tag still does not create a draft; rerun that tag's
push workflow to repair an interrupted draft. Runner/action pins and local
helper commands are documented in [.github/README.md](../.github/README.md).

The workflow prepares a **draft**, never an automatically public release.
Every native build must succeed first. An already published release must not
be overwritten by a rerun. Downloaded build artifacts must contain exactly
the expected three platform archives and their three checksum files.

Source attribution and the MIT project license are mandatory. The project
license was already declared in the README; the root `LICENSE` formalizes it.
`THIRD_PARTY_NOTICES.md` describes the retained pi and jcode notices, the
embedded CLI dependencies, and the boundary around external benchmark tools.
When adding or updating a dependency, review its license and update the
notices in the same change.

Review the draft and publish it manually only after the gates below are met.
There is no signing or notarization configuration in this initial workflow;
do not label its output as a signed release.

## Before the first public publication

1. Confirm the right to publish the project's original contributions and
   preserve all applicable upstream notices. This repository's attribution
   inventory is not a legal opinion or a claim of exhaustive source provenance.
2. Before pushing to a public repository or changing repository visibility,
   review both the working tree and the full history that will become public
   for private configuration, personal data, credentials, and accidental
   artifacts. This preparation has **not** supplied a history-wide clearance.
   Do not paste findings into public issues or logs.
3. Require successful native CI and manually exercise the TUI, first-run flow,
   project trust, Ask-mode approval/cancellation, file tools, shell output,
   session resume, and HTTPS on each advertised platform.
4. Qualify the minimum OS versions, not only the newest hosted runner.
   Windows previews target Windows 10+ x86_64; macOS previews target 15+ on
   Intel and Apple Silicon. Keep the Windows ASCII-path limitation visible
   until its remaining platform calls are migrated.
5. Decide whether an unsigned developer preview is acceptable. Otherwise
   complete Windows signing and macOS signing/notarization through an approved
   signing service before publishing. Do not ask users to disable OS-wide
   security protections.
6. Inspect the draft's filenames, notices, checksums, version, and release
   notes. Publish it deliberately, then confirm the README's download
   instructions match the actual public assets.

Keep historical benchmark jobs, session logs, and build caches out of source
archives. Existing ignore rules are not evidence that no such material entered
older commits. Do not rewrite or delete shared history as part of a routine
release; coordinate any required remediation separately.

## Local preview archives

```powershell
.\build.bat -BuildDir out\release -Test -Package
```

```sh
MACOSX_DEPLOYMENT_TARGET=15.0 bash tools/build.sh --build-dir out/release --test --package
```

The package scripts require the project license, source notices, and matching
notices from the hash-pinned dependencies. They neither copy user profiles nor
publish assets. Checksum files provide integrity, not authenticity.
