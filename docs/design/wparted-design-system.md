<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted design system

Implementation: `apps/wparted-gui/src/theme.cpp` (tokens, fonts, drawing helpers), `disk_map.cpp`,
`inspector.cpp`, `form_dialog.cpp`, `menubar.cpp`, `main_window.cpp`, `planner.cpp`.
Visual language: Fluent-inspired (Windows 11), implemented with Win32 + GDI so that it also runs
on Windows 7; on older systems the same layout falls back to Segoe UI and Segoe MDL2 Assets.

## Principles

1. **Familiar structure** – same information architecture and workflow as GParted: select → plan →
   review → apply.
2. **One thread through the window (roter Faden)** – top to bottom the window follows the workflow:
   command bar (actions, *Apply* as the only primary button at the right), device card with disk
   map, partition list next to the details pane, pending operations, status bar.
3. **Honesty over decoration** – unknown data is shown as unknown; unavailable actions are visibly
   disabled and the details pane says why.
4. **Risk is visible** – system-critical and encrypted partitions are labelled; every planned
   operation has a risk level; the Apply summary lists blocking errors before anything else.
5. **Native and light** – Win32 controls, GDI drawing, no animation framework; transitions ≤ 150 ms
   when introduced.

## Tokens

The accent follows the Windows accent colour (`HKCU\Software\Microsoft\Windows\DWM\AccentColor`),
darkened (light theme) or lightened (dark theme) until the contrast is sufficient; without it the
Fluent default blue is used.

| Token | Light | Dark | Use |
|---|---|---|---|
| `window` | `#F3F3F3` | `#202020` | Layer behind cards (Mica-like neutral) |
| `panel` | `#FBFBFB` | `#2D2D2D` | Control fill (buttons, inputs) |
| `card` | `#FFFFFF` | `#2B2B2B` | Cards, lists, dialogs |
| `subtle` | `#F0F0F0` | `#343434` | Hover fill |
| `pressed` | `#E6E6E6` | `#292929` | Pressed fill |
| `border` | `#E5E5E5` | `#3D3D3D` | Card and control stroke |
| `text` | `#1A1A1A` | `#FFFFFF` | Primary text |
| `mutedText` | `#5D5D5D` | `#C5C5C5` | Secondary text, captions |
| `disabledText` | `#A0A0A0` | `#787878` | Disabled commands |
| `accent` | system accent, default `#005FB8` | system accent, default `#60CDFF` | Primary button, selection ring, focus |
| `onAccent` | `#FFFFFF` | `#000000` | Text on accent |
| `selection` | accent 14 % on white | accent 22 % on card | Selected list row |
| `warning` / `caution` | `#FFF4CE` / `#9D5D00` | `#433519` / `#FCE100` | Info bar background / foreground |
| `danger` | `#C42B1C` | `#FF99A4` | Errors, high risk |
| `success` | `#0F7B0F` | `#6CCB5F` | Passed checks |
| `unallocated` | `#C8C8C8` | `#555555` | Free space in the disk map |
| `used` | `#D6E9F8` | `#2F4A62` | Used part of a partition box |
| `unused` | `#FFFFFF` | `#333333` | Unused part |

File system colours (border of partition boxes, list swatches): NTFS `#42E5AC`, FAT32 `#18D918`,
FAT12/16 `#00C05A`, exFAT `#2E8B57`, ReFS `#3B9CD9`, BitLocker `#494066`, ext2 `#9DB8D2`, ext3
`#7590AE`, ext4 `#314E6C`, btrfs `#FF9955`, XFS `#EED680`, F2FS `#DF4B2C`, swap `#C1665A`, LUKS
`#625B81`, LVM `#B19CD9`, Linux RAID `#82694D`, HFS/HFS+ `#E0B6AF`, APFS `#878787`, UDF `#105A7A`,
ZFS `#6B8E23`, extended container `#7DFCFE`, unknown `#000000`.

Spacing: base unit 4 px scaled per monitor DPI (`scale(px, dpi)`); card padding 12, segment gap 3,
group gap 16. Corner radius: cards 8, partition segments 6, pills fully rounded.
Typography (Fluent type ramp, px at 96 DPI): Caption 12, Body 14, Body Strong 14 semibold, Subtitle 20
semibold, Title 28 semibold; face Segoe UI Variable Text, falling back to Segoe UI. Icons: Segoe Fluent
Icons, falling back to Segoe MDL2 Assets; without an icon font, buttons show text only.

## Components

| Component | Behaviour | States | Keyboard | Accessibility |
|---|---|---|---|---|
| Command bar | Owner-drawn icon + label buttons; *Apply* right-aligned in accent | normal, hover (`subtle`), pressed, disabled (`disabledText`), focus rect | Tab, Space/Enter; accelerators Ctrl+Z, Ctrl+Enter, Ins, Del, Ctrl+C/V | Native buttons (name = label); tooltips |
| Device selector | Drop-down list of disks and opened images | – | Tab, Alt+↓ | Native combo |
| Info bar | Warning-coloured strip with action button (e.g. *Restart as Administrator*); hidden for images | visible only when relevant | Tab to button | Native static + button |
| Device card | Header with device icon, model, size · bus · table · sector sizes and chips (Image, Windows, SSD/HDD, not read) | – | – | Text mirrored in the status bar |
| Disk map | Rounded segments sized proportionally, file system colour bar on top, usage fill, nested logicals, hatched "unknown" | hover, selected (2 px accent ring), focused | ←/→/Home/End, Enter = information, Shift+F10 = context menu | Window name "Disk map"; UIA provider pending (list mirrors all data) |
| Partition list | Report view in a card; file system pills; accent selection; Status column fills the width | selected, hover | ↑/↓, Enter, context menu | Native list view |
| Details pane (inspector) | Title, status pill, key/value rows, hint (normal or warning) for the selection; scrolls | – | mouse wheel | Static text |
| Pending operations | Owner-drawn list: risk dot (low → critical: success, accent, caution, danger) and description; validation warnings/errors indented with icon | empty / items / warning / error | Tab, ↑/↓ | Native list box |
| Form dialog | Modal card with layout preview at the top, fields (number, text, choice, check, note), footer band with accent OK | OK disabled while input is invalid | Tab, Enter = OK, Esc = cancel | Native controls with labels |
| Apply summary / information dialogs | TaskDialog: operations, checks with ✓/✗, export buttons | – | Esc closes | Native |
| Menu bar | Themed in dark mode (UAH draw messages); light mode native | hot, pushed | Alt, mnemonics | Native menu |

## Interaction rules

- Every modifying command is reachable from menu, toolbar (where GParted has it) and context menu.
- Commands that are not available are disabled, never hidden, and the status bar/tooltip says why.
- Selection is synchronised between disk map and list.
- Destructive confirmations (from v0.3) list: target disk (model + serial), partitions affected,
  data that will be lost, whether a reboot/offline step is needed.

## Known gaps

No UIA provider for the disk map yet; high-contrast mode is not yet mapped to system colours
(WP-017); no reduced-motion setting needed until animations exist. Dark mode for native controls
and the menu bar uses undocumented uxtheme ordinals on Windows 10 1809+ and is skipped elsewhere.
