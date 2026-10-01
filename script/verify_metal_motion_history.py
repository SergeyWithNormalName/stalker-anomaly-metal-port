#!/usr/bin/env python3
"""Fail-closed acceptance gate for Direct Metal motion/history diagnostics."""

from __future__ import annotations

import argparse
import json
import math
import re
import sys
from pathlib import Path
from typing import Any


MOTION_FAMILIES = ("LMH", "terrain", "DefVertex", "DefObjectsLod")
MOTION_FAMILY_KEYS = {
    "lmh": "LMH",
    "terrain": "terrain",
    "defvertex": "DefVertex",
    "defobjectslod": "DefObjectsLod",
}
TEMPORAL_PHASES = ("clear", "position-copy", "output")
LIFECYCLE_MARKER = "METAL_TEMPORAL_LIFECYCLE_PROBE"
AO_HISTORY_MARKER = "METAL_AO_IL_HISTORY"
TEMPORAL_STAGE_MARKER = "METAL_TEMPORAL_STAGE"
KEY_VALUE = re.compile(r'([A-Za-z_][A-Za-z0-9_]*)=("[^"\n]*"|[^\s]+)')


def normalized(value: Any) -> str:
    return re.sub(r"[^a-z0-9]", "", str(value).lower())


def parse_fields(line: str, marker: str) -> dict[str, str] | None:
    position = line.find(marker)
    if position < 0:
        return None
    tail = line[position + len(marker) :]
    result: dict[str, str] = {}
    for key, value in KEY_VALUE.findall(tail):
        if len(value) >= 2 and value[0] == value[-1] == '"':
            value = value[1:-1]
        result[key] = value
    return result


def integer(value: Any, default: int | None = None) -> int | None:
    try:
        return int(value)
    except (TypeError, ValueError):
        return default


def finite_number(value: Any) -> float | None:
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def report_frame(report: dict[str, Any]) -> int | None:
    return integer(report.get("engine_frame", report.get("frame")))


def motion_family(report: dict[str, Any]) -> str | None:
    label = normalized(report.get("family", ""))
    for token, family in MOTION_FAMILY_KEYS.items():
        if token in label:
            return family
    return None


def load_reports(directory: Path, failures: list[str], no_coverage: list[str]) -> list[tuple[Path, dict[str, Any]]]:
    if not directory.is_dir():
        no_coverage.append(f"reports directory is missing: {directory}")
        return []

    paths = sorted(set(directory.rglob("r4_static_motion*.json")) | set(directory.rglob("r4_motion_*.json")))
    paths += sorted(directory.rglob("r4_temporal_*.json"))
    if not paths:
        no_coverage.append(f"no motion or temporal readback JSON reports found in {directory}")
        return []

    reports: list[tuple[Path, dict[str, Any]]] = []
    for path in paths:
        try:
            value = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, UnicodeError, json.JSONDecodeError) as error:
            failures.append(f"cannot parse report {path}: {error}")
            continue
        if not isinstance(value, dict):
            failures.append(f"report root must be an object: {path}")
            continue
        reports.append((path, value))
    return reports


