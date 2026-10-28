#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Checks WParted's native file-system operations (core/src/fs_*.cpp) with the
# reference tools: dosfstools + mtools (FAT create/label/serial/check/grow/
# shrink with relocated files and directories), exfatprogs, ntfs-3g,
# e2fsprogs and util-linux (labels, UUIDs). Sparse files only.
#
# Usage: tools/ci/fs-tools-check.sh <wparted_image_tool> <wparted_detect_fs>
set -euo pipefail
TOOL=$(readlink -f "${1:?usage: fs-tools-check.sh <image_tool> <detect_fs>}")
DETECT=$(readlink -f "${2:?usage: fs-tools-check.sh <image_tool> <detect_fs>}")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
export MTOOLS_SKIP_CHECK=1
fail=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
blk() { blkid -p -s "$2" -o value "$1" 2>/dev/null || true; }
same() { [[ "${2,,}" == "${3,,}" ]] && pass "$1 ($2)" || bad "$1: '$2' vs '$3'"; }
fsck_fat() { if fsck.fat -n "$1" >"$W/fsck.txt" 2>&1; then pass "$2: fsck.fat -n clean"; else bad "$2: fsck.fat"; cat "$W/fsck.txt"; fi; }
ours() { if "$TOOL" fs "$1" check "$2" >"$W/ours.txt"; then pass "$3: WParted check clean"; else bad "$3: WParted check"; cat "$W/ours.txt"; fi; }

head -c 2M /dev/urandom >"$W/payload.bin"
sha=$(sha256sum "$W/payload.bin" | cut -d' ' -f1)
payload_ok() {  # image, path, context
  rm -f "$W/out.bin"
  mcopy -i "$1" "$2" "$W/out.bin" 2>/dev/null || true
  [[ -f "$W/out.bin" && "$(sha256sum "$W/out.bin" | cut -d' ' -f1)" == "$sha" ]] && pass "$3: payload intact" || bad "$3: payload missing or changed"
}

# --- FAT: create, fill so that data sits at the end, shrink, grow --------------
fat_cycle() {  # fs, size MiB, filler MiB, shrink MiB, grow MiB
  local fs=$1 size=$2 filler=$3 shrink=$4 grow=$5 img="$W/$1.img"
  truncate -s "${size}M" "$img"
  "$TOOL" fs "$img" create "$fs" WPDATA || { bad "$fs: create"; return; }
  fsck_fat "$img" "$fs create"
  same "$fs create: blkid label" "$(blk "$img" LABEL)" "WPDATA"
  [[ "$(blk "$img" TYPE)" == "vfat" ]] && pass "$fs create: blkid type vfat" || bad "$fs create: type"
  head -c "${filler}M" /dev/zero >"$W/filler.bin"
  mcopy -i "$img" "$W/filler.bin" ::/filler.bin
  mmd -i "$img" ::/late ::/late/sub
  mcopy -i "$img" "$W/payload.bin" ::/late/sub/payload.bin
  mdel -i "$img" ::/filler.bin
  fsck_fat "$img" "$fs filled by mtools"
  "$TOOL" fs "$img" resize "$fs" $((shrink * 1024 * 1024)) && pass "$fs: shrink to ${shrink} MiB" || bad "$fs: shrink"
  truncate -s "${shrink}M" "$img"
  fsck_fat "$img" "$fs after shrink"
  ours "$img" "$fs" "$fs after shrink"
  payload_ok "$img" ::/late/sub/payload.bin "$fs after shrink (data was behind the new end)"
  mdir -i "$img" ::/late >/dev/null 2>&1 && pass "$fs after shrink: relocated directory readable" || bad "$fs: directory after shrink"
  truncate -s "${grow}M" "$img"
  "$TOOL" fs "$img" resize "$fs" $((grow * 1024 * 1024)) && pass "$fs: grow to ${grow} MiB" || bad "$fs: grow"
  fsck_fat "$img" "$fs after grow"
  ours "$img" "$fs" "$fs after grow"
  payload_ok "$img" ::/late/sub/payload.bin "$fs after grow"
  mcopy -i "$img" "$W/payload.bin" ::/again.bin && pass "$fs after grow: new space writable" || bad "$fs: write after grow"
  "$TOOL" fs "$img" label "$fs" "New Label" && same "$fs label: blkid" "$(blk "$img" LABEL)" "NEW LABEL" || bad "$fs: label"
  uuid=$("$TOOL" fs "$img" uuid "$fs") && same "$fs serial: blkid" "$(blk "$img" UUID)" "$uuid" || bad "$fs: serial"
  fsck_fat "$img" "$fs after label and serial"
}
fat_cycle fat32 256 170 120 400
fat_cycle fat16 64 40 30 120

# --- exFAT ---------------------------------------------------------------------
img=$W/exfat.img
truncate -s 64M "$img"
mkfs.exfat -L OLD "$img" >/dev/null
"$TOOL" fs "$img" label exfat "Neu Stick" && same "exfat label: blkid" "$(blk "$img" LABEL)" "Neu Stick" || bad "exfat: label"
uuid=$("$TOOL" fs "$img" uuid exfat) && same "exfat serial: blkid" "$(blk "$img" UUID)" "$uuid" || bad "exfat: serial"
if fsck.exfat -n "$img" >"$W/x.txt" 2>&1; then pass "exfat: fsck.exfat -n clean (boot checksum valid)"; else bad "exfat: fsck.exfat"; cat "$W/x.txt"; fi

