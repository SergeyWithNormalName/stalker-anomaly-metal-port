#!/usr/bin/env python3
"""Fail-closed E01 aggregation for three timing runs of S0, S2, S4, and S5."""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import re
import statistics
import sys
from typing import Any


FIXTURES = ("S0", "S2", "S4", "S5")
SUMMARY_FIELDS = ("p50", "p95", "p99", "max", "one_percent_low_fps")
CORE_PROVENANCE_FIELDS = (
    "backend", "git_head", "build_options", "dirty_patch_sha256", "untracked_list_sha256",
    "shader_receipt_set_sha256",
)
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")


def read_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ValueError(f"{path} must contain a JSON object")
    return value


def finite_number(value: Any) -> float | None:
    if isinstance(value, bool):
        return None
    try:
        result = float(value)
    except (TypeError, ValueError):
        return None
    return result if math.isfinite(result) else None


def median(values: list[float | int | None]) -> float | None:
    clean = [float(value) for value in values if finite_number(value) is not None]
    return statistics.median(clean) if clean else None


def expected_contract(registry_path: pathlib.Path, fixture: str) -> tuple[dict[str, Any], str]:
    registry_bytes = registry_path.read_bytes()
    registry = json.loads(registry_bytes.decode("utf-8"))
    if registry.get("schema") != "direct-metal-release-fixtures/v1":
        raise ValueError(f"unsupported fixture registry schema in {registry_path}")
    contract = registry.get("fixtures", {}).get(fixture)
    if not isinstance(contract, dict) or contract.get("status") != "ready":
        raise ValueError(f"fixture registry does not mark {fixture} ready")
    return contract, hashlib.sha256(registry_bytes).hexdigest()


def canonical_json(value: Any) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)


def run_settings(provenance: dict[str, Any], telemetry: dict[str, Any]) -> dict[str, Any]:
    env = provenance.get("effective_env") or {}
    stable_env = {
        key: value for key, value in env.items()
        if key.startswith(("MTL_", "MVK_", "VK_", "XRAY_METAL_"))
        or key == "XRAY_BENCHMARK_LOGICAL_SIZE"
    }
    stable_env = {key: value for key, value in stable_env.items()
                  if key not in ("XRAY_METAL_E01_FRAMES", "XRAY_METAL_E01_WINDOW_PATH")}
    return {
        "backend": provenance.get("backend"),
        "binary_sha256": (provenance.get("binary") or {}).get("sha256"),
        "core_provenance": {key: provenance.get(key) for key in CORE_PROVENANCE_FIELDS},
        "profile": {
            "manifest_sha256": (provenance.get("profile") or {}).get("manifest_sha256"),
            "user_ltx_sha256": (provenance.get("profile") or {}).get("user_ltx_sha256"),
        },
        "tool_versions": provenance.get("tool_versions"),
        "stable_env": stable_env,
        "drawable": [telemetry.get("actual_drawable_width"), telemetry.get("actual_drawable_height")],
        "engine_extent": [telemetry.get("engine_width"), telemetry.get("engine_height")],
        "engine_aspect": telemetry.get("engine_aspect"),
        "engine_fov": telemetry.get("engine_fov"),
        "options": telemetry.get("options"),
        "tonemap_and_color_grading": telemetry.get("tonemap_and_color_grading"),
        "material_set_sha256": telemetry.get("material_set_sha256"),
    }


def backends_match(provenance_backend: str, telemetry_backend: Any) -> bool:
    value = str(telemetry_backend or "").lower()
    if provenance_backend == "metal":
        return value == "direct metal"
    if provenance_backend == "dxvk":
        return value.startswith("dxvk")
    return provenance_backend.lower() == value


def vector3(value: Any) -> tuple[float, float, float] | None:
    if isinstance(value, str):
        try:
            parts = [float(part.strip()) for part in value.split(",")]
        except ValueError:
            return None
    elif isinstance(value, (list, tuple)):
        parts = [finite_number(part) for part in value]
    else:
        return None
    if len(parts) != 3 or any(part is None or not math.isfinite(float(part)) for part in parts):
        return None
    return (float(parts[0]), float(parts[1]), float(parts[2]))


