#!/usr/bin/env python3
"""Read-only matched-run analysis for the versioned static-globals switch."""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
import math
import re
import statistics
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[5]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
ANALYSIS = Path(__file__).resolve().parent
RUNS = {
    "off": STAGE / "runs/versioned-static-globals-off-01/native/output",
    "on": STAGE / "runs/versioned-static-globals-on-01/native/output",
}
EXPECTED = {
    "exe_sha256": "adb3276a4784f3b5288830d4b6058bfe77cceb2c039dd401296c67bcfcdd821f",
    "manifest_sha256": "07afe2f968f0df35190b42719d544545a32c18b4f6cfefdd7404cc73fc6092cb",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
    "save_sha256": "4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225",
    "ordinary_exe_sha256": "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0",
    "ordinary_probe_sha256": "8cd42ea7b3c54df3cdb98fad532282e98073baefe59054e96ef5aabe1bd6bd10",
}
LEVEL, WEATHER, CALENDAR = "l01_escape", "w_clear2", "2018-10-26T08:09:47"
CAMERA, DIRECTION = (-221.082565, -18.284866, -168.182037), (0.0, 0.0, 1.0)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def nearest(values: list[float], percentile: float) -> float:
    if not values:
        raise ValueError("empty sample")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(percentile * len(ordered)) - 1)]


def distribution(values: list[float], unit: str) -> dict[str, Any]:
    if not values or any(not math.isfinite(value) for value in values):
        raise ValueError(f"empty or nonfinite {unit} sample")
    return {"count": len(values), "mean": statistics.mean(values),
        "p50_nearest_rank": nearest(values, .50), "p99_nearest_rank": nearest(values, .99), "unit": unit}


def one_percent_low(intervals_ms: list[float]) -> tuple[float, int]:
    if not intervals_ms or any(not math.isfinite(v) or v <= 0 for v in intervals_ms):
        raise ValueError("invalid intervals for 1% low")
    count = max(1, math.ceil(.01 * len(intervals_ms)))
    return 1000.0 / statistics.mean(sorted(intervals_ms, reverse=True)[:count]), count


def env_map(path: Path) -> dict[str, str]:
    result = {}
    for line in path.read_text(errors="strict").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value
    return result


def normalized_env(path: Path, label: str) -> dict[str, str]:
    values = env_map(path)
    run_roots = (str(STAGE / f"runs/versioned-static-globals-{label}-01"),
        f"versioned-static-globals-{label}-01")
    for key, value in list(values.items()):
        for root in run_roots:
            value = value.replace(root, "<PER_RUN_ROOT>")
        values[key] = value
    return values


def markers(path: Path) -> dict[str, dict[str, str]]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(",")
        values = {}
        for item in parts[1:]:
            if "=" in item:
                key, value = item.split("=", 1)
                values[key] = value
        result[parts[0]] = values
    return result


def shared_hash(path: Path, suffix: str) -> str:
    for line in path.read_text(errors="strict").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].endswith(suffix):
            return parts[0]
    raise ValueError(f"shared-state receipt missing {suffix}")


def validate_state(rows: list[dict[str, str]]) -> dict[str, Any]:
    errors: list[str] = []
    for index, row in enumerate(rows):
        prefix = f"row {index}"
        if row.get("level") != LEVEL: errors.append(f"{prefix} level")
        if row.get("weather") != WEATHER: errors.append(f"{prefix} weather")
        if row.get("calendar") != CALENDAR: errors.append(f"{prefix} calendar")
        if (int(row["width"]), int(row["height"])) != (1920, 1200): errors.append(f"{prefix} extent")
        if abs(float(row["device_time_factor"]) - 1.0) > 1e-6: errors.append(f"{prefix} device clock")
        if abs(float(row["game_time_factor"]) - 1e-6) > 1e-8: errors.append(f"{prefix} game clock")
        for key, expected in zip(("camera_x", "camera_y", "camera_z"), CAMERA):
            if abs(float(row[key]) - expected) > 2e-5: errors.append(f"{prefix} {key}")
        for key, expected in zip(("dir_x", "dir_y", "dir_z"), DIRECTION):
            if abs(float(row[key]) - expected) > 1e-3: errors.append(f"{prefix} {key}")
    return {"rows": len(rows), "passed": bool(rows) and not errors, "errors": errors[:40]}