def motion_summary(reports: list[tuple[Path, dict[str, Any]]], failures: list[str], no_coverage: list[str],
    *, require_moving_coverage: bool = True, moving_after_frame: int = 0) -> dict[str, Any]:
    by_family: dict[str, list[tuple[Path, dict[str, Any]]]] = {family: [] for family in MOTION_FAMILIES}
    for path, report in reports:
        schema = str(report.get("schema", ""))
        if schema != "r4_static_motion_validation_v1" and "motion" not in path.name:
            continue
        family = motion_family(report)
        if family is None:
            failures.append(f"motion report has an unknown family: {path}")
            continue
        by_family[family].append((path, report))
        if report.get("result") == "FAIL":
            failures.append(f"motion report failed: {path}")

    summary: dict[str, Any] = {}
    for family in MOTION_FAMILIES:
        entries = by_family[family]
        moving: list[dict[str, Any]] = []
        resets: list[dict[str, Any]] = []
        failures_for_family: list[str] = []
        for path, report in entries:
            result = str(report.get("result", "")).upper()
            frame = report_frame(report)
            valid_pixels = integer(report.get("valid_pixel_count"), 0) or 0
            moving_pixels = integer(report.get("moving_pixels", report.get("expected_moving_pixel_count")), 0) or 0
            max_error = finite_number(report.get("max_error_pixels"))
            record = {
                "file": path.name,
                "engine_frame": frame,
                "valid_pixel_count": valid_pixels,
                "moving_pixel_count": moving_pixels,
                "max_pixel_error": max_error,
                "result": result,
            }
            if result == "FAIL":
                failures_for_family.append(path.name)
            if result == "PASS" and moving_pixels > 0 and valid_pixels > 0 and frame is not None and frame > moving_after_frame:
                moving.append(record)
            if result == "PASS" and report.get("reset") is True and valid_pixels > 0:
                resets.append(record)
        if require_moving_coverage and not moving:
            no_coverage.append(f"{family}: no PASS motion report after frame {moving_after_frame} has valid pixels and moving_pixels > 0")
        if not resets:
            no_coverage.append(f"{family}: no PASS reset report has valid pixels")
        family_errors = [record["max_pixel_error"] for record in moving + resets if record["max_pixel_error"] is not None]
        summary[family] = {
            "report_count": len(entries),
            "failed_reports": failures_for_family,
            "moving_reports": moving,
            "reset_reports": resets,
            "max_pixel_error": max(family_errors) if family_errors else None,
        }
    return summary


def temporal_summary(reports: list[tuple[Path, dict[str, Any]]], failures: list[str], no_coverage: list[str]) -> dict[str, Any]:
    by_phase: dict[str, list[tuple[Path, dict[str, Any]]]] = {phase: [] for phase in TEMPORAL_PHASES}
    for path, report in reports:
        if report.get("schema") != "r4_temporal_texture_validation_v1":
            continue
        phase = str(report.get("phase", ""))
        if phase not in by_phase:
            continue
        by_phase[phase].append((path, report))
        if str(report.get("result", "")).upper() == "FAIL":
            failures.append(f"temporal texture report failed: {path}")

    summary: dict[str, Any] = {}
    for phase in TEMPORAL_PHASES:
        entries = by_phase[phase]
        if not entries:
            no_coverage.append(f"no temporal texture report for phase {phase}")
            summary[phase] = {"report_count": 0, "frames": []}
            continue
        output_presence = {"ao_alpha": False, "il_rgb": False}
        frame_records: list[dict[str, Any]] = []
        for path, report in entries:
            result = str(report.get("result", "")).upper()
            if result != "PASS":
                failures.append(f"temporal texture report result is not PASS ({result or 'missing'}): {path}")
            textures = report.get("textures", [])
            if not isinstance(textures, list):
                textures = []
            texture_summary: list[dict[str, Any]] = []
            for texture in textures:
                if not isinstance(texture, dict):
                    continue
                texture_summary.append({
                    "label": texture.get("label"),
                    "width": integer(texture.get("width")),
                    "height": integer(texture.get("height")),
                    "nonzero_rgb_pixel_count": integer(texture.get("nonzero_rgb_pixel_count"), 0),
                    "nonzero_alpha_pixel_count": integer(texture.get("nonzero_alpha_pixel_count"), 0),
                    "nonzero_alpha_channel_count": integer(texture.get("nonzero_alpha_channel_count"), 0),
                    "non_finite_rgb_channel_count": integer(texture.get("non_finite_rgb_channel_count"), 0),
                    "finite_alpha_channel_count": integer(texture.get("finite_alpha_channel_count"), 0),
                    "non_finite_alpha_channel_count": integer(texture.get("non_finite_alpha_channel_count"), 0),
                    "finite_rgba_channel_count": integer(texture.get("finite_rgba_channel_count"), 0),
                    "non_finite_rgba_channel_count": integer(texture.get("non_finite_rgba_channel_count"), 0),
                    "rgb_min": finite_number(texture.get("rgb_min")),
                    "rgb_max": finite_number(texture.get("rgb_max")),
                    "alpha_min": finite_number(texture.get("alpha_min")),
                    "alpha_max": finite_number(texture.get("alpha_max")),
                    "all_rgba_zero": texture.get("all_rgba_zero"),
                })

            if phase == "clear" and result == "PASS":
                if report.get("expect_zero") is not True:
                    failures.append(f"clear report did not enable the zero witness: {path}")
                if not textures or any(not isinstance(item, dict) or item.get("all_rgba_zero") is not True for item in textures):
                    failures.append(f"clear witness contains nonzero RGBA: {path}")
            elif phase == "position-copy" and result == "PASS":
                if report.get("expect_equal_pair") is not True or report.get("pair_exact") is not True:
                    failures.append(f"position-copy witness is not byte-exact: {path}")
            elif phase == "output":
                labels = {normalized(item.get("label", "")): item for item in textures if isinstance(item, dict)}
                missing = [name for name in ("ao", "il") if name not in labels]
                if missing:
                    failures.append(f"output report lacks AO/IL texture(s) {missing}: {path}")
                for name in ("ao", "il"):
                    texture = labels.get(name)
                    if texture is None:
                        continue
                    if (integer(texture.get("finite_rgba_channel_count"), 0) or 0) <= 0 or \
                        (integer(texture.get("non_finite_rgba_channel_count"), 0) or 0) != 0:
                        failures.append(f"output {name.upper()} has non-finite or absent RGBA data: {path}")
                if result == "PASS":
                    ao = labels.get("ao")
                    il = labels.get("il")
                    if ao is not None and (integer(ao.get("nonzero_alpha_pixel_count"), 0) or 0) > 0:
                        output_presence["ao_alpha"] = True
                    if il is not None and (integer(il.get("nonzero_rgb_pixel_count"), 0) or 0) > 0:
                        output_presence["il_rgb"] = True
            frame_records.append({
                "file": path.name,
                "engine_frame": report_frame(report),
                "result": str(report.get("result", "")).upper(),
                "pair_exact": report.get("pair_exact"),
                "textures": texture_summary,
            })
        summary[phase] = {"report_count": len(entries), "frames": frame_records}
        if phase == "output":
            if not output_presence["ao_alpha"]:
                failures.append("output reports never contain nonzero AO alpha")
            if not output_presence["il_rgb"]:
                failures.append("output reports never contain nonzero IL RGB")
            summary[phase]["aggregate_presence"] = output_presence
    return summary