# exFAT created by WParted
img=$W/exfat-new.img
truncate -s 200M "$img"
"$TOOL" fs "$img" create exfat "WP Neu" && pass "exfat: create" || bad "exfat: create"
if fsck.exfat -n "$img" >"$W/x.txt" 2>&1; then pass "exfat created by WParted: fsck.exfat -n clean"; else bad "exfat created: fsck.exfat"; cat "$W/x.txt"; fi
same "exfat created: blkid label" "$(blk "$img" LABEL)" "WP Neu"
same "exfat created: exfatlabel" "$(exfatlabel "$img" 2>/dev/null | sed -n 's/^label: //p')" "WP Neu"

# --- NTFS ----------------------------------------------------------------------
img=$W/ntfs.img
truncate -s 64M "$img"
mkfs.ntfs -q -Q -F -L OLD "$img" >/dev/null
"$TOOL" fs "$img" label ntfs "Neue Daten" || bad "ntfs: label"
same "ntfs label: ntfslabel" "$(ntfslabel "$img" 2>/dev/null || true)" "Neue Daten"
same "ntfs label: blkid" "$(blk "$img" LABEL)" "Neue Daten"
uuid=$("$TOOL" fs "$img" uuid ntfs) && same "ntfs serial: blkid" "$(blk "$img" UUID)" "$uuid" || bad "ntfs: serial"
if ntfsinfo -m "$img" >"$W/n.txt" 2>&1; then pass "ntfs: ntfs-3g opens the volume (\$MFTMirr matches)"; else bad "ntfs: ntfsinfo"; cat "$W/n.txt"; fi
# NTFS grow (with a larger $Bitmap) and shrink, data copied in with ntfscp
ntfscp -f "$img" "$W/payload.bin" payload.bin >/dev/null 2>&1 || bad "ntfs: ntfscp"
ntfs_ok() {  # context
  if ntfsresize --check --force --no-progress-bar "$img" >"$W/r.txt" 2>&1; then pass "$1: ntfsresize --check consistent"
  else bad "$1: ntfsresize --check"; cat "$W/r.txt"; fi
  ntfsinfo -m "$img" >/dev/null 2>&1 && pass "$1: ntfs-3g opens the volume" || bad "$1: ntfsinfo"
  [[ "$(ntfscat "$img" payload.bin 2>/dev/null | sha256sum | cut -d' ' -f1)" == "$sha" ]] && pass "$1: payload intact" || bad "$1: payload"
}
ntfs_ok "ntfs before resize"
truncate -s 512M "$img"
"$TOOL" fs "$img" resize ntfs $((512 * 1024 * 1024)) && pass "ntfs: grow 64 -> 512 MiB (\$Bitmap needs new clusters)" || bad "ntfs: grow"
ntfs_ok "ntfs after grow"
"$TOOL" fs "$img" resize ntfs $((200 * 1024 * 1024)) && pass "ntfs: shrink 512 -> 200 MiB" || bad "ntfs: shrink"
truncate -s 200M "$img"
ntfs_ok "ntfs after shrink"
ntfscp -f "$img" "$W/payload.bin" second.bin >/dev/null 2>&1 && pass "ntfs after shrink: writable" || bad "ntfs: write after shrink"

# --- ext2 / ext4 ---------------------------------------------------------------
for fs in ext2 ext4; do
  img=$W/$fs.img
  truncate -s 64M "$img"
  "mkfs.$fs" -q -F -L old "$img"
  "$TOOL" fs "$img" label "$fs" home-data && same "$fs label: blkid" "$(blk "$img" LABEL)" "home-data" || bad "$fs: label"
  uuid=$("$TOOL" fs "$img" uuid "$fs") && same "$fs uuid: blkid" "$(blk "$img" UUID)" "$uuid" || bad "$fs: uuid"
  if e2fsck -fn "$img" >"$W/e.txt" 2>&1; then pass "$fs: e2fsck -fn clean"; else bad "$fs: e2fsck"; cat "$W/e.txt"; fi
  same "$fs: WParted detection label" "$("$DETECT" "$img" | cut -f2)" "home-data"
done
dumpe2fs -h "$W/ext4.img" 2>/dev/null | grep -q metadata_csum_seed && pass "ext4: metadata_csum_seed keeps checksums valid" || bad "ext4: csum seed"

# --- Linux swap ----------------------------------------------------------------
img=$W/swap.img
truncate -s 16M "$img"
mkswap -L old "$img" >/dev/null
"$TOOL" fs "$img" label linux-swap swap-two && same "swap label: blkid" "$(blk "$img" LABEL)" "swap-two" || bad "swap: label"
uuid=$("$TOOL" fs "$img" uuid linux-swap) && same "swap uuid: blkid" "$(blk "$img" UUID)" "$uuid" || bad "swap: uuid"
swaplabel "$img" | grep -qi "$uuid" && pass "swap: swaplabel reads the new UUID" || bad "swap: swaplabel"

echo "fs tools check: $([[ $fail == 0 ]] && echo passed || echo FAILED)"
exit "$fail"