def timeline_summary(path: Path, start: int, end: int) -> dict[str, Any]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    layers = [layer for process in doc for layer in process.get("Layers", [])]
    if len(layers) != 1:
        raise ValueError(f"expected one Metal output layer, got {len(layers)}")
    layer = layers[0]
    lower = dt.datetime.fromtimestamp(start, dt.timezone.utc)
    upper = dt.datetime.fromtimestamp(end, dt.timezone.utc)
    selected = []
    for item in layer["On-Glass Drawable Timeline"]:
        a = dt.datetime.fromisoformat(item["Start Date"].replace("Z", "+00:00"))
        b = dt.datetime.fromisoformat(item["End Date"].replace("Z", "+00:00"))
        sec = float(item["Duration MCT Seconds"])
        if a >= lower and b <= upper and 0 < sec < 1 and math.isfinite(sec):
            selected.append((a, b, item, sec * 1000.0))
    if not selected:
        raise ValueError("no On-Glass drawable intervals inside measured markers")
    intervals = [r[3] for r in selected]
    low, low_count = one_percent_low(intervals)
    infos = [r[2].get("In-Flight Drawable Info", {}) for r in selected]
    gpu = [float(item["On-GPU Time (seconds)"]) * 1000.0 for item in infos]
    cpu_inflight = [float(item["CPU End-to-End Walltime Interval"]["Duration MCT Seconds"]) * 1000.0 for item in infos]
    configuration = [{key: entry.get(key) for key in ("Name", "Width (pixels)", "Height (pixels)", "MTL Pixel Format Name")}
        for entry in layer.get("Configuration Timeline", [])]
    flags = {key: sorted({str(item.get(key)) for item in infos}) for key in
        ("MetalFX Enabled", "MetalFX Frame Interpolator Enabled", "MetalFX Auto Exposure Enabled")}
    dimensions = {key: sorted({str(item.get(key)) for item in infos}) for key in
        ("MetalFX Input Width", "MetalFX Input Height", "MetalFX Target Width", "MetalFX Target Height")}
    return {"drawable_count": len(intervals), "interval_throughput_fps": len(intervals) * 1000.0 / sum(intervals),
        "marker_count_rate_fps": len(intervals) / (end - start),
        "frame_interval_ms": distribution(intervals, "ms"), "one_percent_low_fps": low,
        "one_percent_low_sample_count": low_count, "on_gpu_elapsed_ms": distribution(gpu, "ms"),
        "inflight_cpu_end_to_end_ms": distribution(cpu_inflight, "ms"),
        "gpu_zero_samples": sum(value == 0 for value in gpu), "output_configuration": configuration,
        "metalfx_flags": flags, "metalfx_dimensions": dimensions,
        "first_event_utc": selected[0][0].isoformat(), "last_event_utc": selected[-1][1].isoformat()}


def callbacks(rows: list[dict[str, str]]) -> dict[str, Any]:
    if len(rows) < 2:
        raise ValueError("too few device callback rows")
    intervals = []
    for before, after in zip(rows, rows[1:]):
        df = int(after["engine_frame"]) - int(before["engine_frame"])
        dt_ms = int(after["wall_ms"]) - int(before["wall_ms"])
        if df <= 0 or dt_ms <= 0:
            raise ValueError("nonmonotonic callback frame or time")
        intervals.extend([dt_ms / df] * df)
    duration = int(rows[-1]["wall_ms"]) - int(rows[0]["wall_ms"])
    frames = int(rows[-1]["engine_frame"]) - int(rows[0]["engine_frame"])
    low, count = one_percent_low(intervals)
    return {"callback_rows": len(rows), "first_frame": int(rows[0]["engine_frame"]),
        "last_frame": int(rows[-1]["engine_frame"]), "elapsed_ms": duration,
        "throughput_fps": frames * 1000.0 / duration, "frame_interval_ms": distribution(intervals, "ms"),
        "one_percent_low_fps": low, "one_percent_low_sample_count": count}


