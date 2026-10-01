#!/usr/bin/env python3
"""Validate native Direct Metal sun-cascade GPU readback reports."""

from __future__ import annotations

import argparse
from array import array
import json
import math
import sys
from pathlib import Path
from typing import Any


REQUIRED_PHASES = ("stencil-baseline", "near", "middle", "far")
SHADOW_COVERED_CUTOFF = 1.0 - 1.0e-7


def validate_report(path: Path) -> tuple[bool, list[str], dict[str, Any]]:
    errors: list[str] = []
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return False, [f"{path.name}: cannot parse report: {exc}"], {}

    if report.get("schema") != "r4_sun_cascade_validation_v1":
        errors.append(f"{path.name}: unsupported schema {report.get('schema')!r}")
    if report.get("result") != "PASS":
        errors.append(f"{path.name}: native report result is {report.get('result')!r}")
    if report.get("command_completed") is not True:
        errors.append(f"{path.name}: Metal command did not complete successfully")

    phases = report.get("phases")
    if not isinstance(phases, list):
        errors.append(f"{path.name}: missing phases array")
        phases = []
    phase_by_name = {phase.get("phase"): phase for phase in phases if isinstance(phase, dict)}
    for name in REQUIRED_PHASES:
        phase = phase_by_name.get(name)
        if phase is None:
            errors.append(f"{path.name}: missing {name} phase")
            continue
        if phase.get("result") != "PASS":
            errors.append(f"{path.name}: {name} phase result is {phase.get('result')!r}")
        if int(phase.get("baseline_marked_pixels", 0)) <= 0:
            errors.append(f"{path.name}: {name} has no baseline sun-mask stencil pixels")
        if name == "stencil-baseline":
            continue
        if int(phase.get("expected_stencil_pass_pixels", 0)) <= 0:
            errors.append(f"{path.name}: {name} has no CPU-predicted cuboid coverage")
        if int(phase.get("shadow_map_covered_pixels", 0)) <= 0:
            errors.append(f"{path.name}: {name} shadow map has no written depth pixels")
        if int(phase.get("shadow_map_nonfinite_pixels", -1)) != 0:
            errors.append(f"{path.name}: {name} shadow map contains non-finite depth")
        if int(phase.get("stencil_mismatch_pixels", -1)) != 0:
            errors.append(f"{path.name}: {name} stencil ownership mismatch count is not zero")
        if int(phase.get("expected_stencil_write_misses", -1)) != 0:
            errors.append(f"{path.name}: {name} expected stencil write misses are not zero")
        if int(phase.get("accumulation_nonfinite_channels", -1)) != 0:
            errors.append(f"{path.name}: {name} accumulation has non-finite channels")
        if int(phase.get("accumulation_finite_channels", 0)) <= 0:
            errors.append(f"{path.name}: {name} accumulation readback has no finite channels")
        if name in ("near", "middle") and int(phase.get(
            "stencil_relevant_changed_pixels", phase.get("stencil_changed_pixels", 0)
        )) <= 0:
            errors.append(f"{path.name}: {name} did not clear any eligible world receiver stencil pixels")
        if name == "far" and int(phase.get("stencil_changed_pixels", -1)) != 0:
            errors.append(f"{path.name}: far pass modified stencil instead of preserving it")
        if float(report.get("sun_color_intensity", 0.0)) > 1.0e-6 and int(
            phase.get("accumulation_nonzero_rgb_pixels", 0)
        ) <= 0:
            errors.append(f"{path.name}: {name} accumulation is black despite positive sun intensity")

    summary = {
        "family": report.get("family"),
        "engine_frame": report.get("engine_frame"),
        "sun_color_intensity": report.get("sun_color_intensity"),
        "phases": {
            name: {
                "result": phase_by_name[name].get("result"),
                "source_cascade_size": phase_by_name[name].get("source_cascade_size"),
                "source_cascade_bias": phase_by_name[name].get("source_cascade_bias"),
                "marked_pixels": phase_by_name[name].get("baseline_marked_pixels"),
                "expected_stencil_pass_pixels": phase_by_name[name].get("expected_stencil_pass_pixels"),
                "stencil_changed_pixels": phase_by_name[name].get("stencil_changed_pixels"),
                "stencil_relevant_changed_pixels": phase_by_name[name].get("stencil_relevant_changed_pixels"),
                "raster_pass_receiver_outside_volume_pixels": phase_by_name[name].get(
                    "raster_pass_receiver_outside_volume_pixels"
                ),
                "stencil_mismatch_pixels": phase_by_name[name].get("stencil_mismatch_pixels"),
                "expected_stencil_write_misses": phase_by_name[name].get("expected_stencil_write_misses"),
                "stencil_comparison_samples": phase_by_name[name].get("stencil_comparison_samples", []),
                "depth_boundary_ambiguous_pixels": phase_by_name[name].get(
                    "depth_boundary_ambiguous_pixels"
                ),
                "depth_boundary_samples": phase_by_name[name].get("depth_boundary_samples", []),
                "shadow_map_covered_pixels": phase_by_name[name].get("shadow_map_covered_pixels"),
                "hud_position_pixels_excluded": phase_by_name[name].get("hud_position_pixels_excluded"),
                "clear_position_pixels_excluded": phase_by_name[name].get("clear_position_pixels_excluded"),
                "accumulation_nonfinite_channels": phase_by_name[name].get("accumulation_nonfinite_channels"),
                "nonfinite_accumulation_samples": phase_by_name[name].get("nonfinite_accumulation_samples", []),
                "accumulation_nonzero_rgb_pixels": phase_by_name[name].get(
                    "accumulation_nonzero_rgb_pixels"
                ),
            }
            for name in REQUIRED_PHASES
            if name in phase_by_name
        },
    }
    return not errors, errors, summary


