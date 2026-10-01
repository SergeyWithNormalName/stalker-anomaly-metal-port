#!/usr/bin/env python3
"""E08 settled E01 summaries; no visible-NPC or visual-parity claim."""
from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import statistics

from analyze_metal_e01 import analyze, read_presentation, read_window


def summarize_run(directory: pathlib.Path, settle_seconds: float) -> dict:
    window = read_window(directory / "timing-window.json")
    if window is None:
        raise ValueError("missing timing window")
    with (directory / "frames.csv").open(newline="") as stream:
        rows = list(csv.DictReader(stream))
    admitted = []
    for row in rows:
        try:
            frame, begin = int(row["engine_frame"]), float(row["begin_ns"])
        except (ValueError, KeyError):
            continue
        if (window["start_engine_frame"] <= frame <= window["end_engine_frame"] and
                row.get("status") == "completed" and math.isfinite(begin) and begin > 0):
            admitted.append((frame, begin))
    if not admitted:
        raise ValueError("no completed frames in timing window")
    settle_start = min(begin for _, begin in admitted) + settle_seconds * 1e9
    selected = [frame for frame, begin in admitted if begin >= settle_start]
    if not selected:
        raise ValueError("no frames after settle")
    trimmed = {"start_engine_frame": min(selected), "end_engine_frame": max(selected)}
    presentation = read_presentation(directory / "frames.csv.presented.csv")
    timing = analyze(directory / "frames.csv", trimmed, presentation)
    displayed_mean_ms = None
    if timing["presentation"]["display_metrics_status"] == "complete":
        serials = sorted(int(row["serial"]) for row in rows
                        if row.get("status") == "completed" and
                        trimmed["start_engine_frame"] <= int(row["engine_frame"]) <= trimmed["end_engine_frame"])
        intervals = [(presentation[b] - presentation[a]) * 1000.0
                     for a, b in zip(serials, serials[1:]) if b == a + 1]
        if len(intervals) == timing["displayed_frame_interval_ms"]["count"] and intervals:
            displayed_mean_ms = statistics.mean(intervals)
    provenance = json.loads((directory / "provenance.json").read_text())
    telemetry = json.loads((directory / "telemetry.json").read_text())
    exit_code = int((directory / "engine_exit_status.txt").read_text().strip())
    return {
        "schema": "direct-metal-e08-run/v1", "run": directory.name,
        "engine_exit_status": exit_code, "settle_seconds": settle_seconds,
        "original_window": window, "timing": timing,
        "displayed_mean_interval_ms": displayed_mean_ms,
        "displayed_mean_fps": 1000.0 / displayed_mean_ms if displayed_mean_ms else None,
        "binary_sha256": provenance.get("binary", {}).get("sha256"),
        "save_sha256": telemetry.get("save_sha256"),
        "shader_receipt_set_sha256": provenance.get("shader_receipt_set_sha256"),
        "fixture": telemetry.get("fixture"), "level": telemetry.get("level_name"),
        "engine_extent": [telemetry.get("engine_width"), telemetry.get("engine_height")],
        "scenario_result": telemetry.get("fixture_scenario", {}).get("status"),
        "note": "Timing evidence only; nonzero process exits cannot be accepted performance runs. "
                "No visual parity or verified visible-NPC count is inferred.",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("runs", nargs="+", type=pathlib.Path)
    parser.add_argument("--settle", type=float, default=30)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    if not math.isfinite(args.settle) or args.settle < 0:
        parser.error("settle must be finite and nonnegative")
    results = [summarize_run(path, args.settle) for path in args.runs]
    args.output.write_text(json.dumps(results, indent=2) + "\n")
    for result in results:
        timing = result["timing"]
        displayed = timing["displayed_frame_interval_ms"]
        ms = displayed["p50"]
        print(f'{result["run"]}: exit={result["engine_exit_status"]} '
              f'displayed_p50={ms:.3f}ms (median rate {1000 / ms:.2f}FPS) '
              f'1%low={displayed["one_percent_low_fps"]:.2f}FPS '
              f'encode_p50={timing["cpu_stage_ms"]["encode_ms"]["p50"]:.3f}ms '
              f'gpu_p50={timing["gpu_frame_ms"]["p50"]:.3f}ms')


if __name__ == "__main__":
    main()
