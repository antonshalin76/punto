#!/usr/bin/env python3
"""Verify the GTK-only suppression still detects a project-owned allocation."""

import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: test_tray_lsan_negative_control.py TRAY_CONTRACT", file=sys.stderr)
        return 2
    result = subprocess.run(
        [sys.argv[1], "--lsan-negative-control"],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
        timeout=10, check=False,
    )
    expected = "SUMMARY: AddressSanitizer: 73 byte(s) leaked in 1 allocation(s)."
    if (result.returncode != 1 or expected not in result.stderr or
            "inject_owned_leak_for_sanitizer_negative_control" not in result.stderr):
        print(f"FAIL: expected owned-leak detection, exit={result.returncode}", file=sys.stderr)
        print(result.stderr[-3000:], file=sys.stderr)
        return 1
    print("PASS: GTK suppression retains detection of the project-owned 73-byte leak")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