def validate_caster_report(path: Path) -> tuple[bool, list[str], dict[str, Any]]:
    errors: list[str] = []
    try:
        report = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        return False, [f"{path.name}: cannot parse caster report: {exc}"], {}
    if report.get("schema") != "r4_sun_cascade_caster_validation_v1":
        errors.append(f"{path.name}: unsupported caster schema {report.get('schema')!r}")
    result = report.get("result")
    if result not in ("PASS", "NO_COVERAGE"):
        errors.append(f"{path.name}: caster witness result is {result!r}")
    if report.get("command_completed") is not True:
        errors.append(f"{path.name}: Metal command did not complete successfully")
    if result == "PASS":
        if int(report.get("off_camera_caster_count", 0)) <= 0:
            errors.append(f"{path.name}: no submitted off-camera casters")
        if int(report.get("shadow_map_covered_pixels", 0)) <= 0:
            errors.append(f"{path.name}: shadow map has no written depth")
        if int(report.get("shadow_map_nonfinite_pixels", -1)) != 0:
            errors.append(f"{path.name}: shadow map contains non-finite depth")
        if int(report.get("valid_receiver_pixels", 0)) <= 0:
            errors.append(f"{path.name}: no valid in-frame receivers")
        if int(report.get("receivers_inside_shadow_map", 0)) <= 0:
            errors.append(f"{path.name}: no receivers projected into the shadow map")
        if report.get("require_shadowed_receivers") is True and int(
            report.get("shadowed_receiver_pixels", 0)
        ) <= 0:
            errors.append(f"{path.name}: off-camera caster produced no sampled in-frame shadow")
    summary = {
        "family": report.get("family"),
        "engine_frame": report.get("engine_frame"),
        "source_cascade_size": report.get("source_cascade_size"),
        "source_cascade_bias": report.get("source_cascade_bias"),
        "off_camera_caster_count": report.get("off_camera_caster_count"),
        "valid_receiver_pixels": report.get("valid_receiver_pixels"),
        "receivers_inside_shadow_map": report.get("receivers_inside_shadow_map"),
        "shadowed_receiver_pixels": report.get("shadowed_receiver_pixels"),
        "shadow_map_covered_pixels": report.get("shadow_map_covered_pixels"),
        "result": result,
    }
    return not errors, errors, summary


