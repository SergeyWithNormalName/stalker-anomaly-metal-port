#!/usr/bin/env python3
"""Summarize E01 frame timing CSVs without inventing missing measurements.

Durations in the input stage columns are milliseconds. GPU timestamps are
seconds on the GPU clock and are differenced only when both endpoints exist.
Frame time is the delta between consecutive valid begin_ns values.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import statistics
import sys
from typing import Any


STAGE_FIELDS = (
    "wait_ms", "drawable_ms", "scene_ms", "encode_ms",
    "present_encode_ms", "commit_ms",
)
OPTIONAL_STAGE_FIELDS = ("grass_ms", "dynamic_prewarm_ms", "scene_other_ms")
COUNTER_FIELDS = ("updated_bytes", "buffer_allocations", "texture_allocations", "indexed_draws", "pso_binds")
REQUIRED_FIELDS = (
    "serial", "engine_frame", "camera_frame", "slot", "begin_ns",
    *STAGE_FIELDS, "scheduled_ns", "completed_ns", "gpu_start_s",
    "gpu_end_s", "presented_s", "queue_depth", "status",
)


def number(value: Any, *, positive: bool = False) -> float | None:
    """Parse finite numeric samples; zero is a missing sentinel."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result) or result == 0 or (positive and result < 0):
        return None
    return result


def nonnegative_number(value: Any) -> float | None:
    """Parse finite counters, preserving zero as a valid observation."""
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(result) or result < 0:
        return None
    return result


def percentile(values: list[float], p: float) -> float | None:
    """Linear interpolation percentile (R-7), stable for small samples."""
    if not values:
        return None
    ordered = sorted(values)
    index = (len(ordered) - 1) * p
    lower = math.floor(index)
    upper = math.ceil(index)
    if lower == upper:
        return ordered[lower]
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def summarize(values: list[float]) -> dict[str, float | int | None]:
    return {
        "count": len(values),
        "p50": percentile(values, 0.50),
        "p95": percentile(values, 0.95),
        "p99": percentile(values, 0.99),
        "max": max(values) if values else None,
    }


def frame_metrics(values: list[float]) -> dict[str, float | int | None]:
    result = summarize(values)
    slow_count = math.ceil(len(values) * 0.01) if values else 0
    slowest = sorted(values, reverse=True)[:slow_count]
    result.update({
        "over_25ms": sum(value > 25 for value in values),
        "over_33ms": sum(value > 33 for value in values),
        "over_50ms": sum(value > 50 for value in values),
        "one_percent_low_fps": 1000.0 / statistics.mean(slowest) if slowest else None,
        "one_percent_low_sample_count": slow_count,
    })
    return result


def confidence(count: int) -> str:
    if count >= 300:
        return "high"
    if count >= 120:
        return "medium"
    return "low"


def read_window(path: pathlib.Path | None) -> dict[str, int] | None:
    if path is None:
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read timing window JSON: {error}") from error
    if not isinstance(value, dict):
        raise ValueError("timing window must be a JSON object")
    bounds: dict[str, int] = {}
    for name in ("start_engine_frame", "end_engine_frame"):
        frame = value.get(name)
        if isinstance(frame, bool) or not isinstance(frame, int) or frame < 0:
            raise ValueError(f"timing window {name} must be a non-negative integer")
        bounds[name] = frame
    if bounds["start_engine_frame"] > bounds["end_engine_frame"]:
        raise ValueError("timing window start_engine_frame must not exceed end_engine_frame")
    return bounds


def read_presentation(path: pathlib.Path | None) -> dict[int, float] | None:
    if path is None:
        return None
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None or "serial" not in reader.fieldnames or "presented_s" not in reader.fieldnames:
            raise ValueError("presentation CSV must contain serial and presented_s columns")
        timestamps: dict[int, float] = {}
        duplicates: set[int] = set()
        for row in reader:
            serial = number(row.get("serial"), positive=True)
            presented = number(row.get("presented_s"), positive=True)
            if serial is None or not serial.is_integer() or presented is None:
                continue
            serial_int = int(serial)
            if serial_int in timestamps:
                duplicates.add(serial_int)
            else:
                timestamps[serial_int] = presented
    for serial in duplicates:
        timestamps.pop(serial, None)
    return timestamps