def parse_log(path: Path, failures: list[str], no_coverage: list[str]) -> tuple[list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]], list[dict[str, Any]]]:
    if not path.is_file():
        no_coverage.append(f"runtime log is missing: {path}")
        return [], [], [], []
    try:
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as error:
        failures.append(f"cannot read runtime log {path}: {error}")
        return [], [], [], []

    lifecycle: list[dict[str, Any]] = []
    ao_history: list[dict[str, Any]] = []
    temporal_stages: list[dict[str, Any]] = []
    for index, line in enumerate(lines):
        fields = parse_fields(line, LIFECYCLE_MARKER)
        if fields is not None:
            fields["_line"] = index  # type: ignore[assignment]
            lifecycle.append(fields)
        fields = parse_fields(line, AO_HISTORY_MARKER)
        if fields is not None:
            fields["_line"] = index  # type: ignore[assignment]
            ao_history.append(fields)
        fields = parse_fields(line, TEMPORAL_STAGE_MARKER)
        if fields is not None:
            fields["_line"] = index  # type: ignore[assignment]
            temporal_stages.append(fields)
    return lifecycle, ao_history, temporal_stages, lines


def check_lifecycle(events: list[dict[str, Any]], failures: list[str], no_coverage: list[str]) -> dict[str, Any]:
    requirements = {
        "skip_acquire": lambda event: True,
        "discard_encoded_frame": lambda event: True,
        "producer_reload": lambda event: event.get("result") == "reloaded",
        "multiple_view": lambda event: event.get("result") == "rejected_before_encoding",
        "resize": lambda event: event.get("changed") == "1",
        "resize_restore": lambda event: event.get("restored") == "1",
    }
    summary: dict[str, Any] = {}
    for event_name, predicate in requirements.items():
        matching = [event for event in events if event.get("event") == event_name]
        accepted = [event for event in matching if predicate(event)]
        if not matching:
            no_coverage.append(f"lifecycle log lacks event {event_name}")
        elif not accepted:
            failures.append(f"lifecycle event {event_name} did not meet its required result")
        summary[event_name] = {
            "count": len(matching),
            "accepted_count": len(accepted),
            "engine_frames": [integer(event.get("engine_frame")) for event in matching],
        }
    return summary


