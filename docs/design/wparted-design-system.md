<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# WParted design system

Implementation: `apps/wparted-gui/src/theme.cpp` (tokens, fonts, drawing helpers), `disk_map.cpp`,
`inspector.cpp`, `form_dialog.cpp`, `menubar.cpp`, `main_window.cpp`, `planner.cpp`.
Visual language: the friendly look of the [website](https://wparted.tracht-digital.de/), implemented natively with
Win32 + GDI: violet and pastel colours, soft rounded cards with a lower edge, bold headings and Parti, the mascot.

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
6. **Friendly, not careless** – Parti and soft colours make the tool approachable; warnings, risk levels and
   confirmations stay exactly as clear as before. Parti never appears in a warning or an error.

## Tokens

The colours are those of the website and do not follow the Windows accent colour. In Windows high-contrast mode all
of them are replaced by system colours.

| Token | Light | Dark | Use |
|---|---|---|---|
| `window` | `#F3F6FF` | `#1C1830` | Layer behind cards |
| `panel` | `#FFFFFF` | `#2A2447` | Control fill |
| `card` | `#FFFFFF` | `#2A2447` | Cards, lists, dialogs |
| `subtle` | `#E8ECFF` | `#332B57` | Hover fill, disabled primary button |
| `pressed` | `#E2DBFF` | `#362D63` | Pressed fill |
| `border` | `#D6DAF2` | `#3D3566` | Card stroke and lower edge |
| `text` | `#2B2547` | `#F1EEFF` | Primary text |
| `mutedText` | `#565072` | `#BDB6DC` | Secondary text, captions |
| `disabledText` | `#9A96B4` | `#7A7399` | Disabled commands |
| `accent` / `accentLip` | `#5B3FD6` / `#3F2AA3` | `#A996FF` / `#7C66E6` | Primary button and its lower edge, selection ring, icons |
| `onAccent` | `#FFFFFF` | `#1C1830` | Text on accent |
| `selection` | `#E2DBFF` | `#362D63` | Selected list row |
| `warning` / `caution` | `#FFF3C9` / `#855714` | `#3D3320` / `#E9B455` | Info bar background / foreground |
| `danger` | `#B3261E` | `#FF7A6E` | Errors, high risk |
| `success` | `#256740` | `#5FD394` | Passed checks |
| `unallocated` / `unused` | `#E3E6F5` / `#F3F5FD` | `#3A3361` / `#2A2447` | The two stripes of free space |

File system colours are pastels, the same in both themes, with dark text: NTFS `#9FE3C8`, FAT32 `#FFE08A`, FAT12/16
`#FFEAB0`, exFAT `#FFB3CF`, ReFS `#A9D8F5`, BitLocker `#B9AEDD`, ext2 `#DFD6FF`, ext3 `#D3C7FF`, ext4 `#C7B8FF`, btrfs
`#FFC9A3`, XFS `#EEDC9A`, F2FS `#FFB8A8`, swap `#F5B5AC`, LUKS `#CDB8E8`, LVM `#E3C8F5`, Linux RAID `#D8C4AA`, HFS/HFS+
`#F2CFC9`, APFS `#CFCFD8`, UDF `#A6DCE0`, ZFS `#CFE3A0`, unknown `#D3D3E0`.

Spacing: base unit 4 px scaled per monitor DPI (`scale(px, dpi)`); card padding 12, segment gap 3, group gap 16. Corner
radius: cards 16-18, partition blocks 12, buttons and pills fully rounded; rounded shapes are anti-aliased. Cards and
partition blocks rest on a lower edge of 4-5 px, the flat equivalent of the website's shadows.
Typography (px at 96 DPI): Caption 12, Body 14, Body Strong 14 bold, Section 16 bold, Subtitle 20 bold, Title 28 bold; face
Segoe UI Variable Text, falling back to Segoe UI. The website's Plus Jakarta Sans is not embedded: the program uses the
system font and ships no font files. Icons: Segoe Fluent Icons, falling back to Segoe MDL2 Assets; without an icon font,
buttons show text only.

### Parti

Parti is the mascot from the website: a smiling hard disk whose platter shows three partitions. Sources:
`docs/images/parti.svg` and `tools/generate_icon.py`, which draws the icon (`apps/wparted-gui/res/wparted.ico`, sizes 16 to
256, with fewer details in the small ones) and `docs/images/parti.png`. In the program Parti appears as the application
icon and in empty states: in the disk card while no disk is selected and in the pending card while nothing is planned.

## Components

| Component | Behaviour | States | Keyboard | Accessibility |
|---|---|---|---|---|
| Command bar | Owner-drawn icon + label buttons, pill on hover; *Apply* right-aligned as violet pill with a lower edge that flattens when pressed | normal, hover (`subtle`), pressed, disabled (`disabledText`), focus rect | Tab, Space/Enter; accelerators Ctrl+Z, Ctrl+Enter, Ins, Del, Ctrl+C/V | Native buttons (name = label); tooltips |
| Device selector | Drop-down list of disks and opened images | – | Tab, Alt+↓ | Native combo |
| Info bar | Warning-coloured strip with action button (e.g. *Restart as Administrator*); hidden for images | visible only when relevant | Tab to button | Native static + button |
| Device card | Header with device icon, model, size · bus · table · sector sizes and chips (Image, Windows, SSD/HDD, not read) | – | – | Text mirrored in the status bar |
| Disk map | Rounded pastel blocks sized proportionally on a darker lower edge; used part solid, free part lighter; free space striped; nested logicals, hatched "unknown" | hover, selected (3 px accent ring), focused | ←/→/Home/End, Enter = information, Shift+F10 = context menu | MSAA list, one item per segment ("#1 ESP, fat32, 16.00 MiB"), selection and focus events |
| Partition list | Report view in a card; pastel file system pills; violet selection tint; Status column fills the width | selected, hover | ↑/↓, Enter, context menu | Native list view |
| Details pane (inspector) | Title, status pill, key/value rows, hint (normal or warning) for the selection; scrolls | – | mouse wheel | Static text |
| Pending operations | Owner-drawn list: risk dot (low → critical: success, accent, caution, danger) and description; validation warnings/errors indented with icon; Parti while empty | empty / items / warning / error | Tab, ↑/↓ | Native list box |
| Form dialog | Modal card with layout preview at the top (striped track, pastel block), fields (number, text, choice, check, note), footer band with violet OK | OK disabled while input is invalid | Tab, Enter = OK, Esc = cancel | Native controls with labels |
| Apply summary / information dialogs | TaskDialog: operations, checks with ✓/✗, export buttons | – | Esc closes | Native |
| Menu bar | Themed in dark mode (UAH draw messages); light mode native | hot, pushed | Alt, mnemonics | Native menu |

## Interaction rules

- Every modifying command is reachable from menu, toolbar (where GParted has it) and context menu.
- Commands that are not available are disabled, never hidden, and the status bar/tooltip says why.
- Selection is synchronised between disk map and list.
- Destructive confirmations (from v0.3) list: target disk (model + serial), partitions affected,
  data that will be lost, whether a reboot/offline step is needed.

## Known gaps

- The disk map exposes its segments through MSAA (a list with one item per segment: name, state,
  location, selection and focus events); UI Automation reaches it through the MSAA proxy. A native
  UIA provider is not implemented.
- High-contrast mode maps the palette to system colours (`GetSysColor`). The disk map, buttons and list were redrawn for
  this design and have to be checked visually in high contrast again (WP-017).
- The design was checked at 100 % scaling on Windows 11; other scaling factors are computed through `scale()`, but were
  not looked at.
- Combo boxes, scroll bars and the column lines of the list come from Windows and keep their native look.
- All planning texts, validation and partition-table diagnostics and the information dialogs are
  translated; reasons inside GPT diagnostics (e.g. "missing signature") and bus names stay English.
- Dark mode for native controls and the menu bar uses undocumented uxtheme ordinals.
- No reduced-motion setting needed until animations exist.
