<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Validation rules

Implementation: `core/src/validation.cpp` (plan rules) and `core/src/partition_table.cpp` (table
diagnostics). Every diagnostic has a stable `code`, an English `message` (CLI, logs, plan files), the
1-based `operation` it belongs to (0 = whole plan or layout) and the message parameters in `args`.
The GUI translates by `code` and `args` (`apps/wparted-gui/src/planner.cpp`, `diagText`) and falls
back to the English message for unknown codes.

## Modes

| Mode | Used for | Capability gaps of this build |
|---|---|---|
| `Planning` | the pending-operations list for physical disks | Warning |
| `Execution` | the Apply summary for physical disks (always adds `execution.unavailable`) | Error |
| `ImageExecution` | the Apply summary and the planning view for disk **image files**, `wparted-cli apply-plan` | Error |

In `ImageExecution` the executor (`core/src/executor.cpp`) can change partition tables and move or
copy partition contents as raw blocks. It cannot create, grow, shrink, check, label or re-UUID file
systems. A move that keeps the size and every copy is a raw block operation and is allowed for any
file system. System partitions are warnings, because an image is not the running system.

## Plan rules

| Code | Severity (Planning / Execution / Image) | Args | Meaning |
|---|---|---|---|
| `plan.source_errors` | Error (Warning if the plan creates a new table) | – | The current table has errors; only a new table can be planned. |
| `device.write_protected` | Error | – | The device reports write protection. |
| `device.managed_by_windows` | Error | – | Dynamic disk (LDM), Storage Spaces member or spanned volume. |
| `plan.identity_mismatch` | – / Error / Error | why | The plan was made for another disk or the disk changed (serial, size, sector size, table type, disk GUID/MBR signature). |
| `plan.identity_unknown` | – / Error / Error | – | The current identity could not be determined. |
| `execution.unavailable` | – / Error / – | – | This build cannot execute on physical disks. |
| `op.invalid` | Error | error | The operation cannot be simulated on the layout before it. |
| `op.system_critical` | Error / Error / Warning | `windows`, number or `type`, number, type | Running Windows, page file, ESP, MSR, recovery, LDM or Storage Spaces partition. |
| `op.busy` | Warning / Error / Error | number | The partition is mounted. |
| `op.encrypted` | Error | number | Resize, move, copy or new UUID on a BitLocker volume. |
| `op.encrypted_format` | Warning | number | Formatting destroys BitLocker-encrypted data. |
| `op.capability` | Warning / Error / Error | action, file system, support level | A file-system action this build cannot do (`create`, `grow`, `shrink`, `move`, `copy`, `check`, `label`, `uuid`). |
| `op.shrink_below_used` | Error | used size | The new size is smaller than the space in use. |
| `op.too_small` | Error | number, file system, minimum | A partition is smaller than its file system allows. |
| `layout.out_of_bounds` | Error | number | Outside the usable area of the disk. |
| `layout.overlap` | Error | `primary` or `logical`, number, number | Two partitions overlap. |
| `layout.unaligned` | Warning | number, alignment | Not on a 1 MiB boundary. |
| `layout.physical_unaligned` | Warning | number | Not on a physical sector boundary (performance). |
| `layout.too_many_primary` | Error | – | More than four primary/extended entries in an msdos table. |
| `layout.multiple_extended` | Error | – | More than one extended partition. |
| `layout.logical_without_extended` | Error | – | Logical partitions without an extended partition. |
| `layout.logical_outside_extended` | Error | number | A logical partition outside the extended partition. |
| `layout.no_room_for_ebr` | Error | number | No free sector in front of a logical partition for its EBR. |
| `mbr.limit_2tib` | Error | number | Beyond the 2 TiB limit of msdos tables. |
| `mbr.multiple_bootable` | Warning | – | More than one partition is marked bootable. |

## Partition-table diagnostics (reading)

| Code | Severity | Args |
|---|---|---|
| `gpt.no_valid_header` | Error | reason primary, reason backup |
| `gpt.primary_invalid` | Error | reason |
| `gpt.backup_invalid` | Warning | reason |
| `gpt.primary_backup_mismatch` | Error | – |
| `gpt.backup_location` | Warning | – |
| `gpt.entry_reversed`, `gpt.entry_outside_usable` | Error | number |
| `gpt.sector_size_mismatch` | Error | sector size of the GPT, sector size of the device |
| `mbr.ebr_loop`, `mbr.too_many_logical`, `mbr.ebr_unreadable`, `mbr.ebr_signature`, `mbr.multiple_extended` | Error | – |
| `mbr.logical_outside_extended` | Error | number |
| `mbr.ebr_link_type`, `mbr.invalid_entries` | Warning | – |
| `layout.beyond_device` | Error | number |
| `layout.overlap` | Error | number, number |
| `device.too_small`, `device.read_failed` | Error | – |
| `layout.whole_device_fs` | Info | file system |
| `table.foreign` | Info | table name |

## Tests

Every plan rule has a unit test in `tests/unit/test_operations.cpp`, `test_rescue.cpp` or
`test_executor.cpp`; the structured `operation`/`args` fields are tested in `test_rescue.cpp`.
