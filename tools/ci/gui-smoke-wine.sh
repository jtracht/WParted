#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Starts wparted.exe under Wine on a virtual X server and checks the
# application log for a completed start-up and discovery. Wine is not
# Windows: this proves the binary loads, creates its window and runs the
# discovery thread, not that real disk access behaves like on Windows.
#   tools/ci/gui-smoke-wine.sh <path/to/wparted.exe>
set -euo pipefail
EXE="$(realpath "${1:?usage: gui-smoke-wine.sh <wparted.exe>}")"
# Wine's X11 driver needs these (they are only "Recommends" of the wine packages).
apt-get install -y -qq --no-install-recommends xvfb xauth libx11-6 libxext6 libxrender1 libxrandr2 \
  libxi6 libxcursor1 libxcomposite1 libxinerama1 libxfixes3 libfreetype6 libfontconfig1 fonts-dejavu-core >/dev/null
export WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml="
export WINEPREFIX="$HOME/.wine-wparted-gui"
# A wineserver started without $DISPLAY keeps a driver-less desktop; the
# prefix must be created and used inside the same X session.
wineserver -k 2>/dev/null || true
rm -rf "$WINEPREFIX"
export EXE
if timeout 90 xvfb-run -a bash -c '
  wineboot --init >/dev/null 2>&1
  wine "$EXE" >/dev/null 2>&1 &
  for _ in $(seq 1 40); do
    sleep 1
    log=$(find "$WINEPREFIX/drive_c/users" -path "*AppData/Local/WParted/logs/wparted.log" 2>/dev/null | head -1)
    if [ -n "$log" ] && grep -q "Discovery finished" "$log"; then cat "$log"; wineserver -k; exit 0; fi
  done
  [ -n "${log:-}" ] && cat "$log" >&2
  wineserver -k; exit 1'; then
  echo "GUI smoke OK"
else
  echo "GUI smoke FAILED - no 'Discovery finished' in log" >&2
  exit 1
fi