def analyze(
    path: pathlib.Path,
    window: dict[str, int] | None = None,
    presentation: dict[int, float] | None = None,
) -> dict[str, Any]:
    with path.open(newline="", encoding="utf-8-sig") as stream:
        reader = csv.DictReader(stream)
        if reader.fieldnames is None:
            raise ValueError("input CSV has no header")
        missing = [field for field in REQUIRED_FIELDS if field not in reader.fieldnames]
        if missing:
            raise ValueError("input CSV missing required columns: " + ", ".join(missing))
        rows = list(reader)

    # Keep only successful submissions, ordered by serial when available.
    valid_rows: list[dict[str, Any]] = []
    rejected = 0
    for row in rows:
        status = (row.get("status") or "").strip().lower()
        if status not in ("", "ok", "pass", "passed", "success", "completed", "submitted"):
            rejected += 1
            continue
        serial = number(row.get("serial"), positive=True)
        begin = number(row.get("begin_ns"), positive=True)
        if serial is None or begin is None:
            rejected += 1
            continue
        row["_serial"] = serial
        row["_begin_ns"] = begin
        valid_rows.append(row)
    valid_rows.sort(key=lambda row: row["_serial"])
    rows_outside_window = 0
    if window is not None:
        start_frame = window["start_engine_frame"]
        end_frame = window["end_engine_frame"]
        selected = []
        for row in valid_rows:
            try:
                parsed_frame = float(row.get("engine_frame", ""))
            except (TypeError, ValueError):
                parsed_frame = math.nan
            if math.isfinite(parsed_frame) and parsed_frame.is_integer() and start_frame <= int(parsed_frame) <= end_frame:
                selected.append(row)
            else:
                rows_outside_window += 1
        valid_rows = selected
        if not valid_rows:
            raise ValueError("timing window contains no valid frames")
    for row in valid_rows:
        if presentation is not None:
            row["_presented_s"] = presentation.get(int(row["_serial"]))
        else:
            row["_presented_s"] = number(row.get("presented_s"), positive=True)

    frame_ms: list[float] = []
    previous_time: float | None = None
    for current in valid_rows:
        current_time = current["_begin_ns"]
        if previous_time is not None:
            delta = (current_time - previous_time) / 1_000_000.0
            # Ignore duplicate/backwards timestamps without moving the baseline.
            if math.isfinite(delta) and delta > 0:
                frame_ms.append(delta)
                previous_time = current_time
        else:
            previous_time = current_time

    frame_summary = frame_metrics(frame_ms)

    presented_rows = [row for row in valid_rows if row["_presented_s"] is not None]
    displayed_count = len(presented_rows)
    zero_timestamp_callbacks = len(valid_rows) - displayed_count
    displayed_interval_ms: list[float] = []
    serial_gap_events = 0
    serial_gap_frames = 0
    for previous, current in zip(presented_rows, presented_rows[1:]):
        serial_delta = int(current["_serial"]) - int(previous["_serial"])
        if serial_delta > 1:
            serial_gap_events += 1
            serial_gap_frames += serial_delta - 1
            continue
        if serial_delta != 1:
            continue
        delta = round((current["_presented_s"] - previous["_presented_s"]) * 1000.0, 9)
        if math.isfinite(delta) and delta > 0:
            displayed_interval_ms.append(delta)
    submission_serial_gap_events = 0
    submission_serial_gap_frames = 0
    for previous, current in zip(valid_rows, valid_rows[1:]):
        serial_delta = int(current["_serial"]) - int(previous["_serial"])
        if serial_delta > 1:
            submission_serial_gap_events += 1
            submission_serial_gap_frames += serial_delta - 1
    complete_presentation = bool(valid_rows) and zero_timestamp_callbacks == 0
    monotonic_presentation = complete_presentation and serial_gap_events == 0 and len(displayed_interval_ms) == max(0, len(valid_rows) - 1)
    if monotonic_presentation:
        display_quality = "complete"
    elif displayed_interval_ms:
        display_quality = "partial"
    else:
        display_quality = "unavailable"

    derived_other = all(field in reader.fieldnames for field in ("grass_ms", "dynamic_prewarm_ms"))
    stage_fields = STAGE_FIELDS + tuple(field for field in OPTIONAL_STAGE_FIELDS
                                  if field in reader.fieldnames or (field == "scene_other_ms" and derived_other))
    counter_fields = tuple(field for field in COUNTER_FIELDS if field in reader.fieldnames)
    stages: dict[str, list[float]] = {field: [] for field in stage_fields}
    counters: dict[str, list[float]] = {field: [] for field in counter_fields}
    gpu_ms: list[float] = []
    scheduled = 0
    completed = 0
    for row in valid_rows:
        for field in stage_fields:
            if field == "scene_other_ms" and derived_other:
                scene = nonnegative_number(row.get("scene_ms"))
                grass = nonnegative_number(row.get("grass_ms"))
                dynamic = nonnegative_number(row.get("dynamic_prewarm_ms"))
                value = scene - grass - dynamic if None not in (scene, grass, dynamic) else None
                if value is not None and value < 0:
                    value = None
            else:
                value = number(row.get(field), positive=True)
            if value is not None:
                stages[field].append(value)
        for field in counter_fields:
            value = nonnegative_number(row.get(field))
            if value is not None:
                counters[field].append(value)
        start = number(row.get("gpu_start_s"), positive=True)
        end = number(row.get("gpu_end_s"), positive=True)
        if start is not None and end is not None and end > start:
            gpu_ms.append((end - start) * 1000.0)
        for field, counter in (("scheduled_ns", "scheduled"), ("completed_ns", "completed")):
            value = number(row.get(field), positive=True)
            if value is not None:
                if counter == "scheduled": scheduled += 1
                else: completed += 1

    timing_metrics = {name: summarize(values) for name, values in stages.items()}
    counter_metrics = {name: summarize(values) for name, values in counters.items()}
    gpu_metrics = summarize(gpu_ms)
    eligible = len(valid_rows)
    pacing_gate = "CLOSED" if complete_presentation and monotonic_presentation and len(displayed_interval_ms) == max(0, eligible - 1) else "OPEN"
    if pacing_gate == "CLOSED":
        pacing_reason = "complete monotonic presented_s timestamps"
    elif displayed_count < eligible:
        pacing_reason = "missing displayed timestamps; callback/throughput is not on-screen pacing"
    else:
        pacing_reason = "presented_s timestamps are not strictly monotonic"
    baseline: dict[str, Any] = {
        "schema": "direct-metal-e01-baseline/v1",
        "input": str(path),
        "presentation_input": None if presentation is None else "serial-keyed presentation CSV",
        "rows_read": len(rows),
        "rows_accepted": eligible,
        "rows_rejected": rejected,
        "rows_filtered_outside_window": rows_outside_window,
        "timing_window": window,
        "frame_interval_ms": frame_summary,
        "displayed_frame_interval_ms": frame_metrics(displayed_interval_ms),
        "cpu_stage_ms": timing_metrics,
        "frame_counters": counter_metrics,
        "gpu_frame_ms": gpu_metrics,
        "submission": {"scheduled_samples": scheduled, "completed_samples": completed},
        "presentation": {
            "displayed_timestamp_samples": displayed_count,
            "displayed_interval_samples": len(displayed_interval_ms),
            "zero_timestamp_callbacks": zero_timestamp_callbacks,
            "serial_gap_events": serial_gap_events,
            "serial_gap_frames": serial_gap_frames,
            "submission_serial_gap_events": submission_serial_gap_events,
            "submission_serial_gap_frames": submission_serial_gap_frames,
            "display_metrics_status": display_quality,
            "eligible_frames": eligible,
            "pacing_gate": pacing_gate,
            "reason": pacing_reason,
        },
        "frame_source_note": "frame_interval_ms is begin_ns delta between consecutive accepted serials; first frame has no interval",
        "display_source_note": "displayed_frame_interval_ms uses only positive presented_s samples from consecutive serials; partial intervals exclude zero/missing callbacks and serial gaps",
        "gpu_source_note": "gpu_frame_ms uses gpu_end_s - gpu_start_s only; it is not per-pass attribution",
    }
    return baseline


