<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted design system

Implementation: `apps/wparted-gui/src/theme.cpp` (tokens), `disk_map.cpp`, `main_window.cpp`.

## Principles

1. **Familiar structure** – same information architecture and workflow as GParted.
2. **Honesty over decoration** – unknown data is shown as unknown; unavailable actions are visibly
   disabled and explain why.
3. **Risk is visible** – system-critical and encrypted partitions are labelled; future write actions
   get a risk badge and a safety summary.
4. **Native and light** – Win32 controls, GDI drawing, no animation framework; transitions ≤ 150 ms
   when introduced.

## Tokens

| Token | Light | Dark | Use |
|---|---|---|---|
| `window` | `#FAFAFA` | `#202020` | Window and list background |
| `panel` | `#F0F0F0` | `#2B2B2B` | Buttons, pending-operations panel |
| `text` | `#1B1B1B` | `#EEEEEE` | Primary text |
| `mutedText` | `#5F5F5F` | `#A8A8A8` | Status bar |
| `border` | `#C8C8C8` | `#454545` | Control outlines, hatch pattern |
| `accent` | `#005FB8` | `#4CA8FF` | Selection, hover outline |
| `unallocated` | `#A9A9A9` | `#6B6B6B` | Free space in the disk map |
| `used` | `#F8F4B8` | `#6E693A` | Used part of a partition box |
| `unused` | `#FFFFFF` | `#333333` | Unused part |
| `warning` | `#FFF4CE` | `#4D4014` | Elevation banner, warnings |

File system colours (border of partition boxes, list swatches): NTFS `#42E5AC`, FAT32 `#18D918`,
FAT12/16 `#00C05A`, exFAT `#2E8B57`, ReFS `#3B9CD9`, BitLocker `#494066`, ext2 `#9DB8D2`, ext3
`#7590AE`, ext4 `#314E6C`, btrfs `#FF9955`, XFS `#EED680`, F2FS `#DF4B2C`, swap `#C1665A`, LUKS
`#625B81`, LVM `#B19CD9`, Linux RAID `#82694D`, HFS/HFS+ `#E0B6AF`, APFS `#878787`, UDF `#105A7A`,
ZFS `#6B8E23`, extended container `#7DFCFE`, unknown `#000000`.

Spacing: base unit 4 px scaled by DPI (`scale(px, dpi)`); outer padding 8, control gap 4, group gap 16.
Typography: system message font (`NONCLIENTMETRICS.lfMessageFont`), scaled per monitor.

## Components

| Component | Behaviour | States | Keyboard | Accessibility |
|---|---|---|---|---|
| Toolbar button | Owner-drawn rounded rect | normal, hover (accent outline), pressed, disabled (muted text, no fill), focus rect | Tab, Space/Enter | Native button (name = label); tooltip on disabled explains "Planned" |
| Device selector | Drop-down list of disks | – | Tab, Alt+↓ | Native combo |
| Elevation banner | Warning-coloured text + shield button | visible only when unelevated | Tab to button | Native static + button |
| Disk map | Proportional boxes; FS-coloured 3 px border; used fill; extended container with nested logicals; hatched "unknown" regions | selected (2 px accent), focused (focus rect) | ←/→/Home/End select, Enter = information, Shift+F10 = context menu | Window name "Disk map"; UIA provider pending (list mirrors all data) |
| Partition list | Report view, full-row select, FS swatch | selected | ↑/↓, Enter = information, context menu | Native list view |
| Pending operations | List box | empty / items | Tab | Native |
| Status line | Scan summary, diagnostics, elevation state | – | – | Static text |
| Information dialog | TaskDialog with technical details | – | Esc closes | Native |

## Interaction rules

- Every modifying command is reachable from menu, toolbar (where GParted has it) and context menu.
- Commands that are not available are disabled, never hidden, and the status bar/tooltip says why.
- Selection is synchronised between disk map and list.
- Destructive confirmations (from v0.3) list: target disk (model + serial), partitions affected,
  data that will be lost, whether a reboot/offline step is needed.

## Known gaps

Menu bar not themed in dark mode; no UIA provider for the disk map yet; no reduced-motion setting
needed until animations exist.
