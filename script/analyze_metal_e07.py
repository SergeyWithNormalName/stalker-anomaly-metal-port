#!/usr/bin/env python3
"""Analyze a diagnostic E07 NPC-count curve from captured Direct Metal runs.

This reports measurements and coverage only. It does not claim a speedup or
apply the release 60-FPS gate to a single diagnostic run.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import pathlib
import re
import statistics
import sys
import tempfile
from typing import Any

import analyze_metal_e01 as e01


COUNTS = (0, 1, 4, 8)
E07_FRAME_FIELDS = (
    "engine_frame", "slot", "rendered", "skin_ms", "bounds_ms", "skin_upload_ms",
    "shadow_cpu_ms", "local_shadow_cpu_ms", "sun_shadow_cpu_ms", "computed_vertices",
    "uploaded_bytes", "submitted_vertices", "shadow_vertices", "pose_reused",
    "cache_hit", "cache_miss", "skinned_draws", "shadow_draws",
)
E07_OBJECT_FIELDS = (
    "object_id", "frame", "view_frustum_visible", "bones", "asset_geometry_count",
    "submitted_to_gbuffer",
)
E07_METRICS = E07_FRAME_FIELDS[3:]


def read_json(path: pathlib.Path, label: str) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {label} {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{label} must contain a JSON object: {path}")
    return value


def integer(value: Any, label: str, *, minimum: int = 0) -> int:
    if isinstance(value, bool):
        raise ValueError(f"{label} must be an integer")
    try:
        numeric = float(value)
    except (TypeError, ValueError) as error:
        raise ValueError(f"{label} must be an integer") from error
    if not math.isfinite(numeric) or not numeric.is_integer() or numeric < minimum:
        raise ValueError(f"{label} must be an integer >= {minimum}")
    return int(numeric)


def csv_rows(path: pathlib.Path, required: tuple[str, ...], label: str) -> tuple[list[str], list[dict[str, str]]]:
    try:
        with path.open(newline="", encoding="utf-8-sig") as stream:
            reader = csv.DictReader(stream)
            fields = reader.fieldnames
            if fields is None:
                raise ValueError(f"{label} CSV has no header: {path}")
            missing = [field for field in required if field not in fields]
            if missing:
                raise ValueError(f"{label} CSV missing columns: {', '.join(missing)}")
            return fields, list(reader)
    except OSError as error:
        raise ValueError(f"cannot read {label} CSV {path}: {error}") from error


def bool01(value: Any, label: str) -> bool:
    if str(value).strip() not in ("0", "1"):
        raise ValueError(f"{label} must be 0 or 1")
    return str(value).strip() == "1"


def valid_e01_row(row: dict[str, str]) -> bool:
    status = (row.get("status") or "").strip().lower()
    return (status in ("", "ok", "pass", "passed", "success", "completed", "submitted") and
            e01.number(row.get("serial"), positive=True) is not None and
            e01.number(row.get("begin_ns"), positive=True) is not None)


def scenario_observations(scenario: dict[str, Any], requested_count: int,
                          settle_seconds: float) -> dict[str, Any]:
    observations: list[dict[str, int]] = []
    for event in scenario.get("events", []):
        if not isinstance(event, dict) or event.get("kind") != "actual_npc_observation":
            continue
        try:
            elapsed = float(event.get("elapsed_seconds"))
        except (TypeError, ValueError):
            continue
        if not math.isfinite(elapsed) or elapsed < settle_seconds:
            continue
        detail = str(event.get("detail", ""))
        values: dict[str, int] = {}
        for name in ("requested_spawn_count", "actual_online", "actual_alive", "alive_known"):
            prefix = name + "="
            for token in detail.split():
                if token.startswith(prefix):
                    values[name] = integer(token[len(prefix):], "observation " + name)
                    break
        if all(name in values for name in ("actual_online", "actual_alive")):
            observations.append(values)
    online_values = sorted({item["actual_online"] for item in observations})
    alive_values = sorted({item["actual_alive"] for item in observations})
    unstable = bool(observations) and (
        len(online_values) > 1 or len(alive_values) > 1 or
        any(item.get("requested_spawn_count", requested_count) != requested_count or
            item["actual_online"] != requested_count or item["actual_alive"] != requested_count
            for item in observations)
    )
    return {
        "samples": observations,
        "sample_count": len(observations),
        "semantics": "periodic Lua level.object_by_id online lookup and alive()/position() results for fixture-spawned IDs; not per-frame render visibility",
        "online_values": online_values,
        "alive_values": alive_values,
        "unstable_or_mismatched": unstable,
        "coverage": "observed" if observations else "missing",
    }


def provenance_signature(provenance: dict[str, Any], telemetry: dict[str, Any],
                         window_json: dict[str, Any], drawable: dict[str, Any] | None) -> dict[str, Any]:
    binary = provenance.get("binary") or {}
    profile = provenance.get("profile") or {}
    environment = provenance.get("effective_env") or {}
    if not binary.get("sha256"):
        raise ValueError("provenance binary.sha256 is missing")
    if not profile.get("manifest_sha256") or not profile.get("user_ltx_sha256"):
        raise ValueError("provenance profile hashes are missing")
    if not provenance.get("shader_receipt_set_sha256"):
        raise ValueError("provenance shader_receipt_set_sha256 is missing")
    save_hash = telemetry.get("save_sha256")
    if not save_hash:
        raise ValueError("telemetry save_sha256 is missing")
    fixture_contract = telemetry.get("fixture_contract") or {}
    if not isinstance(telemetry.get("camera"), dict) or not telemetry["camera"]:
        raise ValueError("telemetry camera settings are missing")
    if not isinstance(telemetry.get("weather"), dict) or not telemetry["weather"]:
        raise ValueError("telemetry weather settings are missing")
    if not isinstance(telemetry.get("options"), dict) or not telemetry["options"]:
        raise ValueError("telemetry renderer options are missing")
    settings = {
        "options": telemetry.get("options"),
        "tonemap_and_color_grading": telemetry.get("tonemap_and_color_grading"),
        "engine_extent": [telemetry.get("engine_width"), telemetry.get("engine_height")],
        "drawable": drawable,
        "fov": telemetry.get("engine_fov"),
        "aspect": telemetry.get("engine_aspect"),
        "logical_size": environment.get("XRAY_BENCHMARK_LOGICAL_SIZE"),
        "native_retina": environment.get("XRAY_METAL_NATIVE_RETINA"),
        "timing_seconds": environment.get("XRAY_METAL_E01_SECONDS"),
        "timing_warmup_seconds": environment.get("XRAY_PARITY_WARMUP_SECONDS"),
        "sample_seconds": window_json.get("sample_seconds"),
        "warmup_seconds": window_json.get("warmup_seconds"),
        "fixture": telemetry.get("fixture"),
        "level": fixture_contract.get("expected_level"),
        "scenario": fixture_contract.get("scenario"),
    }
    return {
        "backend": provenance.get("backend"),
        "platform": provenance.get("platform"),
        "binary_sha256": binary["sha256"],
        "save_sha256": save_hash,
        "profile_manifest_sha256": profile["manifest_sha256"],
        "user_ltx_sha256": profile["user_ltx_sha256"],
        "shader_receipt_set_sha256": provenance["shader_receipt_set_sha256"],
        "camera": telemetry.get("camera"),
        "weather": telemetry.get("weather"),
        "settings": settings,
    }


def read_spawned_ids(telemetry: dict[str, Any], requested_count: int) -> list[str]:
    scenario = telemetry.get("fixture_scenario") or {}
    spawned = scenario.get("spawned")
    if not isinstance(spawned, list):
        raise ValueError("telemetry.fixture_scenario.spawned is missing")
    ids: list[str] = []
    for item in spawned:
        if not isinstance(item, dict) or "id" not in item:
            raise ValueError("malformed spawned-object record in fixture telemetry")
        object_id = str(item["id"])
        if not object_id.isdigit():
            raise ValueError(f"spawned object id is not numeric: {object_id}")
        ids.append(object_id)
    if len(ids) != len(set(ids)):
        raise ValueError("duplicate spawned object ids in fixture telemetry")
    if len(ids) != requested_count:
        raise ValueError(f"fixture recorded {len(ids)} spawned object ids for requested count {requested_count}")
    return ids


def read_drawable_evidence(path: pathlib.Path) -> dict[str, Any] | None:
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        raise ValueError(f"cannot read engine log {path}: {error}") from error
    pattern = re.compile(
        r"METAL_DRAWABLE_SIZE\s+logical=(\d+)x(\d+)\s+backing=(\d+)x(\d+)\s+"
        r"render=(\d+)x(\d+)\s+policy=([^\s]+)"
    )
    records = []
    for line in lines:
        match = pattern.search(line)
        if match:
            logical_w, logical_h, backing_w, backing_h, render_w, render_h = map(int, match.groups()[:6])
            records.append({
                "logical": [logical_w, logical_h],
                "backing": [backing_w, backing_h],
                "render": [render_w, render_h],
                "policy": match.group(7),
            })
    distinct = {json.dumps(record, sort_keys=True) for record in records}
    if len(distinct) > 1:
        raise ValueError("engine log contains multiple distinct METAL_DRAWABLE_SIZE records")
    if not records:
        return None
    evidence = records[0]
    if evidence["render"] != [1920, 1080]:
        raise ValueError(f"E07 requires a 1920x1080 drawable, log reports {evidence['render']}")
    return evidence


def analyze_run(run_dir: pathlib.Path, settle_seconds: float) -> tuple[dict[str, Any], dict[str, Any]]:
    run_dir = run_dir.resolve()
    provenance = read_json(run_dir / "provenance.json", "provenance")
    telemetry = read_json(run_dir / "telemetry.json", "telemetry")
    diagnostic = read_json(run_dir / "e07-diagnostic.json", "E07 diagnostic marker")
    window_json = read_json(run_dir / "timing-window.json", "timing window")
    try:
        exit_status = int((run_dir / "engine_exit_status.txt").read_text(encoding="utf-8").strip())
    except (OSError, ValueError) as error:
        raise ValueError(f"missing or invalid engine_exit_status.txt in {run_dir}") from error
    if exit_status != 0:
        raise ValueError(f"run {run_dir.name} exited with status {exit_status}")
    if diagnostic.get("stage") != "E07" or diagnostic.get("admission") != "DIAGNOSTIC_ONLY_NOT_E00_ADMITTED":
        raise ValueError(f"run {run_dir.name} lacks the E07 diagnostic provenance marker")
    requested = integer(diagnostic.get("requested_spawn_count"), "requested_spawn_count")
    if requested not in COUNTS:
        raise ValueError(f"run {run_dir.name} has unsupported requested spawn count {requested}")
    if provenance.get("effective_env", {}).get("XRAY_METAL_E07_NPC_COUNT") != str(requested):
        raise ValueError(f"run {run_dir.name} E07 count marker disagrees with provenance")
    if telemetry.get("fixture") != "S2" or not telemetry.get("is_direct_metal"):
        raise ValueError(f"run {run_dir.name} is not a Direct Metal S2 run")
    scenario = telemetry.get("fixture_scenario") or {}
    if scenario.get("status") != "OK" or scenario.get("failures"):
        raise ValueError(f"run {run_dir.name} has fixture scenario failures")
    restore_events = [event for event in scenario.get("events", [])
                      if isinstance(event, dict) and event.get("kind") == "restore"]
    if not restore_events or restore_events[-1].get("detail") != "complete":
        raise ValueError(f"run {run_dir.name} did not complete fixture restoration")
    if diagnostic.get("device_clock") and "weather/game clock remains fixture-paused" not in diagnostic["device_clock"]:
        raise ValueError(f"run {run_dir.name} has an unexpected E07 clock contract")
    if not isinstance(diagnostic.get("device_clock"), str):
        raise ValueError(f"run {run_dir.name} is missing the E07 device-clock provenance fact")

    spawned_ids = read_spawned_ids(telemetry, requested)
    scenario_counts = scenario_observations(scenario, requested, settle_seconds)
    scenario_counts["spawned_record_count"] = len(spawned_ids)
    scenario_counts["requested_spawn_count"] = requested
    scenario_counts["unstable_or_mismatched"] = (
        scenario_counts["unstable_or_mismatched"] or len(spawned_ids) != requested
    )
    timing_path = run_dir / "frames.csv"
    presentation_path = run_dir / "frames.csv.presented.csv"
    e07_path = run_dir / "e07-frames.csv"
    objects_path = pathlib.Path(str(e07_path) + ".objects.csv")
    timing_window = e01.read_window(run_dir / "timing-window.json")
    drawable_evidence = read_drawable_evidence(run_dir / "engine.log")
    if timing_window is None:
        raise ValueError(f"run {run_dir.name} has no E01 timing window")
    _, timing_rows = csv_rows(timing_path, e01.REQUIRED_FIELDS, "E01 frame")
    frame_fields, e07_rows = csv_rows(e07_path, E07_FRAME_FIELDS, "E07 frame")
    _, object_rows = csv_rows(objects_path, E07_OBJECT_FIELDS, "E07 object")

    admitted_timing = []
    for row in timing_rows:
        if not valid_e01_row(row):
            continue
        try:
            frame = integer(row.get("engine_frame"), "E01 engine_frame")
        except ValueError:
            continue
        if timing_window["start_engine_frame"] <= frame <= timing_window["end_engine_frame"]:
            admitted_timing.append((row, frame, e01.number(row.get("begin_ns"), positive=True)))
    if not admitted_timing:
        raise ValueError(f"run {run_dir.name} E01 timing window has no accepted rows")
    window_start_ns = min(item[2] for item in admitted_timing if item[2] is not None)
    settle_start_ns = window_start_ns + int(settle_seconds * 1_000_000_000)
    trimmed_timing = [item for item in admitted_timing if item[2] is not None and item[2] >= settle_start_ns]
    if not trimmed_timing:
        raise ValueError(f"run {run_dir.name} has no E01 frames after {settle_seconds:g}s settle")
    final_window = {
        "start_engine_frame": min(item[1] for item in trimmed_timing),
        "end_engine_frame": timing_window["end_engine_frame"],
    }
    final_timing_rows = [item[0] for item in trimmed_timing]
    selected_keys: set[tuple[int, int]] = set()
    duplicate_timing_keys: set[tuple[int, int]] = set()
    for row in final_timing_rows:
        frame = integer(row.get("engine_frame"), "E01 engine_frame")
        slot = integer(row.get("slot"), "E01 slot")
        key = (frame, slot)
        if key in selected_keys:
            duplicate_timing_keys.add(key)
        selected_keys.add(key)
    if duplicate_timing_keys:
        raise ValueError(f"run {run_dir.name} has duplicate E01 engine_frame+slot keys")

    serial_presentation = e01.read_presentation(presentation_path)
    with tempfile.TemporaryDirectory(prefix="e07-e01-") as temp_name:
        filtered_path = pathlib.Path(temp_name) / "frames.csv"
        with filtered_path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=list(final_timing_rows[0].keys()))
            writer.writeheader()
            writer.writerows(final_timing_rows)
        timing_summary = e01.analyze(filtered_path, final_window, serial_presentation)
    timing_summary["timing_window"] = {
        **timing_summary.get("timing_window", {}),
        "original": timing_window,
        "settle_seconds": settle_seconds,
        "settle_start_begin_ns": settle_start_ns,
        "settle_trim_applied_by": "begin_ns relative to first accepted timing-window frame",
    }

    e07_by_key: dict[tuple[int, int], dict[str, str]] = {}
    rows_outside = 0
    for row in e07_rows:
        try:
            frame = integer(row.get("engine_frame"), "E07 engine_frame")
            slot = integer(row.get("slot"), "E07 slot")
        except ValueError:
            rows_outside += 1
            continue
        key = (frame, slot)
        if key not in selected_keys:
            rows_outside += 1
            continue
        if key in e07_by_key:
            raise ValueError(f"run {run_dir.name} has duplicate E07 engine_frame+slot key {key}")
        e07_by_key[key] = row
    if not e07_by_key:
        raise ValueError(f"run {run_dir.name} has no E07 frame rows matching the settled E01 window")
    for key, row in e07_by_key.items():
        if not bool01(row.get("rendered"), "E07 rendered"):
            raise ValueError(f"run {run_dir.name} E07 frame {key} reports rendered=0")
    missing_e07 = len(selected_keys - set(e07_by_key))

    e07_samples: dict[str, list[float]] = {field: [] for field in E07_METRICS}
    invalid_metric_values: dict[str, int] = {field: 0 for field in E07_METRICS}
    for row in e07_by_key.values():
        for field in E07_METRICS:
            value = e01.nonnegative_number(row.get(field))
            if value is None:
                invalid_metric_values[field] += 1
            else:
                e07_samples[field].append(value)
    e07_metrics = {name: e01.summarize(values) for name, values in e07_samples.items()}

    spawned = set(spawned_ids)
    object_groups: dict[tuple[int, str], dict[str, Any]] = {}
    background_ids: set[str] = set()
    background_rows = 0
    relevant_object_frames = {frame for frame, _slot in e07_by_key}
    for row in object_rows:
        try:
            frame = integer(row.get("frame"), "E07 object frame")
            object_id_value = integer(row.get("object_id"), "E07 object id")
        except ValueError:
            continue
        if frame not in relevant_object_frames:
            continue
        object_id = str(object_id_value)
        if object_id not in spawned:
            background_rows += 1
            background_ids.add(object_id)
            continue
        key = (frame, object_id)
        group = object_groups.setdefault(key, {
            "view_frustum_visible": False, "submitted_to_gbuffer": False,
            "bones": None, "asset_geometry_count": 0, "sample_rows": 0,
        })
        group["sample_rows"] += 1
        group["view_frustum_visible"] |= bool01(row.get("view_frustum_visible"), "view_frustum_visible")
        group["submitted_to_gbuffer"] |= bool01(row.get("submitted_to_gbuffer"), "submitted_to_gbuffer")
        bones = e01.nonnegative_number(row.get("bones"))
        if bones is not None:
            group["bones"] = max(group["bones"] or 0.0, bones)
        geometry_count = e01.nonnegative_number(row.get("asset_geometry_count"))
        if geometry_count is not None:
            group["asset_geometry_count"] = max(group["asset_geometry_count"], geometry_count)

    per_frame_objects: list[dict[str, Any]] = []
    visible_counts: list[float] = []
    submitted_counts: list[float] = []
    bone_totals: list[float] = []
    bone_known_counts: list[float] = []
    full_object_coverage = 0
    for frame in sorted(relevant_object_frames):
        groups = [value for (row_frame, _), value in object_groups.items() if row_frame == frame]
        sampled = sum(1 for (row_frame, _), _ in object_groups.items() if row_frame == frame)
        visible = sum(1 for item in groups if item["view_frustum_visible"])
        submitted = sum(1 for item in groups if item["submitted_to_gbuffer"])
        bones_known = [item["bones"] for item in groups if item["bones"] is not None]
        if requested == 0 or sampled >= requested:
            full_object_coverage += 1
        visible_counts.append(float(visible))
        submitted_counts.append(float(submitted))
        bone_totals.append(float(sum(bones_known)))
        bone_known_counts.append(float(len(bones_known)))
        per_frame_objects.append({
            "engine_frame": frame,
            "sampled_spawned_objects": sampled,
            "requested_spawn_count": requested,
            "frustum_visible_spawned_count": visible,
            "submitted_to_gbuffer_spawned_count": submitted,
            "bones_known_spawned_count": len(bones_known),
            "bones_total_unique_objects": sum(bones_known),
            "spawned_objects": [
                {"object_id": object_id, **value}
                for (row_frame, object_id), value in sorted(object_groups.items()) if row_frame == frame
            ],
        })
    coverage = {
        "e01_settled_frames": len(selected_keys),
        "e07_joined_frames": len(e07_by_key),
        "e07_missing_frame_rows": missing_e07,
        "e07_rows_outside_settled_join": rows_outside,
        "e07_frame_join_fraction": len(e07_by_key) / len(selected_keys) if selected_keys else None,
        "spawned_ids_in_telemetry": len(spawned_ids),
        "object_frame_count": len(per_frame_objects),
        "frames_with_all_requested_object_ids": full_object_coverage,
        "object_id_frame_coverage": (
            full_object_coverage / len(per_frame_objects) if per_frame_objects and requested > 0 else
            (1.0 if requested == 0 and per_frame_objects else None)
        ),
        "background_object_rows_separate": background_rows,
        "background_unique_object_ids_separate": len(background_ids),
        "drawable_evidence_status": "VERIFIED_1920x1080" if drawable_evidence else "OPEN_MISSING_LOG_EVIDENCE",
        "invalid_or_missing_metric_samples": invalid_metric_values,
    }
    visible_summary = summarize_range(visible_counts)
    submitted_summary = summarize_range(submitted_counts)
    bone_summary = e01.summarize(bone_totals)
    bone_known_summary = e01.summarize(bone_known_counts)
    bones_unknown_samples = sum(1 for groups in object_groups.values() if groups["bones"] is None)
    coverage["bones_known_object_frame_samples"] = sum(1 for groups in object_groups.values() if groups["bones"] is not None)
    coverage["bones_unknown_object_frame_samples"] = bones_unknown_samples
    incomplete = (missing_e07 > 0 or scenario_counts["coverage"] == "missing" or
                  drawable_evidence is None or
                  coverage["object_id_frame_coverage"] is None or
                  (requested > 0 and coverage["object_id_frame_coverage"] < 1.0) or
                  any(invalid_metric_values.values()) or bones_unknown_samples > 0)
    frustum_counts_match_request = bool(visible_counts) and not incomplete and all(
        value == requested for value in visible_counts
    )
    run_signature = provenance_signature(provenance, telemetry, window_json, drawable_evidence)

    result = {
        "schema": "direct-metal-e07-run/v1",
        "run_id": run_dir.name,
        "run_path": str(run_dir),
        "requested_spawn_count": requested,
        "spawned_object_ids": spawned_ids,
        "provenance_signature": run_signature,
        "timing": timing_summary,
        "e07_metrics": e07_metrics,
        "npc_per_frame": {
            "frustum_visible_spawned_count": visible_summary,
            "submitted_to_gbuffer_spawned_count": submitted_summary,
            "bones_known_object_count": bone_known_summary,
            "bones_total_unique_objects": bone_summary,
            "measurement_limit": "frustum flags are geometric potential visibility; submitted_to_gbuffer records encoded draws, not rasterized or pixel-visible output",
            "samples": per_frame_objects,
        },
        "scenario_object_observations": scenario_counts,
        "visibility_curve_status": "OPEN_VISIBILITY_NOT_PROVEN",
        "frustum_count_match_status": (
            "MATCHES_REQUESTED_IN_CAPTURED_SAMPLE" if frustum_counts_match_request else "DIFFERS_OR_COVERAGE_INCOMPLETE"
        ),
        "coverage": coverage,
        "coverage_status": "INCOMPLETE" if incomplete else "COMPLETE_FOR_RECORDED_FIELDS",
        "interpretation": "diagnostic curve sample; no before/after speedup or release PASS is inferred",
    }
    return result, run_signature


def aggregate_values(runs: list[dict[str, Any]], metric: str) -> dict[str, Any]:
    values = [run["e07_metrics"][metric]["p50"] for run in runs
              if run["e07_metrics"][metric]["p50"] is not None]
    p95s = [run["e07_metrics"][metric]["p95"] for run in runs
            if run["e07_metrics"][metric]["p95"] is not None]
    return {
        "run_count": len(runs),
        "run_p50_median": statistics.median(values) if values else None,
        "run_p95_median": statistics.median(p95s) if p95s else None,
        "frame_samples": sum(run["e07_metrics"][metric]["count"] for run in runs),
    }


def summarize_range(values: list[int | float]) -> dict[str, Any]:
    result = e01.summarize([float(value) for value in values])
    result["min"] = min(values) if values else None
    return result


def write_outputs(output: pathlib.Path, runs: list[dict[str, Any]], settle_seconds: float) -> dict[str, Any]:
    output.mkdir(parents=True, exist_ok=True)
    for run in runs:
        run_output = output / "runs" / f"{run['run_id']}.json"
        run_output.parent.mkdir(parents=True, exist_ok=True)
        run_output.write_text(json.dumps(run, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    curve: dict[str, Any] = {
        "schema": "direct-metal-e07-curve/v1",
        "settle_seconds": settle_seconds,
        "run_count": len(runs),
        "curve": {},
        "exact_visibility_gate": "OPEN_NOT_PROVEN_BY_FRUSTUM_OR_SUBMISSION_FLAGS",
        "interpretation": "diagnostic count curve only; no before/after speedup or release PASS is inferred",
    }
    for count in COUNTS:
        group = [run for run in runs if run["requested_spawn_count"] == count]
        curve["curve"][str(count)] = {
            "runs": [run["run_id"] for run in group],
            "skin_ms": aggregate_values(group, "skin_ms"),
            "bounds_ms": aggregate_values(group, "bounds_ms"),
            "skin_upload_ms": aggregate_values(group, "skin_upload_ms"),
            "shadow_cpu_ms": aggregate_values(group, "shadow_cpu_ms"),
            "local_shadow_cpu_ms": aggregate_values(group, "local_shadow_cpu_ms"),
            "sun_shadow_cpu_ms": aggregate_values(group, "sun_shadow_cpu_ms"),
            "computed_vertices": aggregate_values(group, "computed_vertices"),
            "uploaded_bytes": aggregate_values(group, "uploaded_bytes"),
            "submitted_vertices": aggregate_values(group, "submitted_vertices"),
            "frustum_visible_spawned_count": summarize_range([
                sample["frustum_visible_spawned_count"] for run in group
                for sample in run["npc_per_frame"]["samples"]
            ]),
            "submitted_to_gbuffer_spawned_count": summarize_range([
                sample["submitted_to_gbuffer_spawned_count"] for run in group
                for sample in run["npc_per_frame"]["samples"]
            ]),
            "visibility_curve_statuses": [run["visibility_curve_status"] for run in group],
            "frustum_count_match_statuses": [run["frustum_count_match_status"] for run in group],
            "coverage_statuses": [run["coverage_status"] for run in group],
            "scenario_count_instability_runs": [run["run_id"] for run in group
                if run["scenario_object_observations"]["unstable_or_mismatched"]],
        }
    (output / "curve.json").write_text(json.dumps(curve, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    lines = [
        "# E07 NPC count curve",
        "",
        "Diagnostic measurements after the configured settle interval. This is not a before/after speedup claim, and it does not establish the release 60-FPS gate.",
        "",
        "| Requested spawns | Runs | Skin CPU p50 / p95 (ms, median per run) | DynamicWorld shadow CPU p50 / p95 (ms) | Frustum count range | G-buffer submissions | Exact visibility | Coverage | Count instability |",
        "|---:|---:|---:|---:|---:|---:|---|---|---|",
    ]
    for count in COUNTS:
        group = [run for run in runs if run["requested_spawn_count"] == count]
        skin = curve["curve"][str(count)]["skin_ms"]
        shadow = curve["curve"][str(count)]["shadow_cpu_ms"]
        visible = curve["curve"][str(count)]["frustum_visible_spawned_count"]
        submitted = curve["curve"][str(count)]["submitted_to_gbuffer_spawned_count"]
        visible_range = "n/a" if visible["min"] is None else f"{visible['min']:.0f}–{visible['max']:.0f}"
        submitted_range = "n/a" if submitted["min"] is None else f"{submitted['min']:.0f}–{submitted['max']:.0f}"
        skin_text = "n/a" if skin["run_p50_median"] is None else f"{skin['run_p50_median']:.3f} / {skin['run_p95_median']:.3f}"
        shadow_text = "n/a" if shadow["run_p50_median"] is None else f"{shadow['run_p50_median']:.3f} / {shadow['run_p95_median']:.3f}"
        statuses = ", ".join(run["coverage_status"] for run in group) or "missing"
        visibility_status = "OPEN" if group else "OPEN / no runs"
        frustum_status = ", ".join(run["frustum_count_match_status"] for run in group) or "missing"
        unstable = ", ".join(curve["curve"][str(count)]["scenario_count_instability_runs"]) or "none observed"
        lines.append(f"| {count} | {len(group)} | {skin_text} | {shadow_text} | {visible_range} ({frustum_status}) | {submitted_range} | {visibility_status} | {statuses} | {unstable} |")
    lines += [
        "",
        "The horizontal count is requested fixture spawns. Frustum flags and G-buffer submissions count only IDs listed in fixture_scenario.spawned; background renderables are reported separately in each run JSON. Frustum flags show geometric potential visibility and G-buffer submission means an encoded draw, not rasterized or pixel-visible output. Exact E07 visibility remains OPEN for every count.",
        "",
    ]
    (output / "curve.md").write_text("\n".join(lines), encoding="utf-8")
    return curve


def analyze_curve(run_paths: list[pathlib.Path], output: pathlib.Path, settle_seconds: float) -> dict[str, Any]:
    if not math.isfinite(settle_seconds) or settle_seconds < 0:
        raise ValueError("settle seconds must be finite and non-negative")
    if len({path.resolve() for path in run_paths}) != len(run_paths):
        raise ValueError("duplicate --run paths are not allowed")
    results: list[dict[str, Any]] = []
    signatures: list[dict[str, Any]] = []
    for path in run_paths:
        result, signature = analyze_run(path, settle_seconds)
        results.append(result)
        signatures.append(signature)
    if not results:
        raise ValueError("at least one --run path is required")
    baseline = signatures[0]
    for run, signature in zip(results[1:], signatures[1:]):
        if signature != baseline:
            keys = [key for key in baseline if baseline.get(key) != signature.get(key)]
            raise ValueError(f"run {run['run_id']} mismatches curve provenance/settings: {', '.join(keys)}")
    present = {run["requested_spawn_count"] for run in results}
    missing = sorted(set(COUNTS) - present)
    if missing:
        raise ValueError("curve is missing requested spawn counts: " + ", ".join(map(str, missing)))
    curve = write_outputs(output, results, settle_seconds)
    curve["provenance_signature"] = baseline
    (output / "curve.json").write_text(json.dumps(curve, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return curve


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="append", type=pathlib.Path, required=True,
                        help="E07 run directory (repeat once per run)")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--settle-seconds", type=float, default=30.0,
                        help="discard this many seconds after timing-window.start (default 30)")
    args = parser.parse_args(argv)
    try:
        analyze_curve(args.run, args.output, args.settle_seconds)
    except (OSError, ValueError, csv.Error) as error:
        print(f"E07 analyzer error: {error}", file=sys.stderr)
        return 2
    print(f"E07 diagnostic curve written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