def close_vector(actual: Any, expected: Any, tolerance: Any, *, angular: bool = False) -> bool:
    left = vector3(actual)
    right = vector3(expected)
    limit = finite_number(tolerance)
    if left is None or right is None or limit is None or limit < 0:
        return False
    if not angular:
        return math.dist(left, right) <= limit
    left_len = math.sqrt(sum(component * component for component in left))
    right_len = math.sqrt(sum(component * component for component in right))
    if left_len == 0 or right_len == 0:
        return False
    cosine = sum(a * b for a, b in zip(left, right)) / (left_len * right_len)
    angle = math.acos(max(-1.0, min(1.0, cosine)))
    return angle <= limit


def expected_camera_at_telemetry(fixture: str, contract: dict[str, Any]) -> tuple[Any, Any]:
    position, direction = contract.get("camera"), contract.get("direction")
    if fixture != "S4":
        return position, direction
    base = vector3(position)
    forward = vector3(direction)
    if base is None or forward is None:
        return None, None
    length = math.sqrt(sum(component * component for component in forward))
    if length <= 0:
        return None, None
    direction_unit = tuple(component / length for component in forward)
    # E00 S4 drives the camera 2 m/s for 8 s, then turns 90 degrees.
    # E01 telemetry is dumped at the end of that 120 s scenario, before the
    # asynchronous camera effector has applied the restore on another frame.
    endpoint = tuple(component + 16.0 * delta for component, delta in zip(base, direction_unit))
    turned = (-direction_unit[2], direction_unit[1], direction_unit[0])
    return endpoint, turned


