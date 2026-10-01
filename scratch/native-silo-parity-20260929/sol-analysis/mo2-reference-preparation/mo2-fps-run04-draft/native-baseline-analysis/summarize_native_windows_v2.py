#!/usr/bin/env python3
"""Read-only measured-window summary for the frozen native R1 baseline."""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
import math
import statistics
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[6]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
RUN = STAGE / "runs/native-vs-matched-mo2-baseline-01/native"
OUT = RUN / "output"
WIN_OUT = STAGE / "runs/mo2-reference-startup-01/mo2-controlled-save-run-04/output"
ANALYSIS = Path(__file__).resolve().parent
EXPECTED = {
    "exe_sha256": "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0",
    "manifest_sha256": "25628096e0cc11dc452eb15b1633c94d3398dd65e9d58efcb2959e64956ba0d2",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
}
CALENDAR = "2018-10-26T08:09:47"
LEVEL = "l01_escape"
WEATHER = "w_clear2"
CAMERA = (-221.082565, -18.284866, -168.182037)
DIRECTION = (0.0, 0.0, 1.0)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_markers(path: Path) -> dict[str, dict[str, str]]:
    markers: dict[str, dict[str, str]] = {}
    for line in path.read_text(errors="strict").splitlines():
        parts = line.split(",")
        fields: dict[str, str] = {}
        for item in parts[1:]:
            if "=" in item:
                key, value = item.split("=", 1)
                fields[key] = value
        markers[parts[0]] = fields
    return markers


def nearest_rank(values: list[float], percentile: float) -> float:
    if not values:
        raise ValueError("empty sample")
    values = sorted(values)
    return values[max(0, math.ceil(percentile * len(values)) - 1)]


def low_one_percent(fps: list[float]) -> tuple[float, int]:
    if not fps:
        raise ValueError("empty FPS sample")
    tail = sorted(fps)[:max(1, math.ceil(0.01 * len(fps)))]
    return statistics.mean(tail), len(tail)


def interval_one_percent_low(intervals_ms: list[float]) -> tuple[float, int]:
    """Reciprocal of the mean of the slowest ceil(1% N) frame intervals."""
    if not intervals_ms or any(not math.isfinite(v) or v <= 0 for v in intervals_ms):
        raise ValueError("invalid frame interval sample")
    tail = sorted(intervals_ms, reverse=True)[:max(1, math.ceil(0.01 * len(intervals_ms)))]
    return 1000.0 / statistics.mean(tail), len(tail)


def distribution(values: list[float], unit: str) -> dict[str, Any]:
    if not values or any(not math.isfinite(v) for v in values):
        raise ValueError(f"empty or nonfinite {unit} sample")
    return {"sample_count": len(values), "mean": statistics.mean(values),
        "p50_nearest_rank": nearest_rank(values, .50),
        "p99_nearest_rank": nearest_rank(values, .99), "unit": unit}


def finite(row: dict[str, str], key: str) -> float:
    value = float(row[key])
    if not math.isfinite(value):
        raise ValueError(f"nonfinite {key}")
    return value


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", errors="strict") as stream:
        return list(csv.DictReader(stream))


def shared_state_hash(path: Path, suffix: str) -> str:
    for line in path.read_text(errors="strict").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].endswith(suffix):
            return parts[0]
    raise ValueError(f"shared-state receipt lacks {suffix}")


def weather_value(value: str) -> tuple[float, ...] | float | str:
    if "/" in value:
        return tuple(float(part) for part in value.split("/"))
    try:
        return float(value)
    except ValueError:
        return value


