<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Infrastructure setup (GitHub, CI, code signing)

Code, pull requests, CI and releases are on GitHub: https://github.com/jtracht/WParted (public).

## 1. CI - GitHub Actions

`.github/workflows/ci.yml` runs on GitHub-hosted runners (`ubuntu-latest`; x86 programs run in a normal Wine
prefix, so the jobs work with both Wine's classic and WoW64 mode):

| Job | What it does |
|---|---|
| `static checks` | read-only guard, SPDX, secrets scan, Markdown links |
| `core + unit tests (Linux, GCC)` | native GCC build of the portable core with `-Werror`, plus the checks with the reference tools (`mkfs-fixtures.sh`, `image-apply-check.sh`, `fs-tools-check.sh`) |
| `Windows x64/x86 (MinGW cross build, tests under Wine)` | `tools/ci/build-mingw.sh <arch> --package`, then `tools/ci/gui-smoke-wine.sh` |
| `Windows ARM64 (llvm-mingw cross build, not run)` | `tools/ci/build-arm64.sh --package` |

The scripts use `sudo` for package installation when they do not run as root. Reproduce a job locally
with Docker Desktop:

```powershell
docker run --rm -v "${PWD}:/src" -w /src ubuntu:latest `
  bash tools/ci/build-mingw.sh x64 --package
```

What Wine cannot verify (manual release gate, see `release-process.md`): real raw-disk access,
elevation/UAC, DPI/theme behaviour of real Windows.

## 2. Branch protection

Not used (maintainer decision 2026-10-27). Compensating rules: CI runs on every push and pull request;
releases are only tagged on commits with a green CI run; the release workflow creates drafts only;
agents never force-push.

## 3. Release secrets

| Secret | Purpose |
|---|---|
| (none required) | `release.yml` creates the draft release with the automatic `GITHUB_TOKEN` (`contents: write`). |
| `SIGNING_CERT_PFX_BASE64` / `SIGNING_CERT_PASSWORD` | Optional Authenticode certificate, used by `osslsigncode` on Linux. Without them the release says it is **unsigned**. |
