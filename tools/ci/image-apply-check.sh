#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
#
# Executes WParted plans on image files and checks the result with independent
# tools (WP-030/031/041/042): sgdisk -v and sfdisk for the partition tables,
# e2fsck and debugfs for the moved/copied file systems, blkid for cleared
# signatures. The same plan is also killed in the middle of its overlapping
# move (fault injection: the process ends without clean-up) and resumed from
# its journal; the result must pass the same checks. Works on sparse files in
# a temporary directory only.
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
cp --sparse=always "$img" "$W/crash.img"
if "$TOOL" apply "$img" "$W/plan.json"; then pass "gpt: plan applied"; else bad "gpt: apply failed"; fi

# Fault injection: the overlapping move of partition 2 (+16 MiB, 64 MiB) is
# copied in 4 MiB chunks; the process dies after writing the third one.
set +e
"$TOOL" apply "$W/crash.img" "$W/plan.json" --journal --chunk 4194304 --crash-after-chunks 3 >"$W/crash.txt" 2>&1
rc=$?
set -e
if [[ $rc == 75 && -f "$W/crash.img.wparted-journal" ]]; then pass "crash: process killed mid-move, journal left behind"
else bad "crash: expected exit 75 and a journal (exit $rc)"; cat "$W/crash.txt"; fi
dd if="$W/crash.img" of="$W/half.img" bs=512 skip="$((s2 + 32768))" count="$n2" status=none
if ! e2fsck -fn "$W/half.img" >/dev/null 2>&1; then pass "crash: the half-moved file system is really inconsistent"
else bad "crash: the interruption did not hit the move"; fi
if "$TOOL" apply "$W/crash.img" "$W/plan.json" --journal >"$W/again.txt" 2>&1; then bad "crash: a new run ignored the journal"
else pass "crash: a new run is refused while the journal exists"; fi
if "$TOOL" resume "$W/crash.img"; then pass "crash: resumed from the journal"; else bad "crash: resume failed"; fi
[[ ! -f "$W/crash.img.wparted-journal" ]] && pass "crash: journal removed after success" || bad "crash: journal still there"

moved=$((s2 + 32768))
copy=$(((moved + n2 + 2047) / 2048 * 2048))
verify_gpt() {
local img=$1 tag=$2
sgdisk -v "$img" >"$W/verify.txt" 2>&1 || true
if grep -q "No problems found" "$W/verify.txt"; then pass "$tag: sgdisk -v reports no problems"
else bad "$tag: sgdisk -v"; cat "$W/verify.txt"; fi

sfdisk --json "$img" >"$W/layout.json"
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
then pass "$tag: sfdisk sees the planned geometry"; else bad "$tag: geometry differs from the plan"; fi

for start in "$moved" "$copy"; do
  dd if="$img" of="$W/check.img" bs=512 skip="$start" count="$n2" status=none
  if e2fsck -fn "$W/check.img" >"$W/fsck.txt" 2>&1; then pass "$tag: ext4 at sector $start: e2fsck clean"
  else bad "$tag: ext4 at sector $start: e2fsck"; cat "$W/fsck.txt"; fi
  rm -f "$W/out.bin"
  debugfs -R "dump /payload.bin $W/out.bin" "$W/check.img" >/dev/null 2>&1 || true
  if [[ -f "$W/out.bin" && "$(sha256sum "$W/out.bin" | cut -d' ' -f1)" == "$sha" ]]; then pass "$tag: ext4 at sector $start: payload intact"
  else bad "$tag: ext4 at sector $start: payload missing or changed"; fi
  [[ "$(blkid -p -s LABEL -o value "$W/check.img" || true)" == "wp-data" ]] && pass "$tag: ext4 at sector $start: label" || bad "$tag: ext4 at sector $start: label"
done
dd if="$img" of="$W/new.img" bs=512 skip="$s1" count=4096 status=none
if [[ -z "$(blkid -p -o value -s TYPE "$W/new.img" || true)" ]]; then pass "$tag: old FAT signature cleared in the new partition"
else bad "$tag: old signature still present"; fi
}
verify_gpt "$img" gpt
verify_gpt "$W/crash.img" "crash+resume"