def compare_committed_weather(native_rows: list[dict[str, str]], win_rows: list[dict[str, str]]) -> dict[str, Any]:
    def committed(rows: list[dict[str, str]]) -> dict[str, str]:
        return next((row for row in rows if row.get("marker") == "weather_committed"), {})
    native = committed(native_rows)
    windows = committed(win_rows)
    fields = ("paused", "sky_texture", "clouds_texture", "weight", "far_plane", "fog_distance",
        "fog_density", "rain_density", "wind_velocity", "ambient", "hemisphere", "sun_color", "sun_dir", "fog_color")
    mismatches: dict[str, dict[str, Any]] = {}
    for key in fields:
        a, b = native.get(key), windows.get(key)
        if a is None or b is None:
            mismatches[key] = {"native": a, "windows": b, "reason": "missing"}
            continue
        aa, bb = weather_value(a), weather_value(b)
        if isinstance(aa, tuple) and isinstance(bb, tuple):
            equal = len(aa) == len(bb) and all(abs(x-y) <= 1e-7 for x, y in zip(aa, bb))
        elif isinstance(aa, float) and isinstance(bb, float):
            equal = abs(aa-bb) <= 1e-7
        else:
            equal = aa == bb
        if not equal:
            mismatches[key] = {"native": a, "windows": b}
    native_markers = read_markers(OUT / "phases.csv")
    windows_markers = read_markers(WIN_OUT / "phases.csv")
    return {"passed": bool(native and windows and not mismatches), "mismatches": mismatches,
        "native_loaded_weather": native_markers.get("loaded_state", {}).get("weather"),
        "windows_loaded_weather": windows_markers.get("loaded_state", {}).get("weather"),
        "loaded_state_weather_differs": native_markers.get("loaded_state", {}).get("weather") != windows_markers.get("loaded_state", {}).get("weather"),
        "note": "Initial saved weather names are reported separately; committed and measured clear2 fields are compared here."}


def validate_conditions(rows: list[dict[str, str]]) -> dict[str, Any]:
    failures: list[str] = []
    if not rows:
        failures.append("no phase-1 rows")
    for idx, row in enumerate(rows):
        p = f"phase-1 row {idx}"
        if row.get("level") != LEVEL: failures.append(f"{p}: level")
        if row.get("weather") != WEATHER: failures.append(f"{p}: weather")
        if row.get("calendar") != CALENDAR: failures.append(f"{p}: calendar")
        if int(row["width"]) != 1920 or int(row["height"]) != 1200: failures.append(f"{p}: drawable")
        if abs(finite(row, "device_time_factor") - 1.0) > 1e-6: failures.append(f"{p}: Device factor")
        if abs(finite(row, "game_time_factor") - 1e-6) > 1e-8: failures.append(f"{p}: game factor")
        for key, expected in zip(("camera_x", "camera_y", "camera_z"), CAMERA):
            if abs(finite(row, key) - expected) > 2e-5: failures.append(f"{p}: {key}")
        for key, expected in zip(("dir_x", "dir_y", "dir_z"), DIRECTION):
            if abs(finite(row, key) - expected) > 1e-3: failures.append(f"{p}: {key}")
    return {"rows": len(rows), "passed": not failures, "failures": failures[:50]}


def validate_markers(markers: dict[str, dict[str, str]], weather_rows: list[dict[str, str]]) -> dict[str, Any]:
    required = ("loaded_state", "weather_pending", "weather_committed", "measure_start", "measure_end", "post_window_end")
    missing = [name for name in required if name not in markers]
    failures: list[str] = [f"missing marker {name}" for name in missing]
    for name in required:
        row = markers.get(name)
        if not row: continue
        if row.get("level") != LEVEL or row.get("calendar") != CALENDAR:
            failures.append(f"{name}: level/calendar")
        if abs(float(row.get("device_time_factor", "nan")) - 1.0) > 1e-6:
            failures.append(f"{name}: Device factor")
        if abs(float(row.get("game_time_factor", "nan")) - 1e-6) > 1e-8:
            failures.append(f"{name}: game factor")
        if row.get("drawable") != "1920x1200": failures.append(f"{name}: drawable")
        if name != "loaded_state" and row.get("weather") != WEATHER: failures.append(f"{name}: weather")
    by_marker = {row.get("marker"): row for row in weather_rows}
    committed = by_marker.get("weather_committed", {})
    if committed.get("paused", "").lower() != "true": failures.append("weather_committed not paused")
    if abs(float(committed.get("weight", "nan"))) > 1e-5: failures.append("weather_committed weight")
    if len(weather_rows) != len(markers): failures.append("weather/phase marker row count mismatch")
    return {"phase_markers": len(markers), "weather_rows": len(weather_rows),
        "weather_committed": {k: committed.get(k) for k in ("paused", "weight", "far_plane", "fog_distance", "wind_velocity", "sun_dir")},
        "passed": not failures, "failures": failures}