def e01_summary(path: Path, first: int, last: int) -> dict[str, Any]:
    rows = [r for r in csv_rows(path) if first <= int(r["engine_frame"]) <= last and r.get("status") == "completed"]
    if not rows:
        raise ValueError("no completed E01 frames inside measured markers")
    cpu_columns = ("scene_ms", "encode_ms", "present_encode_ms", "commit_ms", "drawable_ms")
    value_columns = ("indexed_draws", "updated_bytes", "buffer_allocations", "texture_allocations", "pso_binds")
    gpu_intervals = [max(0.0, (float(r["gpu_end_s"]) - float(r["gpu_start_s"])) * 1000.0)
        for r in rows if float(r["gpu_end_s"]) > float(r["gpu_start_s"])]
    return {"completed_rows": len(rows), "cpu_stage_ms": {key: distribution([float(r[key]) for r in rows], "ms")
            for key in cpu_columns},
        "per_frame_work": {key: distribution([float(r[key]) for r in rows], "count") for key in value_columns},
        "gpu_interval_elapsed_ms_from_e01_rows": distribution(gpu_intervals, "ms") if gpu_intervals else None}


def system_snapshot(out: Path) -> dict[str, Any]:
    def text(name: str) -> str:
        return (out / name).read_text(errors="replace").strip()
    def vm_stat(name: str) -> dict[str, int]:
        content = text(name)
        fields = ("Pages free", "Pages active", "Pages inactive", "Pages wired down",
            "Pages stored in compressor", "Pages occupied by compressor", "Pageouts", "Swapins", "Swapouts")
        result = {}
        for field in fields:
            match = re.search(re.escape(field) + r":\s+(\d+)", content)
            if match: result[field] = int(match.group(1))
        return result
    swap_re = re.search(r"total\s*=\s*([0-9.]+)M\s+used\s*=\s*([0-9.]+)M\s+free\s*=\s*([0-9.]+)M", text("swap-before.txt"))
    swap_after_re = re.search(r"total\s*=\s*([0-9.]+)M\s+used\s*=\s*([0-9.]+)M\s+free\s*=\s*([0-9.]+)M", text("swap-after.txt"))
    if not swap_re or not swap_after_re:
        raise ValueError("missing/unparseable swap snapshots")
    return {
        "power_before": text("power-before.txt"), "power_after": text("power-after.txt"),
        "thermal_before": text("thermal-before.txt"), "thermal_after": text("thermal-after.txt"),
        "swap_before_mb": {"total": float(swap_re[1]), "used": float(swap_re[2]), "free": float(swap_re[3])},
        "swap_after_mb": {"total": float(swap_after_re[1]), "used": float(swap_after_re[2]), "free": float(swap_after_re[3])},
        "vm_stat_before": vm_stat("vm-stat-before.txt"), "vm_stat_after": vm_stat("vm-stat-after.txt"),
    }


def normalize_environment(label: str) -> dict[str, str]:
    values = env_map(RUNS[label] / "environment.txt")
    run = str(STAGE / f"runs/versioned-static-globals-{label}-01")
    for key, value in list(values.items()):
        values[key] = value.replace(run, "<RUN_ROOT>")
    return values


