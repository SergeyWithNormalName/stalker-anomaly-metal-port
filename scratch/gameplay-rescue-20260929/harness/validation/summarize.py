#!/usr/bin/env python3
"""Validate and summarize the low-overhead callback trace."""

import csv
import json
import math
import statistics
import sys
from pathlib import Path


def percentile(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return float(ordered[index])


path = Path(sys.argv[1])
with path.open(newline="") as stream:
    rows = [row for row in csv.DictReader(stream) if row["phase"] == "1"]

if len(rows) < 2:
    raise SystemExit(f"need at least two measured callbacks in {path}; found {len(rows)}")

frames = [int(row["engine_frame"]) for row in rows]
times = [int(row["wall_ms"]) for row in rows]
if any(b <= a for a, b in zip(times, times[1:])):
    raise SystemExit("invalid run: callback wall timestamps are not strictly increasing")
if any(b != a + 1 for a, b in zip(frames, frames[1:])):
    raise SystemExit("invalid run: engine frame sequence contains a gap or duplicate")

intervals = [b - a for a, b in zip(times, times[1:])]
elapsed_s = (times[-1] - times[0]) / 1000
if elapsed_s <= 0:
    raise SystemExit("invalid run: non-positive measured wall duration")

for key in ("level", "weather", "device_time_factor", "game_time_factor", "width", "height"):
    values = {row[key] for row in rows}
    if len(values) != 1:
        raise SystemExit(f"invalid run: {key} changed during measurement: {sorted(values)}")

device_factor = [float(row["device_time_factor"]) for row in rows]
game_factor = [float(row["game_time_factor"]) for row in rows]
if any(abs(value - 1.0) > 1e-6 for value in device_factor):
    raise SystemExit("invalid run: Device clock was not held at factor 1")
if any(abs(value - 6.0) > 1e-6 for value in game_factor):
    raise SystemExit("invalid run: expected live game time factor 6")

camera_columns = ("camera_x", "camera_y", "camera_z", "dir_x", "dir_y", "dir_z")
camera = {key: [float(row[key]) for row in rows] for key in camera_columns}
drift = {key: max(values) - min(values) for key, values in camera.items()}
if any(drift[key] > 0.02 for key in camera_columns[:3]):
    raise SystemExit(f"invalid run: camera position drift exceeds 2 cm: {drift}")
if any(drift[key] > 0.002 for key in camera_columns[3:]):
    raise SystemExit(f"invalid run: camera direction drift exceeds tolerance: {drift}")

p50 = float(statistics.median(intervals))
p95 = percentile(intervals, 0.95)
p99 = percentile(intervals, 0.99)
tail_count = max(1, math.ceil(len(intervals) * 0.01))
tail_mean = statistics.mean(sorted(intervals, reverse=True)[:tail_count])
result = {
    "callbacks": len(rows),
    "engine_frame_start": frames[0],
    "engine_frame_end": frames[-1],
    "elapsed_s": elapsed_s,
    "mean_wall_fps": (len(rows) - 1) / elapsed_s,
    "callback_interval_ms": {"p50": p50, "p95": p95, "p99": p99, "max": max(intervals)},
    "callback_tail_counts": {str(threshold): sum(value > threshold for value in intervals) for threshold in (50, 100, 250, 500, 1000)},
    "one_percent_low_fps_from_p99": 1000.0 / p99,
    "one_percent_low_fps_from_mean_slowest_intervals": 1000.0 / tail_mean,
    "slowest_one_percent": {"intervals": tail_count, "mean_interval_ms": tail_mean},
    "state": {
        "level": rows[0]["level"],
        "weather": rows[0]["weather"],
        "calendar_start": rows[0]["calendar"],
        "calendar_end": rows[-1]["calendar"],
        "device_time_factor": device_factor[0],
        "game_time_factor": game_factor[0],
        "render_resolution": f"{rows[0]['width']}x{rows[0]['height']}",
        "camera_start": [camera[key][0] for key in camera_columns],
        "camera_max_axis_drift": drift,
    },
}
print(json.dumps(result, indent=2, sort_keys=True))
