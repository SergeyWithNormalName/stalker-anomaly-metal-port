#!/usr/bin/env python3
"""Read-only analysis for the completed per-frame portal indoor trio.

The script refuses to emit a result until all three owned runs have exited 0,
have complete marker/state/timeline receipts, and match the pinned executable,
manifest, profile, user.ltx and save. No game, build, or trace collection occurs.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[7]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
ANALYSIS = Path(__file__).resolve().parent
RUNS = {
    "observe": STAGE / "runs/portal-perframe-observe-indoor-08/native/output",
    "on": STAGE / "runs/portal-perframe-on-indoor-08/native/output",
    "off": STAGE / "runs/portal-perframe-off-indoor-08/native/output",
}
EXPECTED = {
    "exe_sha256": "23ded64b65784045459e1448115d79277de2d458d75ef95899ab641a112ab49f",
    "manifest_sha256": "077e592d0daa2971147df6aceb45e01f78e7f3a1624c33553ccac0fef42c5aa0",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
    "save_sha256": "4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225",
}
LEVEL, WEATHER = "l01_escape", "w_clear2"
CALENDAR = "2018-10-26T08:09:47"
CAMERA = (-221.082565, -18.284866, -168.182037)
DIRECTION = (0.0, 0.0, 1.0)
WINDOW_SECONDS = 20


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def nearest(values: list[float], p: float) -> float:
    if not values:
        raise ValueError("empty distribution")
    ordered = sorted(values)
    return ordered[max(0, math.ceil(p * len(ordered)) - 1)]


def summarize(values: list[float], unit: str) -> dict[str, Any]:
    if not values or any(not math.isfinite(v) for v in values):
        raise ValueError(f"empty/nonfinite values for {unit}")
    return {"count": len(values), "mean": statistics.mean(values),
        "p50_nearest_rank": nearest(values, .50), "p99_nearest_rank": nearest(values, .99),
        "unit": unit}


def reciprocal_slowest_one_percent(intervals_ms: list[float]) -> tuple[float, int]:
    if not intervals_ms or any(v <= 0 or not math.isfinite(v) for v in intervals_ms):
        raise ValueError("invalid interval sample for 1% low")
    n = max(1, math.ceil(.01 * len(intervals_ms)))
    return 1000.0 / statistics.mean(sorted(intervals_ms, reverse=True)[:n]), n


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def read_markers(path: Path) -> dict[str, dict[str, str]]:
    markers: dict[str, dict[str, str]] = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(",")
        row = {}
        for field in parts[1:]:
            if "=" in field:
                key, value = field.split("=", 1)
                row[key] = value
        markers[parts[0]] = row
    return markers


def env_map(path: Path) -> dict[str, str]:
    return dict(line.split("=", 1) for line in path.read_text(errors="replace").splitlines()
        if "=" in line and not line.lstrip().startswith("#"))


def state_gate(rows: list[dict[str, str]]) -> dict[str, Any]:
    errors: list[str] = []
    if not rows:
        errors.append("no measured device rows")
    for n, r in enumerate(rows):
        tag = f"device row {n}"
        if r.get("level") != LEVEL: errors.append(f"{tag}: level")
        if r.get("weather") != WEATHER: errors.append(f"{tag}: weather")
        if r.get("calendar") != CALENDAR: errors.append(f"{tag}: calendar")
        if (int(r["width"]), int(r["height"])) != (1920, 1200): errors.append(f"{tag}: extent")
        if abs(float(r["device_time_factor"]) - 1.0) > 1e-6: errors.append(f"{tag}: device factor")
        if abs(float(r["game_time_factor"]) - 1e-6) > 1e-8: errors.append(f"{tag}: game factor")
        for key, target in zip(("camera_x", "camera_y", "camera_z"), CAMERA):
            if abs(float(r[key]) - target) > 2e-5: errors.append(f"{tag}: {key}")
        for key, target in zip(("dir_x", "dir_y", "dir_z"), DIRECTION):
            if abs(float(r[key]) - target) > 1e-3: errors.append(f"{tag}: {key}")
    return {"rows": len(rows), "passed": not errors, "errors": errors[:30]}


def read_shared_hash(receipt: Path, suffix: str) -> str:
    for line in receipt.read_text(errors="strict").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].endswith(suffix):
            return parts[0]
    raise ValueError(f"shared state missing {suffix}")


def timeline_stats(path: Path, start_epoch: int, end_epoch: int) -> dict[str, Any]:
    doc = json.loads(path.read_text(encoding="utf-8"))
    layers = [layer for process in doc for layer in process.get("Layers", [])]
    if len(layers) != 1:
        raise ValueError(f"expected one output layer, got {len(layers)}")
    layer = layers[0]
    lower = dt.datetime.fromtimestamp(start_epoch, dt.timezone.utc)
    upper = dt.datetime.fromtimestamp(end_epoch, dt.timezone.utc)
    selected = []
    for row in layer["On-Glass Drawable Timeline"]:
        a = dt.datetime.fromisoformat(row["Start Date"].replace("Z", "+00:00"))
        b = dt.datetime.fromisoformat(row["End Date"].replace("Z", "+00:00"))
        interval = float(row["Duration MCT Seconds"])
        if a >= lower and b <= upper and interval > 0 and math.isfinite(interval):
            selected.append((a, b, row, interval * 1000.0))
    if not selected:
        raise ValueError("no On-Glass intervals in marker window")
    intervals = [item[3] for item in selected]
    low, low_count = reciprocal_slowest_one_percent(intervals)
    inf = [row[2].get("In-Flight Drawable Info", {}) for row in selected]
    gpu_ms = [float(row["On-GPU Time (seconds)"]) * 1000 for row in inf]
    cpu_ms = [float(row["CPU End-to-End Walltime Interval"]["Duration MCT Seconds"]) * 1000 for row in inf]
    flags = {key: sorted({str(row.get(key)) for row in inf}) for key in
        ("MetalFX Enabled", "MetalFX Frame Interpolator Enabled", "MetalFX Auto Exposure Enabled")}
    sizes = {key: sorted({str(row.get(key)) for row in inf}) for key in
        ("MetalFX Input Width", "MetalFX Input Height", "MetalFX Target Width", "MetalFX Target Height")}
    return {
        "event_count": len(intervals),
        "interval_throughput_fps": len(intervals) * 1000.0 / sum(intervals),
        "marker_window_event_rate_fps": len(intervals) / (end_epoch - start_epoch),
        "frame_interval_ms": summarize(intervals, "ms"),
        "one_percent_low_fps": low,
        "one_percent_low_sample_count": low_count,
        "on_gpu_elapsed_ms": summarize(gpu_ms, "ms"),
        "inflight_cpu_end_to_end_ms": summarize(cpu_ms, "ms"),
        "gpu_zero_count": sum(v == 0 for v in gpu_ms),
        "first_utc": selected[0][0].isoformat(), "last_utc": selected[-1][1].isoformat(),
        "layer": [{key: row.get(key) for key in ("Name", "Width (pixels)", "Height (pixels)", "MTL Pixel Format Name")}
            for row in layer.get("Configuration Timeline", [])],
        "metalfx_flags": flags, "metalfx_dimensions": sizes,
        "trace_processes": [{"pid": p.get("PID"), "name": p.get("Process")} for p in doc],
    }


def callback_stats(rows: list[dict[str, str]]) -> dict[str, Any]:
    if len(rows) < 2:
        raise ValueError("too few measured callback rows")
    intervals: list[float] = []
    for a, b in zip(rows, rows[1:]):
        df = int(b["engine_frame"]) - int(a["engine_frame"])
        dw = int(b["wall_ms"]) - int(a["wall_ms"])
        if df <= 0 or dw <= 0:
            raise ValueError("nonmonotonic callback frame/time")
        intervals.extend([dw / df] * df)
    duration = int(rows[-1]["wall_ms"]) - int(rows[0]["wall_ms"])
    frames = int(rows[-1]["engine_frame"]) - int(rows[0]["engine_frame"])
    low, n = reciprocal_slowest_one_percent(intervals)
    return {
        "rows": len(rows), "frame_first": int(rows[0]["engine_frame"]),
        "frame_last": int(rows[-1]["engine_frame"]), "elapsed_ms": duration,
        "callback_throughput_fps": frames * 1000.0 / duration,
        "frame_interval_ms": summarize(intervals, "ms"),
        "one_percent_low_fps": low, "one_percent_low_sample_count": n,
    }


def e01_stats(path: Path, first_frame: int, last_frame: int) -> dict[str, Any]:
    rows = [r for r in read_csv(path) if first_frame <= int(r["engine_frame"]) <= last_frame]
    rows = [r for r in rows if r.get("status") == "completed"]
    if not rows:
        raise ValueError("no completed E01 rows in measured marker frames")
    fields = ("scene_ms", "grass_ms", "dynamic_prewarm_ms", "encode_ms", "drawable_ms")
    return {"rows": len(rows), "statuses": sorted({r["status"] for r in rows}),
        "cpu_stage_ms": {key: summarize([float(r[key]) for r in rows], "ms") for key in fields}}


def portal_stats(path: Path, mode: str, first_frame: int, last_frame: int) -> dict[str, Any]:
    if not path.exists() or path.stat().st_size == 0:
        if mode != "off":
            raise ValueError(f"{mode} mode has no portal-perframe receipt")
        return {"mode": "off", "rows": 0, "qualified_rows": 0, "actual_skips": 0,
            "note": "feature disabled; no per-frame observer CSV expected"}
    all_rows = read_csv(path)
    rows = [r for r in all_rows if first_frame <= int(r["frame"]) <= last_frame]
    if not rows:
        raise ValueError(f"{mode} portal receipt has no measured rows")
    if any(r["mode"] != mode for r in rows):
        raise ValueError(f"portal receipt mode mismatch in {mode} run")
    graph = [float(r["graph_cpu_ms"]) for r in rows]
    mask = [float(r["mask_cpu_ms"]) for r in rows]
    joint = [a + b for a, b in zip(graph, mask)]
    skips = [float(r["actual_skips"]) for r in rows]
    draws = [float(r["draws"]) for r in rows]
    culled = [float(r["camera_culled"]) for r in rows]
    return {
        "mode": mode, "rows": len(rows), "qualified_rows": sum(r["qualified"] in ("1", "true", "True") for r in rows),
        "qualified_fraction": sum(r["qualified"] in ("1", "true", "True") for r in rows) / len(rows),
        "actual_skips_per_frame": summarize(skips, "draws/frame"),
        "actual_skips_total_over_window": int(sum(skips)),
        "draws_per_frame": summarize(draws, "draws/frame"),
        "camera_culled_per_frame": summarize(culled, "draws/frame"),
        "wind_unknown_sum": sum(int(r["wind_unknown"]) for r in rows),
        "owner_unknown_sum": sum(int(r["owner_unknown"]) for r in rows),
        "graph_cpu_ms": summarize(graph, "ms"), "mask_cpu_ms": summarize(mask, "ms"),
        # Graph capture in Calculate and callback mask assembly are disjoint phases.
        "graph_plus_mask_cpu_ms": summarize(joint, "ms"),
        "reasons": sorted({r["reason"] for r in rows if r["reason"]}),
    }


def analyze_run(label: str, out: Path, exe_sha: str, manifest_sha: str) -> dict[str, Any]:
    required = ("exit-status.txt", "phases.csv", "phases.csv.device.csv", "phases.csv.weather.csv",
        "metal-timeline.json", "matched_profile.lua", "e01-frames.csv", "environment.txt",
        "argv.txt", "shared-state-sha256.txt")
    if label != "off":
        required += ("portal-perframe.csv",)
    missing = [name for name in required if not (out / name).is_file()]
    if missing:
        raise ValueError(f"{label} output incomplete, missing {missing}")
    if (out / "exit-status.txt").read_text().strip() != "0":
        raise ValueError(f"{label} did not exit zero")
    if sha(out / "matched_profile.lua") != EXPECTED["profile_sha256"]:
        raise ValueError(f"{label} matched Lua profile pin mismatch")
    if read_shared_hash(out / "shared-state-sha256.txt", "/appdata/user.ltx") != EXPECTED["user_ltx_sha256"]:
        raise ValueError(f"{label} user.ltx receipt mismatch")
    save_suffix = "/savedgames/rescue-cordon-control.scop"
    if read_shared_hash(out / "shared-state-sha256.txt", save_suffix) != EXPECTED["save_sha256"]:
        raise ValueError(f"{label} controlled save receipt mismatch")
    argv = (out / "argv.txt").read_text(errors="strict").splitlines()[0]
    if Path(argv).resolve() != (STAGE / "candidate-portal-perframe-08/Anomaly.app/Contents/MacOS/AnomalyDX11").resolve():
        raise ValueError(f"{label} did not run the pinned candidate executable path")
    env = env_map(out / "environment.txt")
    expected_mode = {"observe": "observe", "on": "on", "off": "off"}[label]
    actual_mode = env.get("XRAY_METAL_FLORA_PORTAL_PERFRAME")
    if actual_mode != expected_mode:
        raise ValueError(f"{label} mode env mismatch: {actual_mode!r}")
    if env.get("XRAY_METAL_R1_STATIC_POLICY") != "0":
        raise ValueError(f"{label} R1 static policy is not off")
    if env.get("METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS") != "1":
        raise ValueError(f"{label} E01 signpost environment differs")

    markers = read_markers(out / "phases.csv")
    for marker in ("measure_start", "measure_end", "weather_committed"):
        if marker not in markers: raise ValueError(f"{label} missing marker {marker}")
    start, end = int(markers["measure_start"]["epoch_s"]), int(markers["measure_end"]["epoch_s"])
    if end - start != WINDOW_SECONDS: raise ValueError(f"{label} window is not exactly 20 marker seconds")
    first, last = int(markers["measure_start"]["frame"]), int(markers["measure_end"]["frame"])
    if markers["weather_committed"].get("weather") != WEATHER or markers["weather_committed"].get("drawable") != "1920x1200":
        raise ValueError(f"{label} committed weather/extent marker mismatch")
    devices_all = read_csv(out / "phases.csv.device.csv")
    device_rows = [r for r in devices_all if r.get("phase") == "1"]
    state = state_gate(device_rows)
    if not state["passed"]: raise ValueError(f"{label} measured state gate failed: {state['errors'][:4]}")
    measured = [r for r in device_rows if first <= int(r["engine_frame"]) <= last]
    if len(measured) < 2: raise ValueError(f"{label} not enough callbacks in marker-frame bounds")
    timeline = timeline_stats(out / "metal-timeline.json", start, end)
    if timeline["metalfx_flags"] != {
        "MetalFX Enabled": ["False"], "MetalFX Frame Interpolator Enabled": ["False"],
        "MetalFX Auto Exposure Enabled": ["False"]}:
        raise ValueError(f"{label} MetalFX/frame interpolation was active or unknown")
    if timeline["layer"] != [{"Name": "CAMetalLayer: SDL_cocoametalview", "Width (pixels)": 1920,
            "Height (pixels)": 1200, "MTL Pixel Format Name": "BGRA8Unorm"}]:
        raise ValueError(f"{label} output drawable configuration mismatch")
    callback = callback_stats(measured)
    e01 = e01_stats(out / "e01-frames.csv", first, last)
    portal = portal_stats(out / "portal-perframe.csv", label, first, last)
    if label != "off" and portal["rows"] not in (last - first, last - first + 1):
        raise ValueError(f"{label} portal rows {portal['rows']} inconsistent with marker frame span {first}..{last}")
    if label != "off" and portal["qualified_rows"] != portal["rows"]:
        raise ValueError(f"{label} has unqualified measured portal rows")
    hash_names = ["phases.csv", "phases.csv.device.csv", "phases.csv.weather.csv",
        "metal-timeline.json", "e01-frames.csv", "environment.txt", "argv.txt",
        "matched_profile.lua", "shared-state-sha256.txt"]
    if (out / "portal-perframe.csv").is_file(): hash_names.append("portal-perframe.csv")
    hashes = {name: sha(out / name) for name in hash_names}
    weather_rows = read_csv(out / "phases.csv.weather.csv")
    committed_rows = [row for row in weather_rows if row.get("marker") == "weather_committed"]
    if len(committed_rows) != 1:
        raise ValueError(f"{label} needs one committed weather receipt")
    committed = committed_rows[0]
    if committed.get("paused", "").lower() != "true" or abs(float(committed["weight"])) > 1e-7:
        raise ValueError(f"{label} committed weather is not paused at zero weight")
    weather_signature = {k: v for k, v in committed.items() if k not in ("marker", "epoch_s")}
    return {
        "mode": label, "exit_status": 0, "start_epoch_s": start, "end_epoch_s": end,
        "start_frame": first, "end_frame": last, "callback_state": state,
        "callback_rows_in_marker_frames": len(measured), "on_glass": timeline,
        "callback": callback, "e01_cpu": e01, "portal": portal,
        "committed_weather_signature": weather_signature,
        "metalfx_flags": timeline["metalfx_flags"], "metalfx_dimensions": timeline["metalfx_dimensions"],
        "input_sha256": hashes,
    }


def shared_build_gate() -> dict[str, Any]:
    exe = STAGE / "candidate-portal-perframe-08/Anomaly.app/Contents/MacOS/AnomalyDX11"
    manifest = STAGE / "candidate-portal-perframe-08/source-manifest.json"
    build_result = ANALYSIS.parent / "runtime-root-build-08/result.json"
    result = json.loads(build_result.read_text(encoding="utf-8"))
    if result.get("build_exit") != 0 or result.get("candidate_executable_sha256") != EXPECTED["exe_sha256"] or \
            result.get("manifest_sha256") != EXPECTED["manifest_sha256"]:
        raise ValueError("runtime-root-build-08 candidate/source manifest pin mismatch")
    ordinary = "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0"
    if result.get("ordinary_exe_before") != ordinary:
        raise ValueError("pre-existing ordinary baseline executable pin mismatch")
    restored = result.get("ordinary_outputs_restored", {})
    if restored.get("app") != ordinary or restored.get("bare") != ordinary:
        raise ValueError("build's ordinary executable restoration receipt mismatch")
    source_pre = result.get("production_prestate", {})
    source_post = result.get("restored_source_hashes", {})
    if not source_pre or source_pre != source_post:
        raise ValueError("temporary source inputs were not restored to exact prestate")
    if sha(exe) != EXPECTED["exe_sha256"] or sha(manifest) != EXPECTED["manifest_sha256"]:
        raise ValueError("candidate executable or source manifest current hash mismatch")
    return {"candidate_executable_sha256": sha(exe), "source_manifest_sha256": sha(manifest),
        "source_prestate_restored": True, "ordinary_outputs_restored_to_preexisting_baseline": True,
        "restored_source_file_count": len(source_pre)}


def markdown(summary: dict[str, Any]) -> str:
    runs = summary["runs"]
    o, n, f = runs["observe"], runs["on"], runs["off"]
    rows = []
    def row(label: str, fmt) -> None:
        rows.append(f"| {label} | {fmt(o)} | {fmt(n)} | {fmt(f)} |")
    row("On-Glass interval throughput", lambda r: f"{r['on_glass']['interval_throughput_fps']:.2f} FPS")
    row("On-Glass frame interval p50 / p99", lambda r: f"{r['on_glass']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['on_glass']['frame_interval_ms']['p99_nearest_rank']:.2f} ms")
    row("On-Glass 1% low", lambda r: f"{r['on_glass']['one_percent_low_fps']:.2f} FPS ({r['on_glass']['one_percent_low_sample_count']} intervals)")
    row("Lua callback throughput", lambda r: f"{r['callback']['callback_throughput_fps']:.2f} FPS")
    row("Callback interval p50 / p99", lambda r: f"{r['callback']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['callback']['frame_interval_ms']['p99_nearest_rank']:.2f} ms")
    row("Callback 1% low", lambda r: f"{r['callback']['one_percent_low_fps']:.2f} FPS")
    row("E01 scene CPU p50 / encode CPU p50", lambda r: f"{r['e01_cpu']['cpu_stage_ms']['scene_ms']['p50_nearest_rank']:.3f} / {r['e01_cpu']['cpu_stage_ms']['encode_ms']['p50_nearest_rank']:.3f} ms")
    row("On-GPU elapsed p50 / p99", lambda r: f"{r['on_glass']['on_gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['on_gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms")
    row("In-flight CPU end-to-end p50 / p99", lambda r: f"{r['on_glass']['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms")
    row("Portal-qualified frames / measured rows", lambda r: f"{r['portal']['qualified_rows']} / {r['portal']['rows']}")
    row("Actual portal skips per frame p50 / p99", lambda r: "n/a" if r['portal']['rows'] == 0 else f"{r['portal']['actual_skips_per_frame']['p50_nearest_rank']:.0f} / {r['portal']['actual_skips_per_frame']['p99_nearest_rank']:.0f}")
    row("Graph + mask CPU p50 / p99", lambda r: "n/a" if r['portal']['rows'] == 0 else f"{r['portal']['graph_plus_mask_cpu_ms']['p50_nearest_rank']:.3f} / {r['portal']['graph_plus_mask_cpu_ms']['p99_nearest_rank']:.3f} ms")
    lines = ["# Per-frame portal ROI: matched 20-second indoor diagnostic", "",
        "The three runs passed the same binary/source, Lua profile, user.ltx/save, window-state, and exit gates. This is a diagnostic comparison only; the observer-only, suppression-on, and feature-off runs are each one short window.", "",
        "| Metric | Observe | Suppress on | Feature off |", "|---|---:|---:|---:|", *rows, "",
        "On-Glass throughput is selected drawable count divided by the sum of selected MetalPerfTrace interval durations; it is not the mean of reciprocal FPS samples. The 1% low is `1000 / mean(slowest ceil(1% N) frame intervals in ms)`. Callback throughput is frame delta divided by callback wall-time delta; callback 1% low uses the same slow-interval formula.", "",
        "E01 CPU stages, On-GPU elapsed, and in-flight CPU end-to-end are separate measurements; do not add them. Only graph and mask CPU times are summed, because they are disjoint Calculate and callback phases. CSV/file logging and E01 instrumentation remain diagnostic overhead.", "",
        "The run trio is limited to the first 20/20 pair. It establishes neither motion/visual acceptance across changing views nor shipping safety, and it makes no FPS winner claim. The portal hypothesis remains default-off outside this controlled diagnostic.", ""]
    return "\n".join(lines)


def main() -> int:
    build = shared_build_gate()
    runs = {label: analyze_run(label, out, build["candidate_executable_sha256"], build["source_manifest_sha256"])
        for label, out in RUNS.items()}
    # All three arms must retain identical canonical setup and camera timing gates.
    ref = runs["off"]
    for label, run in runs.items():
        if run["callback_state"]["rows"] <= 1: raise ValueError(f"{label}: no measured state coverage")
        if run["callback"]["frame_first"] < run["start_frame"] - 2 or run["callback"]["frame_last"] > run["end_frame"] + 2:
            raise ValueError(f"{label}: callback frame coverage outside markers")
    signatures = [runs[name]["committed_weather_signature"] for name in ("observe", "on", "off")]
    if not signatures[0] == signatures[1] == signatures[2]:
        raise ValueError("committed weather numeric/state receipts differ across arms")
    # Cross-arm committed state is compared by all measured rows in state_gate and common pinned files.
    summary = {
        "schema": "portal-perframe-indoor-roi-review/v1",
        "scope": "single matched 20/20 indoor diagnostic; no motion/general safety/shipping or FPS acceptance claim",
        "build_gate": build,
        "shared_pins": EXPECTED,
        "runs": runs,
        "interpretation": "Observer cost and suppression counts are measurements of this captured indoor view only; E01 and CSV observers perturb CPU workload.",
    }
    json_path = ANALYSIS / "perframe-roi-summary.json"
    md_path = ANALYSIS / "RESULT.md"
    json_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    md_path.write_text(markdown(summary), encoding="utf-8")
    receipt = {str(path.relative_to(ROOT)): sha(path) for path in
        [json_path, md_path, Path(__file__), STAGE / "candidate-portal-perframe-08/source-manifest.json",
         STAGE / "sol-analysis/source-topology-wind-census/portal-architecture/general-context/runtime-root-build-08/result.json"]}
    (ANALYSIS / "analysis-sha256.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "complete", "written": [str(json_path), str(md_path)],
        "runs": {k: {"on_glass_fps": v["on_glass"]["interval_throughput_fps"],
            "portal": v["portal"]} for k, v in runs.items()}}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"not ready or invalid receipts: {exc}", file=sys.stderr)
        raise SystemExit(2)