def timeline_stats(timeline_path: Path, start_epoch: int, end_epoch: int) -> dict[str, Any]:
    doc = json.loads(timeline_path.read_text(errors="strict"))
    layers = [layer for proc in doc for layer in proc.get("Layers", [])]
    if len(layers) != 1:
        raise ValueError(f"expected one Metal layer, got {len(layers)}")
    layer = layers[0]
    data = layer["On-Glass Drawable Timeline"]
    start = dt.datetime.fromtimestamp(start_epoch, dt.timezone.utc)
    end = dt.datetime.fromtimestamp(end_epoch, dt.timezone.utc)
    selected = []
    for item in data:
        a = dt.datetime.fromisoformat(item["Start Date"].replace("Z", "+00:00"))
        b = dt.datetime.fromisoformat(item["End Date"].replace("Z", "+00:00"))
        if a >= start and b <= end:
            fps = float(item["Instantaneous Frames Per Second"])
            if fps > 0 and math.isfinite(fps):
                selected.append((a, b, fps, item))
    frame_intervals_ms = [float(item[3]["Duration MCT Seconds"]) * 1000.0 for item in selected]
    low, low_count = interval_one_percent_low(frame_intervals_ms)
    reciprocal_fps = [1000.0 / value for value in frame_intervals_ms]
    gaps = [max(0.0, (selected[i+1][0] - selected[i][1]).total_seconds()) for i in range(len(selected)-1)]
    config = layer.get("Configuration Timeline", [])
    drawable_info = [item[3].get("In-Flight Drawable Info", {}) for item in selected]
    gpu_ms = [float(row["On-GPU Time (seconds)"]) * 1000.0 for row in drawable_info]
    inflight_cpu_ms = [float(row["CPU End-to-End Walltime Interval"]["Duration MCT Seconds"]) * 1000.0 for row in drawable_info]
    flag_keys = ("MetalFX Enabled", "MetalFX Frame Interpolator Enabled", "MetalFX Auto Exposure Enabled")
    size_keys = ("MetalFX Input Width", "MetalFX Input Height", "MetalFX Target Width", "MetalFX Target Height")
    flags = {key: sorted({str(row.get(key)) for row in drawable_info}) for key in flag_keys}
    sizes = {key: sorted({str(row.get(key)) for row in drawable_info}) for key in size_keys}
    return {
        "event_count": len(frame_intervals_ms),
        "marker_window_event_count_rate_fps": len(frame_intervals_ms) / (end_epoch-start_epoch),
        "interval_throughput_fps": len(frame_intervals_ms) / sum(frame_intervals_ms) * 1000.0,
        "mean_instantaneous_reciprocal_fps_supplemental": statistics.mean(reciprocal_fps),
        "frame_interval_ms_p50_nearest_rank": nearest_rank(frame_intervals_ms, .50),
        "frame_interval_ms_p99_nearest_rank": nearest_rank(frame_intervals_ms, .99),
        "one_percent_low_fps_reciprocal_slowest_interval_mean": low,
        "one_percent_low_sample_count": low_count,
        "arithmetic_mean_lowest_instantaneous_fps_supplemental": low_one_percent(reciprocal_fps)[0],
        "min_reciprocal_interval_fps_supplemental": min(reciprocal_fps), "max_reciprocal_interval_fps_supplemental": max(reciprocal_fps),
        "first_utc": selected[0][0].isoformat() if selected else None,
        "last_utc": selected[-1][1].isoformat() if selected else None,
        "max_event_gap_ms": max(gaps, default=0.0) * 1000.0,
        "slowest_intervals_ms_top5": sorted(frame_intervals_ms, reverse=True)[:5],
        "gpu_elapsed_ms": distribution(gpu_ms, "ms"),
        "inflight_cpu_end_to_end_ms": distribution(inflight_cpu_ms, "ms"),
        "gpu_elapsed_zero_sample_count": sum(value == 0 for value in gpu_ms),
        "all_events_within_marker_bounds": bool(selected) and selected[0][0] >= start and selected[-1][1] <= end,
        "source_window_seconds": (end_epoch-start_epoch),
        "trace_processes": [{"pid": proc.get("PID"), "process": proc.get("Process")} for proc in doc],
        "layer_configuration": [{key: row.get(key) for key in ("Name", "Width (pixels)", "Height (pixels)", "MTL Pixel Format Name")} for row in config],
        "measured_drawable_metalfx_flags": flags,
        "measured_drawable_metalfx_dimensions": sizes,
    }