def validate_run(fixture: str, run_dir: pathlib.Path, registry_path: pathlib.Path) -> dict[str, Any]:
    paths = {name: run_dir / name for name in (
        "baseline.json", "provenance.json", "telemetry.json", "timing-window.json",
    )}
    absent = [str(path) for path in paths.values() if not path.is_file()]
    if absent:
        raise ValueError(f"{fixture} run {run_dir} is missing: {', '.join(absent)}")
    baseline = read_json(paths["baseline.json"])
    provenance = read_json(paths["provenance.json"])
    telemetry = read_json(paths["telemetry.json"])
    window = read_json(paths["timing-window.json"])
    expected, registry_hash = expected_contract(registry_path, fixture)

    if provenance.get("fixture") != fixture or telemetry.get("fixture") != fixture:
        raise ValueError(f"{fixture} run {run_dir} has a fixture ID mismatch")
    backend = provenance.get("backend")
    if not isinstance(backend, str) or not backend or telemetry.get("backend") is None:
        raise ValueError(f"{fixture} run {run_dir} lacks backend provenance")
    if not backends_match(backend, telemetry.get("backend")):
        raise ValueError(f"{fixture} run {run_dir} telemetry backend mismatch")

    binary = provenance.get("binary") or {}
    if not isinstance(binary.get("sha256"), str) or not SHA256_RE.fullmatch(binary["sha256"]):
        raise ValueError(f"{fixture} run {run_dir} has no binary SHA-256")
    profile = provenance.get("profile") or {}
    for key in ("manifest_sha256", "user_ltx_sha256"):
        if not isinstance(profile.get(key), str) or not SHA256_RE.fullmatch(profile[key]):
            raise ValueError(f"{fixture} run {run_dir} has no profile {key}")
    for key in CORE_PROVENANCE_FIELDS:
        if not isinstance(provenance.get(key), str) or not provenance[key]:
            raise ValueError(f"{fixture} run {run_dir} has no provenance {key}")
    if str(provenance["build_options"]).startswith("UNAVAILABLE:"):
        raise ValueError(f"{fixture} run {run_dir} build options are unavailable")
    if not isinstance(provenance.get("tool_versions"), dict) or not provenance["tool_versions"]:
        raise ValueError(f"{fixture} run {run_dir} has no tool_versions provenance")
    if not isinstance(telemetry.get("material_set_sha256"), str) or not SHA256_RE.fullmatch(telemetry["material_set_sha256"]):
        raise ValueError(f"{fixture} run {run_dir} has no material-set SHA-256")

    if telemetry.get("loading_screen_active") is not False:
        raise ValueError(f"{fixture} run {run_dir} has an active or unknown loading screen")
    scenario = telemetry.get("fixture_scenario")
    if not isinstance(scenario, dict):
        raise ValueError(f"{fixture} run {run_dir} lacks fixture_scenario status")
    expected_status = "NOT_APPLICABLE" if fixture == "S0" else "OK"
    if scenario.get("status") != expected_status or scenario.get("failures") != []:
        raise ValueError(f"{fixture} run {run_dir} fixture scenario status/failures are invalid")
    if fixture != "S0" and scenario.get("fixture") not in (None, fixture):
        raise ValueError(f"{fixture} run {run_dir} scenario fixture mismatch")

    telemetry_contract = telemetry.get("fixture_contract")
    if not isinstance(telemetry_contract, dict):
        raise ValueError(f"{fixture} run {run_dir} lacks fixture_contract")
    env = provenance.get("effective_env") or {}
    if telemetry_contract.get("registry_sha256") != registry_hash or env.get("XRAY_PARITY_FIXTURE_REGISTRY_SHA256") != registry_hash:
        raise ValueError(f"{fixture} run {run_dir} fixture registry hash mismatch")
    contract_checks = {
        "expected_level": expected.get("level"),
        "camera_pos": expected.get("camera"),
        "camera_dir": expected.get("direction"),
        "weather": expected.get("weather"),
        "weather_weight": expected.get("weather_weight"),
        "time": expected.get("time"),
        "save_name": expected.get("save_name"),
        "scenario": expected.get("scenario"),
    }
    for key, expected_value in contract_checks.items():
        actual = telemetry_contract.get(key)
        if canonical_json(actual) != canonical_json(expected_value):
            raise ValueError(f"{fixture} run {run_dir} fixture_contract.{key} mismatch")
    if telemetry.get("level_name") != expected.get("level") or telemetry.get("save_sha256") != expected.get("save_sha256"):
        raise ValueError(f"{fixture} run {run_dir} level/save does not match fixture registry")
    camera = telemetry.get("camera") or {}
    expected_position, expected_direction = expected_camera_at_telemetry(fixture, expected)
    if not close_vector(camera.get("pos"), expected_position, telemetry_contract.get("camera_position_tolerance_m")):
        raise ValueError(f"{fixture} run {run_dir} camera position does not match fixture registry")
    if not close_vector(camera.get("dir"), expected_direction, telemetry_contract.get("camera_angle_tolerance_rad"), angular=True):
        raise ValueError(f"{fixture} run {run_dir} camera direction does not match fixture registry")
    weather = telemetry.get("weather") or {}
    if weather.get("weather_name") != expected.get("weather") or finite_number(weather.get("weather_weight")) != finite_number(expected.get("weather_weight")):
        raise ValueError(f"{fixture} run {run_dir} weather does not match fixture registry")
    game_time = telemetry.get("game_time") or {}
    try:
        expected_h, expected_m, expected_s = (int(part) for part in expected["time"].split(":"))
    except (KeyError, ValueError, TypeError):
        raise ValueError(f"fixture registry has invalid time for {fixture}")
    if (game_time.get("hours"), game_time.get("minutes"), game_time.get("seconds")) != (expected_h, expected_m, expected_s):
        raise ValueError(f"{fixture} run {run_dir} time does not match fixture registry")

    if window.get("warmup_seconds") != 60 or window.get("sample_seconds") != 120:
        raise ValueError(f"{fixture} run {run_dir} must use a 60-second warmup and 120-second sample window")
    start = window.get("start_engine_frame")
    end = window.get("end_engine_frame")
    if (isinstance(start, bool) or not isinstance(start, int) or
            isinstance(end, bool) or not isinstance(end, int) or start < 0 or end < start):
        raise ValueError(f"{fixture} run {run_dir} has invalid engine-frame window bounds")
    if baseline.get("timing_window") != {"start_engine_frame": start, "end_engine_frame": end}:
        raise ValueError(f"{fixture} run {run_dir} baseline does not match timing-window bounds")
    accepted_rows = finite_number(baseline.get("rows_accepted"))
    rejected_rows = finite_number(baseline.get("rows_rejected"))
    if accepted_rows is None or accepted_rows < 2 or rejected_rows != 0:
        raise ValueError(f"{fixture} run {run_dir} has insufficient or rejected timing rows")
    presentation = baseline.get("presentation") or {}
    if presentation.get("pacing_gate") not in ("CLOSED", "OPEN"):
        raise ValueError(f"{fixture} run {run_dir} lacks displayed-timestamp gate status")
    displayed = baseline.get("displayed_frame_interval_ms")
    if not isinstance(displayed, dict):
        raise ValueError(f"{fixture} run {run_dir} lacks displayed interval metrics")
    display_status = presentation.get("display_metrics_status")
    if presentation.get("pacing_gate") == "CLOSED" and display_status != "complete":
        raise ValueError(f"{fixture} run {run_dir} has inconsistent complete-presentation status")
    if presentation.get("pacing_gate") == "OPEN" and display_status == "complete":
        raise ValueError(f"{fixture} run {run_dir} has inconsistent open-presentation status")
    if displayed.get("count", 0) > 0 and any(finite_number(displayed.get(field)) is None for field in SUMMARY_FIELDS):
        raise ValueError(f"{fixture} run {run_dir} has incomplete displayed interval metrics")
    if displayed.get("count", 0) == 0 and display_status not in ("partial", "unavailable"):
        raise ValueError(f"{fixture} run {run_dir} has invalid empty displayed interval status")
    displayed_samples = finite_number(presentation.get("displayed_timestamp_samples", 0))
    zero_samples = finite_number(presentation.get("zero_timestamp_callbacks", 0))
    interval_samples = finite_number(presentation.get("displayed_interval_samples"))
    metric_count = finite_number(displayed.get("count"))
    if (displayed_samples is None or zero_samples is None or interval_samples is None or metric_count is None or
            displayed_samples + zero_samples != accepted_rows or interval_samples != metric_count):
        raise ValueError(f"{fixture} run {run_dir} presentation counts do not match baseline")

    return {
        "fixture": fixture,
        "path": str(run_dir),
        "baseline": baseline,
        "provenance": provenance,
        "telemetry": telemetry,
        "window": window,
        "settings": run_settings(provenance, telemetry),
    }


