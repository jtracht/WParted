<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Third-party licences

WParted itself: GPL-2.0-or-later, full text in [LICENSE](LICENSE).

No third-party source libraries are included in v0.1, so no further licence texts are required. When a
dependency is added, its full licence text is appended here and an entry is added to
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) and to the SBOM generator.

## Licence compatibility review (summary)

| Candidate for later phases | Licence | Compatible with GPL-2.0-or-later | Notes |
|---|---|---|---|
| GParted code (if ever reused) | GPL-2.0-or-later | Yes | Requires attribution and preserving notices |
| e2fsprogs tools (offline mode) | GPL-2.0 (programs) | Yes (separate programs) | Shipped as separate executables in the recovery image |
| ntfs-3g / ntfsprogs (offline mode) | GPL-2.0-or-later | Yes | Separate programs |
| Qt 5/6 (rejected, see ADR-0001) | LGPL-3.0 / commercial | LGPL-3 with GPL-2.0-only code would be problematic; fine with "or later" | Not used |
| Windows PE (recovery option) | Microsoft licence | Cannot be redistributed by us | Users build it with the Windows ADK |