def callback_stats(rows: list[dict[str, str]]) -> dict[str, Any]:
    if len(rows) < 2:
        raise ValueError("not enough measured callback rows")
    frames = [int(row["engine_frame"]) for row in rows]
    wall = [int(row["wall_ms"]) for row in rows]
    intervals: list[float] = []
    per_frame_fps: list[float] = []
    frame_gaps: list[int] = []
    for i in range(len(rows)-1):
        df = frames[i+1] - frames[i]
        dt_ms = wall[i+1] - wall[i]
        if df <= 0 or dt_ms <= 0:
            raise ValueError("nonmonotonic measured frame or wall clock")
        frame_gaps.append(df)
        per = dt_ms / df
        intervals.extend([per] * df)
        per_frame_fps.extend([1000.0 / per] * df)
    low, low_count = interval_one_percent_low(intervals)
    duration_ms = wall[-1] - wall[0]
    total_frames = frames[-1] - frames[0]
    return {
        "measured_rows": len(rows),
        "engine_frame_first": frames[0], "engine_frame_last": frames[-1],
        "frame_delta_min": min(frame_gaps), "frame_delta_max": max(frame_gaps),
        "callback_fps_mean_throughput": total_frames * 1000.0 / duration_ms,
        "callback_interval_ms_p50_nearest_rank": nearest_rank(intervals, .50),
        "callback_interval_ms_p99_nearest_rank": nearest_rank(intervals, .99),
        "callback_frame_interval_ms_max": max(intervals),
        "callback_instantaneous_fps_p50_nearest_rank": nearest_rank(per_frame_fps, .50),
        "callback_instantaneous_fps_p99_nearest_rank": nearest_rank(per_frame_fps, .99),
        "callback_one_percent_low_fps_reciprocal_slowest_interval_mean": low,
        "callback_arithmetic_mean_lowest_instantaneous_fps_supplemental": low_one_percent(per_frame_fps)[0],
        "one_percent_low_sample_count": low_count,
        "interval_count": len(intervals), "duration_ms": duration_ms,
    }


def e01_summary(path: Path, first_frame: int, last_frame: int) -> dict[str, Any]:
    rows = [row for row in read_csv(path) if first_frame <= int(row["engine_frame"]) <= last_frame]
    statuses = sorted({row["status"] for row in rows})
    def med(field: str) -> float:
        return statistics.median(float(row[field]) for row in rows)
    return {"rows_inclusive_frame_bounds": len(rows), "statuses": statuses,
        "all_completed": bool(rows) and statuses == ["completed"],
        "scene_cpu_ms_p50": med("scene_ms"), "encode_cpu_ms_p50": med("encode_ms"),
        "drawable_acquire_ms_p50": med("drawable_ms"),
        "indexed_draws_p50": nearest_rank([float(row["indexed_draws"]) for row in rows], .50)}