def _metric(run: dict[str, Any], parent: str, child: str | None = None, field: str = "p50") -> float | None:
    source = run["baseline"].get(parent) or {}
    value = source.get(field) if child is None else (source.get(child) or {}).get(field)
    return finite_number(value)


def aggregate(runs: list[dict[str, Any]]) -> dict[str, Any]:
    by_fixture: dict[str, list[dict[str, Any]]] = {fixture: [] for fixture in FIXTURES}
    settings_by_fixture: dict[str, str] = {}
    core_settings: str | None = None
    for run in runs:
        fixture = run["fixture"]
        settings = canonical_json(run["settings"])
        if fixture in settings_by_fixture and settings_by_fixture[fixture] != settings:
            raise ValueError(f"{fixture} runs have mismatched fixture settings/provenance")
        settings_by_fixture[fixture] = settings
        # Active options such as SSFX LUT can be weather/level dependent.
        # Compare them strictly within a fixture; across fixtures retain the
        # binary, build, profile, display, and process-environment contract.
        stable_text = canonical_json({key: value for key, value in run["settings"].items()
                                      if key not in ("options", "tonemap_and_color_grading")})
        if core_settings is not None and core_settings != stable_text:
            raise ValueError("runs have mismatched binary/build/backend/profile/settings")
        core_settings = stable_text
        by_fixture[fixture].append(run)

    fixtures_result: dict[str, Any] = {}
    for fixture, fixture_runs in by_fixture.items():
        fixture_runs.sort(key=lambda run: pathlib.Path(run["path"]).name)
        if len(fixture_runs) != 3:
            raise ValueError(f"expected exactly three {fixture} runs; found {len(fixture_runs)}")
        run_rows = []
        for run in fixture_runs:
            base = run["baseline"]
            display = base["displayed_frame_interval_ms"]
            run_rows.append({
                "run_id": pathlib.Path(run["path"]).name,
                "path": run["path"],
                "displayed_frame_interval_ms": {field: display.get(field) for field in SUMMARY_FIELDS},
                "pacing_gate": (base.get("presentation") or {}).get("pacing_gate"),
                "display_metrics_status": (base.get("presentation") or {}).get("display_metrics_status"),
                "gpu_frame_ms_p50": _metric(run, "gpu_frame_ms"),
                "cpu_scene_ms_p50": _metric(run, "cpu_stage_ms", "scene_ms", "p50"),
                "counts": {
                    "rows_accepted": base.get("rows_accepted"),
                    "rows_rejected": base.get("rows_rejected"),
                    "displayed_timestamps": (base.get("presentation") or {}).get("displayed_timestamp_samples"),
                    "zero_timestamp_callbacks": (base.get("presentation") or {}).get("zero_timestamp_callbacks", 0),
                    "serial_gap_events": (base.get("presentation") or {}).get("serial_gap_events", 0),
                    "serial_gap_frames": (base.get("presentation") or {}).get("serial_gap_frames", 0),
                    "gpu_timestamp_pairs": (base.get("gpu_frame_ms") or {}).get("count"),
                    "cpu_scene_samples": (((base.get("cpu_stage_ms") or {}).get("scene_ms") or {}).get("count")),
                },
                "window": run["window"],
            })
        medians: dict[str, float | None] = {}
        for field in SUMMARY_FIELDS:
            medians[f"displayed_{field}"] = median([
                row["displayed_frame_interval_ms"].get(field) for row in run_rows
            ])
        medians["gpu_frame_ms_p50"] = median([row["gpu_frame_ms_p50"] for row in run_rows])
        medians["cpu_scene_ms_p50"] = median([row["cpu_scene_ms_p50"] for row in run_rows])
        fixtures_result[fixture] = {
            "all_three_status": "VALID",
            "run_count": 3,
            "runs": run_rows,
            "median_of_run_metrics": medians,
        }

    costs = []
    for fixture, group in fixtures_result.items():
        samples = [row["cpu_scene_ms_p50"] for row in group["runs"]]
        observed = [value for value in samples if value is not None]
        if len(observed) == 3:
            centre = statistics.median(observed)
            costs.append({
                "fixture": fixture,
                "metric": "CPU scene render/build (ms)",
                "median_run_p50_ms": centre,
                "min_run_p50_ms": min(observed),
                "max_run_p50_ms": max(observed),
                "relative_run_span_percent": 100.0 * (max(observed) - min(observed)) / centre if centre else None,
                "runs_with_samples": len(observed),
            })
    costs.sort(key=lambda item: item["median_run_p50_ms"], reverse=True)
    return {
        "schema": "direct-metal-e01-aggregate/v1",
        "stage": "E01",
        "classification": "DIAGNOSTIC_BUDGET_ONLY",
        "release_60_fps_gate": "FAIL" if any(
            (run["baseline"].get("displayed_frame_interval_ms") or {}).get("one_percent_low_fps") is not None and
            float(run["baseline"]["displayed_frame_interval_ms"]["one_percent_low_fps"]) < 60.0
            for run in runs
        ) else "OPEN",
        "run_count": len(runs),
        "comparability_contract_sha256": hashlib.sha256((core_settings or "").encode("utf-8")).hexdigest(),
        "fixture_groups": fixtures_result,
        "top_three_measured_scene_costs": costs[:3],
        "cost_ranking_note": "Ranks the three largest fixture CPU scene scopes. Each is a different scene workload, not a separate component to sum. GPU frame durations are shown above and overlap CPU work; source-level attribution comes from a separate CPU sample.",
        "all_fixture_groups_three_of_three": all(group["all_three_status"] == "VALID" for group in fixtures_result.values()),
    }


