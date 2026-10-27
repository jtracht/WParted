#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Creates file system images with the real mkfs tools (on sparse files, never
# on devices) and checks that WParted detects type, label and UUID exactly as
# blkid (util-linux) reports them. Runs in the Linux CI job (WP-011).
#
# Usage: tools/ci/mkfs-fixtures.sh <path to detect_fs binary>
set -euo pipefail
DETECT=${1:?usage: mkfs-fixtures.sh <detect_fs>}
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
fail=0
pass=0

# name, size MiB, expected WParted type, label, mkfs command (image path appended)
cases=(
  "ext2|64|ext2|wp-ext2|mkfs.ext2 -q -F -L wp-ext2"
  "ext3|64|ext3|wp-ext3|mkfs.ext3 -q -F -L wp-ext3"
  "ext4|64|ext4|wp-ext4|mkfs.ext4 -q -F -L wp-ext4"
  "fat16|64|fat16|WPFAT16|mkfs.vfat -F 16 -n WPFAT16"
  "fat32|300|fat32|WPFAT32|mkfs.vfat -F 32 -n WPFAT32"
  "exfat|64|exfat|WPEXFAT|mkfs.exfat -L WPEXFAT"
  "ntfs|64|ntfs|wp-ntfs|mkfs.ntfs -q -F -f -L wp-ntfs"
  "btrfs|256|btrfs|wp-btrfs|mkfs.btrfs -q -f -L wp-btrfs"
  "xfs|512|xfs|wp-xfs|mkfs.xfs -q -f -L wp-xfs"
  "f2fs|256|f2fs|wp-f2fs|mkfs.f2fs -q -f -l wp-f2fs"
  "swap|64|linux-swap|wp-swap|mkswap -L wp-swap"
)

for c in "${cases[@]}"; do
  IFS='|' read -r name size want_type want_label cmd <<<"$c"
  tool=${cmd%% *}
  if ! command -v "$tool" >/dev/null; then
    echo "MISSING $name: $tool not installed"; fail=$((fail + 1)); continue
  fi
  img="$WORK/$name.img"
  truncate -s "${size}M" "$img"
  if ! $cmd "$img" >"$WORK/$name.log" 2>&1; then
    echo "MKFS-FAILED $name"; cat "$WORK/$name.log"; fail=$((fail + 1)); continue
  fi
  before=$(sha256sum "$img" | cut -d' ' -f1)
  out=$("$DETECT" "$img")  # tab separated; cut keeps empty fields
  got_type=$(cut -f1 <<<"$out")
  got_label=$(cut -f2 <<<"$out")
  got_uuid=$(cut -f3 <<<"$out")
  ref_uuid=$(blkid -p -s UUID -o value "$img" 2>/dev/null || true)
  ref_label=$(blkid -p -s LABEL -o value "$img" 2>/dev/null || true)
  after=$(sha256sum "$img" | cut -d' ' -f1)
  problems=()
  [[ "$got_type" == "$want_type" ]] || problems+=("type '$got_type' != '$want_type'")
  [[ "$got_label" == "$want_label" ]] || problems+=("label '$got_label' != '$want_label'")
  [[ "$ref_label" == "$want_label" ]] || problems+=("blkid label '$ref_label' (fixture problem)")
  if [[ -n "$got_uuid" ]]; then
    [[ "${got_uuid,,}" == "${ref_uuid,,}" ]] || problems+=("uuid '$got_uuid' != blkid '$ref_uuid'")
    uuid_note="uuid $got_uuid"
  else
    uuid_note="uuid not read (blkid: $ref_uuid)"
  fi
  [[ "$before" == "$after" ]] || problems+=("image modified")
  if ((${#problems[@]})); then
    echo "FAIL $name: ${problems[*]}"; fail=$((fail + 1))
  else
    ver=$($tool -V 2>&1 | head -1 || true)
    [[ "$ver" == *"invalid option"* ]] && ver=$($tool --version 2>&1 | head -1 || true)
    echo "PASS $name: $got_type '$got_label' $uuid_note ($ver)"; pass=$((pass + 1))
  fi
done
echo "mkfs fixtures: $pass passed, $fail failed"
((fail == 0))
