#!/usr/bin/env python3
"""Select one authoritative clang-tidy compile command per source file."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
from typing import Any


TEST_TARGETS = {
    "clipboard_manager.cpp": "punto-clipboard-contract",
    "key_injector.cpp": "punto-key-injector-contract",
    "macro_lock.cpp": "punto-runtime-files-contract",
    "sound_manager.cpp": "punto-sound-manager-contract",
    "terminal_detection.cpp": "punto-tests",
    "undo_detector.cpp": "punto-undo-detector-contract",
}


def load_database(path: pathlib.Path) -> list[dict[str, Any]]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(value, list) or not all(isinstance(item, dict) for item in value):
        raise ValueError(f"{path} is not a compilation database array")
    return value


def target_name(entry: dict[str, Any]) -> str | None:
    output = entry.get("output", "")
    if not isinstance(output, str):
        return None
    match = re.search(r"(?:^|/)CMakeFiles/([^/]+)\.dir/", output)
    return match.group(1) if match else None


def source_path(entry: dict[str, Any]) -> pathlib.Path | None:
    source = entry.get("file")
    if not isinstance(source, str):
        return None
    path = pathlib.Path(source)
    if not path.is_absolute():
        directory = entry.get("directory")
        if not isinstance(directory, str):
            return None
        path = pathlib.Path(directory) / path
    return path.resolve()


def indexed(database: list[dict[str, Any]]) -> dict[pathlib.Path, list[dict[str, Any]]]:
    result: dict[pathlib.Path, list[dict[str, Any]]] = {}
    for entry in database:
        source = source_path(entry)
        if source is not None:
            result.setdefault(source, []).append(entry)
    return result


def select_exact(
    candidates: list[dict[str, Any]], target: str, source: pathlib.Path
) -> dict[str, Any]:
    selected = [entry for entry in candidates if target_name(entry) == target]
    if len(selected) != 1:
        observed = sorted(
            name for entry in candidates if (name := target_name(entry)) is not None
        )
        raise ValueError(
            f"{source}: expected one {target} command, found {len(selected)}; "
            f"targets={observed}"
        )
    return selected[0]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=pathlib.Path, required=True)
    parser.add_argument("--product-db", type=pathlib.Path, required=True)
    parser.add_argument("--contract-db", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    all_sources = sorted(path.resolve() for path in source_root.rglob("*.cpp"))
    if not all_sources:
        raise ValueError(f"no C++ sources found in {source_root}")

    product = indexed(load_database(args.product_db.resolve()))
    contract = indexed(load_database(args.contract_db.resolve()))
    selected: list[dict[str, Any]] = []
    selections: list[str] = []
    for source in all_sources:
        product_candidates = product.get(source, [])
        product_targets = {target_name(entry) for entry in product_candidates}
        if "punto" in product_targets:
            target = "punto"
            entry = select_exact(product_candidates, target, source)
        elif "punto-tray" in product_targets:
            target = "punto-tray"
            entry = select_exact(product_candidates, target, source)
        else:
            target = TEST_TARGETS.get(source.name, "")
            if not target:
                raise ValueError(
                    f"{source}: absent from product targets and has no explicit test owner"
                )
            entry = select_exact(contract.get(source, []), target, source)
        selected.append(entry)
        selections.append(f"{source.name}={target}")

    selected_paths = [source_path(entry) for entry in selected]
    if len(selected_paths) != len(set(selected_paths)):
        raise ValueError(
            "selected compilation database contains duplicate source paths"
        )
    if set(selected_paths) != set(all_sources):
        raise ValueError("selected compilation database does not cover cpp/src exactly")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(selected, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    print(
        f"selected {len(selected)} unique clang-tidy commands: " + ", ".join(selections)
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        print(f"clang-tidy database selection failed: {error}", file=sys.stderr)
        raise SystemExit(1)