def write_outputs(out_dir: pathlib.Path, baseline: dict[str, Any]) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    (out_dir / "baseline.json").write_text(json.dumps(baseline, indent=2, sort_keys=True) + "\n", encoding="utf-8")

    ordered = sorted(
        ((name, data) for name, data in baseline["cpu_stage_ms"].items() if data["count"]),
        key=lambda pair: pair[1]["p50"], reverse=True,
    )
    lines = ["# E01 CPU stage summary", "", "Stages are ranked by measured p50 duration. Missing/zero/non-finite values are excluded.", "encode_ms contains scene_ms; scene_ms contains grass_ms, dynamic_prewarm_ms, and scene_other_ms. Compare these diagnostically and do not add their durations.", "Sample confidence is coverage only: low <120, medium 120–299, high ≥300 samples; it does not establish causality.", ""]
    if not ordered:
        lines.append("No valid CPU stage samples were present.")
    else:
        lines.extend(["| Stage | Count | p50 ms | p95 ms | p99 ms | Max ms | Confidence |", "|---|---:|---:|---:|---:|---:|---|"])
        for name, data in ordered:
            lines.append(f"| {name} | {data['count']} | {data['p50']:.3f} | {data['p95']:.3f} | {data['p99']:.3f} | {data['max']:.3f} | {confidence(data['count'])} |")
    (out_dir / "cpu_top.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    counter_lines = ["", "## Per-frame counters", "", "Counters are measurements only; they have no timing attribution.", ""]
    counter_metrics = baseline.get("frame_counters", {})
    if any(data["count"] for data in counter_metrics.values()):
        counter_lines += ["| Counter | Samples | p50 | p95 | Max |", "|---|---:|---:|---:|---:|"]
        for name, data in counter_metrics.items():
            if not data["count"]:
                continue
            counter_lines.append(f"| {name} | {data['count']} | {data['p50']:.3f} | {data['p95']:.3f} | {data['max']:.3f} |")
    else:
        counter_lines.append("No per-frame counter columns were present.")
    with (out_dir / "cpu_top.md").open("a", encoding="utf-8") as stream:
        stream.write("\n".join(counter_lines) + "\n")

    gpu_path = out_dir / "gpu_passes.csv"
    with gpu_path.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.writer(stream)
        writer.writerow(("status", "reason"))
        writer.writerow(("UNAVAILABLE", "input contains frame GPU timestamp pairs only; no per-pass counters/timings"))

    measured_stages = {
        name: data for name, data in baseline["cpu_stage_ms"].items() if data["count"]
    }
    overlap_names = ("scene_ms", "encode_ms", "grass_ms", "dynamic_prewarm_ms", "scene_other_ms")
    stage_order = [(name, data) for name, data in measured_stages.items()
                   if name not in overlap_names]
    overlap_members = [(name, measured_stages[name]) for name in overlap_names
                       if name in measured_stages]
    if overlap_members:
        representative_name, representative = max(overlap_members, key=lambda pair: pair[1]["p50"])
        stage_order.append((f"scene nested-work overlap group (representative: {representative_name})", representative))
    stage_order = sorted(stage_order, key=lambda pair: pair[1]["p50"], reverse=True)[:3]
    budget_lines = ["# E01 measured budget", "", "Top CPU stage costs ranked by p50. This ranking does not establish a GPU bottleneck or additive frame time.", "encode_ms contains scene_ms; scene_ms contains grass_ms, dynamic_prewarm_ms, and scene_other_ms. These are one nested-work overlap group; the representative is the member with the larger p50, and durations are never summed.", "Sample confidence is coverage only: low <120, medium 120–299, high ≥300 samples; it does not establish causality.", ""]
    if stage_order:
        budget_lines.extend(["| Rank | Measured cost | p50 ms | p95 ms | Samples | Confidence |", "|---:|---|---:|---:|---:|---|"])
        for rank, (name, data) in enumerate(stage_order, 1):
            budget_lines.append(f"| {rank} | {name} | {data['p50']:.3f} | {data['p95']:.3f} | {data['count']} | {confidence(data['count'])} |")
    else:
        budget_lines.append("No valid CPU stage timing samples; three dominant costs cannot be measured from this input.")
    gpu = baseline["gpu_frame_ms"]
    displayed = baseline["displayed_frame_interval_ms"]
    display_line = f"Displayed frame interval samples: {displayed['count']} (p50 {displayed['p50']:.3f} ms)" if displayed["count"] else "Displayed frame intervals unavailable unless presentation timestamps are complete."
    budget_lines += ["", f"GPU frame duration samples: {gpu['count']} (p50 {gpu['p50']:.3f} ms)" if gpu["count"] else "GPU frame duration unavailable: valid timestamp pairs were not present.", "GPU pass attribution: unavailable; see gpu_passes.csv. No per-pass cost is inferred from CPU encode durations.", display_line, f"Presentation pacing gate: {baseline['presentation']['pacing_gate']} ({baseline['presentation']['reason']})."]
    (out_dir / "budget.md").write_text("\n".join(budget_lines) + "\n", encoding="utf-8")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--frames", required=True, type=pathlib.Path, help="input frames CSV")
    parser.add_argument("--output", required=True, type=pathlib.Path, help="output directory")
    parser.add_argument("--window", type=pathlib.Path, help="optional JSON with inclusive start_engine_frame/end_engine_frame bounds")
    parser.add_argument("--presentation", type=pathlib.Path, help="optional serial,presented_s CSV from drawable presentation callbacks")
    args = parser.parse_args(argv)
    try:
        baseline = analyze(args.frames, read_window(args.window), read_presentation(args.presentation))
        if args.presentation is not None:
            baseline["presentation_input"] = str(args.presentation)
        write_outputs(args.output, baseline)
    except (OSError, ValueError, csv.Error) as error:
        print(f"E01 analyzer error: {error}", file=sys.stderr)
        return 2
    print(f"E01 summaries written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