def load_shadow_map(report_path: Path) -> tuple[dict[str, Any], array]:
    report = json.loads(report_path.read_text(encoding="utf-8"))
    if report.get("schema") != "r4_sun_cascade_caster_validation_v1":
        raise ValueError(f"{report_path}: unsupported caster report schema")
    if report.get("shadow_map_export_requested") is not True:
        raise ValueError(f"{report_path}: shadow map export was not requested")
    raw_name = report.get("shadow_map_raw_file")
    width, height = int(report.get("shadow_map_width", 0)), int(report.get("shadow_map_height", 0))
    if not raw_name or width <= 0 or height <= 0:
        raise ValueError(f"{report_path}: missing raw map filename or dimensions")
    for key in ("stage_volume_view_projection", "current_view_projection", "inverse_view"):
        matrix = report.get(key)
        if not isinstance(matrix, list) or len(matrix) != 16 or any(
            not isinstance(value, (int, float)) or not math.isfinite(value) for value in matrix
        ):
            raise ValueError(f"{report_path}: missing or invalid {key} metadata")
    raw_path = report_path.parent / raw_name
    raw_bytes = raw_path.read_bytes()
    expected_bytes = width * height * 4
    if len(raw_bytes) != expected_bytes:
        raise ValueError(f"{raw_path}: expected {expected_bytes} bytes, found {len(raw_bytes)}")
    values = array("f")
    if values.itemsize != 4:
        raise ValueError("host float array is not float32")
    values.frombytes(raw_bytes)
    if sys.byteorder != "little":
        values.byteswap()
    return report, values


