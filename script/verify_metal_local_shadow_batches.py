#!/usr/bin/env python3
"""Validate production local-shadow batch stdout and native GPU JSON readbacks.

This is an evidence gate, not a renderer simulator: JSON comes from the native
Metal readback callback, while identities and per-frame totals are cross-checked
against the engine stdout emitted by renderLocalShadowBatch.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


SCHEMA = "xray.r4.local-shadow-batch-validation.v1"
CAPACITY = 16
FACE_SET = set(range(6))

FRAME_RE = re.compile(r"METAL_LOCAL_SHADOW_BATCH_FRAME\s+(?P<fields>[^\r\n]+)")
RECORD_RE = re.compile(r"METAL_LOCAL_SHADOW_BATCH_RECORD\s+(?P<fields>[^\r\n]+)")
FIELD_RE = re.compile(r"([A-Za-z_][A-Za-z0-9_]*)=([^\s]+)")
CAMERA_DETAIL_RE = re.compile(
    r"(?:^|\s)frame=(?P<frame>\d+).*?eye=(?P<eye>-?[\d.eE+-]+,-?[\d.eE+-]+,-?[\d.eE+-]+)"
    r"\s+direction=(?P<direction>-?[\d.eE+-]+,-?[\d.eE+-]+,-?[\d.eE+-]+)"
    r".*?angle_from_base_deg=(?P<angle>-?[\d.eE+-]+)"
    r"\s+offset_from_base_m=(?P<offset>-?[\d.eE+-]+)"
)


class EvidenceError(Exception):
    pass


def parse_fields(line: str) -> dict[str, str]:
    return dict(FIELD_RE.findall(line))


def required_int(value: Any, label: str, errors: list[str], *, minimum: int = 0) -> int | None:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        errors.append(f"{label} must be an integer >= {minimum}")
        return None
    return value


def finite_number(value: Any, label: str, errors: list[str], *, minimum: float | None = None) -> float | None:
    if isinstance(value, bool) or not isinstance(value, (int, float)) or not math.isfinite(float(value)):
        errors.append(f"{label} must be a finite number")
        return None
    result = float(value)
    if minimum is not None and result < minimum:
        errors.append(f"{label} must be >= {minimum}")
        return None
    return result


def finite_vector(value: Any, length: int, label: str, errors: list[str]) -> list[float] | None:
    if not isinstance(value, list) or len(value) != length:
        errors.append(f"{label} must be a {length}-element array")
        return None
    output: list[float] = []
    for index, item in enumerate(value):
        number = finite_number(item, f"{label}[{index}]", errors)
        if number is None:
            return None
        output.append(number)
    return output


def parse_vector_text(value: str) -> list[float] | None:
    try:
        output = [float(part) for part in value.split(",")]
    except ValueError:
        return None
    return output if len(output) == 3 and all(math.isfinite(x) for x in output) else None


def distance(a: list[float], b: list[float]) -> float:
    return math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)))


def direction_angle_degrees(a: list[float], b: list[float]) -> float:
    la = math.sqrt(sum(x * x for x in a))
    lb = math.sqrt(sum(x * x for x in b))
    if la <= 1e-12 or lb <= 1e-12:
        return 180.0
    cosine = sum(x * y for x, y in zip(a, b)) / (la * lb)
    return math.degrees(math.acos(max(-1.0, min(1.0, cosine))))


def load_json(path: Path) -> Any:
    try:
        with path.open("r", encoding="utf-8") as source:
            return json.load(source)
    except (OSError, json.JSONDecodeError) as exc:
        raise EvidenceError(f"cannot read JSON {path}: {exc}") from exc


def load_native_reports(probe_dir: Path, errors: list[str]) -> dict[tuple[int, int], dict[str, Any]]:
    reports: dict[tuple[int, int], dict[str, Any]] = {}
    files = sorted(probe_dir.glob("local-shadow-frame-*-batch-*.json"))
    if not files:
        errors.append(f"no native GPU validation JSON files found in {probe_dir}")
        return reports
    for path in files:
        try:
            report = load_json(path)
        except EvidenceError as exc:
            errors.append(str(exc))
            continue
        if not isinstance(report, dict):
            errors.append(f"{path.name}: top-level JSON value must be an object")
            continue
        if report.get("schema") != SCHEMA:
            errors.append(f"{path.name}: schema must be {SCHEMA!r}")
            continue
        frame = required_int(report.get("frame"), f"{path.name}.frame", errors)
        batch = required_int(report.get("batch"), f"{path.name}.batch", errors)
        if frame is None or batch is None:
            continue
        key = (frame, batch)
        if key in reports:
            errors.append(f"duplicate native report for frame={frame} batch={batch}")
            continue
        reports[key] = report
    return reports


def validate_native_report(report: dict[str, Any], label: str, errors: list[str]) -> list[dict[str, Any]]:
    if report.get("pass") is not True:
        errors.append(f"{label}: native GPU proof pass=false (gpu_error={report.get('gpu_error')!r})")
    if report.get("gpu_completed") is not True or report.get("artifacts_written") is not True:
        errors.append(f"{label}: GPU completion or artifact write did not succeed")
    atlas_width = required_int(report.get("atlas_width"), f"{label}.atlas_width", errors, minimum=1)
    atlas_height = required_int(report.get("atlas_height"), f"{label}.atlas_height", errors, minimum=1)
    width = required_int(report.get("camera_width"), f"{label}.camera_width", errors, minimum=1)
    height = required_int(report.get("camera_height"), f"{label}.camera_height", errors, minimum=1)
    if atlas_width != 2048 or atlas_height != 2048:
        errors.append(f"{label}: expected 2048x2048 production atlas")
    if report.get("accumulation_nonfinite_channels") != 0:
        errors.append(f"{label}: accumulation contains non-finite channels")
    if report.get("nonfinite_primary_depth_pixels") != 0:
        errors.append(f"{label}: primary depth contains non-finite pixels")
    for name in ("accumulation_finite_channels", "receiver_position_pixels", "camera_reprojectable_pixels"):
        required_int(report.get(name), f"{label}.{name}", errors)
    finite_number(report.get("camera_max_reprojection_error_pixels"),
        f"{label}.camera_max_reprojection_error_pixels", errors, minimum=0.0)
    for name in ("camera_eye", "camera_forward"):
        vector = finite_vector(report.get(name), 3, f"{label}.{name}", errors)
        if vector is not None and name == "camera_forward" and math.sqrt(sum(x*x for x in vector)) <= 1e-9:
            errors.append(f"{label}.camera_forward has zero length")
    for name in ("view", "projection", "inverse_view"):
        finite_vector(report.get(name), 16, f"{label}.{name}", errors)
    for name in ("depth_hash_fnv1a64", "geometry_stencil_bit_hash_fnv1a64"):
        value = report.get(name)
        if not isinstance(value, str) or not re.fullmatch(r"[0-9a-fA-F]{16}", value):
            errors.append(f"{label}.{name} must be a 16-digit FNV-1a hex string")

    records = report.get("records")
    if not isinstance(records, list) or not records:
        errors.append(f"{label}.records must be a non-empty array")
        return []
    if len(records) > CAPACITY:
        errors.append(f"{label}: batch has {len(records)} records, over capacity {CAPACITY}")
    checked: list[dict[str, Any]] = []
    seen_ids: set[int] = set()
    for index, record in enumerate(records):
        rlabel = f"{label}.records[{index}]"
        if not isinstance(record, dict):
            errors.append(f"{rlabel} must be an object")
            continue
        light_id = required_int(record.get("light_id"), f"{rlabel}.light_id", errors, minimum=1)
        parent_id = required_int(record.get("parent_id"), f"{rlabel}.parent_id", errors, minimum=1)
        face = required_int(record.get("face"), f"{rlabel}.face", errors)
        tile = required_int(record.get("tile"), f"{rlabel}.tile", errors)
        if light_id is not None:
            if light_id in seen_ids:
                errors.append(f"{label}: duplicate source light_id {light_id} within batch")
            seen_ids.add(light_id)
        if face is not None and face > 5:
            errors.append(f"{rlabel}.face must be in [0,5]")
        if tile != index:
            errors.append(f"{rlabel}.tile={tile} but record order requires tile={index}")
        if record.get("rendered_depth") is not True:
            errors.append(f"{rlabel}.rendered_depth is not true")
        rect = record.get("atlas_rect")
        if not isinstance(rect, list) or len(rect) != 3 or not all(
                isinstance(v, int) and not isinstance(v, bool) for v in rect):
            errors.append(f"{rlabel}.atlas_rect must be integer [x,y,size]")
        elif atlas_width and atlas_height:
            x, y, size = rect
            if size != 512 or x < 0 or y < 0 or x + size > atlas_width or y + size > atlas_height:
                errors.append(f"{rlabel}.atlas_rect is not a valid 512 tile in the atlas")
            elif x % size or y % size:
                errors.append(f"{rlabel}.atlas_rect is not tile-aligned")
        covered = required_int(record.get("covered_pixels"), f"{rlabel}.covered_pixels", errors)
        nonfinite = required_int(record.get("nonfinite_pixels"), f"{rlabel}.nonfinite_pixels", errors)
        if nonfinite not in (None, 0):
            errors.append(f"{rlabel} has non-finite atlas pixels")
        minimum = finite_number(record.get("minimum_depth"), f"{rlabel}.minimum_depth", errors)
        maximum = finite_number(record.get("maximum_depth"), f"{rlabel}.maximum_depth", errors)
        if minimum is not None and maximum is not None and (minimum < -1.0 or maximum > 1.0 or minimum > maximum):
            errors.append(f"{rlabel} depth range is invalid")
        for name in ("primary_receiver_candidates", "receivers_in_light_frustum", "shadowed_receiver_pixels"):
            required_int(record.get(name), f"{rlabel}.{name}", errors)
        tile_hash = record.get("tile_hash_fnv1a64")
        if not isinstance(tile_hash, str) or not re.fullmatch(r"[0-9a-fA-F]{16}", tile_hash):
            errors.append(f"{rlabel}.tile_hash_fnv1a64 must be a 16-digit hex string")
        if light_id is not None and parent_id is not None and face is not None and tile is not None:
            checked.append({**record, "light_id": light_id, "parent_id": parent_id,
                "face": face, "tile": tile, "covered_pixels": covered})
    if width and height:
        finite_channels = report.get("accumulation_finite_channels")
        if isinstance(finite_channels, int) and finite_channels != width * height * 4:
            errors.append(f"{label}: accumulation_finite_channels does not match RGBA image extent")
    return checked


def parse_stdout(path: Path, errors: list[str]) -> tuple[dict[int, dict[str, Any]], dict[int, list[dict[str, Any]]], list[dict[str, Any]]]:
    summaries: dict[int, dict[str, Any]] = {}
    records: dict[int, list[dict[str, Any]]] = defaultdict(list)
    cameras: list[dict[str, Any]] = []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        errors.append(f"cannot read engine stdout {path}: {exc}")
        return summaries, records, cameras
    for line_number, line in enumerate(lines, 1):
        match = FRAME_RE.search(line)
        if match:
            fields = parse_fields(match.group("fields"))
            try:
                frame = int(fields["frame"])
                summary = {key: int(fields[key]) for key in
                    ("batches", "records", "rendered", "shadowed", "submitted", "culled", "fallback", "resets", "marker")}
                summary["eye"] = parse_vector_text(fields["eye"])
                if summary["eye"] is None:
                    raise ValueError("bad eye vector")
            except (KeyError, ValueError) as exc:
                errors.append(f"stdout line {line_number}: malformed local-shadow batch frame: {exc}")
                continue
            if frame in summaries:
                previous = summaries[frame]
                empty_summary = all(summary[key] == 0 for key in
                    ("batches", "records", "rendered", "shadowed", "submitted", "culled", "fallback", "resets"))
                if not (empty_summary and summary == previous):
                    errors.append(f"stdout contains non-empty or conflicting duplicate batch-frame summary for frame {frame}")
            else:
                summaries[frame] = summary
        match = RECORD_RE.search(line)
        if match:
            fields = parse_fields(match.group("fields"))
            try:
                record = {key: int(fields[key]) for key in ("frame", "batch", "tile", "id", "parent", "face", "type")}
            except (KeyError, ValueError) as exc:
                errors.append(f"stdout line {line_number}: malformed local-shadow batch record: {exc}")
                continue
            records[record["frame"]].append(record)
        if "METAL_ENGINE_CAMERA" in line:
            camera_match = re.search(r"METAL_ENGINE_CAMERA\s+frame=(\d+)\s+eye=([^\s]+)\s+forward=([^\s]+)", line)
            if camera_match:
                eye = parse_vector_text(camera_match.group(2))
                direction = parse_vector_text(camera_match.group(3))
                if eye and direction:
                    cameras.append({"frame": int(camera_match.group(1)), "eye": eye, "direction": direction})
    return summaries, records, cameras


def validate_stdout_and_join(reports: dict[tuple[int, int], dict[str, Any]],
    summaries: dict[int, dict[str, Any]], stdout_records: dict[int, list[dict[str, Any]]],
    errors: list[str], probe_every: int) -> tuple[dict[int, list[dict[str, Any]]], dict[int, dict[str, Any]]]:
    report_records_by_frame: dict[int, list[dict[str, Any]]] = defaultdict(list)
    reports_by_frame: dict[int, dict[str, Any]] = defaultdict(dict)
    if not summaries:
        errors.append("engine stdout has no METAL_LOCAL_SHADOW_BATCH_FRAME summaries")
    for frame, summary in summaries.items():
        frame_records = stdout_records.get(frame, [])
        if len(frame_records) != summary["records"]:
            errors.append(f"frame {frame}: stdout records={len(frame_records)} but summary says {summary['records']}")
        if summary["rendered"] != summary["records"]:
            errors.append(f"frame {frame}: rendered={summary['rendered']} differs from records={summary['records']}")
        if summary["fallback"] != 0:
            errors.append(f"frame {frame}: local-shadow fallback count is {summary['fallback']}")
        if summary["records"] > 16 and summary["batches"] < 2:
            errors.append(f"frame {frame}: {summary['records']} records did not overflow into multiple batches")
        batches: dict[int, list[dict[str, Any]]] = defaultdict(list)
        ids: set[int] = set()
        for record in frame_records:
            batches[record["batch"]].append(record)
            if record["id"] in ids:
                errors.append(f"frame {frame}: source light id {record['id']} appears more than once")
            ids.add(record["id"])
        if len(batches) != summary["batches"]:
            errors.append(f"frame {frame}: stdout contains {len(batches)} batches but summary says {summary['batches']}")
        if batches and set(batches) != set(range(summary["batches"])):
            errors.append(f"frame {frame}: batch ids are not contiguous from zero")
        for batch_id, batch_records in batches.items():
            if len(batch_records) > CAPACITY:
                errors.append(f"frame {frame} batch {batch_id}: {len(batch_records)} records exceeds capacity {CAPACITY}")
            ordered_tiles = sorted(record["tile"] for record in batch_records)
            if ordered_tiles != list(range(len(batch_records))):
                errors.append(f"frame {frame} batch {batch_id}: tile indices are not contiguous from zero")
            for record in batch_records:
                if record["type"] == 2 and record["face"] not in FACE_SET:
                    errors.append(f"frame {frame}: OmniFace id {record['id']} has invalid face {record['face']}")
        if frame_records and summary["records"] > CAPACITY and not any(r["type"] == 2 for r in frame_records):
            errors.append(f"frame {frame}: overrun frame has no captured OmniFace point group")

    for (frame, batch), report in reports.items():
        if frame not in summaries:
            errors.append(f"native report frame {frame} has no matching stdout batch-frame summary")
            continue
        summary = summaries[frame]
        if batch >= summary["batches"]:
            errors.append(f"native report frame {frame} batch {batch} exceeds stdout batch count")
        if report.get("batch") != batch:
            errors.append(f"native report filename and JSON batch disagree for frame {frame} batch {batch}")
        source_eye = summaries[frame].get("eye")
        native_eye = report.get("camera_eye")
        if isinstance(source_eye, list) and isinstance(native_eye, list) and len(native_eye) == 3:
            if distance(source_eye, native_eye) > 0.05:
                errors.append(f"frame {frame} batch {batch}: native camera eye disagrees with engine stdout")
        checked = validate_native_report(report, f"frame {frame} batch {batch}", errors)
        reports_by_frame[frame][batch] = report
        report_records_by_frame[frame].extend(checked)
        expected = [r for r in stdout_records.get(frame, []) if r["batch"] == batch]
        actual = report.get("records") if isinstance(report.get("records"), list) else []
        by_id: dict[int, dict[str, Any]] = {}
        for record in actual:
            if not isinstance(record, dict) or not isinstance(record.get("light_id"), int):
                continue
            if record["light_id"] in by_id:
                errors.append(f"frame {frame} batch {batch}: native JSON repeats light id {record['light_id']}")
            by_id[record["light_id"]] = record
        expected_by_id = {record["id"]: record for record in expected}
        if set(by_id) != set(expected_by_id):
            errors.append(f"frame {frame} batch {batch}: stdout/native JSON source IDs differ")
        for light_id in set(by_id) & set(expected_by_id):
            native = by_id[light_id]
            source = expected_by_id[light_id]
            checks = (("parent_id", "parent"), ("tile", "tile"))
            for native_key, source_key in checks:
                if native.get(native_key) != source[source_key]:
                    errors.append(f"frame {frame} batch {batch} light {light_id}: {native_key} disagrees with stdout {source_key}")
            if source["type"] == 2 and native.get("face") != source["face"]:
                errors.append(f"frame {frame} batch {batch} OmniFace {light_id}: face disagrees with stdout")
            elif source["type"] != 2 and native.get("face") != 0:
                errors.append(f"frame {frame} batch {batch} non-Omni light {light_id}: native face must be zero")
    for frame, summary in summaries.items():
        frame_reports = reports_by_frame.get(frame, {})
        if summary["batches"] == 0:
            continue
        if frame % probe_every != 0:
            continue
        if frame not in reports_by_frame:
            errors.append(f"frame {frame}: expected native JSON GPU proof at probe interval {probe_every}")
            continue
        if set(frame_reports) != set(range(summary["batches"])):
            errors.append(f"frame {frame}: native JSON does not include every batch in this stdout frame")
        report_ids = [record["light_id"] for record in report_records_by_frame.get(frame, [])]
        if len(report_ids) != summary["records"] or len(set(report_ids)) != summary["records"]:
            errors.append(f"frame {frame}: native GPU records are not an exact-once copy of source records")
        if summary["records"] > CAPACITY and summary["rendered"] == summary["records"] and summary["fallback"] == 0:
            if sum(int(r.get("shadowed_receiver_pixels", 0)) for r in report_records_by_frame.get(frame, [])) <= 0:
                errors.append(f"frame {frame}: overrun frame has no positive aggregate shadowed receiver evidence")
    return report_records_by_frame, reports_by_frame


def verify_point_groups(frame: int, stdout_records: list[dict[str, Any]],
    native_records: list[dict[str, Any]], errors: list[str]) -> list[dict[str, Any]]:
    groups: dict[int, list[dict[str, Any]]] = defaultdict(list)
    batch_by_parent: dict[int, set[int]] = defaultdict(set)
    for record in stdout_records:
        if record["type"] == 2:
            groups[record["parent"]].append(record)
            batch_by_parent[record["parent"]].add(record["batch"])
    native_by_id = {record["light_id"]: record for record in native_records}
    evidence: list[dict[str, Any]] = []
    if not groups:
        errors.append(f"frame {frame}: overrun frame contains no point-light OmniFace group")
        return evidence
    for parent, faces in groups.items():
        face_values = [record["face"] for record in faces]
        if len(faces) != 6 or set(face_values) != FACE_SET:
            errors.append(f"frame {frame}: point parent {parent} does not have each of six faces exactly once")
        if len(batch_by_parent[parent]) != 1:
            errors.append(f"frame {frame}: point parent {parent} was split across atlas batches")
        by_face = {record["face"]: record for record in faces}
        face_metrics: list[dict[str, Any]] = []
        for face in range(6):
            source = by_face.get(face)
            metric = native_by_id.get(source["id"]) if source else None
            covered = metric.get("covered_pixels", 0) if metric else 0
            in_frustum = metric.get("receivers_in_light_frustum", 0) if metric else 0
            shadowed = metric.get("shadowed_receiver_pixels", 0) if metric else 0
            if metric is None:
                errors.append(f"frame {frame}: point parent {parent} face {face} has no matching GPU tile data")
            elif covered <= 0:
                errors.append(f"frame {frame}: point parent {parent} face {face} atlas tile has no covered depth pixels")
            face_metrics.append({"face": face, "covered_pixels": covered,
                "receivers_in_light_frustum": in_frustum, "shadowed_receiver_pixels": shadowed,
                "empty_receiver_face": in_frustum == 0})
        evidence.append({"parent_id": parent, "batch": next(iter(batch_by_parent[parent]), None),
            "faces": face_metrics})
    return evidence


def parse_scenario(path: Path, errors: list[str]) -> dict[str, Any] | None:
    try:
        telemetry = load_json(path)
    except EvidenceError as exc:
        errors.append(str(exc))
        return None
    if not isinstance(telemetry, dict):
        errors.append("scenario report must be a telemetry JSON object")
        return None
    scenario: Any = telemetry.get("fixture_scenario")
    if isinstance(scenario, str):
        try:
            scenario = json.loads(scenario)
        except json.JSONDecodeError as exc:
            errors.append(f"fixture_scenario string is not valid JSON: {exc}")
            return None
    if not isinstance(scenario, dict):
        errors.append("scenario report has no fixture_scenario object/string")
        return None
    if scenario.get("status") != "OK" or scenario.get("failures") not in ([], None):
        errors.append(f"fixture_scenario must have status OK and no failures (status={scenario.get('status')!r})")
    observed: list[dict[str, Any]] = []
    for event in scenario.get("events", []):
        if not isinstance(event, dict) or event.get("kind") != "camera_observed":
            continue
        detail = event.get("detail")
        if not isinstance(detail, str):
            continue
        match = CAMERA_DETAIL_RE.search(detail)
        if not match:
            continue
        eye = parse_vector_text(match.group("eye"))
        direction = parse_vector_text(match.group("direction"))
        if eye is None or direction is None:
            continue
        observed.append({"frame": int(match.group("frame")), "eye": eye, "forward": direction,
            "angle_degrees": float(match.group("angle")), "offset_m": float(match.group("offset")),
            "elapsed_seconds": float(event.get("elapsed_seconds", 0.0))})
    if len(observed) < 3:
        errors.append("fixture_scenario does not contain enough camera_observed stdout samples")
        return {"telemetry": telemetry, "scenario": scenario, "observed": observed}
    base = observed[0]
    excursions = [sample for sample in observed[1:]
        if sample["angle_degrees"] >= 20.0 and sample["offset_m"] >= 1.5]
    if not excursions:
        errors.append("scenario never observed both >=20 degree camera turn and >=1.5 m camera offset")
        return {"telemetry": telemetry, "scenario": scenario, "observed": observed, "base": base}
    peak = max(excursions, key=lambda sample: (sample["angle_degrees"], sample["offset_m"]))
    returns = [sample for sample in observed if sample["frame"] > peak["frame"] and
        distance(sample["eye"], base["eye"]) <= 0.1 and
        direction_angle_degrees(sample["forward"], base["forward"]) <= 1.0]
    if not returns:
        errors.append("scenario has no camera_observed return sample matching the initial eye/direction")
    return {"telemetry": telemetry, "scenario": scenario, "observed": observed,
        "base": base, "peak": peak, "return": returns[0] if returns else None}


def scenario_gpu_endpoints(scenario: dict[str, Any], reports_by_frame: dict[int, dict[int, dict[str, Any]]],
    all_records: dict[int, list[dict[str, Any]]], errors: list[str], frame_slop: int) -> dict[str, Any]:
    if "base" not in scenario or "peak" not in scenario or not scenario.get("return"):
        return {"verified": False}
    observed = scenario["observed"]
    base, peak, returned = scenario["base"], scenario["peak"], scenario["return"]
    samples: list[dict[str, Any]] = []
    for frame, batch_reports in reports_by_frame.items():
        if not batch_reports:
            continue
        representative = batch_reports[min(batch_reports)]
        eye = representative.get("camera_eye")
        forward = representative.get("camera_forward")
        if not isinstance(eye, list) or not isinstance(forward, list):
            continue
        closest = min(observed, key=lambda row: abs(row["frame"] - frame))
        if abs(closest["frame"] - frame) > frame_slop:
            continue
        eye_error = distance(eye, closest["eye"])
        direction_error = direction_angle_degrees(forward, closest["forward"])
        if eye_error > 0.2 or direction_error > 1.0:
            continue
        samples.append({"frame": frame, "eye": eye, "forward": forward,
            "scenario_frame": closest["frame"], "eye_error_m": eye_error,
            "direction_error_degrees": direction_error,
            "records": all_records.get(frame, [])})
    baseline = [sample for sample in samples if sample["frame"] <= peak["frame"] and
        distance(sample["eye"], base["eye"]) <= 0.1 and
        direction_angle_degrees(sample["forward"], base["forward"]) <= 1.0]
    returned_samples = [sample for sample in samples if sample["frame"] > peak["frame"] and
        distance(sample["eye"], base["eye"]) <= 0.1 and
        direction_angle_degrees(sample["forward"], base["forward"]) <= 1.0]
    out_samples = [sample for sample in samples if sample["frame"] <= returned["frame"] and
        sample["frame"] >= base["frame"] and
        any(row["frame"] == sample["scenario_frame"] and row["angle_degrees"] >= 20.0 and row["offset_m"] >= 1.5
            for row in observed)]
    if not baseline:
        errors.append("native GPU JSON has no initial-camera A endpoint matching scenario stdout")
    if not returned_samples:
        errors.append("native GPU JSON has no returned-camera A endpoint matching scenario stdout")
    if not out_samples:
        errors.append("native GPU JSON has no camera-out sample matching the measured scenario excursion")
    comparison: dict[str, Any] = {"verified": bool(baseline and returned_samples and out_samples),
        "scenario_peak": {key: peak[key] for key in ("frame", "eye", "forward", "angle_degrees", "offset_m")},
        "baseline_frame": baseline[0]["frame"] if baseline else None,
        "camera_out_frame": out_samples[0]["frame"] if out_samples else None,
        "returned_frame": returned_samples[-1]["frame"] if returned_samples else None}
    if baseline and returned_samples:
        first = min(baseline, key=lambda sample: sample["frame"])
        last = max(returned_samples, key=lambda sample: sample["frame"])
        first_by_id = {record["light_id"]: record for record in first["records"]}
        last_by_id = {record["light_id"]: record for record in last["records"]}
        common = sorted(set(first_by_id) & set(last_by_id))
        if not common:
            errors.append("same-camera A endpoints have no common source light identities")
        coverage = []
        for light_id in common:
            a, b = first_by_id[light_id], last_by_id[light_id]
            if a["covered_pixels"] <= 0 or b["covered_pixels"] <= 0:
                errors.append(f"same-camera A endpoints have an empty common tile for source id {light_id}")
            coverage.append({"light_id": light_id, "face": a["face"],
                "first_covered_pixels": a["covered_pixels"], "return_covered_pixels": b["covered_pixels"],
                "hash_equal": a.get("tile_hash_fnv1a64") == b.get("tile_hash_fnv1a64")})
        comparison["same_camera_common_tiles"] = len(common)
        comparison["same_camera_tile_comparison"] = coverage
        comparison["tile_hashes_are_informational_only"] = True
    return comparison


def build_summary(args: argparse.Namespace) -> dict[str, Any]:
    errors: list[str] = []
    reports = load_native_reports(args.probe_dir, errors)
    summaries, stdout_records, _camera_stdout = parse_stdout(args.stdout, errors)
    records_by_frame, reports_by_frame = validate_stdout_and_join(
        reports, summaries, stdout_records, errors, args.probe_every)

    # Every batch from one primary camera frame must observe identical primary
    # depth and geometry-bit-0 hashes. The accumulator may change across batches.
    for frame, batch_reports in reports_by_frame.items():
        if len(batch_reports) < 2:
            continue
        depth_hashes = {report.get("depth_hash_fnv1a64") for report in batch_reports.values()}
        stencil_hashes = {report.get("geometry_stencil_bit_hash_fnv1a64") for report in batch_reports.values()}
        eye_vectors = [report.get("camera_eye") for report in batch_reports.values()]
        forward_vectors = [report.get("camera_forward") for report in batch_reports.values()]
        if len(depth_hashes) != 1:
            errors.append(f"frame {frame}: primary depth hash differs between atlas batches")
        if len(stencil_hashes) != 1:
            errors.append(f"frame {frame}: geometry stencil bit hash differs between atlas batches")
        if all(isinstance(vector, list) and len(vector) == 3 for vector in eye_vectors):
            if any(distance(eye_vectors[0], vector) > 0.01 for vector in eye_vectors[1:]):
                errors.append(f"frame {frame}: camera eye differs between atlas batches")
        if all(isinstance(vector, list) and len(vector) == 3 for vector in forward_vectors):
            if any(direction_angle_degrees(forward_vectors[0], vector) > 0.05 for vector in forward_vectors[1:]):
                errors.append(f"frame {frame}: camera direction differs between atlas batches")

    overrun_frames = [frame for frame, summary in summaries.items() if summary["records"] > CAPACITY]
    if not overrun_frames:
        errors.append(f"no frame exceeded the {CAPACITY}-record atlas capacity")

    scenario_result = None
    endpoints = {"verified": False}
    if args.scenario_report:
        scenario_result = parse_scenario(args.scenario_report, errors)
        if scenario_result:
            endpoints = scenario_gpu_endpoints(scenario_result, reports_by_frame,
                records_by_frame, errors, args.camera_frame_slop)
    else:
        errors.append("scenario report is required to prove camera-out/back identity and A-endpoint coverage")

    sampled_overrun_frames = [frame for frame in overrun_frames if frame in reports_by_frame]
    if not sampled_overrun_frames:
        errors.append("no overrun frame has native GPU JSON proof")
    point_face_evidence: dict[str, list[dict[str, Any]]] = {}
    for frame in sampled_overrun_frames:
        point_face_evidence[str(frame)] = verify_point_groups(frame,
            stdout_records.get(frame, []), records_by_frame.get(frame, []), errors)
    return {
        "schema": "xray.r4.local-shadow-batch-analysis.v1",
        "passed": not errors,
        "capacity": CAPACITY,
        "probe_every": args.probe_every,
        "native_gpu_json_files": len(reports),
        "stdout_frames": len(summaries),
        "overrun_frames": overrun_frames,
        "sampled_overrun_frames": sampled_overrun_frames,
        "verified_frames": sorted(reports_by_frame),
        "point_face_coverage_by_overrun_frame": point_face_evidence,
        "camera_endpoints": endpoints,
        "scenario_status": (scenario_result["scenario"].get("status") if scenario_result else None),
        "errors": errors,
        "limitations": [
            "Per-face zero shadowed-receiver pixels are reported as evidence; they do not fail individually.",
            "Same-camera tile hashes are informational because wind and NPC motion can change caster pixels.",
            "The native JSON readback gate does not compare against a Windows reference image.",
        ],
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--stdout", type=Path, required=True,
        help="engine stdout/log containing local-shadow frame and record lines")
    parser.add_argument("--probe-dir", type=Path, required=True,
        help="directory containing native local-shadow batch JSON readbacks")
    parser.add_argument("--scenario-report", type=Path,
        help="telemetry.json containing fixture_scenario camera_observed stdout events")
    parser.add_argument("--output", type=Path, required=True,
        help="write the machine-readable analysis summary here")
    parser.add_argument("--probe-every", type=int, default=20,
        help="engine-frame modulus used to emit asynchronous native GPU JSON reports")
    parser.add_argument("--camera-frame-slop", type=int, default=20,
        help="maximum frame distance from the actual scenario camera_observed record")
    args = parser.parse_args()
    if args.probe_every <= 0:
        parser.error("--probe-every must be positive")
    if args.camera_frame_slop < 0:
        parser.error("--camera-frame-slop must be non-negative")
    result = build_summary(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps({"passed": result["passed"], "summary": str(args.output),
        "native_reports": result["native_gpu_json_files"], "overrun_frames": result["overrun_frames"],
        "error_count": len(result["errors"])}))
    for error in result["errors"]:
        print(f"FAIL: {error}", file=sys.stderr)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
