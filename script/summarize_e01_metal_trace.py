#!/usr/bin/env python3
"""Summarize labelled GPU activity intervals exported from Metal System Trace.

These are trace encoder intervals, not hardware occupancy or an additive GPU
frame budget. The trace is a separate intrusive diagnostic run.
"""

import argparse
import csv
import json
import pathlib
import re
import statistics
import xml.etree.ElementTree as ET
from collections import defaultdict


def percentile(values, fraction):
    ordered = sorted(values)
    index = (len(ordered) - 1) * fraction
    lower = int(index)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (index - lower)


def union_ns(intervals):
    merged = []
    for start, end in sorted(intervals):
        if merged and start <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], end))
        else:
            merged.append((start, end))
    return sum(end - start for start, end in merged)


def summarize(xml_path, process_name):
    label_pattern = re.compile(
        rf"R4 Metal frame (\d+):(.+?)\s+\({re.escape(process_name)} \(\d+\)\)")
    intervals = defaultdict(list)
    matched_rows = 0
    for _, row in ET.iterparse(xml_path, events=("end",)):
        if row.tag != "row":
            continue
        label = next((item.attrib.get("fmt", "") for item in row
                      if item.tag == "formatted-label" and "R4 Metal frame " in item.attrib.get("fmt", "")), "")
        match = label_pattern.search(label)
        if match:
            start = row.find("start-time")
            duration = row.find("duration")
            if start is not None and duration is not None and start.text and duration.text:
                try:
                    start_ns, duration_ns = int(start.text), int(duration.text)
                except ValueError:
                    pass
                else:
                    if duration_ns > 0:
                        pass_name = match.group(2).strip()
                        if pass_name.startswith("R4 private buffer upload"):
                            pass_name = "R4 private buffer upload (grouped)"
                        intervals[(int(match.group(1)), pass_name)].append(
                            (start_ns, start_ns + duration_ns))
                        matched_rows += 1
        row.clear()
    by_pass = defaultdict(list)
    counts = defaultdict(int)
    for (_, pass_name), group in intervals.items():
        by_pass[pass_name].append(union_ns(group) / 1_000_000.0)
        counts[pass_name] += len(group)
    rows = []
    for pass_name, values in by_pass.items():
        rows.append({
            "pass": pass_name,
            "frames": len(values),
            "median_gpu_activity_ms": statistics.median(values),
            "p95_gpu_activity_ms": percentile(values, 0.95),
            "max_gpu_activity_ms": max(values),
            "encoder_intervals": counts[pass_name],
        })
    rows.sort(key=lambda row: row["median_gpu_activity_ms"], reverse=True)
    summary = {
        "schema": "direct-metal-e01-trace-passes/v1",
        "source": str(xml_path),
        "process": process_name,
        "matched_gpu_interval_rows": matched_rows,
        "labelled_frames": len({frame for frame, _ in intervals}),
        "pass_count": len(rows),
        "interpretation": "Per-pass union of labelled GPU activity intervals within each traced frame. Passes can overlap and must not be summed; trace recording is intrusive and separate from baseline.",
    }
    return rows, summary


def summarize_allocated_size(xml_path):
    values = []
    for _, row in ET.iterparse(xml_path, events=("end",)):
        if row.tag != "row":
            continue
        size = row.find("size-in-bytes")
        if size is not None and size.text and size.text.isdigit():
            values.append(int(size.text))
        row.clear()
    if not values:
        return {"source": str(xml_path), "samples": 0}
    return {
        "source": str(xml_path), "samples": len(values),
        "first_bytes": values[0], "last_bytes": values[-1],
        "min_bytes": min(values), "max_bytes": max(values),
        "note": "MTLDevice.currentAllocatedSize from Metal System Trace; this is allocated resource size, not GPU occupancy or bandwidth.",
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-xml", required=True, type=pathlib.Path)
    parser.add_argument("--output-csv", required=True, type=pathlib.Path)
    parser.add_argument("--output-json", required=True, type=pathlib.Path)
    parser.add_argument("--process", default="AnomalyDX11")
    parser.add_argument("--memory-xml", type=pathlib.Path)
    args = parser.parse_args()
    rows, summary = summarize(args.input_xml, args.process)
    if args.memory_xml:
        summary["current_allocated_size"] = summarize_allocated_size(args.memory_xml)
    if not rows:
        parser.error("no labelled Direct Metal GPU intervals found")
    args.output_csv.parent.mkdir(parents=True, exist_ok=True)
    with args.output_csv.open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)
    args.output_json.write_text(json.dumps(summary, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(f"E01 trace summary: {summary['labelled_frames']} frames, {summary['pass_count']} passes")


if __name__ == "__main__":
    main()
