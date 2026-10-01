#!/usr/bin/env python3
"""Summarize Direct Metal light-HOM diagnostics without claiming FPS savings."""
from __future__ import annotations

import argparse
import json
import math
import pathlib
import re
import statistics
from collections import Counter, defaultdict
from collections.abc import Mapping
from typing import Any


PREFIX = "METAL_LIGHT_HOM_"
FRAME_FIELDS = {
    "frame", "sample", "status", "geometry_verified", "suppressed", "groups", "tested",
    "hidden", "eligible_hidden", "excluded_hud", "excluded_volumetric", "invalid",
    "planned_batches", "planned_shadow_records", "unshadowed_records",
    "build_ms", "test_ms", "overhead_ms",
}
GROUP_FIELDS = {
    "frame", "sample", "parent", "state", "records", "eligible", "reason", "bounds_min", "bounds_max",
}
TILE_FIELDS = {
    "frame", "sample", "batch", "tile", "parent", "hidden", "eligible", "prep_cpu_ms",
    "shadow_cpu_ms", "static_rows", "static_draws", "dynamic_draws", "transform_bytes",
}
BATCH_FIELDS = {"frame", "sample", "batch", "records", "hidden_records", "eligible_hidden_records", "total_cpu_ms"}
ACCUM_FIELDS = {"frame", "sample", "batch", "records", "hidden_records", "eligible_hidden_records", "cpu_ms"}
COUNT_FIELDS = {
    "frame": "frame", "sample": "sample", "parent": "parent", "batch": "batch", "tile": "tile",
    "groups": "groups", "tested": "tested", "hidden": "hidden", "eligible_hidden": "eligible_hidden",
    "excluded_hud": "excluded_hud", "excluded_volumetric": "excluded_volumetric", "invalid": "invalid",
    "planned_batches": "planned_batches", "planned_shadow_records": "planned_shadow_records",
    "unshadowed_records": "unshadowed_records",
    "records": "records", "static_rows": "static_rows", "static_draws": "static_draws",
    "dynamic_draws": "dynamic_draws", "transform_bytes": "transform_bytes",
    "hidden_records": "hidden_records", "eligible_hidden_records": "eligible_hidden_records",
}
DURATION_FIELDS = {"build_ms", "test_ms", "overhead_ms", "prep_cpu_ms", "shadow_cpu_ms", "total_cpu_ms", "cpu_ms"}


def _integer(value: str, field: str, *, minimum: int = 0) -> int:
    if not re.fullmatch(r"-?(?:0|[1-9][0-9]*)", value):
        raise ValueError(f"{field} must be an integer, got {value!r}")
    number = int(value)
    if number < minimum:
        raise ValueError(f"{field} must be >= {minimum}, got {number}")
    return number


def _duration(value: str, field: str) -> float:
    try:
        number = float(value)
    except ValueError as exc:
        raise ValueError(f"{field} must be a finite nonnegative number, got {value!r}") from exc
    if not math.isfinite(number) or number < 0:
        raise ValueError(f"{field} must be a finite nonnegative number, got {value!r}")
    return number


def _bounds(value: str, field: str) -> tuple[float, float, float]:
    parts = value.split(",")
    if len(parts) != 3:
        raise ValueError(f"{field} must contain three comma-separated coordinates")
    try:
        result = tuple(float(part) for part in parts)
    except ValueError as exc:
        raise ValueError(f"{field} contains a nonnumeric coordinate") from exc
    if not all(math.isfinite(part) for part in result):
        raise ValueError(f"{field} coordinates must be finite")
    return result  # type: ignore[return-value]


