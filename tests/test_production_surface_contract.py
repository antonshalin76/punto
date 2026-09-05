#!/usr/bin/env python3
"""Check the restored executor link boundary and exclude the obsolete injector."""

from __future__ import annotations

import pathlib
import re
import subprocess
import sys


FORBIDDEN_SOURCES = {
    "src/key_injector.cpp",
}

FORBIDDEN_SYMBOLS = (
    "punto::KeyInjector",
)


def fail(message: str) -> int:
    print(f"production surface contract failed: {message}", file=sys.stderr)
    return 1


def run_tool(*command: str, timeout: float = 5.0) -> str:
    result = subprocess.run(
        command,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=timeout,
    )
    if result.returncode != 0:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(
            f"{' '.join(command)} returned {result.returncode}: {detail}"
        )
    return result.stdout


def main() -> int:
    if len(sys.argv) != 3:
        print(
            "usage: test_production_surface_contract.py CMAKE BINARY",
            file=sys.stderr,
        )
        return 2

    cmake_path = pathlib.Path(sys.argv[1]).resolve()
    binary_path = pathlib.Path(sys.argv[2]).resolve()
    if not cmake_path.is_file() or not binary_path.is_file():
        return fail("CMake source or production binary is missing")

    cmake = cmake_path.read_text(encoding="utf-8")
    source_match = re.search(r"set\(PUNTO_SOURCES\s+(.*?)\n\)", cmake, flags=re.DOTALL)
    if source_match is None:
        return fail("PUNTO_SOURCES cannot be parsed")
    production_sources = set(
        re.findall(r"src/[A-Za-z0-9_./-]+\.cpp", source_match.group(1))
    )
    if not {"src/main.cpp", "src/event_loop.cpp"} <= production_sources:
        return fail("PUNTO_SOURCES parse did not find the production entry points")
    forbidden = sorted(production_sources & FORBIDDEN_SOURCES)
    if forbidden:
        return fail("forbidden source(s): " + ", ".join(forbidden))
    required = {
        "src/word_editor.cpp", "src/clipboard_manager.cpp", "src/macro_lock.cpp",
        "src/sound_manager.cpp",
        "src/undo_detector.cpp",
    }
    if not required <= production_sources:
        return fail("word editor implementation is missing from production")

    production_prefix = cmake.split("# Tests (CTest)", maxsplit=1)[0]
    if "PUNTO_ENABLE_TEST_SEAMS" in production_prefix:
        return fail("test-only exception seam is enabled before the test section")

    try:
        dynamic = run_tool("readelf", "-dW", str(binary_path)).lower()
        symbols = run_tool("nm", "-C", str(binary_path), timeout=15.0)
        strings = run_tool("strings", "-a", str(binary_path))
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        return fail(str(error))

    if "xcb-xtest" not in dynamic or "xcb-xfixes" not in dynamic:
        return fail("production ELF is missing the bounded word-editor transport")
    if "punto::WordEditor::execute(" not in symbols:
        return fail("production ELF does not contain word-edit execution")
    linked_symbols = [symbol for symbol in FORBIDDEN_SYMBOLS if symbol in symbols]
    if linked_symbols:
        return fail("forbidden linked symbol(s): " + ", ".join(linked_symbols))
    if "PUNTO_TEST_MAIN_EXCEPTION" in strings:
        return fail("production ELF contains the test-only exception seam")

    print("test_production_surface_contract: OK")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
