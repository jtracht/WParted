<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Infrastructure setup (Gitea runners, GitHub mirror, code signing)

State on 2026-10-27: Gitea 28.2 at https://git.tracht-digital.de, repository `jtracht/WParted`.
**No Actions runner is registered** for this user/repository, so workflow runs stay queued until the
steps below are done (issues WP-004, WP-005).

## 1. Linux runner (`linux-build`) – static checks and portable core tests

On the Debian host that already runs Docker:

```sh
# Download act_runner (pick the current release from https://gitea.com/gitea/act_runner/releases)
./act_runner register --no-interactive \
  --instance https://git.tracht-digital.de \
  --token <registration token from Repo → Settings → Actions → Runners> \
  --name wparted-linux \
  --labels "linux-build:docker://node:20-bookworm"
./act_runner daemon
```

`node:20-bookworm` provides Node (needed by `actions/checkout`), Python 3 and apt.

## 2. Windows host runner (`windows-modern`) – native builds and tests

A Docker container on Linux cannot build or run native Windows binaries. Use a Windows 10/11 x64 VM
or PC dedicated to CI (no personal data, no attached production disks):

1. Install Visual Studio 2022 Build Tools with "Desktop development with C++" (+ "MSVC v143 ARM64
   build tools" for ARM64), Git for Windows, Python 3, Node.js LTS.
2. `python -m pip install cmake ninja` (system-wide) or put CMake/Ninja on `PATH`.
3. `Set-ExecutionPolicy RemoteSigned -Scope LocalMachine`.
4. Register and install act_runner as a service:

```powershell
.\act_runner.exe register --no-interactive --instance https://git.tracht-digital.de `
  --token <token> --name wparted-win11 --labels "windows-modern:host"
.\act_runner.exe daemon   # or install as a service with nssm / sc.exe
```

Later: a second runner `windows-legacy:host` on a Windows 7 SP1 / 8.1 VM for runtime smoke tests
(binaries are built on `windows-modern`; legacy runners only *run* them).

## 3. Branch protection (after runners work)

`main`: no force push, no deletion, require the status checks `ci / static checks`,
`ci / core + unit tests (Linux, GCC)`, `ci / Windows x64`, `ci / Windows x86`. Required approvals: 1
once a second maintainer exists. Tags `v*` protected.

## 4. GitHub mirror (Gitea → GitHub, one direction)

1. Create a **fine-grained GitHub token** limited to the repository `WParted` with
   *Contents: Read and write* (and *Workflows* only if needed). Do not reuse a personal OAuth token.
2. Gitea: Repo → Settings → Mirror Settings → *Push Mirror*: remote
   `https://github.com/<owner>/WParted.git`, username `<owner>`, password = token, sync on commit,
   interval 8h.
3. On GitHub disable Issues, Projects, Wiki, Discussions and Actions (Settings → General/Actions).
4. Release binaries are not mirrored by Git; an optional workflow job may copy finished Gitea release
   assets to a GitHub release using a separate secret `GITHUB_MIRROR_TOKEN`.

## 5. Release secrets

| Secret | Purpose |
|---|---|
| `RELEASE_TOKEN` | Gitea token (`write:repository`) used by `release.yml` to create releases |
| `SIGNING_CERT_PFX_BASE64` / `SIGNING_CERT_PASSWORD` | Optional Authenticode certificate. Without them releases are published as **unsigned** and say so. Prefer an HSM/cloud signing service (e.g. Azure Trusted Signing) for production. |
