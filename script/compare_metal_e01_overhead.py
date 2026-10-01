#!/usr/bin/env python3
"""Compare repeated E01 telemetry-on/off throughput on one binary and fixture."""

from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import statistics
import sys


def read_run(path: pathlib.Path, *, telemetry_on: bool) -> dict:
    def document(name: str) -> dict:
        data = json.loads((path / name).read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            raise ValueError(f"{path / name} is not an object")
        return data

    provenance = document("provenance.json")
    telemetry = document("telemetry.json")
    window = document("timing-window.json")
    if (path / "engine_exit_status.txt").read_text(encoding="utf-8").strip() != "0":
        raise ValueError(f"{path}: nonzero engine exit")
    if provenance.get("backend") != "metal" or telemetry.get("loading_screen_active") is not False:
        raise ValueError(f"{path}: invalid backend or loading state")
    if telemetry_on != (path / "frames.csv").is_file():
        raise ValueError(f"{path}: telemetry mode does not match frame CSV")
    drawable_lines = [line for line in (path / "engine.log").read_text(encoding="utf-8", errors="replace").splitlines()
                      if "METAL_DRAWABLE_SIZE" in line]
    if len(drawable_lines) != 1:
        raise ValueError(f"{path}: expected one actual drawable-size log")
    drawable_match = re.search(r"logical=(\d+x\d+) backing=(\d+x\d+) render=(\d+x\d+) policy=([^\s]+)", drawable_lines[0])
    if drawable_match is None:
        raise ValueError(f"{path}: malformed drawable-size log")
    if window.get("warmup_seconds") != 60 or window.get("sample_seconds") != 30:
        raise ValueError(f"{path}: expected 60 s warm-up and 30 s sample")
    start, end = window.get("start_engine_frame"), window.get("end_engine_frame")
    if isinstance(start, bool) or isinstance(end, bool) or not isinstance(start, int) or not isinstance(end, int) or end <= start:
        raise ValueError(f"{path}: invalid frame window")
    rate = (end - start) / 30.0
    if not math.isfinite(rate) or rate <= 0:
        raise ValueError(f"{path}: invalid throughput")
    return {
        "run_id": path.name,
        "fps": rate,
        "frames": end - start,
        "binary_sha256": (provenance.get("binary") or {}).get("sha256"),
        "fixture": provenance.get("fixture"),
        "profile_manifest_sha256": (provenance.get("profile") or {}).get("manifest_sha256"),
        "profile_user_ltx_sha256": (provenance.get("profile") or {}).get("user_ltx_sha256"),
        "save_sha256": telemetry.get("save_sha256"),
        "material_set_sha256": telemetry.get("material_set_sha256"),
        "level_name": telemetry.get("level_name"),
        "drawable": list(drawable_match.groups()),
        "engine_extent": [telemetry.get("engine_width"), telemetry.get("engine_height")],
        "engine_fov": telemetry.get("engine_fov"),
        "engine_aspect": telemetry.get("engine_aspect"),
    }


def compare(on_paths: list[pathlib.Path], off_paths: list[pathlib.Path]) -> dict:
    if len(on_paths) != 3 or len(off_paths) != 3:
        raise ValueError("exactly three telemetry-on and three telemetry-off runs are required")
    if len(set(path.resolve() for path in on_paths + off_paths)) != 6:
        raise ValueError("all six run directories must be distinct")
    on = [read_run(path, telemetry_on=True) for path in on_paths]
    off = [read_run(path, telemetry_on=False) for path in off_paths]
    fields = ("binary_sha256", "fixture", "profile_manifest_sha256", "profile_user_ltx_sha256",
              "save_sha256", "material_set_sha256", "level_name", "drawable", "engine_extent", "engine_fov", "engine_aspect")
    for field in fields:
        values = [json.dumps(run[field], sort_keys=True) for run in on + off]
        if not values[0] or values[0] == "null" or len(set(values)) != 1:
            raise ValueError(f"run provenance or fixture mismatch: {field}")
    on_rates = [run["fps"] for run in on]
    off_rates = [run["fps"] for run in off]
    on_median, off_median = statistics.median(on_rates), statistics.median(off_rates)
    slowdown = 100.0 * (off_median - on_median) / off_median
    # A point estimate near 2% cannot pass when run-to-run spread is wider.
    on_span = 100.0 * (max(on_rates) - min(on_rates)) / on_median
    off_span = 100.0 * (max(off_rates) - min(off_rates)) / off_median
    gate = "PASS" if 0 <= slowdown <= 2 and max(on_span, off_span) <= 2 else "OPEN_NOISY_OR_INTRUSIVE"
    return {
        "schema": "direct-metal-e01-overhead/v1",
        "fixture": on[0]["fixture"],
        "binary_sha256": on[0]["binary_sha256"],
        "method": "engine frame delta / 30 s after 60 s warm-up; no display pacing claim",
        "telemetry_on": on,
        "telemetry_off": off,
        "median_on_fps": on_median,
        "median_off_fps": off_median,
        "median_slowdown_percent": slowdown,
        "on_range_fps": [min(on_rates), max(on_rates)],
        "off_range_fps": [min(off_rates), max(off_rates)],
        "on_relative_span_percent": on_span,
        "off_relative_span_percent": off_span,
        "overhead_gate": gate,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--on", type=pathlib.Path, action="append", required=True)
    parser.add_argument("--off", type=pathlib.Path, action="append", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    try:
        result = compare(args.on, args.off)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"E01 overhead comparison error: {error}", file=sys.stderr)
        return 2
    print(f"E01 overhead: {result['overhead_gate']} ({result['median_slowdown_percent']:.2f}% point estimate)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
