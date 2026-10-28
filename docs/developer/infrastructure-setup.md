<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Infrastructure setup (GitHub, Gitea project management, code signing)

Since 2026-10-28 (maintainer decision) WParted follows the standard model of the TDS-Ops repository
sync (`Tracht-Digital-Solutions/tds-ops`, `scripts/sync-repos.mjs`) for public repositories:

| What | Where |
|---|---|
| Code, pull requests, CI, releases | GitHub: https://github.com/jtracht/WParted (public) |
| Issues (`WP-nnn`), milestones, labels, project board | Gitea: https://git.tracht-digital.de/jtracht/WParted-PM |
| Read-only copy of the code | Gitea pull mirror `jtracht/WParted` (created by the sync, Actions disabled) |

## 1. CI - GitHub Actions

`.github/workflows/ci.yml` runs on GitHub-hosted Ubuntu runners (`ubuntu-latest`); the jobs are the
same as on the former Gitea runner:

| Job | What it does |
|---|---|
| `static checks` | read-only guard, SPDX, secrets scan, Markdown links |
| `core + unit tests (Linux, GCC)` | native GCC build of the portable core with `-Werror`, plus the checks with the reference tools (`mkfs-fixtures.sh`, `image-apply-check.sh`, `fs-tools-check.sh`) |
| `Windows x64/x86 (MinGW cross build, tests under Wine)` | `tools/ci/build-mingw.sh <arch> --package`, then `tools/ci/gui-smoke-wine.sh` |
| `Windows ARM64 (llvm-mingw cross build, not run)` | `tools/ci/build-arm64.sh --package` |

The scripts use `sudo` for package installation when they do not run as root. Reproduce a job locally
with Docker Desktop (the image matches Ubuntu 24.04):

```powershell
docker run --rm -v "${PWD}:/src" -w /src docker.gitea.com/runner-images:ubuntu-latest `
  bash tools/ci/build-mingw.sh x64 --package
```

What Wine cannot verify (manual release gate, see `release-process.md`): real raw-disk access,
elevation/UAC, DPI/theme behaviour of real Windows versions, Windows 7/8.1 specifics.

## 2. Branch protection

Not used (maintainer decision 2026-10-27). Compensating rules: CI runs on every push and pull request;
releases are only tagged on commits with a green CI run; the release workflow creates drafts only;
agents never force-push.

## 3. Repository sync (TDS-Ops)

WParted is a public GitHub repository and therefore **not** listed in `GITEA_PRIMARY`: the hourly sync
keeps the Gitea pull mirror `jtracht/WParted` up to date and disables Gitea Actions on it. GitHub
Actions stay enabled. Project management lives in the separate Gitea repository `jtracht/WParted-PM`,
which the sync does not touch (it has no GitHub counterpart).

## 4. Release secrets

| Secret | Purpose |
|---|---|
| (none required) | `release.yml` creates the draft release with the automatic `GITHUB_TOKEN` (`contents: write`). |
| `SIGNING_CERT_PFX_BASE64` / `SIGNING_CERT_PASSWORD` | Optional Authenticode certificate, used by `osslsigncode` on Linux. Without them the release says it is **unsigned**. |