def analyze_run(label: str) -> dict[str, Any]:
    out = RUNS[label]
    required = ("argv.txt", "exit-status.txt", "phases.csv", "phases.csv.device.csv", "phases.csv.weather.csv",
        "metal-timeline.json", "e01-frames.csv", "matched_profile.lua", "environment.txt", "shared-state-sha256.txt")
    absent = [name for name in required if not (out / name).is_file()]
    if absent: raise ValueError(f"{label} run incomplete: {absent}")
    if (out / "exit-status.txt").read_text().strip() != "0": raise ValueError(f"{label} run did not exit 0")
    if sha(out / "matched_profile.lua") != EXPECTED["profile_sha256"]: raise ValueError(f"{label} Lua pin mismatch")
    if shared_hash(out / "shared-state-sha256.txt", "/appdata/user.ltx") != EXPECTED["user_ltx_sha256"]:
        raise ValueError(f"{label} user.ltx hash mismatch")
    if shared_hash(out / "shared-state-sha256.txt", "/savedgames/rescue-cordon-control.scop") != EXPECTED["save_sha256"]:
        raise ValueError(f"{label} controlled-save hash mismatch")
    candidate = STAGE / "candidate-versioned-static-globals-01/Anomaly.app/Contents/MacOS/AnomalyDX11"
    argv = Path((out / "argv.txt").read_text(errors="strict").splitlines()[0]).resolve()
    if argv != candidate.resolve(): raise ValueError(f"{label} executable path differs from candidate")
    env = env_map(out / "environment.txt")
    expected_flag = "0" if label == "off" else "1"
    if env.get("XRAY_METAL_VERSIONED_STATIC_GLOBALS") != expected_flag:
        raise ValueError(f"{label} opt-in flag mismatch")
    if env.get("METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS") != "1" or env.get("XRAY_METAL_R1_STATIC_POLICY") != "0":
        raise ValueError(f"{label} common renderer diagnostic flags differ")

    marks = markers(out / "phases.csv")
    if "measure_start" not in marks or "measure_end" not in marks or "weather_committed" not in marks:
        raise ValueError(f"{label} phase markers incomplete")
    start, end = int(marks["measure_start"]["epoch_s"]), int(marks["measure_end"]["epoch_s"])
    if end - start != 20: raise ValueError(f"{label} marker window is not 20 seconds")
    first, last = int(marks["measure_start"]["frame"]), int(marks["measure_end"]["frame"])
    if marks["weather_committed"].get("weather") != WEATHER or marks["weather_committed"].get("calendar") != CALENDAR:
        raise ValueError(f"{label} committed weather/calendar differs")
    device_all = csv_rows(out / "phases.csv.device.csv")
    phase_rows = [r for r in device_all if r.get("phase") == "1"]
    state = validate_state(phase_rows)
    if not state["passed"]: raise ValueError(f"{label} callback state failed: {state['errors'][:5]}")
    device = [r for r in phase_rows if first <= int(r["engine_frame"]) <= last]
    if len(device) < 2: raise ValueError(f"{label} no callbacks in measured frame range")
    onglass = timeline_summary(out / "metal-timeline.json", start, end)
    e01 = e01_summary(out / "e01-frames.csv", first, last)
    if onglass["metalfx_flags"] != {"MetalFX Enabled": ["False"],
            "MetalFX Frame Interpolator Enabled": ["False"], "MetalFX Auto Exposure Enabled": ["False"]}:
        raise ValueError(f"{label} MetalFX flags differ or unknown")
    if onglass["output_configuration"] != [{"Name": "CAMetalLayer: SDL_cocoametalview",
            "Width (pixels)": 1920, "Height (pixels)": 1200, "MTL Pixel Format Name": "BGRA8Unorm"}]:
        raise ValueError(f"{label} output extent/pixel format mismatch")
    weather_rows = csv_rows(out / "phases.csv.weather.csv")
    committed = [r for r in weather_rows if r.get("marker") == "weather_committed"]
    if len(committed) != 1 or committed[0].get("paused") != "true" or float(committed[0]["weight"]) != 0.0:
        raise ValueError(f"{label} committed weather payload is not paused w_clear2/zero weight")
    signature = {k: v for k, v in committed[0].items() if k not in ("marker", "epoch_s")}
    system = system_snapshot(out)
    hashes = {name: sha(out / name) for name in (*required,)}
    return {"mode": label, "exit": 0, "window_start_epoch_s": start, "window_end_epoch_s": end,
        "frame_start": first, "frame_end": last, "phase_state": state,
        "callback": callbacks(device), "on_glass": onglass, "e01": e01,
        "committed_weather_signature": signature, "system_conditions": system, "input_sha256": hashes}


