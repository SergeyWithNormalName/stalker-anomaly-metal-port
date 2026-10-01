#!/usr/bin/env python3
"""Build one provenance-bound C/D manifest from isolated E00 fixture directories."""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
from typing import Any

parser = argparse.ArgumentParser()
parser.add_argument("--ref1", required=True, type=pathlib.Path)
parser.add_argument("--ref2", required=True, type=pathlib.Path)
parser.add_argument("--ref3", required=True, type=pathlib.Path)
parser.add_argument("--metal", required=True, type=pathlib.Path)
parser.add_argument("--output", required=True, type=pathlib.Path)
parser.add_argument("--expected-level", required=True)
parser.add_argument("--expected-fixture", required=True)
args = parser.parse_args()

ROOT = pathlib.Path(__file__).resolve().parents[1]
SHA256 = re.compile(r"^[0-9a-f]{64}$")
REFERENCE_BACKEND = "DXVK Native / MoltenVK (proxy)"
METAL_BACKEND = "Direct Metal"
RUNS = (("ref_run1", args.ref1, "dxvk"), ("ref_run2", args.ref2, "dxvk"), ("ref_run3", args.ref3, "dxvk"), ("metal_run", args.metal, "metal"))


def fail(message: str) -> None:
    raise SystemExit(message)


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as error:
        fail(f"{path}: unreadable JSON: {error}")
    if not isinstance(value, dict):
        fail(f"{path}: expected JSON object")
    return value


def require_file(source: pathlib.Path, name: str) -> pathlib.Path:
    path = source / name
    if not path.is_file() or path.stat().st_size == 0:
        fail(f"{source}: missing or empty {name}")
    return path


def require_sha(value: Any, label: str) -> None:
    if not isinstance(value, str) or not SHA256.fullmatch(value):
        fail(f"{label}: expected lowercase SHA-256")


def validate(name: str, source: pathlib.Path, kind: str):
    files = {artifact: require_file(source, artifact) for artifact in ("telemetry.json", "scene_no_hud.png", "scene_with_hud.png", "motion.mov", "provenance.json")}
    telemetry, provenance = load_json(files["telemetry.json"]), load_json(files["provenance.json"])
    if provenance.get("schema") != "direct-metal-release-e00/v1":
        fail(f"{source}: unsupported provenance schema")
    if provenance.get("backend") != kind or provenance.get("fixture") != args.expected_fixture:
        fail(f"{source}: provenance backend/fixture does not match requested C/D fixture")
    expected_backend, expected_metal = (REFERENCE_BACKEND, False) if kind == "dxvk" else (METAL_BACKEND, True)
    if telemetry.get("backend") != expected_backend or telemetry.get("is_direct_metal") is not expected_metal:
        fail(f"{source}: telemetry backend identity does not match {kind}")
    if telemetry.get("fixture") != args.expected_fixture or telemetry.get("level_name") != args.expected_level:
        fail(f"{source}: telemetry fixture/level does not match requested fixture")
    binary, profile = provenance.get("binary"), provenance.get("profile")
    if not isinstance(binary, dict) or not isinstance(profile, dict):
        fail(f"{source}: missing binary/profile provenance")
    for value, label in ((binary.get("sha256"), "binary.sha256"), (provenance.get("dirty_patch_sha256"), "dirty_patch_sha256"), (provenance.get("untracked_list_sha256"), "untracked_list_sha256"), (profile.get("manifest_sha256"), "profile.manifest_sha256"), (profile.get("user_ltx_sha256"), "profile.user_ltx_sha256")):
        require_sha(value, f"{source}: {label}")
    if not isinstance(binary.get("mtime_ns"), int) or binary["mtime_ns"] <= 0 or not isinstance(binary.get("size"), int) or binary["size"] <= 0:
        fail(f"{source}: invalid binary mtime/size")
    if not isinstance(provenance.get("git_head"), str) or not provenance["git_head"]:
        fail(f"{source}: missing git_head")
    if not isinstance(profile.get("manifest"), str) or not profile["manifest"]:
        fail(f"{source}: missing profile resolver identity")
    if kind == "metal":
        files["shader_admission.json"] = require_file(source, "shader_admission.json")
        admission = load_json(files["shader_admission.json"])
        passes = admission.get("passes")
        if admission.get("schema") != "direct-metal-release-admission/v1" or not isinstance(passes, dict) or not passes:
            fail(f"{source}: missing usable Metal shader admission")
        for marker, entry in passes.items():
            if not isinstance(marker, str) or not isinstance(entry, dict) or not isinstance(entry.get("state"), str) or not isinstance(entry.get("line"), str) or not entry["line"]:
                fail(f"{source}: malformed Metal shader admission entry")
    return provenance, files


validated = [(name, source, validate(name, source, kind)) for name, source, kind in RUNS]
identity_paths = (("git_head",), ("dirty_patch_sha256",), ("untracked_list_sha256",), ("e00_inputs",), ("binary", "sha256"), ("binary", "mtime_ns"), ("binary", "size"), ("profile", "manifest"), ("profile", "manifest_sha256"), ("profile", "user_ltx_sha256"))


def nested(value, path):
    for key in path:
        value = value[key]
    return value


baseline = validated[0][2][0]
for _, source, (provenance, _) in validated[1:]:
    for path in identity_paths:
        if nested(provenance, path) != nested(baseline, path):
            fail(f"{source}: provenance identity differs at {'.'.join(path)}")

if args.output.exists():
    fail(f"output already exists: {args.output}")
args.output.mkdir(parents=True)
for name, _, (_, files) in validated:
    for artifact, source_path in files.items():
        suffix = "_no_hud.png" if artifact == "scene_no_hud.png" else "_with_hud.png" if artifact == "scene_with_hud.png" else f"_{artifact}"
        shutil.copy2(source_path, args.output / f"{name}{suffix}")

env = os.environ | {"XRAY_PARITY_EXPECTED_LEVEL": args.expected_level, "XRAY_PARITY_EXPECTED_FIXTURE": args.expected_fixture}
subprocess.run(["python3", str(ROOT / "scratch/compute_parity_metrics.py"), str(args.output)], check=True, env=env)
