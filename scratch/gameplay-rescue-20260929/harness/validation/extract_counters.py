#!/usr/bin/env python3
"""Extract frame-scoped visibility counters from the engine diagnostic log."""

import json
import re
import statistics
import sys
from pathlib import Path


phases_path = Path(sys.argv[1])
log_path = Path(sys.argv[2])
phase_text = phases_path.read_text(errors="replace")

def marker_frame(name: str) -> int:
    match = re.search(rf"^{re.escape(name)},frame=(\d+),", phase_text, re.MULTILINE)
    if not match:
        raise SystemExit(f"missing {name} marker in {phases_path}")
    return int(match.group(1))


start, end = marker_frame("measure_start"), marker_frame("measure_end")
if end <= start:
    raise SystemExit(f"invalid measurement frame bounds: {start}..{end}")
log = log_path.read_text(errors="replace")
field_re = re.compile(r"([a-z_]+)=([0-9]+)")
face_rows = []
light_rows = []
bypass_reasons = []
for line in log.splitlines():
    if "METAL_LOCAL_LIGHT_FACE_CULL" in line:
        if " status=bypassed " in line:
            bypass_reasons.append(line.strip())
            continue
        frame_match = re.search(r"\bframe=(\d+)", line)
        if frame_match and start <= int(frame_match.group(1)) <= end:
            face_rows.append({key: int(value) for key, value in field_re.findall(line)})
    elif "METAL_LIGHTING_FRAME" in line:
        frame_match = re.search(r"\bframe=(\d+)", line)
        if frame_match and start <= int(frame_match.group(1)) <= end:
            light_rows.append({key: int(value) for key, value in field_re.findall(line)})

def summarize(rows: list[dict[str, int]]) -> dict[str, object]:
    fields = sorted({key for row in rows for key in row if key != "frame"})
    return {
        "frames": len(rows),
        "fields": {
            key: {
                "min": min(row[key] for row in rows if key in row),
                "p50": statistics.median(row[key] for row in rows if key in row),
                "max": max(row[key] for row in rows if key in row),
            }
            for key in fields
        },
    }


removed = [row.get("removed", 0) for row in face_rows]
result = {
    "measured_engine_frames": [start, end],
    "face_cull": summarize(face_rows),
    "face_cull_positive_removed_frames": sum(value > 0 for value in removed),
    "face_cull_removed_total_over_logged_frames": sum(removed),
    "face_cull_bypassed": bypass_reasons,
    "lighting_admission": summarize(light_rows),
}
print(json.dumps(result, indent=2, sort_keys=True))