def _parse_record(tag: str, payload: str, line_number: int) -> dict[str, Any]:
    field_sets = {
        "FRAME": FRAME_FIELDS, "GROUP": GROUP_FIELDS, "TILE": TILE_FIELDS,
        "BATCH": BATCH_FIELDS, "ACCUM": ACCUM_FIELDS,
    }
    if tag not in field_sets:
        raise ValueError(f"line {line_number}: unknown {PREFIX}{tag} record")
    values: dict[str, str] = {}
    for token in payload.split():
        if token.count("=") != 1:
            raise ValueError(f"line {line_number}: malformed field {token!r}")
        key, value = token.split("=", 1)
        if key in values:
            raise ValueError(f"line {line_number}: duplicate field {key!r}")
        values[key] = value
    expected = field_sets[tag]
    missing = sorted(expected - values.keys())
    if missing:
        raise ValueError(f"line {line_number}: {tag} fields missing: {missing}")

    row: dict[str, Any] = {}
    for key, value in values.items():
        if key in COUNT_FIELDS:
            minimum = -1 if tag == "ACCUM" and key == "batch" else 0
            row[key] = _integer(value, key, minimum=minimum)
        elif key in DURATION_FIELDS:
            row[key] = _duration(value, key)
        elif key in ("geometry_verified", "suppressed", "eligible", "hidden"):
            row[key] = _integer(value, key)
            if row[key] not in (0, 1):
                raise ValueError(f"line {line_number}: {key} must be 0 or 1")
        elif key in ("bounds_min", "bounds_max"):
            row[key] = _bounds(value, key)
        elif key == "eye":
            row[key] = _bounds(value, key)
        else:
            row[key] = value

    if tag == "FRAME":
        if row["status"] not in ("ready", "unavailable"):
            raise ValueError(f"line {line_number}: invalid frame status")
        if row["geometry_verified"] != 0:
            raise ValueError(f"line {line_number}: geometry_verified must remain 0")
        if row["suppressed"] != 0:
            raise ValueError(f"line {line_number}: suppressed must be 0; refusing incomplete diagnostics")
        if row["status"] == "ready" and not {"eye", "camera_hash"}.issubset(values):
            raise ValueError(f"line {line_number}: ready FRAME requires eye and camera_hash")
        if "camera_hash" in row and not re.fullmatch(r"[0-9a-fA-F]+", row["camera_hash"]):
            raise ValueError(f"line {line_number}: camera_hash must be hexadecimal")
    elif tag == "GROUP":
        if row["state"] not in ("hidden", "visible", "exempt", "invalid"):
            raise ValueError(f"line {line_number}: invalid group state")
        if row["reason"] not in ("none", "hud", "volumetric", "invalid_volume"):
            raise ValueError(f"line {line_number}: invalid group reason")
        if any(lo > hi for lo, hi in zip(row["bounds_min"], row["bounds_max"])):
            raise ValueError(f"line {line_number}: bounds_min exceeds bounds_max")
    elif tag == "TILE":
        if row["hidden"] not in (0, 1) or row["eligible"] not in (0, 1):
            raise ValueError(f"line {line_number}: TILE hidden and eligible must be 0 or 1")
    elif tag in ("BATCH", "ACCUM"):
        if not (row["eligible_hidden_records"] <= row["hidden_records"] <= row["records"]):
            raise ValueError(f"line {line_number}: hidden record counts exceed total records")
    return row


def _p50(values: list[float]) -> float | None:
    return statistics.median(values) if values else None


def _validate_window(value: Any) -> dict[str, int]:
    if not isinstance(value, Mapping):
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


def read_window(path: pathlib.Path | None) -> dict[str, int] | None:
    if path is None:
        return None
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read timing window JSON: {error}") from error
    return _validate_window(value)


