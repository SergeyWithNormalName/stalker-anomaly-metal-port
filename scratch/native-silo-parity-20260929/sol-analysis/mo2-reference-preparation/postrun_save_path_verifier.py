#!/usr/bin/env python3
"""Read-only parser for exact X-Ray successful-save-load path receipts."""
from __future__ import annotations

import ntpath
import re
from dataclasses import dataclass


@dataclass(frozen=True)
class SaveLoadReceipt:
    save_name: str
    observed_path: str
    normalized_path: str
    line: str


def normalize_windows_path(value: str) -> str:
    # Wine and X-Ray logs can mix slash styles; these are path spellings,
    # not shell-escaped strings. Collapse separators, then resolve dot segments.
    value = re.sub(r"[\\/]+", r"\\", value.strip())
    return ntpath.normcase(ntpath.normpath(value))


def find_exact_save_load(text: str, save_name: str, expected_full_path: str) -> list[SaveLoadReceipt]:
    """Match basename with optional .scop, then require exact normalized path."""
    expected = normalize_windows_path(expected_full_path)
    save_pattern = re.compile(
        r"\*\s*Game\s+" + re.escape(save_name) + r"(?:\.scop)?\s+"
        r"is successfully loaded from file\s+'([^']+)'",
        re.IGNORECASE,
    )
    results: list[SaveLoadReceipt] = []
    for line in text.splitlines():
        match = save_pattern.search(line)
        if not match:
            continue
        path = match.group(1)
        normalized = normalize_windows_path(path)
        if normalized == expected:
            results.append(SaveLoadReceipt(save_name, path, normalized, line))
    return results
