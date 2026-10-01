#!/usr/bin/env python3
"""Preserve per-pass runtime admission facts without treating them as parity."""
import argparse, json, pathlib, re

parser = argparse.ArgumentParser()
parser.add_argument("log", type=pathlib.Path)
parser.add_argument("output", type=pathlib.Path)
args = parser.parse_args()
entries = {}
pattern = re.compile(r"\b(METAL_[A-Z0-9_]+)\b")
for line in args.log.read_text(errors="replace").splitlines():
    match = pattern.search(line)
    if not match:
        continue
    marker = match.group(1)
    upper = line.upper()
    if any(word in upper for word in ("REJECT", "SKIP", "FALLBACK", "ERROR", "FAIL")):
        state = "rejected_or_fallback"
    elif "DISABLED" in upper:
        state = "disabled"
    elif any(word in upper for word in ("PREPARED", "READY", "DRAW", "LOAD_OK")):
        state = "active"
    else:
        continue
    entries[marker] = {"state": state, "line": line}
args.output.write_text(json.dumps({"schema": "direct-metal-release-admission/v1", "passes": entries}, indent=2, sort_keys=True) + "\n")