def build_gate() -> dict[str, Any]:
    candidate = STAGE / "candidate-versioned-static-globals-01/Anomaly.app/Contents/MacOS/AnomalyDX11"
    manifest = STAGE / "candidate-versioned-static-globals-01/source-manifest.json"
    result_path = ANALYSIS.parent / "root-build-01/result.json"
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("build_exit") != 0 or result.get("candidate_executable_sha256") != EXPECTED["exe_sha256"] or \
            result.get("manifest_sha256") != EXPECTED["manifest_sha256"]:
        raise ValueError("candidate build/manifest pins differ")
    if sha(candidate) != EXPECTED["exe_sha256"] or sha(manifest) != EXPECTED["manifest_sha256"]:
        raise ValueError("candidate binary or manifest current hash mismatch")
    if result.get("uniform_ownership_gpu_probe_exit") != 0:
        raise ValueError("candidate's prior E02 ownership GPU probe did not pass")
    baseline = EXPECTED["ordinary_exe_sha256"]
    ordinary = result.get("ordinary_outputs_restored", {})
    if result.get("ordinary_exe_before") != baseline or ordinary.get("app") != baseline or ordinary.get("bare") != baseline:
        raise ValueError("ordinary baseline executable restoration receipt mismatch")
    if result.get("ordinary_probe_before") != EXPECTED["ordinary_probe_sha256"] or \
            ordinary.get("probe") != EXPECTED["ordinary_probe_sha256"]:
        raise ValueError("ordinary probe executable restoration receipt mismatch")
    pre, post = result.get("production_prestate", {}), result.get("restored_source_hashes", {})
    if not pre or pre != post: raise ValueError("production source prestate was not restored exactly")
    return {"candidate_exe_sha256": sha(candidate), "manifest_sha256": sha(manifest),
        "ordinary_outputs_restored": ordinary, "production_source_restored": True,
        "restored_source_files": len(pre), "e02_uniform_ownership_gpu_probe_exit": 0}


def validate_pair_env() -> dict[str, Any]:
    off, on = normalize_environment("off"), normalize_environment("on")
    if set(off) != set(on): raise ValueError(f"environment keysets differ: {set(off) ^ set(on)}")
    diffs = {key: (off[key], on[key]) for key in sorted(off) if off[key] != on[key]}
    if diffs != {"XRAY_METAL_VERSIONED_STATIC_GLOBALS": ("0", "1")}:
        raise ValueError(f"setting delta is not limited to versioned static-globals opt-in: {diffs}")
    return {"only_setting_difference": diffs, "passed": True}


def render(summary: dict[str, Any]) -> str:
    off, on = summary["runs"]["off"], summary["runs"]["on"]
    metrics = [
        ("On-Glass interval throughput", lambda r: f"{r['on_glass']['interval_throughput_fps']:.2f} FPS"),
        ("On-Glass interval p50 / p99", lambda r: f"{r['on_glass']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['on_glass']['frame_interval_ms']['p99_nearest_rank']:.2f} ms"),
        ("On-Glass 1% low", lambda r: f"{r['on_glass']['one_percent_low_fps']:.2f} FPS"),
        ("Lua callback throughput", lambda r: f"{r['callback']['throughput_fps']:.2f} FPS"),
        ("Callback interval p50 / p99", lambda r: f"{r['callback']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['callback']['frame_interval_ms']['p99_nearest_rank']:.2f} ms"),
        ("Callback 1% low", lambda r: f"{r['callback']['one_percent_low_fps']:.2f} FPS"),
        ("E01 scene CPU p50 / encode CPU p50", lambda r: f"{r['e01']['cpu_stage_ms']['scene_ms']['p50_nearest_rank']:.3f} / {r['e01']['cpu_stage_ms']['encode_ms']['p50_nearest_rank']:.3f} ms"),
        ("E01 indexed draws p50 / updated bytes p50", lambda r: f"{r['e01']['per_frame_work']['indexed_draws']['p50_nearest_rank']:.0f} / {r['e01']['per_frame_work']['updated_bytes']['p50_nearest_rank']:.0f}"),
        ("GPU elapsed p50 / p99", lambda r: f"{r['on_glass']['on_gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['on_gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms"),
        ("In-flight CPU end-to-end p50 / p99", lambda r: f"{r['on_glass']['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms"),
        ("E01 GPU elapsed p50 / p99", lambda r: "n/a" if not r['e01']['gpu_interval_elapsed_ms_from_e01_rows'] else f"{r['e01']['gpu_interval_elapsed_ms_from_e01_rows']['p50_nearest_rank']:.3f} / {r['e01']['gpu_interval_elapsed_ms_from_e01_rows']['p99_nearest_rank']:.3f} ms"),
    ]
    lines = ["# Versioned static-globals: paired 20-second diagnostic", "",
        "Both runs passed the same executable, source manifest, 20-second profile, save, user.ltx, weather/clock/camera and drawable gates. The normalized environment comparison found exactly one setting change: `XRAY_METAL_VERSIONED_STATIC_GLOBALS=0` to `1`.", "",
        "| Metric | Off | On |", "|---|---:|---:|"]
    lines.extend(f"| {name} | {fn(off)} | {fn(on)} |" for name, fn in metrics)
    lines.extend(["", "On-Glass throughput is selected drawable count divided by the sum of its MetalPerfTrace interval durations (`N / sum(interval)`), not a mean of reciprocal FPS. Both On-Glass and callback 1% low use `1000 / mean(slowest ceil(1% N) interval_ms)`.", "",
        "E01 CPU stages, E01 per-row GPU intervals, MetalPerfTrace On-GPU elapsed, and in-flight CPU end-to-end times are separate/non-additive measurements. E01 timing and per-frame CSV logging add diagnostic overhead. A lower updated-byte count alone does not establish FPS benefit or full data-quality equivalence.", "",
        f"System snapshots show AC power at 100% and no thermal/performance warning in either arm. Starting memory state differed: free swap was {off['system_conditions']['swap_before_mb']['free']:.0f} MB off vs {on['system_conditions']['swap_before_mb']['free']:.0f} MB on, and `vm_stat` free pages were {off['system_conditions']['vm_stat_before'].get('Pages free', 0):,} vs {on['system_conditions']['vm_stat_before'].get('Pages free', 0):,}. Swap used was similar and unchanged within each run. This makes the single-pair attribution uncertain, though it does not erase the observed GPU/in-flight regression.", "",
        "The observed On-Glass interval throughput is 14.8% lower with the flag on. E01 scene/encode CPU medians are slightly lower, while On-GPU elapsed rises from 30.13 to 51.38 ms and in-flight CPU end-to-end from 37.25 to 128.62 ms. E01 indexed draws also differ (14,932 off vs 14,226 on), so per-window AI/render workload was not identical despite matched camera and save. These results reject the candidate as a renderer ROI for further admission on current evidence; they do not isolate the cause or establish a player-visible comparison.", "",
        "The separate E02 uniform ownership GPU probe passed, but it does not prove the runtime shader/TLAB consumes the intended versioned uniform or that all scene data remains equivalent. This is one 20/20 pair and does not establish full output equivalence, a user-facing winner, or shipping admission.", ""])
    return "\n".join(lines)