def render_budget(result: dict[str, Any]) -> str:
    def fmt(value: Any) -> str:
        return "—" if finite_number(value) is None else f"{float(value):.3f}"

    lines = [
        "# E01 diagnostic timing budget", "",
        "This is an E01 diagnostic budget. It does not close the 60 FPS release gate or establish visual parity.",
        "CPU and GPU intervals overlap asynchronously; do not sum them. encode_ms contains scene_ms, which contains grass/prewarm work.", "",
        "## Fixture runs", "",
        "| Fixture | Run | Display p50 ms | p95 ms | p99 ms | Max ms | 1% low FPS | GPU p50 ms | CPU scene p50 ms | Rows | Display timestamps | Zero callbacks / serial gaps | Display gate | GPU pairs | Three-run group |",
        "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---:|---|",
    ]
    for fixture in FIXTURES:
        group = result["fixture_groups"][fixture]
        for row in group["runs"]:
            display = row["displayed_frame_interval_ms"]
            counts = row["counts"]
            lines.append(
                f"| {fixture} | {row['run_id']} | {fmt(display['p50'])} | {fmt(display['p95'])} | {fmt(display['p99'])} | {fmt(display['max'])} | {fmt(display['one_percent_low_fps'])} | {fmt(row['gpu_frame_ms_p50'])} | {fmt(row['cpu_scene_ms_p50'])} | {counts['rows_accepted']} accepted / {counts['rows_rejected']} rejected | {counts['displayed_timestamps']} | {counts['zero_timestamp_callbacks']} / {counts['serial_gap_events']} events ({counts['serial_gap_frames']} frames) | {row['pacing_gate']} / {row['display_metrics_status']} | {counts['gpu_timestamp_pairs']} | VALID (3/3) |"
            )
        med = group["median_of_run_metrics"]
        lines.append(
            f"| **{fixture} median** | 3 runs | {fmt(med['displayed_p50'])} | {fmt(med['displayed_p95'])} | {fmt(med['displayed_p99'])} | {fmt(med['displayed_max'])} | {fmt(med['displayed_one_percent_low_fps'])} | {fmt(med['gpu_frame_ms_p50'])} | {fmt(med['cpu_scene_ms_p50'])} | — | — | — | — | — | **VALID** |"
        )
    lines += ["", "## Three largest measured scene costs", ""]
    if result["top_three_measured_scene_costs"]:
        lines += ["| Rank | Fixture | Measured scope | Median run p50 ms | Run p50 range ms | Relative span | Confidence evidence |", "|---:|---|---|---:|---:|---:|---|"]
        for index, cost in enumerate(result["top_three_measured_scene_costs"], 1):
            lines.append(f"| {index} | {cost['fixture']} | {cost['metric']} | {cost['median_run_p50_ms']:.3f} | {cost['min_run_p50_ms']:.3f}–{cost['max_run_p50_ms']:.3f} | {cost['relative_run_span_percent']:.1f}% | {cost['runs_with_samples']}/3 valid runs; range shown |")
    else:
        lines.append("No independent cost samples were present.")
    lines += ["", result["cost_ranking_note"], "", f"60 FPS gate: {result['release_60_fps_gate']}. E01 measurement does not establish a release performance pass.", ""]
    return "\n".join(lines)


