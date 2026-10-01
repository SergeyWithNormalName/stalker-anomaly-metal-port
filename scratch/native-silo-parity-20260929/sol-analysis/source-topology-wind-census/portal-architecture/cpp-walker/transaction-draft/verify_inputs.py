#!/usr/bin/env python3
"""Check that the proposed C++ replay manifest is tied to the observed frame-700 inputs."""
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(sys.argv[1]).resolve() if len(sys.argv) == 2 else Path.cwd()
BASE = Path("scratch/native-silo-parity-20260929")
CAPTURE = BASE / "runs/portal-active-inputs-01/native/output/source-topology.json"
ACTIVE_RESULT = BASE / "sol-analysis/source-topology-wind-census/portal-architecture/active-input-observer-v2/runtime-result.json"
LEVEL = BASE / "runs/portal-active-inputs-01/native/output/active-engine-level.bin"
GEOM = BASE / "runs/portal-active-inputs-01/native/runtime/gamedata/levels/l01_escape/level.geom"
IDS = BASE / "runs/ssa-main-ids-observe-01/native/output/main-flora-ids/main-flora-ids-frame-700.csv"
FLAGS = BASE / "sol-analysis/source-topology-wind-census/portal-architecture/cpu-prototype/active-frame700-per-id.csv"
MANIFEST = BASE / "sol-analysis/source-topology-wind-census/portal-architecture/cpp-walker/transaction-draft/frame700-inputs.json"

EXPECTED = {
    CAPTURE: "5aa97c643198555a6e8f22b84ca2d1e624ba90e266fa9e472734d75228c17f1e",
    ACTIVE_RESULT: "426197a8551c75e0f2e17fb968bfe23610eccdf16700f69b56f435404c894c90",
    LEVEL: "1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7",
    GEOM: "a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a",
    IDS: "dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc",
    FLAGS: "0c04630d336ef0f74f75b29afb75dd30d863bfdba44bcfedcd221cc46d520d16",
}

def require(condition, message):
    if not condition:
        raise SystemExit(message)

for path, expected in EXPECTED.items():
    actual = hashlib.sha256((ROOT / path).read_bytes()).hexdigest()
    require(actual == expected, f"input hash mismatch: {path}: {actual}")

capture = json.loads((ROOT / CAPTURE).read_text())
active_result = json.loads((ROOT / ACTIVE_RESULT).read_text())
manifest = json.loads((ROOT / MANIFEST).read_text())
require(capture["schema"] == "r4-source-topology-count-v1" and capture["engine_frame"] == 700,
        "capture is not the observed frame-700 source record")
require(capture["copied_level_generation"] == 1 and capture["geometry_verified"] is False and capture["suppression"] == 0,
        "capture generation/scope gate failed")
require(active_result["active_level_sha256"] == EXPECTED[LEVEL] and
        manifest["active_reader_copy_sha256"] == EXPECTED[LEVEL] and
        active_result["active_level_copy_generation"] == capture["copied_level_generation"],
        "active reader bytes are not the pinned generation-1 copy")
require(manifest["capture_sha256"] == EXPECTED[CAPTURE] and manifest["matching_level_geom"]["sha256"] == EXPECTED[GEOM],
        "replay manifest hash receipts drifted")
require(capture["source_sector_count"] == 0 and capture["source_visual_count"] == 0,
        "this transaction expects the current absent legacy graph/visual inputs")
require(capture["traversal_options"] == 7 and capture["portal_draw_flag"] == 0,
        "observed option/portal flag drift")
require(capture["viewport_near_bits"] == [1045220557] and
        capture["eye_bits"] == capture["saved_camera_eye_bits"], "saved eye or near-plane drift")
require(all(ray["hit"] and ray["sector"] == 25 for ray in capture["cform_vertical_rays"]),
        "live CFORM start sector is no longer 25")
require(manifest["live_cform_sector_down"] == 25 and manifest["live_cform_sector_up"] == 25 and
        manifest["start_sector_for_this_replay"] == 25, "replay start sector is not the measured CFORM sector")
for key in ("view_bits", "projection_bits", "full_transform_bits", "r_ssaDISCARD", "r_ssaLOD_A", "r_ssaLOD_B"):
    require(capture[key] == manifest[{"view_bits":"view_bits_u32", "projection_bits":"projection_bits_u32",
        "full_transform_bits":"full_transform_bits_u32", "r_ssaDISCARD":"r_ssaDISCARD",
        "r_ssaLOD_A":"r_ssaLOD_A", "r_ssaLOD_B":"r_ssaLOD_B"}[key]], f"captured field mismatch: {key}")
require(manifest["observed_graph_options"] == 7 and manifest["comparison_graph_options_hom_off"] == 6,
        "HOM-off superset options are not derived from the observed options")
require(sum(1 for line in (ROOT / IDS).open() if line[:1].isdigit()) == 5316, "frame-700 Flora ID count mismatch")
require(sum(1 for line in (ROOT / FLAGS).open() if line[:1].isdigit()) == 5316, "prior raw-flag count mismatch")
print("inputs=pass frame=700 level_generation=1 start_sector=25 options=7->6 ids=5316 suppression=0")