# --- file-system steps in plans: create FAT32, then grow, label, serial, check ---
export MTOOLS_SKIP_CHECK=1
"$TOOL" plan "$img" fat-create "$W/fat1.json"
if "$TOOL" apply "$img" "$W/fat1.json"; then pass "fat: plan with new FAT32 partition applied"; else bad "fat: create plan"; fi
fat_start=$(sfdisk --json "$img" | python3 -c 'import json,sys; print(max(p["start"] for p in json.load(sys.stdin)["partitiontable"]["partitions"]))')
dd if="$img" of="$W/fat.img" bs=512 skip="$fat_start" count=$((40 * 2048)) status=none
fsck.fat -n "$W/fat.img" >"$W/f.txt" 2>&1 && pass "fat: fsck.fat clean after create" || { bad "fat: fsck.fat after create"; cat "$W/f.txt"; }
[[ "$(blkid -p -s LABEL -o value "$W/fat.img" || true)" == "WPFAT" ]] && pass "fat: label from the plan" || bad "fat: label"
mcopy -i "$W/fat.img" "$W/root/payload.bin" ::/payload.bin
dd if="$W/fat.img" of="$img" bs=512 seek="$fat_start" conv=notrunc status=none
"$TOOL" plan "$img" fat-grow "$W/fat2.json"
if "$TOOL" apply "$img" "$W/fat2.json"; then pass "fat: grow + label + serial + check plan applied"; else bad "fat: grow plan"; fi
dd if="$img" of="$W/fat.img" bs=512 skip="$fat_start" count=$((56 * 2048)) status=none
fsck.fat -n "$W/fat.img" >"$W/f.txt" 2>&1 && pass "fat: fsck.fat clean after grow" || { bad "fat: fsck.fat after grow"; cat "$W/f.txt"; }
[[ "$(blkid -p -s LABEL -o value "$W/fat.img" || true)" == "WPGROWN" ]] && pass "fat: new label" || bad "fat: new label"
rm -f "$W/out.bin"; mcopy -i "$W/fat.img" ::/payload.bin "$W/out.bin" 2>/dev/null || true
[[ -f "$W/out.bin" && "$(sha256sum "$W/out.bin" | cut -d' ' -f1)" == "$sha" ]] && pass "fat: payload intact after grow" || bad "fat: payload after grow"
sgdisk -v "$img" >"$W/verify.txt" 2>&1 || true
grep -q "No problems found" "$W/verify.txt" && pass "fat: sgdisk -v still clean" || bad "fat: sgdisk -v"

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

# --- msdos: move the extended partition (start 1 MiB earlier, grow to 123 MiB) ---
l5=$(sfdisk --json "$mbr" | python3 -c 'import json,sys; print(sorted(p["start"] for p in json.load(sys.stdin)["partitiontable"]["partitions"])[2])')
head -c 8M /dev/urandom | dd of="$mbr" bs=512 seek="$((l5 + 2048))" conv=notrunc status=none
lsha=$(dd if="$mbr" bs=512 skip="$((l5 + 2048))" count=16384 status=none | sha256sum | cut -d' ' -f1)
"$TOOL" plan "$mbr" ext-move "$W/ext.json"
if "$TOOL" apply "$mbr" "$W/ext.json"; then pass "mbr: extended partition moved"; else bad "mbr: extended move failed"; fi
if sfdisk -V "$mbr" >"$W/v.txt" 2>&1; then pass "mbr: sfdisk -V clean after the move"; else bad "mbr: sfdisk -V after the move"; cat "$W/v.txt"; fi
sfdisk --json "$mbr" >"$W/mbr-moved.json"
if python3 - "$W/mbr-moved.json" "$l5" <<'PY'
import json, sys
t = json.load(open(sys.argv[1]))["partitiontable"]
ext = [p for p in t["partitions"] if p["type"] in ("5", "f", "85")]
logical = sorted(p["start"] for p in t["partitions"] if int(p["node"].rsplit("img", 1)[1]) >= 5)
print(ext, logical)
sys.exit(0 if len(ext) == 1 and ext[0]["start"] == 33 * 2048 and ext[0]["start"] + ext[0]["size"] == 123 * 2048
         and logical[0] == int(sys.argv[2]) and len(logical) == 2 else 1)
PY
then pass "mbr: extended partition at 33-123 MiB, logical partitions unchanged"; else bad "mbr: layout after the extended move"; fi
[[ "$(dd if="$mbr" bs=512 skip="$((l5 + 2048))" count=16384 status=none | sha256sum | cut -d' ' -f1)" == "$lsha" ]] &&
  pass "mbr: data in the logical partition unchanged" || bad "mbr: logical partition data changed"
[[ "$(dd if="$mbr" bs=1 skip=$((34 * 1048576 + 510)) count=2 status=none | od -An -tx1 | tr -d ' ')" == "0000" ]] &&
  pass "mbr: old first EBR cleared" || bad "mbr: old first EBR still present"

echo "image apply check: $([[ $fail == 0 ]] && echo passed || echo FAILED)"
exit "$fail"
