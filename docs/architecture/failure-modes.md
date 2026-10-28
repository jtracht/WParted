<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Failure modes of plan execution (image files)

What happens when an execution on a disk image file is interrupted by a crash, power loss, a killed
process or a cancel. Physical disks are not written by WParted yet; the privileged helper
(ADR-0002) will have to meet the same rules.

## The execution journal

While a plan runs with a journal (GUI and CLI always use one), WParted keeps
`<image>.wparted-journal` next to the image:

- **body** (written once, before the first write): the plan, and a copy of every sector the layout
  detection read, so the plan can be rebuilt exactly against the original layout;
- **header** (512 bytes, rewritten in place, with a checksum): operations completed, the phase
  (`idle`, `copy`, `work`), the running operation and the bytes copied so far.

The image is flushed to disk (`_commit` / `fsync`) **before** the header records progress, so the
journal never claims more than is on disk. The journal is deleted when the plan finishes or fails
cleanly (the failure is then reported in full); it stays after a crash, a power loss or a cancel.
A new execution on an image with a journal is refused until the journal is resumed or discarded.

## Per phase

| Interrupted … | Image state | What WParted does |
|---|---|---|
| before the first write (validation, pre-checks) | unchanged | nothing to do; no journal is left |
| between two operations (`idle`) | consistent: the completed operations are applied and verified | **resume** continues with the next operation |
| during a block move or copy (`copy`) | the partition data is split between the old and the new position; the table still describes the old position | **resume** continues the copy where the journal says, then writes the table and the remaining steps |
| during a step that changes data in place (`work`): file-system create/resize/label/UUID, writing the partition table | unknown | **not resumable**; reported with the step name. Restore the image from a copy, then discard the journal |
| a damaged journal header (checksum) | unknown | refused as damaged; nothing is written |

## Why resuming a move is safe

A move whose source and destination overlap is copied in chunks no larger than the distance between
them, in the direction that never overwrites unread source data. The chunk being written when the
process dies therefore never overlaps the source data it was read from, and repeating it on resume
reads the same data again. Chunks before it are only recorded after they are on disk.

## What is checked

- Unit tests (`tests/unit/test_executor.cpp`): forward and backward overlapping moves "crashed" after
  the third chunk and resumed with the data pattern intact; cancel between operations and resume;
  a changed header refused as damaged.
- CI (`tools/ci/image-apply-check.sh`): the GPT demo plan is killed (`_Exit`, no clean-up) in the
  middle of its overlapping 64 MiB ext4 move; the half-moved file system is confirmed inconsistent,
  a new run is refused, resume finishes it, and `sgdisk -v`, `sfdisk`, `e2fsck -fn`, payload hashes
  and labels match the uninterrupted run.
- Windows 11: the GUI offers *Resume* for such an image and finishes the plan.

## Not covered

- A failing disk under the image (read or write errors) stops the plan; WParted does not retry.
- The operating system's own write cache below `fsync`/`_commit` is trusted.
- In-place steps have no undo; making a copy of the image first remains the advice.
