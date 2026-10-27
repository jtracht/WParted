#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Executes WParted plans on image files and checks the result with independent
# tools (WP-030/031/041/042): sgdisk -v and sfdisk for the partition tables,
# e2fsck and debugfs for the moved/copied file systems, blkid for cleared
# signatures. Works on sparse files in a temporary directory only.
#
# Usage: tools/ci/image-apply-check.sh <path to wparted_image_tool>
set -euo pipefail
TOOL=$(readlink -f "${1:?usage: image-apply-check.sh <image_tool>}")
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
fail=0
pass() { echo "PASS $*"; }
bad() { echo "FAIL $*"; fail=1; }
sector_of() { sgdisk -i "$2" "$1" | awk -v k="$3" '$0 ~ k {print $3; exit}'; }

# --- GPT: delete, create, move (+16 MiB), copy, flag -----------------------------
img=$W/gpt.img
truncate -s 256M "$img"
s1=2048; e1=67583; s2=67584; e2=198655   # 32 MiB and 64 MiB
sgdisk -n1:$s1:$e1 -t1:8300 -c1:first -n2:$s2:$e2 -t2:8300 -c2:data "$img" >"$W/sgdisk.txt" 2>&1 ||
  { cat "$W/sgdisk.txt"; exit 1; }
for i in 1 2; do
  sgdisk -i "$i" "$img" | grep -E "First sector|Last sector"
done
[[ "$(sector_of "$img" 1 "First sector")" == "$s1" && "$(sector_of "$img" 2 "Last sector")" == "$e2" ]] ||
  { echo "sgdisk did not create the expected partitions"; sgdisk -p "$img"; exit 1; }
n2=$((e2 - s2 + 1))
# Partition 1: a FAT file system that the new partition must not show any more.
truncate -s $(((e1 - s1 + 1) * 512)) "$W/p1.img"
mkfs.vfat -F 16 -n OLDFAT "$W/p1.img" >/dev/null
dd if="$W/p1.img" of="$img" bs=512 seek="$s1" conv=notrunc status=none
# Partition 2: ext4 with a random payload.
mkdir "$W/root"
head -c 3M /dev/urandom >"$W/root/payload.bin"
sha=$(sha256sum "$W/root/payload.bin" | cut -d' ' -f1)
truncate -s $((n2 * 512)) "$W/p2.img"
mkfs.ext4 -q -F -L wp-data -d "$W/root" "$W/p2.img"
dd if="$W/p2.img" of="$img" bs=512 seek="$s2" conv=notrunc status=none

"$TOOL" plan "$img" gpt-demo "$W/plan.json"
if "$TOOL" apply "$img" "$W/plan.json"; then pass "gpt: plan applied"; else bad "gpt: apply failed"; fi

sgdisk -v "$img" >"$W/verify.txt" 2>&1 || true
if grep -q "No problems found" "$W/verify.txt"; then pass "gpt: sgdisk -v reports no problems"
else bad "gpt: sgdisk -v"; cat "$W/verify.txt"; fi

sfdisk --json "$img" >"$W/layout.json"
moved=$((s2 + 32768))
copy=$(((moved + n2 + 2047) / 2048 * 2048))
if python3 - "$W/layout.json" "$s1" "$moved" "$copy" "$n2" <<'PY'
import json, sys
t = json.load(open(sys.argv[1]))["partitiontable"]
s1, moved, copy, n2 = map(int, sys.argv[2:])
parts = sorted((p["start"], p["size"], p.get("name", "")) for p in t["partitions"])
want = [(s1, None, "wparted-new"), (moved, n2, "data"), (copy, n2, "data")]
ok = t["label"] == "gpt" and len(parts) == 3 and all(
    p[0] == w[0] and (w[1] is None or p[1] == w[1]) and p[2] == w[2] for p, w in zip(parts, want))
print(parts)
sys.exit(0 if ok else 1)
PY
then pass "gpt: sfdisk sees the planned geometry"; else bad "gpt: geometry differs from the plan"; fi

for start in "$moved" "$copy"; do
  dd if="$img" of="$W/check.img" bs=512 skip="$start" count="$n2" status=none
  if e2fsck -fn "$W/check.img" >"$W/fsck.txt" 2>&1; then pass "ext4 at sector $start: e2fsck clean"
  else bad "ext4 at sector $start: e2fsck"; cat "$W/fsck.txt"; fi
  rm -f "$W/out.bin"
  debugfs -R "dump /payload.bin $W/out.bin" "$W/check.img" >/dev/null 2>&1 || true
  if [[ -f "$W/out.bin" && "$(sha256sum "$W/out.bin" | cut -d' ' -f1)" == "$sha" ]]; then pass "ext4 at sector $start: payload intact"
  else bad "ext4 at sector $start: payload missing or changed"; fi
  [[ "$(blkid -p -s LABEL -o value "$W/check.img" || true)" == "wp-data" ]] && pass "ext4 at sector $start: label" || bad "ext4 at sector $start: label"
done
dd if="$img" of="$W/new.img" bs=512 skip="$s1" count=4096 status=none
if [[ -z "$(blkid -p -o value -s TYPE "$W/new.img" || true)" ]]; then pass "gpt: old FAT signature cleared in the new partition"
else bad "gpt: old signature still present"; fi

# --- msdos: new table, primary, extended, two logicals ---------------------------
mbr=$W/mbr.img
truncate -s 128M "$mbr"
"$TOOL" plan "$mbr" mbr-demo "$W/mbr.json"
if "$TOOL" apply "$mbr" "$W/mbr.json"; then pass "mbr: plan applied"; else bad "mbr: apply failed"; fi
if sfdisk -V "$mbr" >"$W/v.txt" 2>&1; then pass "mbr: sfdisk -V clean"; else bad "mbr: sfdisk -V"; cat "$W/v.txt"; fi
sfdisk --json "$mbr" >"$W/mbr-layout.json"
if python3 - "$W/mbr-layout.json" <<'PY'
import json, sys
t = json.load(open(sys.argv[1]))["partitiontable"]
nodes = sorted(int(p["node"].rsplit("img", 1)[1]) for p in t["partitions"])
starts = sorted(p["start"] for p in t["partitions"])
print(nodes, starts)
sys.exit(0 if t["label"] == "dos" and nodes == [1, 2, 5, 6] and starts[0] == 2048 else 1)
PY
then pass "mbr: sfdisk sees primary, extended and logicals 5 and 6"; else bad "mbr: unexpected layout"; fi

echo "image apply check: $([[ $fail == 0 ]] && echo passed || echo FAILED)"
exit "$fail"
