<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Release process

1. All issues of the milestone that are marked *release-blocking* are closed with verified acceptance
   criteria.
2. `CHANGELOG.md` moves `[Unreleased]` to the version; `CMakeLists.txt` `project(VERSION)` and
   `WPARTED_VERSION_SUFFIX` match the tag.
3. Tag on `main`: `v0.1.0-alpha` (annotated). Tags are protected on Gitea.
4. `.gitea/workflows/release.yml` (Windows runner) builds x64 and x86, runs all tests, packages
   portable ZIPs, writes `WParted-v<version>-checksums.sha256` and `WParted-v<version>-sbom.json`,
   creates a source ZIP, and creates the Gitea release with all artefacts. It fails – and creates no
   release – if any step fails or an artefact is missing.
5. Signing: only when `SIGNING_CERT_*` secrets exist (see `infrastructure-setup.md`). Unsigned
   releases say so in the release notes.
6. The Gitea push mirror updates GitHub (code and tags). Release binaries are copied to GitHub only by
   the optional `mirror-release` job.

Release gates (all required): build OK, tests OK, read-only/security checks OK, artefacts complete and
start (`wparted-cli version` smoke test), checksums present, platforms stated correctly, docs updated,
release notes written.

Pre-release naming: `-alpha`, `-beta.N`, `-rc.N`. Artefact names state the platform range,
e.g. `WParted-v0.1.0-alpha-win7plus-x64-portable.zip`.
