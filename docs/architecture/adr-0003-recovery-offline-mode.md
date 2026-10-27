<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# ADR-0003: Recovery and offline mode

- Status: Accepted (concept). Nothing in this ADR is implemented yet; the support matrix keeps
  offline mode as *Planned*.
- Date: 2026-10-27
- Context packages: WP-070, WP-071 (BitLocker/EFI/BCD safety), WP-041 (resize/move)

## Context

GParted runs from a live system (GParted Live), so no partition is in use. On Windows, the running
system volume, the page file volume, the EFI system partition and the recovery partition are in use
or protected while Windows runs:

- The Windows volume can be shrunk online through the Volume Shrink API, but not moved, and
  only down to unmovable files.
- Volumes with open handles cannot be locked/dismounted; writing beneath a mounted NTFS volume
  corrupts it.
- Moving the start of the system partition invalidates BCD device entries and, with BitLocker,
  TPM measurements.

Without an offline mode, WParted must refuse these operations in desktop mode. That is the current
behaviour (`op.system_critical`, `op.busy`, `op.encrypted`). The question is how to support them
later without new risks.

## Decision

1. **Desktop mode stays the default** and keeps refusing operations on in-use and system-critical
   partitions. The validation rules are not relaxed in desktop mode.
2. **Offline mode = WParted running in Windows PE / Windows RE**, started from media that the user
   builds. Releases do not ship Microsoft binaries (WinPE is part of the Windows ADK under Microsoft's
   licence terms, not redistributable by this project). WParted provides:
   - a script `tools/offline/make-media.ps1` (future) that uses the user's installed ADK
     (`copype`, `MakeWinPEMedia`) and copies the portable WParted files into the image;
   - documentation for adding WParted to an existing Windows RE (`reagentc` is **not** run
     automatically; the user decides).
3. **Plan handoff.** The user plans in desktop mode, exports the plan (*Save plan*, schema
   `wparted-plan` v1), and in offline mode opens it. The plan identifies disks by serial, size and
   table identity, not by disk number. Disk numbers often differ in WinPE. The existing TOCTOU check
   (`plan.identity_mismatch`) therefore covers the boot-media case.
4. **Offline validation context.** In WinPE no partition is the running Windows; the context sets
   `windowsVolume = none` and evaluates `op.busy` against WinPE's own volumes only. System-critical
   partitions become *Caution* instead of *Error* only if the operation has a defined post-step:

   | Operation | Required post-step | Until implemented |
   |---|---|---|
   | Move/resize the Windows volume | none for NTFS geometry; BCD `device`/`osdevice` reference by partition GUID/offset must be rechecked | refuse |
   | Move the ESP | rewrite firmware boot entries (`bcdedit /set {fwbootmgr}`) or recreate via `bcdboot` | refuse |
   | Any operation on a BitLocker volume | user suspends BitLocker in desktop mode first (WParted never suspends it) | refuse (`op.encrypted`) |
   | Delete/recreate WinRE partition | `reagentc` re-registration | refuse |

   WParted never runs `bcdedit`, `bcdboot` or `reagentc` on its own. It shows the commands and the
   user runs them, or a later ADR defines a reviewed implementation.
5. **BitLocker.** Offline mode does not unlock volumes. Encrypted volumes stay *Error*; the only
   supported path is "suspend in Windows (user action), then operate offline, then resume". This
   follows the master rule that BitLocker is never disabled or bypassed without explicit
   authorisation.
6. **Rescue scan in offline mode** is the same read-only scan as in desktop mode
   (`rescueScan`). Recovering a found file system remains a normal *New partition* operation that
   goes through validation.

## Technical constraints to verify (not yet checked)

| Item | Expectation | Status |
|---|---|---|
| Static binaries in WinPE | MinGW builds depend only on system DLLs incl. `msvcrt.dll`, present in WinPE | unverified |
| WinPE architectures | x64 and ARM64 WinPE exist in current ADKs; x86 WinPE was dropped in newer ADKs | unverified per ADK version |
| GUI in WinPE | Win32/GDI works; no DWM composition, no dark title bar, `TaskDialogIndirect` needs comctl32 v6 side-by-side | unverified; GUI must degrade |
| File dialogs in WinPE | `GetOpenFileNameW` availability without optional components | unverified; fallback: plan path as command-line argument (already supported for images) |
| Disk numbering | differs from desktop | handled by identity matching (tested in unit tests) |

## Alternatives considered

| Option | Why not |
|---|---|
| Linux-based live medium (like GParted Live) | Would make WParted a Linux distribution; Windows file systems would need the ntfs-3g path again; not "native Windows". |
| Offline operation by scheduling at next boot (`BootExecute`, native app) | Native NT applications without Win32 are a large separate code base, hard to debug, risky with BitLocker; boot-time failures can leave the system unbootable. |
| Online move of the Windows volume (VSS + filter driver) | Kernel driver, signing requirements, far beyond the project's risk budget. |

## Consequences

- No new privileged code in desktop mode; the safety rules stay as strict as they are.
- The plan format and identity matching become the contract between desktop and offline mode, so
  they keep their versioning (`schemaVersion`) and their tests.
- Offline support for each operation gets its own manual gate in a VM (maintainer decision: no
  agent ever boots or writes real hardware).
