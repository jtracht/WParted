#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Authenticode-signs the executables inside the portable ZIPs on Linux with
# osslsigncode when PFX_B64/PFX_PASS are set. Without a certificate it writes
# dist/UNSIGNED.txt and exits 0 - it never pretends to sign.
#   tools/sign-release.sh dist
set -euo pipefail
DIST="$(cd "${1:?usage: sign-release.sh <dist-dir>}" && pwd)"
if [ -z "${PFX_B64:-}" ]; then
  echo "This release is NOT code-signed (no signing certificate configured). Verify the SHA-256 checksums." > "$DIST/UNSIGNED.txt"
  echo "warning: no signing certificate configured - release is unsigned" >&2
  exit 0
fi
command -v osslsigncode >/dev/null || { apt-get install -y -qq osslsigncode >/dev/null; }
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
echo "$PFX_B64" | base64 -d > "$WORK/cert.pfx"
for zip in "$DIST"/*-portable.zip; do
  rm -rf "$WORK/x" && mkdir "$WORK/x"
  (cd "$WORK/x" && unzip -q "$zip")
  for exe in "$WORK"/x/*/*.exe; do
    osslsigncode sign -pkcs12 "$WORK/cert.pfx" -pass "$PFX_PASS" -h sha256 \
      -ts http://timestamp.digicert.com -in "$exe" -out "$exe.signed"
    mv "$exe.signed" "$exe"
    osslsigncode verify -in "$exe" >/dev/null
  done
  rm "$zip"
  (cd "$WORK/x" && zip -qr "$zip" .)
done
