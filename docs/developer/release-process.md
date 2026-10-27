<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Release process

1. All issues of the milestone that are marked *release-blocking* are closed with verified acceptance
   criteria.
2. `CHANGELOG.md` moves `[Unreleased]` to the version; `CMakeLists.txt` `project(VERSION)` and
   `WPARTED_VERSION_SUFFIX` match the tag.
3. Tag on `main`: `v0.1.0-alpha` (annotated). Tags are protected on Gitea.
4. `.gitea/workflows/release.yml` (Linux runner) cross-builds x64 and x86 with MinGW-w64, runs all
   tests under Wine plus the GUI smoke test, packages portable ZIPs (with toolchain licence texts),
   writes `WParted-v<version>-checksums.sha256` and `WParted-v<version>-sbom.json`, creates a source
   ZIP and a **draft** Gitea release with all artefacts. It fails - and creates no release - if any
   step fails or an artefact is missing.
5. **Manual real-Windows gate:** a maintainer downloads the draft artefacts on a real Windows machine,
   verifies the checksum, starts `wparted.exe` (unelevated and elevated) and runs
   `wparted-cli list --json` and `inspect-image` on a test image. Only then the draft is published.
   Results go into `docs/compatibility/windows-support-matrix.md`.
6. Signing: only when `SIGNING_CERT_*` secrets exist (see `infrastructure-setup.md`). Unsigned
   releases say so in the release notes.
7. The Gitea push mirror updates GitHub (code and tags). Release binaries are copied to GitHub only by
   the optional `mirror-release` job.

Release gates (all required): build OK, tests OK, read-only/security checks OK, artefacts complete and
start (`wparted-cli version` smoke test), checksums present, platforms stated correctly, docs updated,
release notes written.

Pre-release naming: `-alpha`, `-beta.N`, `-rc.N`. Artefact names state the platform range,
e.g. `WParted-v0.1.0-alpha-win7plus-x64-portable.zip`.
