<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Security and data-integrity architecture

## Threat model (summary)

| Threat | Mitigation |
|---|---|
| Accidental data loss through a UI or logic bug | Read-only discovery; planning without privileges; validation engine; explicit safety summary; calibration (re-read) before each step; tests on images/VHDs only |
| Wrong target disk (TOCTOU: disk replaced/renumbered between planning and execution) | Plans reference disks by serial + table identity (GPT disk GUID / MBR signature) + size, never only by `PhysicalDriveN`; helper re-identifies and aborts on any mismatch |
| Malicious local process abusing the privileged helper | Helper runs only for the duration of an apply; named pipe with DACL for the interactive user's SID; caller PID/image verified; declarative plan validated server-side; no command strings, no paths to executables in the protocol |
| Tampered binaries | Signed releases (when signing infrastructure exists), SHA-256 checksums, SBOM |
| Encrypted volumes | BitLocker/LUKS never unlocked, suspended or decrypted by WParted; operations that would need it are refused |
| Power loss / device removal mid-operation | Operation ordering that keeps file systems consistent at step boundaries where the underlying tool allows it; journal of executed steps; honest documentation that no general rollback exists |

## Privilege model

- `wparted.exe` (GUI) and `wparted-cli.exe` run **asInvoker**. Reading raw partition tables needs
  administrator rights; in v0.1 the user can restart the GUI elevated. This is acceptable because
  the v0.1 code base has **no write path at all** (enforced by `tools/ci/check_readonly.py`).
- From v0.3 the GUI stays unelevated and starts `wparted-helper.exe` elevated for "Apply". The helper:
  1. accepts exactly one plan over a per-session named pipe,
  2. re-discovers devices itself (never trusts GUI data),
  3. validates the plan with the same core validation engine,
  4. executes step by step with calibration,
  5. streams progress and a structured log back, then exits.

  Protocol details: [ADR-0002](adr-0002-privileged-helper-ipc.md). Offline/recovery mode for system
  partitions: [ADR-0003](adr-0003-recovery-offline-mode.md).

## Read-only guarantees in v0.1

- All device handles: `GENERIC_READ` at most, `FILE_SHARE_READ | FILE_SHARE_WRITE`.
- No `IOCTL_DISK_SET_*`, `CREATE_DISK`, `FSCTL_LOCK/DISMOUNT/SHRINK/EXTEND`, mount point or label APIs.
- CI fails on any such usage outside reviewed exemptions (currently: the user-chosen JSON report file).
- End-to-end test verifies that image analysis leaves the image byte-identical (SHA-256).

## Rules for future write paths

1. No write feature is "Supported" before it passed automated tests on VHD/VHDX or image files with
   read-back verification, plus fault injection where possible.
2. High/critical risk features need a second, human review (AI review alone is insufficient).
3. System-critical partitions (ESP, MSR, WinRE, LDM, Storage Spaces, running Windows volume) are
   refused in desktop mode unless an offline strategy exists.
4. Agents never run write tests against physical disks.