def compare_shadow_maps(reference_path: Path, candidate_path: Path, tolerance: float) -> dict[str, Any]:
    ref_report, reference = load_shadow_map(reference_path)
    candidate_report, candidate = load_shadow_map(candidate_path)
    ref_dims = (int(ref_report["shadow_map_width"]), int(ref_report["shadow_map_height"]))
    candidate_dims = (int(candidate_report["shadow_map_width"]), int(candidate_report["shadow_map_height"]))
    if ref_dims != candidate_dims:
        raise ValueError(f"shadow map dimensions differ: reference={ref_dims} candidate={candidate_dims}")

    matrix_comparisons: dict[str, dict[str, Any]] = {}
    for key in ("stage_volume_view_projection", "current_view_projection", "inverse_view"):
        before = ref_report[key]
        after = candidate_report[key]
        matrix_comparisons[key] = {
            "exact_match": before == after,
            "max_abs_delta": max(abs(a - b) for a, b in zip(before, after)),
        }

    changed = added = removed = nonfinite = 0
    max_delta = 0.0
    delta_sum = 0.0
    for before, after in zip(reference, candidate):
        if not math.isfinite(before) or not math.isfinite(after):
            nonfinite += 1
            continue
        before_covered = 0.0 <= before < SHADOW_COVERED_CUTOFF
        after_covered = 0.0 <= after < SHADOW_COVERED_CUTOFF
        if after_covered and not before_covered:
            added += 1
        elif before_covered and not after_covered:
            removed += 1
        delta = abs(after - before)
        delta_sum += delta
        max_delta = max(max_delta, delta)
        if delta > tolerance:
            changed += 1

    return {
        "schema": "metal_sun_shadow_map_comparison_v1",
        "result": "PASS" if nonfinite == 0 else "FAIL",
        "reference": str(reference_path.resolve()),
        "candidate": str(candidate_path.resolve()),
        "reference_family": ref_report.get("family"),
        "candidate_family": candidate_report.get("family"),
        "reference_frame": ref_report.get("engine_frame"),
        "candidate_frame": candidate_report.get("engine_frame"),
        "matrices": matrix_comparisons,
        "exact_camera_match": matrix_comparisons["current_view_projection"]["exact_match"] and
            matrix_comparisons["inverse_view"]["exact_match"],
        "exact_light_volume_match": matrix_comparisons["stage_volume_view_projection"]["exact_match"],
        "width": ref_dims[0],
        "height": ref_dims[1],
        "compared_pixels": len(reference),
        "changed_depth_pixels": changed,
        "removed_covered_pixels": removed,
        "added_covered_pixels": added,
        "nonfinite_pixels": nonfinite,
        "max_abs_depth_delta": max_delta,
        "mean_abs_depth_delta": delta_sum / max(1, len(reference) - nonfinite),
        "change_tolerance": tolerance,
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reports", type=Path, help="directory containing r4_sun_cascade JSON reports")
    parser.add_argument(
        "--compare-maps",
        nargs=2,
        metavar=("REFERENCE_JSON", "CANDIDATE_JSON"),
        type=Path,
        help="compare exported raw shadow maps named by two caster JSON reports",
    )
    parser.add_argument(
        "--map-change-tolerance",
        type=float,
        default=1.0e-6,
        help="minimum absolute depth delta counted as a changed pixel (default: 1e-6)",
    )
    parser.add_argument(
        "--skip-caster-witnesses",
        action="store_true",
        help="validate production cascade phases without requiring off-camera caster witness reports",
    )
    parser.add_argument(
        "--require-witness-prefix",
        action="append",
        default=[],
        metavar="FAMILY_PREFIX",
        help="add a semantic caster family prefix that must have at least one PASS report; repeat as needed",
    )
    args = parser.parse_args()

    if args.compare_maps:
        if not math.isfinite(args.map_change_tolerance) or args.map_change_tolerance < 0.0:
            print(json.dumps({"schema": "metal_sun_shadow_map_comparison_v1", "result": "FAIL",
                "errors": ["map change tolerance must be finite and nonnegative"]}))
            return 1
        try:
            summary = compare_shadow_maps(*args.compare_maps, args.map_change_tolerance)
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            print(json.dumps({"schema": "metal_sun_shadow_map_comparison_v1", "result": "FAIL",
                "errors": [str(exc)]}, indent=2, sort_keys=True))
            return 1
        print(json.dumps(summary, indent=2, sort_keys=True))
        return 0 if summary["result"] == "PASS" else 1

    if args.reports is None:
        parser.error("--reports is required unless --compare-maps is used")

    if not args.reports.is_dir():
        print(json.dumps({"result": "FAIL", "errors": [f"reports directory does not exist: {args.reports}"]}))
        return 1
    report_paths = sorted(
        path for path in args.reports.glob("r4_sun_cascade_*_frame_*.json")
        if not path.name.endswith("_caster.json")
    )
    if not report_paths:
        print(json.dumps({"result": "FAIL", "errors": ["no r4_sun_cascade reports found"]}))
        return 1

    errors: list[str] = []
    reports: list[dict[str, Any]] = []
    for path in report_paths:
        passed, report_errors, summary = validate_report(path)
        reports.append(summary)
        errors.extend(report_errors)
        if not passed and not report_errors:
            errors.append(f"{path.name}: validation failed")

    caster_paths = [] if args.skip_caster_witnesses else sorted(
        args.reports.glob("r4_sun_cascade_*_frame_*_caster.json")
    )
    caster_reports: list[dict[str, Any]] = []
    if not args.skip_caster_witnesses:
        if not caster_paths:
            errors.append("expected off-camera caster witnesses, found none")
    for path in caster_paths:
        passed, report_errors, summary = validate_caster_report(path)
        caster_reports.append(summary)
        errors.extend(report_errors)
        if not passed and not report_errors:
            errors.append(f"{path.name}: caster validation failed")
    caster_families: dict[str, list[dict[str, Any]]] = {"flora": [], "dynamic": []}
    for report in caster_reports:
        family = str(report.get("family") or "")
        if family.startswith("offcamera_flora_"):
            caster_families["flora"].append(report)
        elif family.startswith("offcamera_dynamic_"):
            caster_families["dynamic"].append(report)
    if not args.skip_caster_witnesses:
        required_prefixes = ["offcamera_flora_", "offcamera_dynamic_"] + args.require_witness_prefix
        for prefix in required_prefixes:
            family_reports = [
                report for report in caster_reports
                if str(report.get("family") or "").startswith(prefix)
            ]
            if not any(report.get("result") == "PASS" for report in family_reports):
                errors.append(f"expected at least one PASS caster witness with family prefix {prefix!r}")

    requested_prefix_summaries = {}
    for prefix in args.require_witness_prefix:
        matching = [report for report in caster_reports
            if str(report.get("family") or "").startswith(prefix)]
        requested_prefix_summaries[prefix] = {
            "report_count": len(matching),
            "pass_count": sum(report.get("result") == "PASS" for report in matching),
            "no_coverage_count": sum(report.get("result") == "NO_COVERAGE" for report in matching),
        }

    result = "PASS" if not errors else "FAIL"
    print(json.dumps({
        "schema": "metal_sun_cascade_verification_v1",
        "result": result,
        "reports_directory": str(args.reports.resolve()),
        "report_count": len(report_paths),
        "frames": reports,
        "caster_witnesses_skipped": args.skip_caster_witnesses,
        "caster_witness_count": len(caster_paths),
        "caster_witnesses": caster_reports,
        "caster_witness_families": {
            family: {
                "report_count": len(family_reports),
                "pass_count": sum(report.get("result") == "PASS" for report in family_reports),
                "no_coverage_count": sum(report.get("result") == "NO_COVERAGE" for report in family_reports),
            }
            for family, family_reports in caster_families.items()
        },
        "required_witness_prefixes": args.require_witness_prefix,
        "required_witness_prefix_results": requested_prefix_summaries,
        "errors": errors,
    }, indent=2, sort_keys=True))
    return 0 if result == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