def main() -> int:
    manifest = STAGE / "candidate-r1-current-baseline-01/source-manifest.json"
    exe = STAGE / "candidate-r1-current-baseline-01/Anomaly.app/Contents/MacOS/AnomalyDX11"
    profile = OUT / "matched_profile.lua"
    result_path = ANALYSIS / "native-windows-summary-v2.json"
    phases = read_markers(OUT / "phases.csv")
    start = int(phases["measure_start"]["epoch_s"])
    end = int(phases["measure_end"]["epoch_s"])
    device = [row for row in read_csv(OUT / "phases.csv.device.csv") if row.get("phase") == "1"]
    weather = read_csv(OUT / "phases.csv.weather.csv")
    state_checks = validate_conditions(device)
    marker_checks = validate_markers(phases, weather)
    if not state_checks["passed"] or not marker_checks["passed"]:
        raise SystemExit("state/weather condition validation failed; see generated diagnostics")
    onglass = timeline_stats(OUT / "metal-timeline.json", start, end)
    callback = callback_stats(device)
    e01 = e01_summary(OUT / "e01-frames.csv", int(phases["measure_start"]["frame"]), int(phases["measure_end"]["frame"]))
    exe_hash, manifest_hash, profile_hash = sha(exe), sha(manifest), sha(profile)
    manifest_data = json.loads(manifest.read_text(errors="strict"))
    if exe_hash != EXPECTED["exe_sha256"] or manifest_hash != EXPECTED["manifest_sha256"] or profile_hash != EXPECTED["profile_sha256"]:
        raise SystemExit("candidate, manifest, or matched profile hash mismatch")
    # The game rewrites user.ltx on clean shutdown. Use its frozen preparation
    # receipt rather than mistaking normal post-run writes for pre-run inputs.
    user_ltx_hash = shared_state_hash(OUT / "shared-state-sha256.txt", "/appdata/user.ltx")
    if user_ltx_hash != EXPECTED["user_ltx_sha256"]:
        raise SystemExit("native baseline pre-run user.ltx receipt mismatch")
    native_save_hash = shared_state_hash(OUT / "shared-state-sha256.txt", "/savedgames/rescue-cordon-control.scop")
    win_result_path = WIN_OUT / "result.json"
    win_result = json.loads(win_result_path.read_text(errors="strict"))
    if not win_result.get("result_is_acceptance") or not win_result.get("primary_unchanged"):
        raise SystemExit("Windows run04 did not pass its condition/primary-integrity gate")
    win_markers = read_markers(WIN_OUT / "phases.csv")
    win_start, win_end = int(win_markers["measure_start"]["epoch_s"]), int(win_markers["measure_end"]["epoch_s"])
    win_device = [row for row in read_csv(WIN_OUT / "phases.csv.device.csv") if row.get("phase") == "1"]
    win_weather = read_csv(WIN_OUT / "phases.csv.weather.csv")
    win_state_checks = validate_conditions(win_device)
    win_marker_checks = validate_markers(win_markers, win_weather)
    if not win_state_checks["passed"] or not win_marker_checks["passed"]:
        raise SystemExit("Windows state/weather condition validation failed")
    win_on_glass = timeline_stats(WIN_OUT / "metal-timeline.json", win_start, win_end)
    win_callback = callback_stats(win_device)
    weather_pair = compare_committed_weather(weather, win_weather)
    win_clone = win_result.get("clone_preflight", {})
    if win_clone.get("user_ltx_sha256") != EXPECTED["user_ltx_sha256"]:
        raise SystemExit("Windows run04 preflight user.ltx pin mismatch")
    win_save_hash = win_clone.get("save_sha256")
    if native_save_hash != win_save_hash:
        raise SystemExit("native and Windows save bytes differ")
    if win_result.get("clone_preflight", {}).get("game_exe_sha256") != "c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3":
        raise SystemExit("Windows stock EXE pin mismatch")
    if win_result.get("clone_preflight", {}).get("hook_sha256") is None:
        raise SystemExit("Windows fresh private hook receipt missing")
    hosts = win_result.get("new_game_host_processes", {})
    if len(hosts) != 1:
        raise SystemExit("Windows run04 must have exactly one game host PID")
    win_host_pid = int(next(iter(hosts)))
    if win_result.get("module_load_evidence", {}).get("game_host_pid") != win_host_pid:
        raise SystemExit("Windows game host PID disagrees with module attribution")
    if win_on_glass["trace_processes"][0]["pid"] != win_host_pid:
        raise SystemExit("Windows MetalPerfTrace timeline PID is not the actual game host")
    if win_result.get("hook_execution_receipt_verified") is not True or win_result.get("loaded_save_path_verified") is not True:
        raise SystemExit("Windows run04 hook/save identity gates failed")
    if sha(WIN_OUT / "matched_profile_saved_slowclock_20s.lua") != EXPECTED["profile_sha256"]:
        raise SystemExit("Windows staged 20s Lua profile pin mismatch")
    native_exit = (OUT / "exit-status.txt").read_text().strip()
    if native_exit != "0":
        raise SystemExit("native baseline did not exit cleanly")
    input_files = [
        OUT / "phases.csv", OUT / "phases.csv.device.csv", OUT / "phases.csv.weather.csv",
        OUT / "e01-frames.csv", OUT / "e01-frames.csv.presented.csv", OUT / "metal-timeline.json",
        OUT / "matched_profile.lua", OUT / "environment.txt", OUT / "runtime-manifest.json", OUT / "shared-state-sha256.txt",
        WIN_OUT / "phases.csv", WIN_OUT / "phases.csv.device.csv", WIN_OUT / "phases.csv.weather.csv",
        WIN_OUT / "metal-timeline.json", WIN_OUT / "matched_profile_saved_slowclock_20s.lua", WIN_OUT / "result.json",
    ]
    summary = {
        "schema": "native-vs-windows-measured-window-summary/v1",
        "scope": "matched fixed-camera diagnostic summary; no global winner or quality acceptance",
        "baseline_identity": {
            "executable_sha256": exe_hash, "source_manifest_sha256": manifest_hash,
            "selected_source_hash_count": len(manifest_data["source_hashes"]),
            "source_scope": manifest_data.get("scope"),
            "source_provenance_limit": manifest_data.get("source_provenance_limit"),
            "matched_lua_sha256": profile_hash, "user_ltx_sha256": user_ltx_hash,
            "native_save_sha256": native_save_hash,
            "windows_stock_exe_sha256": win_result.get("clone_preflight", {}).get("game_exe_sha256"),
            "windows_save_sha256": win_save_hash,
            "windows_primary_unchanged": win_result.get("primary_unchanged"),
            "windows_hook_receipt_verified": win_result.get("hook_execution_receipt_verified"),
            "windows_loaded_save_path_verified": win_result.get("loaded_save_path_verified"),
            "windows_game_host_pid": win_host_pid,
            "windows_mo2_launcher_pid": win_result.get("owned_mo2_launcher_pid"),
            "metal_performance_per_frame_signposts": "METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS=1",
            "r1_policy": "XRAY_METAL_R1_STATIC_POLICY=0",
            "flora_portal_roi": "XRAY_METAL_FLORA_PORTAL_ROI=0",
        },
        "measured_window": {
            "start_epoch_s": start, "end_epoch_s": end, "duration_s": end-start,
            "start_engine_frame": int(phases["measure_start"]["frame"]),
            "end_engine_frame": int(phases["measure_end"]["frame"]),
            "start_utc": dt.datetime.fromtimestamp(start, dt.timezone.utc).isoformat(),
            "end_utc": dt.datetime.fromtimestamp(end, dt.timezone.utc).isoformat(),
            "callback_state_checks": state_checks,
            "phase_marker_weather_checks": marker_checks,
            "windows_start_epoch_s": win_start, "windows_end_epoch_s": win_end,
            "windows_start_utc": dt.datetime.fromtimestamp(win_start, dt.timezone.utc).isoformat(),
            "windows_end_utc": dt.datetime.fromtimestamp(win_end, dt.timezone.utc).isoformat(),
            "windows_callback_state_checks": win_state_checks,
            "windows_phase_marker_weather_checks": win_marker_checks,
            "committed_weather_comparison": weather_pair,
        },
        "on_glass_display_fps": onglass,
        "lua_callback_fps_and_frame_intervals": callback,
        "windows_on_glass_display_fps": win_on_glass,
        "windows_lua_callback_fps_and_frame_intervals": win_callback,
        "e01_diagnostic": e01,
        "instrumentation_comparison": {
            "native_e01_enabled": True,
            "windows_e01_enabled": False,
            "native_lua_per_frame_csv_enabled": True,
            "windows_lua_per_frame_csv_enabled": True,
            "interpretation": "Native includes E01 clock reads and async CSV logging; Windows does not. On-Glass/callback numbers are diagnostic, not a clean uninstrumented A/B."
        },
        "limitations": [
            "E01 is enabled: the backend conditionally reads high-resolution clocks at frame stages and writes one CSV row from command-buffer completion under a mutex. It perturbs the measured workload; treat callback and On-Glass values as instrumented diagnostic baseline, not clean final FPS acceptance.",
            "The Lua observer also writes a per-frame device CSV during the measured phase.",
            "No visual/quality proof, shadow-effectiveness counter, or Windows matched result is analyzed here. A baseline is not a winner or cross-backend decision.",
            "The R1 manifest covers 48 selected source inputs; its own receipt states it is not a proof of the full repository/build dependency closure."
        ],
        "input_sha256": {str(path.relative_to(STAGE)): sha(path) for path in input_files},
    }
    result_path.write_text(json.dumps(summary, indent=2) + "\n")
    write_markdown(summary, ANALYSIS / "RESULT-v2.md")
    print(json.dumps({"written": [str(result_path), str(ANALYSIS / "RESULT-v2.md")],
        "on_glass": onglass, "callback": callback, "state_rows": len(device), "e01": e01}, indent=2))
    return 0


