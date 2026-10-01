#!/usr/bin/env python3
"""Draft-only post-collector for a completed, root-approved MO2 run04."""
from __future__ import annotations

import argparse
import csv
import json
import math
import re
import subprocess
from pathlib import Path
from typing import Any

EXPECTED_CALENDAR = "2018-10-26T08:09:47"
EXPECTED_LEVEL = "l01_escape"
EXPECTED_WEATHER = "w_clear2"
EXPECTED_CAMERA = (-221.082565, -18.284866, -168.182037)
EXPECTED_DIRECTION = (0.0, 0.0, 1.0)


def phase_markers(path: Path) -> dict[str, dict[str, str]]:
    result: dict[str, dict[str, str]] = {}
    for row in path.read_text(errors="strict").splitlines():
        parts = row.split(",")
        if not parts:
            continue
        values: dict[str, str] = {}
        for item in parts[1:]:
            if "=" in item:
                key, value = item.split("=", 1)
                values[key] = value
        result[parts[0]] = values
    return result


def read_measure_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", errors="strict") as stream:
        return [row for row in csv.DictReader(stream) if row.get("phase") == "1"]


def finite_float(row: dict[str, str], key: str) -> float:
    value = float(row[key])
    if not math.isfinite(value):
        raise ValueError(f"nonfinite {key}")
    return value


def validate_measured_rows(rows: list[dict[str, str]]) -> list[str]:
    errors: list[str] = []
    if len(rows) < 100:
        errors.append(f"only {len(rows)} measured rows; need at least 100")
    for index, row in enumerate(rows):
        prefix = f"row {index}"
        if row.get("level") != EXPECTED_LEVEL: errors.append(f"{prefix}: wrong level")
        if row.get("calendar") != EXPECTED_CALENDAR: errors.append(f"{prefix}: calendar drift")
        if row.get("weather") != EXPECTED_WEATHER: errors.append(f"{prefix}: wrong weather")
        try:
            if abs(finite_float(row, "device_time_factor") - 1.0) > 1e-6: errors.append(f"{prefix}: Device factor")
            if abs(finite_float(row, "game_time_factor") - 1e-6) > 1e-8: errors.append(f"{prefix}: game factor")
            if int(row["width"]) != 1920 or int(row["height"]) != 1200: errors.append(f"{prefix}: drawable")
            for axis, expected in zip(("camera_x", "camera_y", "camera_z"), EXPECTED_CAMERA):
                if abs(finite_float(row, axis) - expected) > 2e-5: errors.append(f"{prefix}: {axis}")
            for axis, expected in zip(("dir_x", "dir_y", "dir_z"), EXPECTED_DIRECTION):
                if abs(finite_float(row, axis) - expected) > 1e-3: errors.append(f"{prefix}: {axis}")
        except (KeyError, ValueError, OverflowError) as exc:
            errors.append(f"{prefix}: malformed/nonfinite value: {exc}")
    return errors


def unique_game_host_pid(result: dict[str, Any]) -> int:
    hosts = result.get("new_game_host_processes")
    if not isinstance(hosts, dict) or len(hosts) != 1:
        raise ValueError("expected exactly one newly observed AnomalyDX11 host process")
    pid_text = next(iter(hosts))
    pid = int(pid_text)
    if result.get("module_load_evidence", {}).get("game_host_pid") != pid:
        raise ValueError("host PID does not agree with module-load evidence")
    return pid


def collect(run_output: Path, do_collect: bool) -> dict[str, Any]:
    result = json.loads((run_output / "result.json").read_text(errors="strict"))
    if not result.get("result_is_acceptance"):
        raise ValueError("run result did not pass the saved-state/world-load gate")
    if result.get("clone_preflight", {}).get("environment", {}).get("METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS") != "1":
        raise ValueError("signpost environment not pinned to 1 in the run preflight")
    if not result.get("hook_execution_receipt_verified") or not result.get("loaded_save_path_verified"):
        raise ValueError("hook nonce or exact loaded-save path gate missing")
    phase = phase_markers(run_output / "phases.csv")
    if "measure_start" not in phase or "measure_end" not in phase or "weather_committed" not in phase:
        raise ValueError("condition-qualified measure markers are incomplete")
    start = int(phase["measure_start"]["epoch_s"])
    end = int(phase["measure_end"]["epoch_s"])
    if end <= start:
        raise ValueError("measurement time bounds are invalid")
    rows = read_measure_rows(run_output / "phases.csv.device.csv")
    errors = validate_measured_rows(rows)
    if errors:
        raise ValueError("measured phase conditions failed: " + "; ".join(errors[:30]))
    weather_rows = list(csv.DictReader((run_output / "phases.csv.weather.csv").open(newline="", errors="strict")))
    committed = [row for row in weather_rows if row.get("marker") == "weather_committed"]
    if not any(row.get("clouds_texture") is not None and row.get("paused", "").lower() == "true"
        and row.get("weight") is not None and abs(float(row["weight"])) <= 1e-5 for row in committed):
        raise ValueError("weather_committed marker did not show paused clear weather at zero weight")
    pid = unique_game_host_pid(result)
    report: dict[str, Any] = {
        "scope": "initial matched-frame diagnostic; no winner or quality acceptance",
        "host_game_pid": pid,
        "mo2_launcher_pid": result.get("owned_mo2_launcher_pid"),
        "guest_wine_process_id": result.get("fresh_guest_wpid", {}).get("guest_wpid"),
        "module_log_thread_id_hex": result.get("module_load_evidence", {}).get("module_log_thread_id_hex"),
        "measurement_epoch_seconds": {"start": start, "end": end, "span": end-start},
        "measured_row_count": len(rows),
        "all_measured_rows_qualified": True,
        "collection_performed": False,
    }
    if not do_collect:
        return report
    trace_dir = run_output / "metal-traces"
    if trace_dir.exists():
        raise ValueError(f"refusing existing trace output: {trace_dir}")
    trace_dir.mkdir()
    subprocess.run(["metalperftrace", "collect", "--start", f"@{start}", "--end", f"@{end}",
        "--json", str(trace_dir)], check=True, stdout=(run_output/"metal-collect.json").open("w"),
        stderr=(run_output/"metal-collect.log").open("w"))
    traces = sorted(trace_dir.glob("*.atrc"))
    if len(traces) != 1:
        raise ValueError(f"expected exactly one collected .atrc, found {len(traces)}")
    subprocess.run(["metalperftrace", "overview", "--json", "--json-include-timeline",
        "--predicate", f"pid == {pid}", str(traces[0])], check=True,
        stdout=(run_output/"metal-timeline.json").open("w"))
    report.update({"collection_performed": True, "trace_path": str(traces[0]),
        "overview_pid_predicate": f"pid == {pid}"})
    return report


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("run_output", type=Path)
    parser.add_argument("--collect", action="store_true", help="collect the bounded run04 phase after validation")
    args = parser.parse_args()
    report = collect(args.run_output, args.collect)
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