def parse_run(value: str) -> tuple[str, pathlib.Path]:
    fixture, sep, path = value.partition(":")
    if not sep or fixture not in FIXTURES or not path:
        raise argparse.ArgumentTypeError("--run must be FIXTURE:PATH where FIXTURE is S0, S2, S4, or S5")
    return fixture, pathlib.Path(path)


def write_frames_csv(runs: list[dict[str, Any]], output: pathlib.Path) -> None:
    """Preserve every raw frame with its admitted fixture and run identity."""
    fields: list[str] | None = None
    with output.open("w", newline="", encoding="utf-8") as destination:
        writer: csv.DictWriter | None = None
        for run in runs:
            path = pathlib.Path(run["path"]) / "frames.csv"
            with path.open(newline="", encoding="utf-8-sig") as source:
                reader = csv.DictReader(source)
                if not reader.fieldnames:
                    raise ValueError(f"{path} has no frame header")
                if fields is None:
                    fields = reader.fieldnames
                    writer = csv.DictWriter(destination, fieldnames=["fixture", "run_id", *fields])
                    writer.writeheader()
                elif reader.fieldnames != fields:
                    raise ValueError(f"{path} has a different frame schema")
                assert writer is not None
                for row in reader:
                    writer.writerow({"fixture": run["fixture"], "run_id": path.parent.name, **row})


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="append", required=True, type=parse_run, help="FIXTURE:RUN_DIR (repeat 12 times)")
    parser.add_argument("--output", required=True, type=pathlib.Path)
    parser.add_argument("--registry", type=pathlib.Path, default=pathlib.Path(__file__).resolve().parents[1] / "docs/direct-metal-release/FIXTURES.json")
    args = parser.parse_args(argv)
    try:
        if len(args.run) != 12:
            raise ValueError(f"expected exactly 12 run directories; found {len(args.run)}")
        counts = {fixture: 0 for fixture in FIXTURES}
        for fixture, _ in args.run:
            counts[fixture] += 1
        if any(counts[fixture] != 3 for fixture in FIXTURES):
            raise ValueError("exactly three runs are required for each of S0, S2, S4, and S5")
        resolved_paths = [path.resolve() for _, path in args.run]
        if len(set(resolved_paths)) != len(resolved_paths):
            raise ValueError("each --run must identify a distinct run directory")
        runs = [validate_run(fixture, path, args.registry) for fixture, path in args.run]
        result = aggregate(runs)
        args.output.mkdir(parents=True, exist_ok=True)
        (args.output / "baseline.json").write_text(json.dumps(result, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        (args.output / "budget.md").write_text(render_budget(result), encoding="utf-8")
        write_frames_csv(runs, args.output / "frames.csv")
    except (OSError, ValueError, json.JSONDecodeError) as error:
        print(f"E01 aggregation error: {error}", file=sys.stderr)
        return 2
    print(f"E01 aggregate written to {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
