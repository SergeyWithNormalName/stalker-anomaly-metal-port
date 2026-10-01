#!/usr/bin/env python3
"""Record lightweight identity evidence for cloned runtime config/scripts and links."""
import hashlib
import json
import os
import sys
from pathlib import Path

root, output = Path(sys.argv[1]), Path(sys.argv[2])
files = []
for rel in ("appdata/user.ltx", "fsgame.ltx"):
    p = root / rel
    if p.is_file():
        files.append({"path": rel, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()})
for rel in ("gamedata/configs", "gamedata/scripts"):
    base = root / rel
    if base.exists():
        for p in sorted(base.rglob("*")):
            if p.is_file() and not p.is_symlink():
                files.append({"path": str(p.relative_to(root)),
                              "sha256": hashlib.sha256(p.read_bytes()).hexdigest()})
links = []
for current, dirs, names in os.walk(root, followlinks=False):
    for name in dirs + names:
        p = Path(current) / name
        if p.is_symlink():
            target = os.readlink(p)
            try:
                resolved = str(p.resolve(strict=True))
            except FileNotFoundError:
                resolved = None
            links.append({"path": str(p.relative_to(root)), "target": target,
                          "resolved_target": resolved})
output.parent.mkdir(parents=True, exist_ok=True)
output.write_text(json.dumps({"runtime_root": str(root), "file_hashes": files,
                              "asset_symlinks": links}, indent=2) + "\n")