def check_history_logs(ao_history: list[dict[str, Any]], events: list[dict[str, Any]], failures: list[str], no_coverage: list[str]) -> dict[str, Any]:
    if not ao_history:
        no_coverage.append("runtime log lacks METAL_AO_IL_HISTORY records")
        return {"record_count": 0, "reuse_count": 0, "gap_checks": []}

    discarded_frames = {
        integer(event.get("engine_frame"))
        for event in events
        if event.get("event") == "discard_encoded_frame" and integer(event.get("engine_frame")) is not None
    }
    eligible = [record for record in ao_history if integer(record.get("frame")) not in discarded_frames]
    reuse_records: list[dict[str, Any]] = []
    for record in ao_history:
        if record.get("reuse") != "1":
            continue
        source_frame = integer(record.get("source_frame"))
        camera_source = integer(record.get("camera_source"))
        reuse_records.append({"frame": integer(record.get("frame")), "source_frame": source_frame, "camera_source": camera_source})
        if source_frame is None or camera_source is None or source_frame != camera_source:
            failures.append(
                "METAL_AO_IL_HISTORY reuse=1 has source_frame != camera_source "
                f"at frame {record.get('frame')}"
            )

    gap_checks: list[dict[str, Any]] = []
    gap_events = sorted(
        [event for event in events if event.get("event") in {"skip_acquire", "discard_encoded_frame"}],
        key=lambda event: int(event.get("_line", "-1")),
    )
    for event in gap_events:
        event_line = int(event.get("_line", "-1"))
        prior = [record for record in eligible if int(record.get("_line", "-1")) < event_line]
        following = [record for record in eligible if int(record.get("_line", "-1")) > event_line]
        if not prior or not following:
            no_coverage.append(f"cannot check AO history preservation after {event.get('event')} at line {event_line}")
            continue
        before = prior[-1]
        after = following[0]
        before_producer_frame = integer(before.get("frame"))
        after_source = integer(after.get("source_frame"))
        after_camera = integer(after.get("camera_source"))
        ok = (after.get("reuse") == "1" and before_producer_frame is not None and
              after_source == before_producer_frame and after_camera == before_producer_frame)
        if not ok:
            failures.append(
                f"AO history source changed across {event.get('event')} at frame {event.get('engine_frame')}: "
                f"last_submitted_producer={before_producer_frame} "
                f"after=({after_source},{after_camera}) reuse={after.get('reuse')}"
            )
        gap_checks.append({
            "event": event.get("event"),
            "event_frame": integer(event.get("engine_frame")),
            "before_frame": integer(before.get("frame")),
            "last_submitted_producer_frame": before_producer_frame,
            "after_frame": integer(after.get("frame")),
            "after_source_frame": after_source,
            "after_camera_source": after_camera,
            "preserved": ok,
        })
    return {"record_count": len(ao_history), "reuse_count": len(reuse_records), "reuse_records": reuse_records, "gap_checks": gap_checks}


def check_temporal_stages(stages: list[dict[str, Any]], events: list[dict[str, Any]], failures: list[str], no_coverage: list[str]) -> list[dict[str, Any]]:
    interruptions = sorted(
        [event for event in events if event.get("event") in {"skip_acquire", "discard_encoded_frame"}],
        key=lambda event: int(event.get("_line", "-1")),
    )
    discarded_frames = {
        integer(event.get("engine_frame"))
        for event in events
        if event.get("event") == "discard_encoded_frame" and integer(event.get("engine_frame")) is not None
    }
    successful_stages = [stage for stage in stages if integer(stage.get("frame")) not in discarded_frames]
    checks: list[dict[str, Any]] = []
    if not stages:
        no_coverage.append("runtime log lacks METAL_TEMPORAL_STAGE records")
        return checks
    for event in interruptions:
        event_line = int(event.get("_line", "-1"))
        prior = [stage for stage in successful_stages if int(stage.get("_line", "-1")) < event_line]
        following = [stage for stage in successful_stages if int(stage.get("_line", "-1")) > event_line]
        if not prior or not following:
            no_coverage.append(f"cannot verify previous-frame continuity after {event.get('event')} at line {event_line}")
            continue
        last_staged = integer(prior[-1].get("frame"))
        next_stage = following[0]
        actual_previous = integer(next_stage.get("previous_frame"))
        ok = last_staged is not None and actual_previous == last_staged
        if not ok:
            failures.append(
                f"METAL_TEMPORAL_STAGE after {event.get('event')} has previous_frame={actual_previous}; "
                f"last successful staged frame was {last_staged}"
            )
        checks.append({
            "event": event.get("event"),
            "event_frame": integer(event.get("engine_frame")),
            "last_successful_staged_frame": last_staged,
            "next_frame": integer(next_stage.get("frame")),
            "next_previous_frame": actual_previous,
            "preserved": ok,
        })
    return checks


