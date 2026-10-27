<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# GParted UI analysis and WParted mapping

## Workflow GParted users expect

Select device → select partition (graphic or list) → plan an action (dialog) → the graphic shows the
*future* layout and the action appears in "pending operations" → Apply → confirmation → progress
dialog with expandable details → refreshed view.

WParted keeps this exact sequence. The difference is that WParted adds an explicit **safety summary**
before applying and **clearly separates planning (unprivileged) from execution (privileged)**.

## Element-by-element mapping

| GParted element | Behaviour | WParted v0.1 | WParted target |
|---|---|---|---|
| Menu `GParted` | Refresh, Devices list, Quit | `WParted` menu: Refresh (Ctrl+R/F5), Devices, Restart as Administrator, Quit (Ctrl+Q) | same |
| Menu `Edit` | Undo, Clear, Apply | present, disabled with status-bar explanation | functional (v0.2) |
| Menu `View` | Device Information, Pending Operations, File System Support | Device Information (dialog), Pending Operations toggle, Theme (System/Light/Dark), File System Support | Device information as side pane option |
| Menu `Device` | Create Partition Table, Attempt Data Rescue | Create Partition Table (disabled), Export Report (JSON) | + Data rescue (read-only scan) |
| Menu `Partition` | New, Delete, Resize/Move, Copy, Paste, Format to, Open/Close Encryption, Mount/Unmount, Name, Flags, Check, Label, New UUID, Information | all entries present; only Information enabled | progressively enabled per parity matrix |
| Menu `Help` | Contents, Keyboard shortcuts, About | Contents (opens online manual, EN/DE), About | + shortcut overview |
| (new) Menu `Tools` | — | Open Log Folder | diagnostics bundle, benchmark |
| Toolbar | New, Delete, Resize/Move, Copy, Paste, Undo, Apply | same order + Refresh; disabled items are visibly greyed in both themes and explain why via tooltip | same |
| Device combo | model, path, size | `Disk N  Vendor Model  (size)  [Windows]` | same |
| Disk graphic | proportional boxes, FS colour border, used/unused fill, nested logicals | implemented (`disk_map.cpp`), minimum width for tiny partitions, keyboard navigation (←/→/Home/End/Enter) | drag handles for resize planning |
| Partition list | Partition, Name, File System, Mount Point, Label, Size, Used, Unused, Flags | same columns + `Status` (Windows, mounted, system-protected, encrypted, table-not-read) | + warning icons |
| Pending operations | collapsible list | present (empty in read-only build) | full queue with reorder/undo |
| Status bar | scan progress, operation count | scan result, timings, diagnostics, elevation state | + background task progress |
| Partition Information | dialog with usage bar and details | TaskDialog with full technical details incl. alignment checks | dedicated dialog with usage bar |

## Deliberate improvements over GParted

1. **Unelevated by default.** The UI explains what is missing without admin rights and offers a
   one-click "Restart as administrator" (UAC shield).
2. **Honest unknowns.** When the partition table cannot be read, regions between Windows volumes are
   shown as *unknown (hatched)*, never as "unallocated", and no partition numbers are invented.
3. **System-critical markers.** ESP, MSR, recovery, dynamic-disk and Storage Spaces partitions and the
   volume containing the running Windows are marked and will be protected from changes.
4. **Diagnostics in plain language** in the status bar and device information (e.g. GPT backup header
   invalid, Windows layout differs from on-disk table).
5. **Dark mode** following the Windows app setting, plus explicit Light/Dark override.

## Known gaps in v0.1 (tracked as issues)

- Menu bar is not themed in dark mode (Win32 menu limitation; owner-drawn menu bar planned).
- Disk graphic has no MSAA/UIA provider yet; the partition list exposes the same information to
  screen readers.
- Information dialogs are TaskDialogs (text only); no usage bar.
- No drag-and-drop resize (requires the operation engine).
