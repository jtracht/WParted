<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Third-party notices

WParted v0.1 contains **no third-party source code**.

| Component | How it is used | Licence | Redistributed |
|---|---|---|---|
| Microsoft Visual C++ runtime (static, `/MT`) | Linked into the executables | Microsoft Visual Studio licence terms (redistributable code) | Yes, as part of the binaries |
| Windows SDK headers and import libraries | Build only | Microsoft Windows SDK licence | No |
| Windows system DLLs (kernel32, user32, gdi32, comctl32, comdlg32, shell32, ole32, advapi32, dwmapi, uxtheme) | Loaded at run time from the OS | Part of Windows | No |
| GNU General Public License v2 text | `LICENSE` | Verbatim copying permitted | Yes |

## References (no code reused)

- **GParted** (https://gitlab.gnome.org/GNOME/gparted, GPL-2.0-or-later; manual GFDL-1.2) is used as
  a functional reference only. No GParted source code, documentation text, logos or icons are included.
- On-disk formats were implemented from public specifications: UEFI Specification (GPT), Microsoft
  FAT32 File System Specification, and published superblock layouts of the respective file systems.

The machine-readable bill of materials is generated per release (`WParted-<version>-sbom.json`,
CycloneDX 1.5) by `tools/generate_sbom.py`.