def check_discard_reports(reports: list[tuple[Path, dict[str, Any]]], events: list[dict[str, Any]], failures: list[str]) -> list[dict[str, Any]]:
    discarded_frames = {
        integer(event.get("engine_frame"))
        for event in events
        if event.get("event") == "discard_encoded_frame" and integer(event.get("engine_frame")) is not None
    }
    violations: list[dict[str, Any]] = []
    for path, report in reports:
        frame = report_frame(report)
        if frame not in discarded_frames:
            continue
        if report.get("command_completed") is True:
            violations.append({"file": path.name, "engine_frame": frame})
            failures.append(f"discarded encoded frame {frame} has a completed GPU readback report: {path}")
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports", required=True, type=Path, help="directory containing motion/history JSON reports")
    parser.add_argument("--log", required=True, type=Path, help="runtime log containing temporal lifecycle diagnostics")
    mode_group = parser.add_mutually_exclusive_group()
    mode_group.add_argument("--motion-only", action="store_true",
        help="allow runs without injected lifecycle events while retaining motion, texture, and reuse validation")
    mode_group.add_argument("--lifecycle-only", action="store_true",
        help="require full lifecycle and reset-motion coverage while allowing fixed-camera zero motion")
    parser.add_argument("--moving-after-frame", type=int, default=0,
        help="exclude startup/precache reports from required moving coverage")
    args = parser.parse_args()

    failures: list[str] = []
    no_coverage: list[str] = []
    reports = load_reports(args.reports, failures, no_coverage)
    motion = motion_summary(reports, failures, no_coverage,
        require_moving_coverage=not args.lifecycle_only, moving_after_frame=args.moving_after_frame)
    temporal = temporal_summary(reports, failures, no_coverage)
    lifecycle, ao_history, stages, _lines = parse_log(args.log, failures, no_coverage)
    history_result = check_history_logs(ao_history, lifecycle, failures, no_coverage)
    if args.motion_only and not lifecycle:
        lifecycle_result = {
            "checks_skipped": True,
            "reason": "no injected lifecycle probe events in log",
        }
        stage_checks: list[dict[str, Any]] = []
        discarded_readbacks: list[dict[str, Any]] = []
    else:
        lifecycle_result = check_lifecycle(lifecycle, failures, no_coverage)
        stage_checks = check_temporal_stages(stages, lifecycle, failures, no_coverage)
        discarded_readbacks = check_discard_reports(reports, lifecycle, failures)

    status = "FAIL" if failures else ("NO_COVERAGE" if no_coverage else "PASS")
    summary = {
        "status": status,
        "failures": failures,
        "no_coverage": no_coverage,
        "motion_families": motion,
        "temporal_phases": temporal,
        "lifecycle_events": lifecycle_result,
        "ao_il_history": history_result,
        "stage_continuity": stage_checks,
        "discarded_frame_completed_readbacks": discarded_readbacks,
        "moving_after_frame": args.moving_after_frame,
        "mode": "motion-only" if args.motion_only else ("lifecycle-only" if args.lifecycle_only else "full-lifecycle"),
        "inputs": {"reports": str(args.reports), "log": str(args.log)},
    }
    print(json.dumps(summary, indent=2, sort_keys=True, allow_nan=False))
    return 0 if status == "PASS" else 1


if __name__ == "__main__":
    raise SystemExit(main())