def _no_evidence(source: str, reason: str, *, window: dict[str, int] | None = None,
                 excluded_records: dict[str, int] | None = None,
                 excluded_samples: int = 0, record_counts: dict[str, int] | None = None) -> dict[str, Any]:
    return {
        "schema": "direct-metal-light-hom-analysis/v1",
        "evidence_status": "no_evidence",
        "source": source,
        "reason": reason,
        "frame_count": 0,
        "ready_frames": 0,
        "unavailable_frames": 0,
        "record_counts": record_counts or {},
        "selection": {
            "window": window,
            "samples_excluded": excluded_samples,
            "records_excluded_by_type": excluded_records or {},
        },
        "diagnostic_coverage": {
            "frames_without_group_rows": 0,
            "missing_group_rows": 0,
            "ready_frames_without_tile_rows": 0,
            "ready_frames_without_batch_rows": 0,
            "full_eligible_batches_without_accum": 0,
            "missing_unshadowed_accum": 0,
            "unshadowed_record_count_mismatches": 0,
            "batches_with_incomplete_tile_rows": 0,
            "samples_with_missing_batch_rows": 0,
            "samples_with_planned_work_mismatch": 0,
            "mixed_batch_accumulations_unknown": 0,
            "mixed_unshadowed_accumulation_unknown": 0,
            "samples_with_required_record_kinds_present": 0,
            "missing_frame_records": 1,
        },
        "comparison": {
            "overhead_ms_p50": None,
            "optimistic_known_savings_ms_p50": None,
            "margin_ms_p50": None,
            "p50_sample_count": 0,
            "selected_sample_count": 0,
            "full_attribution_coverage_status": "no_evidence",
            "decision": "no_evidence",
        },
        "claims": {
            "geometry_correspondence": "pending",
            "gpu_savings": "unmeasured",
            "correctness": "pending",
            "controlled_ab": "pending",
            "fps_claim": "not_made",
            "automatic_acceptance": False,
        },
        "note": "Missing or unavailable logs are no evidence, never zero performance or zero savings.",
    }


