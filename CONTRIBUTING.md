<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Contributing to WParted

Thank you for helping. Development happens on **Gitea**:
https://git.tracht-digital.de/jtracht/WParted. GitHub (if present) is a mirror; pull requests there are
not merged.

## Ground rules

1. **Data safety first.** Never run write tests against physical disks. Use image files or VHD/VHDX.
2. **No fake functionality.** Unimplemented features stay disabled and are marked `Planned` in the
   parity matrix and in `capabilitiesFor()`.
3. **Licence.** Contributions are accepted under GPL-2.0-or-later. Every source file starts with
   `SPDX-License-Identifier: GPL-2.0-or-later`. Do not copy code or documentation from other projects
   without a documented licence review (add an entry to `THIRD_PARTY_NOTICES.md`).
4. **Core stays portable.** `core/` must not include Windows headers.

## Workflow

1. Pick or open an issue (title prefix `WP-nnn:`). Discuss larger changes first.
2. Branch from `main`: `feature/WP-nnn-short-name` or `fix/WP-nnn-short-name`.
3. Build and test locally: `.\tools\build.ps1 -Arch x64` and `-Arch x86`; run
   `python tools/ci/check_readonly.py` and `python tools/ci/check_spdx.py`.
4. Open a pull request on Gitea referencing the issue. CI must pass. Changes to write paths need a
   second human reviewer.
5. Update `CHANGELOG.md`, the parity matrix and docs in the same PR.

## Style

C++17, 4-space indentation, 120 columns (`.clang-format`), RAII for all handles, no exceptions across
module boundaries for expected errors (use diagnostics/return values), no global mutable state except
the logger.
