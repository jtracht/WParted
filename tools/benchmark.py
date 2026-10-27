#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 The WParted contributors
"""Performance baseline for WParted (WP-018).

Measures wall-clock times of read-only CLI commands, the rescue scan over a
large synthetic image, GUI start-up time/memory and binary sizes. All images
are synthetic files created in a temporary directory; no device is opened
for writing. Results are printed as a Markdown table (and JSON with --json).

Usage: python tools/benchmark.py --build build/x64-release [--runs 5] [--image-gib 16]
"""
import argparse
import json
import os
import platform
import statistics
import struct
import subprocess
import sys
import tempfile
import time

MiB = 1024 * 1024


def lost_fat32_image(path, size_bytes):
    """Empty MBR plus a FAT32 boot sector at 4 MiB that no partition covers.
    The file is extended with truncate(), so it costs almost no disk space."""
    head = bytearray(8 * MiB)
    head[510:512] = b"\x55\xaa"
    b = 8192 * 512
    head[b:b + 3] = b"\xeb\x58\x90"
    head[b + 3:b + 11] = b"MSDOS5.0"
    struct.pack_into("<HBHB", head, b + 11, 512, 8, 32, 2)
    struct.pack_into("<I", head, b + 32, 100000)
    struct.pack_into("<I", head, b + 36, 100)
    head[b + 66] = 0x29
    head[b + 71:b + 82] = b"BENCH      "
    head[b + 82:b + 90] = b"FAT32   "
    head[b + 510:b + 512] = b"\x55\xaa"
    with open(path, "wb") as f:
        f.write(head)
        f.truncate(size_bytes)


def timed(cmd, runs):
    samples = []
    for _ in range(runs):
        t = time.perf_counter()
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        samples.append((time.perf_counter() - t) * 1000)
        if r.returncode not in (0, 1):  # inspect-image returns 1 for diagnostics with errors
            sys.exit("command failed (%d): %s\n%s" % (r.returncode, " ".join(cmd), r.stderr.decode(errors="replace")))
    return statistics.median(samples), r.stdout.decode(errors="replace")


def gui_startup(gui, image):
    """Time until the main window exists and private memory after 3 s (Windows only)."""
    if os.name != "nt":
        return None, None
    import ctypes
    from ctypes import wintypes
    user32 = ctypes.windll.user32
    kernel32 = ctypes.windll.kernel32
    p = subprocess.Popen([gui, image])
    t0 = time.perf_counter()
    found = None
    proc = wintypes.DWORD()
    while time.perf_counter() - t0 < 15 and found is None:
        h = user32.FindWindowW("WPartedMainWindow", None)
        while h:
            user32.GetWindowThreadProcessId(h, ctypes.byref(proc))
            if proc.value == p.pid:
                found = (time.perf_counter() - t0) * 1000
                break
            h = user32.FindWindowExW(None, h, "WPartedMainWindow", None)
        time.sleep(0.01)
    time.sleep(3)

    class PMC(ctypes.Structure):
        _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD)] + [
            (n, ctypes.c_size_t) for n in ("PeakWorkingSetSize", "WorkingSetSize", "QuotaPeakPagedPoolUsage",
                                           "QuotaPagedPoolUsage", "QuotaPeakNonPagedPoolUsage",
                                           "QuotaNonPagedPoolUsage", "PagefileUsage", "PeakPagefileUsage",
                                           "PrivateUsage")]
    pmc = PMC()
    pmc.cb = ctypes.sizeof(PMC)
    hp = kernel32.OpenProcess(0x1000 | 0x0010, False, p.pid)  # QUERY_LIMITED_INFORMATION | VM_READ
    ok = ctypes.windll.psapi.GetProcessMemoryInfo(hp, ctypes.byref(pmc), pmc.cb)
    kernel32.CloseHandle(hp)
    if found:
        user32.PostMessageW(user32.FindWindowW("WPartedMainWindow", None), 0x0010, 0, 0)  # WM_CLOSE
    try:
        p.wait(10)
    except subprocess.TimeoutExpired:
        p.kill()
    return found, (pmc.PrivateUsage / MiB if ok else None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--build", default="build/x64-release")
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--image-gib", type=int, default=16)
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    ext = ".exe" if os.name == "nt" else ""
    cli = os.path.join(a.build, "apps", "wparted-cli", "wparted-cli" + ext)
    gui = os.path.join(a.build, "apps", "wparted-gui", "wparted" + ext)
    mk = os.path.join(a.build, "tests", "wparted_make_test_image" + ext)
    res = {"system": platform.platform(), "runs": a.runs, "results": []}

    def add(metric, method, value, unit):
        res["results"].append({"metric": metric, "method": method, "value": round(value, 2), "unit": unit})

    with tempfile.TemporaryDirectory() as tmp:
        gpt = os.path.join(tmp, "gpt.img")
        subprocess.run([mk, gpt], check=True)
        big = os.path.join(tmp, "big.img")
        lost_fat32_image(big, a.image_gib * 1024 * MiB)

        ms, out = timed([cli, "list", "--json"], a.runs)
        elevated = '"elevated":true' in out.replace(" ", "")
        add("Disk discovery (%s)" % ("elevated" if elevated else "unelevated, volumes only"),
            "wparted-cli list --json, median", ms, "ms")
        ms, _ = timed([cli, "inspect-image", gpt, "--json"], a.runs)
        add("Analyse 64 MiB GPT image (4 partitions)", "wparted-cli inspect-image, median", ms, "ms")
        ms, out = timed([cli, "rescue-scan-image", big, "--json"], a.runs)
        if '"BENCH"' not in out:
            sys.exit("rescue scan did not find the benchmark file system: " + out)
        add("Rescue scan, %d GiB unallocated" % a.image_gib, "wparted-cli rescue-scan-image, median", ms, "ms")
        # Not a read throughput: the scan reads a few probe sectors per MiB step, and the
        # image is a sparse file served from the cache. Real disks add seek latency.
        add("Rescue scan rate (address space, sparse file)", "image size / median time",
            a.image_gib * 1024 / (ms / 1000), "MiB/s")
        if os.path.exists(gui):
            start, mem = gui_startup(gui, gpt)
            if start is not None:
                add("GUI start until main window exists", "FindWindow polling, 1 run", start, "ms")
            if mem is not None:
                add("GUI private memory 3 s after start", "GetProcessMemoryInfo PrivateUsage", mem, "MiB")
    for name, path in (("wparted" + ext, gui), ("wparted-cli" + ext, cli)):
        if os.path.exists(path):
            add("Executable size " + name, "file size", os.path.getsize(path) / 1024, "KiB")

    if a.json:
        print(json.dumps(res, indent=2))
        return
    print("System: %s; median of %d runs where stated\n" % (res["system"], a.runs))
    print("| Metric | Method | Result |\n|---|---|---|")
    for r in res["results"]:
        print("| %s | %s | %s %s |" % (r["metric"], r["method"], r["value"], r["unit"]))


if __name__ == "__main__":
    main()