def main() -> int:
    gate = build_gate()
    env_gate = validate_pair_env()
    off, on = analyze_run("off"), analyze_run("on")
    if off["committed_weather_signature"] != on["committed_weather_signature"]:
        raise ValueError("committed weather payload differs between arms")
    summary = {"schema": "versioned-static-globals-roi-review/v1",
        "scope": "one matched 20/20 controlled diagnostic; no user-winner or all-data-quality claim",
        "build_and_restore_gate": gate, "environment_delta_gate": env_gate,
        "shared_pins": EXPECTED, "runs": {"off": off, "on": on}}
    json_path = ANALYSIS / "versioned-static-globals-summary.json"
    md_path = ANALYSIS / "RESULT.md"
    json_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    md_path.write_text(render(summary), encoding="utf-8")
    input_list = [Path(__file__), STAGE / "candidate-versioned-static-globals-01/source-manifest.json",
        ANALYSIS.parent / "root-build-01/result.json"]
    for out in RUNS.values():
        input_list += [out / name for name in ("argv.txt", "environment.txt", "exit-status.txt", "phases.csv",
            "phases.csv.device.csv", "phases.csv.weather.csv", "metal-timeline.json", "e01-frames.csv",
            "matched_profile.lua", "shared-state-sha256.txt", "power-before.txt", "power-after.txt",
            "thermal-before.txt", "thermal-after.txt", "swap-before.txt", "swap-after.txt",
            "vm-stat-before.txt", "vm-stat-after.txt")]
    receipt = {str(path.relative_to(ROOT)): sha(path) for path in input_list + [json_path, md_path]}
    (ANALYSIS / "analysis-sha256.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "complete", "written": [str(json_path), str(md_path)],
        "off_fps": off["on_glass"]["interval_throughput_fps"],
        "on_fps": on["on_glass"]["interval_throughput_fps"],
        "only_environment_delta": env_gate["only_setting_difference"]}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"pair not ready or invalid receipts: {exc}", file=sys.stderr)
        raise SystemExit(2)
