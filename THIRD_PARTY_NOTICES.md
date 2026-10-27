<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Third-party notices

WParted contains **no third-party source code**. Official release binaries are built with MinGW-w64
GCC and statically include the following runtime code:

| Component | How it is used | Licence | Redistributed |
|---|---|---|---|
| mingw-w64 CRT startup/support code and winpthreads | Statically linked (release builds) | Mainly ZPL-2.1 / public domain, winpthreads MIT + BSD-3-Clause; full text in `licenses/mingw-w64-runtime-and-winpthreads.txt` | Yes, in the binaries |
| GCC runtime libraries (libgcc, libstdc++) | Statically linked (release builds) | GPL-3.0-or-later **with GCC Runtime Library Exception 3.1**, which permits distributing the combined work under WParted's licence; full text in `licenses/gcc-runtime-libraries.txt` | Yes, in the binaries |
| Microsoft Visual C++ runtime (static, `/MT`) | Only in local MSVC developer builds, not in releases | Microsoft Visual Studio licence terms (redistributable code) | No (not in official releases) |
| Windows SDK headers and import libraries | Build only | Microsoft Windows SDK licence | No |
| Windows system DLLs (kernel32, user32, gdi32, comctl32, comdlg32, shell32, ole32, advapi32, msvcrt, dwmapi, uxtheme) | Loaded at run time from the OS | Part of Windows | No |
| GNU General Public License v2 text | `LICENSE` | Verbatim copying permitted | Yes |

## References (no code reused)

- **GParted** (https://gitlab.gnome.org/GNOME/gparted, GPL-2.0-or-later; manual GFDL-1.2) is used as
  a functional reference only. No GParted source code, documentation text, logos or icons are included.
- On-disk formats were implemented from public specifications: UEFI Specification (GPT), Microsoft
  FAT32 File System Specification, and published superblock layouts of the respective file systems.

The machine-readable bill of materials is generated per release (`WParted-<version>-sbom.json`,
CycloneDX 1.5) by `tools/generate_sbom.py`.
