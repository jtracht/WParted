<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Infrastructure setup (Gitea runner, GitHub mirror, code signing)

## 1. CI runner - the single instance runner

The Gitea instance has **one** Actions runner: `instance-linux-runner` (act_runner v5.2.2, label
`ubuntu-latest`, image `docker.gitea.com/runner-images:ubuntu-latest`, Ubuntu 24.04, root, Node
available). It is shared with other repositories, so jobs run one after another. No Windows runner
exists or is planned.

All WParted workflows use `runs-on: ubuntu-latest`:

| Job | What it does |
|---|---|
| `static checks` | read-only guard, SPDX, secrets scan, Markdown links |
| `core + unit tests (Linux, GCC)` | native GCC build of the portable core with `-Werror` |
| `Windows x64/x86 (MinGW cross build, tests under Wine)` | `tools/ci/build-mingw.sh <arch> --package`, then `tools/ci/gui-smoke-wine.sh` |

Reproduce any job locally with Docker Desktop:

```powershell
docker run --rm -v "${PWD}:/src" -w /src docker.gitea.com/runner-images:ubuntu-latest `
  bash tools/ci/build-mingw.sh x64 --package
```

What Wine cannot verify (manual release gate, see `release-process.md`): real raw-disk access,
elevation/UAC, DPI/theme behaviour of real Windows versions, Windows 7/8.1 specifics.

## 2. Branch protection

`main`: no force push, no deletion, required status checks = the four CI job names above. Required
approvals: 1 once a second maintainer exists. Tags `v*` protected.

## 3. GitHub mirror (Gitea -> GitHub, one direction)

1. Create a **fine-grained GitHub token** limited to `jtracht/WParted` with *Contents: Read and write*.
2. Gitea: Repo -> Settings -> Mirror Settings -> *Push Mirror*: `https://github.com/jtracht/WParted.git`,
   username `jtracht`, password = token, sync on commit.
3. GitHub Issues, Projects, Wiki, Discussions and Actions are already disabled.
4. Release binaries are not mirrored by Git; an optional job may copy published Gitea release assets
   using a separate secret `GITHUB_MIRROR_TOKEN`.

## 4. Release secrets

| Secret | Purpose |
|---|---|
| `RELEASE_TOKEN` | Gitea token (`write:repository`) used by `release.yml` to create the draft release |
| `SIGNING_CERT_PFX_BASE64` / `SIGNING_CERT_PASSWORD` | Optional Authenticode certificate, used by `osslsigncode` on Linux. Without them the release says it is **unsigned**. |