def write_markdown(summary: dict[str, Any], path: Path) -> None:
    glass = summary["on_glass_display_fps"]
    callback = summary["lua_callback_fps_and_frame_intervals"]
    checks = summary["measured_window"]
    e01 = summary["e01_diagnostic"]
    win_glass = summary["windows_on_glass_display_fps"]
    win_callback = summary["windows_lua_callback_fps_and_frame_intervals"]
    identity = summary["baseline_identity"]
    text = f'''# Matched native/Silo measured-window diagnostics (v2)\n\nThese are read-only summaries of the completed fixed-camera windows. Native spans {checks['start_utc']}–{checks['end_utc']}; Windows/Silo spans {checks['windows_start_utc']}–{checks['windows_end_utc']}. Both use the same Lua SHA-256 `{identity['matched_lua_sha256']}`. All {checks['callback_state_checks']['rows']} native and {checks['windows_callback_state_checks']['rows']} Windows measured callback rows passed level, calendar, factors, weather, camera, and 1920x1200 gates. The saved-game bytes match SHA-256 `{identity['native_save_sha256']}`.\n\n| Measured metric | Native R1 Metal | Windows/Silo D3DMetal |\n|---|---:|---:|\n| On-Glass interval throughput (`N / sum(interval)`) | {glass['interval_throughput_fps']:.2f} FPS | {win_glass['interval_throughput_fps']:.2f} FPS |\n| On-Glass frame interval p50 / p99 | {glass['frame_interval_ms_p50_nearest_rank']:.2f} / {glass['frame_interval_ms_p99_nearest_rank']:.2f} ms | {win_glass['frame_interval_ms_p50_nearest_rank']:.2f} / {win_glass['frame_interval_ms_p99_nearest_rank']:.2f} ms |\n| On-Glass 1% low (`1000 / mean(slowest 1% intervals)`) | {glass['one_percent_low_fps_reciprocal_slowest_interval_mean']:.2f} ({glass['one_percent_low_sample_count']} intervals) | {win_glass['one_percent_low_fps_reciprocal_slowest_interval_mean']:.2f} ({win_glass['one_percent_low_sample_count']} intervals) |\n| Lua callback throughput | {callback['callback_fps_mean_throughput']:.2f} FPS | {win_callback['callback_fps_mean_throughput']:.2f} FPS |\n| Callback interval p50 / p99 | {callback['callback_interval_ms_p50_nearest_rank']:.0f} / {callback['callback_interval_ms_p99_nearest_rank']:.0f} ms | {win_callback['callback_interval_ms_p50_nearest_rank']:.0f} / {win_callback['callback_interval_ms_p99_nearest_rank']:.0f} ms |\n| Callback 1% low (`1000 / mean(slowest 1% intervals)`) | {callback['callback_one_percent_low_fps_reciprocal_slowest_interval_mean']:.2f} | {win_callback['callback_one_percent_low_fps_reciprocal_slowest_interval_mean']:.2f} |\n| On-GPU elapsed p50 / p99 | {glass['gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {glass['gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms | {win_glass['gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {win_glass['gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms |\n| In-flight CPU end-to-end p50 / p99 | {glass['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {glass['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms | {win_glass['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {win_glass['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms |\n\nOn-Glass interval throughput uses selected drawable count divided by the sum of their recorded MCT interval durations; it is not the mean of per-event reciprocal FPS. The marker epochs have one-second timestamp resolution, so event inclusion is bounded by those markers and the trace dates, while throughput uses only included event intervals. The On-Glass 1% low is `1000 / mean(slowest ceil(0.01*N) interval_ms)`. The callback 1% low applies the same definition to per-engine-frame intervals. The v2 JSON retains the arithmetic mean of lowest instantaneous FPS as a separately labeled supplemental metric. Frame-interval percentiles use nearest-rank quantiles. Callback throughput remains frame deltas divided by wall-clock span.\n\nOn-GPU elapsed and in-flight CPU end-to-end measurements are shown separately; they overlap in the frame pipeline and must not be added. Native On-GPU samples include {glass['gpu_elapsed_zero_sample_count']} zero values of {glass['gpu_elapsed_ms']['sample_count']}; its p50/p99 therefore need the E01/instrumentation caveat below and are not a direct cross-backend work attribution. The native trace has E01 enabled and the Windows trace does not. E01 conditionally reads high-resolution clocks and writes a completion CSV; both runs also have Lua per-frame CSV logging. These are short, instrumented diagnostics, not an uninstrumented acceptance A/B.\n\nBoth traces report a 1920x1200 BGRA8 output layer. MetalFX scaling, frame interpolation, and auto exposure are false in every measured drawable; input and target dimensions are zero. The Windows environment set `D3DM_ENABLE_METALFX=1`, but the trace confirms MetalFX was not active. Internal render-target dimensions are not independently certified. Committed weather fields match, including clear2 weight 0, far plane 648, fog distance 638, wind 430, and the recorded lighting/fog vectors. Initial loaded weather names differ before both reach paused `w_clear2`.\n\nNative executable `{identity['executable_sha256']}` is the R1 baseline with a 48-input selected-source receipt that does not prove the full build dependency closure. Windows used stock executable `{identity['windows_stock_exe_sha256']}` through the reviewed MO2/Wine D3DMetal route. Primary-integrity checks passed. No visual/quality proof or effective-shadow counter comparison is available, so this comparison declares no overall winner.\n'''
    path.write_text(text)


if __name__ == "__main__":
    raise SystemExit(main())
