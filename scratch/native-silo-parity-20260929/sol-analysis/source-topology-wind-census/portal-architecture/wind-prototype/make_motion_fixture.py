#!/usr/bin/env python3
"""Extract exact float32 render uniforms from the frozen frame-700 wind receipt."""
import csv
import argparse
import hashlib
import json
import struct
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[5]
SOURCE = REPO / "scratch/native-silo-parity-20260929/runs/source-topology-wind-census-01/native/output/flora-wind.json"
EXPECTED = "7515eff2f59b25f5357fb3a02cf5e9f3b48acecb8a9dc4dd1607fe2fc8101643"
EXPECTED_ACTIVE = "59fdd9315c49fe3c978a6dbae3dac22ed44f801f4fcf30dabff733631f56009e"


def f32(value):
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def row(name, values):
    return [name, *[format(f32(value), ".9g") for value in values]]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wind-json", type=Path, default=SOURCE)
    parser.add_argument("--output", default="frame700-motion.csv")
    args = parser.parse_args()
    if Path(args.output).name != args.output or not args.output.endswith(".csv"):
        raise SystemExit("output must be a simple CSV filename in this folder")
    raw = args.wind_json.read_bytes()
    source_sha = hashlib.sha256(raw).hexdigest()
    if source_sha not in (EXPECTED, EXPECTED_ACTIVE):
        raise SystemExit(f"unexpected live wind receipt hash {source_sha}")
    # JSON's default integer parser turns the token -0 into integer 0. Keep
    # its IEEE sign when a captured float was serialized without a decimal.
    observation = json.loads(raw, parse_int=lambda token: -0.0 if token == "-0" else int(token))
    fixture = observation["flora_view_state"]
    callback = observation["callback_view"]
    if (not observation.get("available") or not fixture.get("wind_valid") or
            callback.get("engine_frame") != 700 or callback.get("previous_frame") != 699 or
            fixture.get("environment_engine_frame") != 700 or
            fixture.get("motion_snapshot_engine_frame") != 700 or
            len(fixture["current_benders"]) != 32 or len(fixture["previous_benders"]) != 32):
        raise SystemExit("wind fixture is invalid or does not contain 32 current/previous benders")
    rows = [
        ["schema", "flora-wind-uniforms/v1"], ["frame", "700"], ["previous_frame", "699"], ["valid", "1"],
        row("wind_params", fixture["wind_params"]), row("tree_setup", fixture["wind_setup_trees"]),
        row("bender_setup", fixture["benders_setup"]),
        row("animation_current", fixture["wind_animation_current"]),
        row("animation_previous", fixture["wind_animation_previous"]),
    ]
    for label, key in (("current_bender", "current_benders"), ("previous_bender", "previous_benders")):
        rows.extend(row(f"{label}_{index}", values) for index, values in enumerate(fixture[key]))
    output = HERE / args.output
    with output.open("w", newline="") as stream:
        csv.writer(stream, lineterminator="\n").writerows(rows)
    receipt = {
        "schema": "flora-wind-uniforms-receipt/v1", "source_json_sha256": source_sha,
        "fixture_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "frame": 700, "previous_frame": 699,
        "conversion": "Captured decimals are rounded through IEEE754 float32 and serialized with 9 significant digits.",
    }
    output.with_suffix(".receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps(receipt, indent=2))


if __name__ == "__main__":
    main()