def analyze_log(path: pathlib.Path, window: Mapping[str, int] | None = None) -> dict[str, Any]:
    selected_window = _validate_window(window) if window is not None else None
    if not path.is_file():
        return _no_evidence(str(path), "log file is missing", window=selected_window)
    # Game logs can contain legacy-encoded localized text; HOM fields are separately
    # validated as integers, finite numbers, enums, and hexadecimal provenance.
    content = path.read_text(encoding="utf-8", errors="replace")
    frames: dict[tuple[int, int], dict[str, Any]] = {}
    groups: dict[tuple[int, int, int], dict[str, Any]] = {}
    tiles: dict[tuple[int, int, int, int], dict[str, Any]] = {}
    batches: dict[tuple[int, int, int], dict[str, Any]] = {}
    accum: dict[tuple[int, int, int], dict[str, Any]] = {}
    line_counts: Counter[str] = Counter()
    tags = {"FRAME": frames, "GROUP": groups, "TILE": tiles, "BATCH": batches, "ACCUM": accum}
    last_sample = -1
    sample_frames: dict[int, int] = {}
    for line_number, line in enumerate(content.splitlines(), 1):
        marker = line.find(PREFIX)
        if marker < 0:
            continue
        item = line[marker + len(PREFIX):].split(None, 1)
        if not item:
            raise ValueError(f"line {line_number}: missing HOM record type")
        tag = item[0]
        payload = item[1] if len(item) > 1 else ""
        if tag == "LOAD":
            line_counts[tag] += 1
            continue
        row = _parse_record(tag, payload, line_number)
        if row["sample"] < last_sample:
            raise ValueError(f"line {line_number}: sample IDs must be monotonically nondecreasing")
        if row["sample"] > (1 << 64) - 1:
            raise ValueError(f"line {line_number}: sample exceeds uint64")
        prior_frame = sample_frames.setdefault(row["sample"], row["frame"])
        if prior_frame != row["frame"]:
            raise ValueError(f"line {line_number}: sample {row['sample']} is associated with multiple frames")
        last_sample = row["sample"]
        line_counts[tag] += 1
        if tag == "FRAME":
            key: Any = (row["frame"], row["sample"])
        elif tag == "GROUP":
            key = (row["frame"], row["sample"], row["parent"])
        elif tag == "TILE":
            key = (row["frame"], row["sample"], row["batch"], row["tile"])
        else:
            key = (row["frame"], row["sample"], row["batch"])
        if key in tags[tag]:
            raise ValueError(f"line {line_number}: duplicate {tag} key {key}")
        tags[tag][key] = row

    if not frames:
        if groups or tiles or batches or accum:
            raise ValueError("diagnostic row references a missing FRAME record")
        return _no_evidence(str(path), "no METAL_LIGHT_HOM_FRAME records", window=selected_window,
                            record_counts=dict(line_counts))

    excluded_samples = 0
    excluded_records: Counter[str] = Counter()
    if selected_window is not None:
        start = selected_window["start_engine_frame"]
        end = selected_window["end_engine_frame"]
        excluded_samples = sum(not start <= row["frame"] <= end for row in frames.values())
        for name, records in (("FRAME", frames), ("GROUP", groups), ("TILE", tiles),
                              ("BATCH", batches), ("ACCUM", accum)):
            for key, row in list(records.items()):
                if not start <= row["frame"] <= end:
                    excluded_records[name] += 1
                    del records[key]
        line_counts = Counter({"LOAD": line_counts["LOAD"]})
        for name, records in (("FRAME", frames), ("GROUP", groups), ("TILE", tiles),
                              ("BATCH", batches), ("ACCUM", accum)):
            line_counts[name] = len(records)
        if not frames:
            return _no_evidence(
                str(path), "no FRAME samples selected by timing window", window=selected_window,
                excluded_records=dict(excluded_records), excluded_samples=excluded_samples,
                record_counts=dict(line_counts),
            )

    rows_by_frame: dict[str, dict[tuple[int, int], list[dict[str, Any]]]] = {
        name: defaultdict(list) for name in ("GROUP", "TILE", "BATCH", "ACCUM")
    }
    for name, records in (("GROUP", groups.values()), ("TILE", tiles.values()),
                          ("BATCH", batches.values()), ("ACCUM", accum.values())):
        for row in records:
            join = (row["frame"], row["sample"])
            if join not in frames:
                raise ValueError(f"{name} row references stale or missing frame/sample {join}")
            rows_by_frame[name][join].append(row)

    for (frame, sample, _, _), tile in tiles.items():
        parent = groups.get((frame, sample, tile["parent"]))
        if parent is None:
            unavailable_baseline = (
                frames[(frame, sample)]["status"] == "unavailable" and
                tile["hidden"] == 0 and tile["eligible"] == 0
            )
            if unavailable_baseline:
                continue
            raise ValueError(f"TILE row references missing parent group {(frame, sample, tile['parent'])}")
        if tile["eligible"] != parent["eligible"]:
            raise ValueError(f"TILE eligible flag disagrees with parent {(frame, sample, tile['parent'])}")
        if tile["hidden"] != int(parent["state"] == "hidden"):
            raise ValueError(f"TILE hidden flag disagrees with parent {(frame, sample, tile['parent'])}")
    for (frame, sample, batch), row in accum.items():
        if batch >= 0:
            batch_row = batches.get((frame, sample, batch))
            if batch_row is None:
                continue
            for field in ("records", "hidden_records", "eligible_hidden_records"):
                if row[field] != batch_row[field]:
                    raise ValueError(f"ACCUM {field} does not match BATCH for {(frame, sample, batch)}")

    # Metadata is authoritative for expected groups. Partial GROUP detail is counted as
    # missing coverage; duplicate, stale, and contradictory joins remain hard errors.
    ready_rows = [row for row in frames.values() if row["status"] == "ready"]
    missing_group_rows = 0
    frames_without_groups = 0
    for frame_sample, meta in frames.items():
        observed = rows_by_frame["GROUP"].get(frame_sample, [])
        expected = meta["groups"]
        if len(observed) > expected:
            raise ValueError(f"GROUP rows exceed frame/sample {frame_sample} groups count")
        if not observed and expected:
            frames_without_groups += 1
        missing_group_rows += expected - len(observed)
        if len(observed) == expected:
            derived = {
                "groups": len(observed),
                "tested": sum(row["state"] in ("hidden", "visible") for row in observed),
                "hidden": sum(row["state"] == "hidden" for row in observed),
                "eligible_hidden": sum(row["state"] == "hidden" and row["eligible"] == 1 for row in observed),
                "excluded_hud": sum(row["state"] == "exempt" and row["reason"] == "hud" for row in observed),
                "excluded_volumetric": sum(row["state"] == "exempt" and row["reason"] == "volumetric" for row in observed),
                "invalid": sum(row["state"] == "invalid" for row in observed),
            }
            for field, actual in derived.items():
                if meta[field] != actual:
                    raise ValueError(f"frame/sample {frame_sample} {field}={meta[field]} disagrees with GROUP rows ({actual})")

    frame_costs: dict[tuple[int, int], dict[str, Any]] = {}
    overheads: list[float] = []
    all_ready_overheads: list[float] = []
    estimated_savings: list[float] = []
    complete_tile_component_samples: list[float] = []
    positive_tile_component_samples: list[float] = []
    missing_full_accum = 0
    missing_unshadowed_accum = 0
    unshadowed_record_mismatches = 0
    batches_with_incomplete_tile_rows = 0
    samples_with_missing_batch_rows = 0
    samples_with_planned_work_mismatch = 0
    mixed_batch_accumulations = 0
    mixed_unshadowed_accum = 0
    ready_without_tiles = 0
    ready_without_batches = 0
    complete_savings_frames = 0
    for frame_sample, meta in frames.items():
        if meta["status"] != "ready":
            continue
        all_ready_overheads.append(meta["overhead_ms"])
        frame_tiles = rows_by_frame["TILE"].get(frame_sample, [])
        frame_batches = rows_by_frame["BATCH"].get(frame_sample, [])
        if meta["planned_shadow_records"] > 0 and not frame_tiles:
            ready_without_tiles += 1
        if meta["planned_batches"] > 0 and not frame_batches:
            ready_without_batches += 1
        tile_ms = sum(row["prep_cpu_ms"] + row["shadow_cpu_ms"]
                      for row in frame_tiles if row["hidden"] == 1 and row["eligible"] == 1)
        accum_ms = 0.0
        frame_missing_accum = 0
        frame_missing_full_accum = 0
        frame_mixed_batch_accum = 0
        frame_incomplete_tiles = 0
        frame_missing_batches = 0
        observed_batches = {row["batch"] for row in frame_batches}
        frame_tile_groups: dict[int, list[dict[str, Any]]] = defaultdict(list)
        for tile_row in frame_tiles:
            frame_tile_groups[tile_row["batch"]].append(tile_row)
        # A shadow batch is fully covered only when every ordinal is present and its
        # summary counts agree with the per-tile facts.
        tile_batches_complete: dict[int, bool] = {}
        for batch_row in frame_batches:
            batch_id = batch_row["batch"]
            expected = batch_row["records"]
            observed_tiles = frame_tile_groups.get(batch_id, [])
            if any(tile_row["tile"] >= expected for tile_row in observed_tiles):
                raise ValueError(f"TILE ordinal is outside BATCH record range for {(frame_sample, batch_id)}")
            ordinals = {tile_row["tile"] for tile_row in observed_tiles}
            complete_tiles = ordinals == set(range(expected))
            tile_batches_complete[batch_id] = complete_tiles
            if not complete_tiles:
                frame_incomplete_tiles += 1
            else:
                derived_hidden = sum(tile_row["hidden"] == 1 for tile_row in observed_tiles)
                derived_eligible_hidden = sum(
                    tile_row["hidden"] == 1 and tile_row["eligible"] == 1
                    for tile_row in observed_tiles
                )
                if batch_row["hidden_records"] != derived_hidden:
                    raise ValueError(f"BATCH hidden_records disagrees with TILE rows for {(frame_sample, batch_id)}")
                if batch_row["eligible_hidden_records"] != derived_eligible_hidden:
                    raise ValueError(f"BATCH eligible_hidden_records disagrees with TILE rows for {(frame_sample, batch_id)}")
            full = (expected > 0 and
                    batch_row["hidden_records"] == batch_row["records"] and
                    batch_row["eligible_hidden_records"] == batch_row["records"])
            accum_row = accum.get((frame_sample[0], frame_sample[1], batch_row["batch"]))
            if expected > 0 and accum_row is None:
                frame_missing_accum += 1
            if full:
                if accum_row is None:
                    frame_missing_full_accum += 1
                else:
                    if complete_tiles:
                        accum_ms += accum_row["cpu_ms"]
            elif batch_row["eligible_hidden_records"] > 0:
                # Inclusive accumulation for a mixed batch cannot be apportioned.
                frame_mixed_batch_accum += 1
        for batch_id in frame_tile_groups:
            if batch_id not in observed_batches:
                frame_missing_batches += 1
        if len(frame_batches) > meta["planned_batches"]:
            raise ValueError(f"BATCH rows exceed planned_batches for {frame_sample}")
        if sum(row["records"] for row in frame_batches) > meta["planned_shadow_records"]:
            raise ValueError(f"BATCH rows exceed planned_shadow_records for {frame_sample}")
        plan_mismatch = (
            len(frame_batches) != meta["planned_batches"] or
            sum(row["records"] for row in frame_batches) != meta["planned_shadow_records"]
        )
        if plan_mismatch:
            samples_with_planned_work_mismatch += 1
        tile_component_complete = bool(
            not plan_mismatch and frame_missing_batches == 0 and
            len(frame_batches) == meta["planned_batches"] and
            sum(tile_batches_complete.get(row["batch"], False) for row in frame_batches) == len(frame_batches)
        )
        if tile_component_complete:
            complete_tile_component_samples.append(tile_ms)
            if any(row["hidden"] == 1 and row["eligible"] == 1 for row in frame_tiles):
                positive_tile_component_samples.append(tile_ms)

        unshadowed = accum.get((frame_sample[0], frame_sample[1], -1))
        if meta["unshadowed_records"] > 0 and unshadowed is None:
            missing_unshadowed_accum += 1
        unshadowed_ms = 0.0
        mixed_unshadowed = False
        if unshadowed is not None:
            if unshadowed["records"] != meta["unshadowed_records"]:
                unshadowed_record_mismatches += 1
            all_hidden_eligible = (
                unshadowed["records"] > 0 and
                unshadowed["hidden_records"] == unshadowed["records"] and
                unshadowed["eligible_hidden_records"] == unshadowed["records"]
            )
            if all_hidden_eligible:
                unshadowed_ms = unshadowed["cpu_ms"]
            elif unshadowed["eligible_hidden_records"] > 0:
                mixed_unshadowed = True
                mixed_unshadowed_accum += 1
        mixed_batch_accumulations += frame_mixed_batch_accum
        missing_full_accum += frame_missing_full_accum
        group_rows_complete = len(rows_by_frame["GROUP"].get(frame_sample, [])) == meta["groups"]
        no_planned_work = (
            meta["planned_batches"] == 0 and meta["planned_shadow_records"] == 0 and
            meta["unshadowed_records"] == 0
        )
        work_records_present = (
            (meta["planned_batches"] > 0 and len(frame_batches) == meta["planned_batches"] and
             sum(tile_batches_complete.get(row["batch"], False) for row in frame_batches) == len(frame_batches)) or
            (meta["planned_batches"] == 0 and meta["planned_shadow_records"] == 0)
        )
        unshadowed_present = (
            meta["unshadowed_records"] == 0 or
            (unshadowed is not None and unshadowed["records"] == meta["unshadowed_records"])
        )
        unshadowed_count_matches = (
            unshadowed is None or unshadowed["records"] == meta["unshadowed_records"]
        )
        record_coverage_complete = bool(
            group_rows_complete and work_records_present and unshadowed_present and
            unshadowed_count_matches and frame_missing_batches == 0 and frame_missing_accum == 0 and
            not plan_mismatch and
            (no_planned_work or frame_batches or unshadowed is not None)
        )
        is_complete = bool(
            record_coverage_complete and frame_missing_full_accum == 0 and
            frame_mixed_batch_accum == 0 and not mixed_unshadowed
        )
        if frame_incomplete_tiles:
            batches_with_incomplete_tile_rows += frame_incomplete_tiles
        if frame_missing_batches:
            samples_with_missing_batch_rows += 1
        frame_costs[frame_sample] = {
            "tile_prep_shadow_ms": tile_ms,
            "full_batch_accum_ms": accum_ms,
            "fully_hidden_unshadowed_accum_ms": unshadowed_ms,
            "estimated_upper_bound_ms": tile_ms + accum_ms + unshadowed_ms if is_complete else None,
            "record_coverage_complete": record_coverage_complete,
            "full_attribution_complete": is_complete,
            "planned_batches": meta["planned_batches"],
            "planned_shadow_records": meta["planned_shadow_records"],
            "unshadowed_records": meta["unshadowed_records"],
            "eligible_hidden_tile_rows": sum(
                row["hidden"] == 1 and row["eligible"] == 1 for row in frame_tiles
            ),
            "mixed_unshadowed_accumulation_unknown": mixed_unshadowed,
        }
        if is_complete:
            complete_savings_frames += 1
            overheads.append(meta["overhead_ms"])
            estimated_savings.append(tile_ms + accum_ms + unshadowed_ms)

    comparison = {
            "overhead_ms_p50": _p50(overheads),
            "optimistic_known_savings_ms_p50": _p50(estimated_savings),
            "margin_ms_p50": None,
            "p50_sample_count": len(estimated_savings),
            "selected_sample_count": len(frames),
            "full_attribution_coverage_status": (
                "no_evidence" if not ready_rows else
                "complete" if len(estimated_savings) == len(frames) else "partial"
            ),
            "population": "full_attribution_samples_only",
            "selection_bias_warning": (
                "samples without complete cost attribution, including mixed accumulation, are excluded; this subset may not represent the selected window"
                if len(estimated_savings) < len(frames) else None
            ),
        "decision": "diagnostic_only_no_automatic_acceptance",
        "basis": "eligible-hidden tile prep+shadow CPU plus accumulation from fully eligible-hidden batches only",
        "batch_total_cpu_ms_used": False,
        "mixed_batch_accumulation": "unknown_and_excluded",
    }
    if (len(estimated_savings) == len(frames) and comparison["overhead_ms_p50"] is not None and
            comparison["optimistic_known_savings_ms_p50"] is not None):
        comparison["margin_ms_p50"] = (
            comparison["optimistic_known_savings_ms_p50"] - comparison["overhead_ms_p50"]
        )
    return {
        "schema": "direct-metal-light-hom-analysis/v1",
        "evidence_status": "diagnostic_data_present" if ready_rows else "no_evidence",
        "source": str(path),
        "frame_count": len(frames),
        "sample_count": len(frames),
        "engine_frame_count": len({frame for frame, _ in frames}),
        "ready_frames": len(ready_rows),
        "unavailable_frames": sum(row["status"] == "unavailable" for row in frames.values()),
        "record_counts": dict(line_counts),
        "selection": {
            "window": selected_window,
            "samples_excluded": excluded_samples,
            "records_excluded_by_type": dict(excluded_records),
        },
        "diagnostic_coverage": {
            "frames_without_group_rows": frames_without_groups,
            "missing_group_rows": missing_group_rows,
            "ready_frames_without_tile_rows": ready_without_tiles,
            "ready_frames_without_batch_rows": ready_without_batches,
            "full_eligible_batches_without_accum": missing_full_accum,
            "missing_unshadowed_accum": missing_unshadowed_accum,
            "unshadowed_record_count_mismatches": unshadowed_record_mismatches,
            "batches_with_incomplete_tile_rows": batches_with_incomplete_tile_rows,
            "samples_with_missing_batch_rows": samples_with_missing_batch_rows,
            "samples_with_planned_work_mismatch": samples_with_planned_work_mismatch,
            "mixed_batch_accumulations_unknown": mixed_batch_accumulations,
            "mixed_unshadowed_accumulation_unknown": mixed_unshadowed_accum,
            "samples_with_required_record_kinds_present": sum(
                values["record_coverage_complete"] for values in frame_costs.values()
            ),
            "samples_with_full_attribution": complete_savings_frames,
        },
        "eligible_hidden_tile_cpu_component": {
            "prep_plus_shadow_ms_p50": _p50(complete_tile_component_samples),
            "sample_count": len(complete_tile_component_samples),
            "eligible_hidden_sample_p50_ms": _p50(positive_tile_component_samples),
            "eligible_hidden_sample_count": len(positive_tile_component_samples),
            "semantics": "measured eligible-hidden TILE subinterval only; not an end-to-end savings estimate",
        },
        "known_tile_work_diagnostic": {
            "eligible_hidden_tile_prep_shadow_ms_p50": _p50(complete_tile_component_samples),
            "tile_coverage_sample_count": len(complete_tile_component_samples),
            "eligible_hidden_sample_p50_ms": _p50(positive_tile_component_samples),
            "eligible_hidden_sample_count": len(positive_tile_component_samples),
            "overall_ready_overhead_ms_p50": _p50(all_ready_overheads),
            "overall_ready_sample_count": len(all_ready_overheads),
            "known_only_margin_ms_p50": (
                _p50(complete_tile_component_samples) - _p50(all_ready_overheads)
                if complete_tile_component_samples and all_ready_overheads else None
            ),
            "interpretation": "CPU diagnostic for known TILE subinterval work versus all selected ready HOM overhead; not total savings, end-to-end speedup, or an ROI decision",
        },
        "sample_known_savings_ms": {
            f"{frame}:{sample}": values for (frame, sample), values in sorted(frame_costs.items())
        },
        "comparison": comparison,
        "claims": {
            "geometry_correspondence": "pending; geometry_verified=0 by contract",
            "gpu_savings": "unmeasured",
            "correctness": "pending",
            "controlled_ab": "pending; shared/encoder effects require A/B",
            "fps_claim": "not_made",
            "automatic_acceptance": False,
        },
        "note": "CPU counters are diagnostic only. Fully attributable savings excludes mixed accumulation samples; known eligible-hidden TILE subinterval work is reported separately across complete planned TILE/BATCH coverage. overhead_ms includes HOM query/group work but excludes later TILE/BATCH/ACCUM log costs, so it is not the full instrumentation cost. Shared/encoder effects require controlled A/B; no GPU, correctness, visual, or FPS conclusion is inferred.",
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=pathlib.Path, help="logfile containing METAL_LIGHT_HOM records")
    parser.add_argument("--output", type=pathlib.Path, help="optional JSON output path")
    parser.add_argument("--window", type=pathlib.Path,
                        help="optional JSON with inclusive start_engine_frame/end_engine_frame bounds")
    args = parser.parse_args()
    try:
        result = analyze_log(args.log, window=read_window(args.window))
    except (OSError, UnicodeError, ValueError) as exc:
        parser.error(str(exc))
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output:
        args.output.write_text(rendered, encoding="utf-8")
    print(rendered, end="")


if __name__ == "__main__":
    main()
