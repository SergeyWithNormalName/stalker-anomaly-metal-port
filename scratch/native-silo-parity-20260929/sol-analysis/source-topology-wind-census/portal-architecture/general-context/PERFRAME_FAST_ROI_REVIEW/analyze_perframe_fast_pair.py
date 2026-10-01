#!/usr/bin/env python3
"""Read-only matched analysis of stage-10 fast-on vs full-on portal runs.

Requires both saved runs to be complete. No game/build/profile/trace is started.
This pair toggles population-join reuse and prepared-motion reuse together.
"""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import importlib.util
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[7]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
ANALYSIS = Path(__file__).resolve().parent
GENERAL = ANALYSIS.parent
RUNS = {
    "fast_on": STAGE / "runs/portal-perframe-fast-on-indoor-10/native/output",
    "full_on": STAGE / "runs/portal-perframe-full-on-indoor-10/native/output",
}
EXPECTED = {
    "exe_sha256": "b2e3bcc523e8aae7f645ffa4377f02a6a8c227cb66de3a55edb9439d09bde804",
    "manifest_sha256": "7056e333e5dc01e474b3d9148408870d311d00989c36e77f032830a398131aa0",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
    "save_sha256": "4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225",
    "ordinary_exe_sha256": "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0",
}

BASE_PATH = GENERAL / "PERFRAME_ROI_REVIEW/analyze_perframe_roi.py"
_spec = importlib.util.spec_from_file_location("perframe_roi_base", BASE_PATH)
if _spec is None or _spec.loader is None:
    raise RuntimeError(f"cannot load shared metric definitions from {BASE_PATH}")
BASE = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(BASE)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def read_env(path: Path) -> dict[str, str]:
    result = {}
    for line in path.read_text(errors="strict").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            k, v = line.split("=", 1)
            result[k] = v
    return result


def normalize_env(label: str) -> dict[str, str]:
    env = read_env(RUNS[label] / "environment.txt")
    run_root = str(STAGE / f"runs/portal-perframe-{label.replace('_', '-')}-indoor-10")
    # Actual folder spelling maps fast_on -> fast-on, full_on -> full-on.
    for key, value in list(env.items()):
        env[key] = value.replace(run_root, "<RUN_ROOT>")
    return env


def setting_delta() -> dict[str, Any]:
    fast = normalize_env("fast_on")
    full = normalize_env("full_on")
    if set(fast) != set(full):
        raise ValueError(f"environment keys differ: {sorted(set(fast) ^ set(full))}")
    differences = {k: (fast[k], full[k]) for k in sorted(fast) if fast[k] != full[k]}
    expected = {
        "XRAY_METAL_FLORA_PORTAL_CACHE_POPULATION": ("1", "0"),
        "XRAY_METAL_FLORA_PORTAL_PREPARED_MOTION": ("1", "0"),
    }
    if differences != expected:
        raise ValueError(f"paired environment differs beyond the two intended reuse flags: {differences}")
    if any(fast.get(k) != "on" or full.get(k) != "on" for k in (
        "XRAY_METAL_FLORA_PORTAL_PERFRAME",)):
        raise ValueError("both arms must exercise per-frame suppression")
    if any(fast.get(k) != "1" or full.get(k) != "1" for k in ("METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS",)):
        raise ValueError("E01 instrumentation differs between arms")
    return {"passed": True, "only_setting_differences": differences}


def build_gate() -> dict[str, Any]:
    exe = STAGE / "candidate-portal-perframe-10/Anomaly.app/Contents/MacOS/AnomalyDX11"
    manifest = STAGE / "candidate-portal-perframe-10/source-manifest.json"
    # The build package is stage 10. Resolve it without relying on current cwd.
    result_path = GENERAL / "runtime-root-build-10/result.json"
    result = json.loads(result_path.read_text(encoding="utf-8"))
    if result.get("build_exit") != 0 or result.get("candidate_executable_sha256") != EXPECTED["exe_sha256"] or \
            result.get("manifest_sha256") != EXPECTED["manifest_sha256"]:
        raise ValueError("stage-10 executable/build-manifest pins mismatch")
    ordinary = result.get("ordinary_outputs_restored", {})
    if result.get("ordinary_exe_before") != EXPECTED["ordinary_exe_sha256"] or \
            ordinary.get("app") != EXPECTED["ordinary_exe_sha256"] or ordinary.get("bare") != EXPECTED["ordinary_exe_sha256"]:
        raise ValueError("stage-10 ordinary R1 outputs were not restored")
    pre, post = result.get("production_prestate", {}), result.get("restored_source_hashes", {})
    if not pre or pre != post:
        raise ValueError("stage-10 source files were not restored to exact prestate")
    if sha(exe) != EXPECTED["exe_sha256"] or sha(manifest) != EXPECTED["manifest_sha256"]:
        raise ValueError("current candidate executable/manifest hashes mismatch")
    return {"candidate_exe_sha256": sha(exe), "manifest_sha256": sha(manifest),
        "ordinary_R1_restored": True, "production_source_restored": True,
        "restored_source_count": len(pre)}


def committed_weather(out: Path) -> dict[str, str]:
    rows = BASE.read_csv(out / "phases.csv.weather.csv")
    matches = [row for row in rows if row.get("marker") == "weather_committed"]
    if len(matches) != 1:
        raise ValueError("expected one committed weather row")
    row = matches[0]
    if row.get("paused") != "true" or float(row["weight"]) != 0.0:
        raise ValueError("weather is not paused at zero weight")
    return {key: value for key, value in row.items() if key not in ("marker", "epoch_s")}


def e01_work(path: Path, first: int, last: int) -> dict[str, Any]:
    rows = [r for r in BASE.read_csv(path) if first <= int(r["engine_frame"]) <= last and r.get("status") == "completed"]
    if not rows:
        raise ValueError("no completed E01 rows within measured frame window")
    def dist(key: str, unit: str) -> dict[str, Any]:
        vals = [float(row[key]) for row in rows]
        return {"count": len(vals), "mean": statistics.mean(vals),
            "p50_nearest_rank": BASE.nearest(vals, .50), "p99_nearest_rank": BASE.nearest(vals, .99), "unit": unit}
    gpu = [(float(r["gpu_end_s"]) - float(r["gpu_start_s"])) * 1000.0 for r in rows
        if float(r["gpu_end_s"]) > float(r["gpu_start_s"])]
    return {"rows": len(rows), "cpu_stage_ms": {k: dist(k, "ms") for k in
            ("scene_ms", "encode_ms", "present_encode_ms", "commit_ms", "drawable_ms")},
        "work": {k: dist(k, "per-frame") for k in
            ("indexed_draws", "updated_bytes", "buffer_allocations", "texture_allocations", "pso_binds")},
        "gpu_elapsed_from_e01_rows_ms": dist("gpu_elapsed_ms", "ms") if "gpu_elapsed_ms" in rows[0] else
            ({"count": len(gpu), "mean": statistics.mean(gpu), "p50_nearest_rank": BASE.nearest(gpu, .50),
                "p99_nearest_rank": BASE.nearest(gpu, .99), "unit": "ms"} if gpu else None)}


def run_stats(label: str) -> dict[str, Any]:
    out = RUNS[label]
    required = ("argv.txt", "exit-status.txt", "phases.csv", "phases.csv.device.csv", "phases.csv.weather.csv",
        "metal-timeline.json", "e01-frames.csv", "portal-perframe.csv", "matched_profile.lua", "environment.txt",
        "shared-state-sha256.txt")
    missing = [name for name in required if not (out / name).is_file()]
    if missing: raise ValueError(f"{label} is incomplete: missing {missing}")
    if (out / "exit-status.txt").read_text().strip() != "0": raise ValueError(f"{label} game did not exit 0")
    if sha(out / "matched_profile.lua") != EXPECTED["profile_sha256"]: raise ValueError(f"{label} Lua profile pin mismatch")
    if BASE.read_shared_hash(out / "shared-state-sha256.txt", "/appdata/user.ltx") != EXPECTED["user_ltx_sha256"]:
        raise ValueError(f"{label} user.ltx hash mismatch")
    if BASE.read_shared_hash(out / "shared-state-sha256.txt", "/savedgames/rescue-cordon-control.scop") != EXPECTED["save_sha256"]:
        raise ValueError(f"{label} save hash mismatch")
    exe = STAGE / "candidate-portal-perframe-10/Anomaly.app/Contents/MacOS/AnomalyDX11"
    argv = Path((out / "argv.txt").read_text(errors="strict").splitlines()[0]).resolve()
    if argv != exe.resolve(): raise ValueError(f"{label} did not run the pinned candidate executable")
    env = read_env(out / "environment.txt")
    if env.get("XRAY_METAL_FLORA_PORTAL_PERFRAME") != "on": raise ValueError(f"{label} was not per-frame ON")
    if env.get("XRAY_METAL_R1_STATIC_POLICY") != "0": raise ValueError(f"{label} R1 static policy differs")
    marks = BASE.read_markers(out / "phases.csv")
    for marker in ("measure_start", "measure_end", "weather_committed"):
        if marker not in marks: raise ValueError(f"{label} missing {marker} marker")
    start, end = int(marks["measure_start"]["epoch_s"]), int(marks["measure_end"]["epoch_s"])
    if end - start != 20: raise ValueError(f"{label} measured window is not 20 marker seconds")
    first, last = int(marks["measure_start"]["frame"]), int(marks["measure_end"]["frame"])
    if marks["weather_committed"].get("weather") != "w_clear2" or marks["weather_committed"].get("drawable") != "1920x1200":
        raise ValueError(f"{label} marker weather/extent mismatch")
    all_device = BASE.read_csv(out / "phases.csv.device.csv")
    phase_rows = [r for r in all_device if r.get("phase") == "1"]
    gate = BASE.state_gate(phase_rows)
    if not gate["passed"]: raise ValueError(f"{label} device state gate failed: {gate['errors'][:5]}")
    measured = [r for r in phase_rows if first <= int(r["engine_frame"]) <= last]
    timeline = BASE.timeline_stats(out / "metal-timeline.json", start, end)
    callback = BASE.callback_stats(measured)
    e01 = e01_work(out / "e01-frames.csv", first, last)
    portal = BASE.portal_stats(out / "portal-perframe.csv", "on", first, last)
    if portal["qualified_rows"] != portal["rows"]: raise ValueError(f"{label} portal candidate not qualified on every row")
    if portal["rows"] not in (last - first, last - first + 1): raise ValueError(f"{label} portal row count mismatch")
    hashes = {name: sha(out / name) for name in required}
    return {"label": label, "window_epoch_s": [start, end], "frame_markers": [first, last],
        "state_rows": gate["rows"], "measured_callback_rows": len(measured), "on_glass": timeline,
        "callback": callback, "e01": e01, "portal": portal, "weather_signature": committed_weather(out),
        "input_sha256": hashes}


def render(summary: dict[str, Any]) -> str:
    fast, full = summary["runs"]["fast_on"], summary["runs"]["full_on"]
    table = [
        ("Portal-qualified frames / rows", lambda r: f"{r['portal']['qualified_rows']} / {r['portal']['rows']}"),
        ("Actual skips per-frame p50 / p99", lambda r: f"{r['portal']['actual_skips_per_frame']['p50_nearest_rank']:.0f} / {r['portal']['actual_skips_per_frame']['p99_nearest_rank']:.0f}"),
        ("Candidate draws per-frame p50", lambda r: f"{r['portal']['draws_per_frame']['p50_nearest_rank']:.0f}"),
        ("Graph CPU p50 / p99", lambda r: f"{r['portal']['graph_cpu_ms']['p50_nearest_rank']:.3f} / {r['portal']['graph_cpu_ms']['p99_nearest_rank']:.3f} ms"),
        ("Mask CPU p50 / p99", lambda r: f"{r['portal']['mask_cpu_ms']['p50_nearest_rank']:.3f} / {r['portal']['mask_cpu_ms']['p99_nearest_rank']:.3f} ms"),
        ("Graph + mask CPU p50 / p99", lambda r: f"{r['portal']['graph_plus_mask_cpu_ms']['p50_nearest_rank']:.3f} / {r['portal']['graph_plus_mask_cpu_ms']['p99_nearest_rank']:.3f} ms"),
        ("On-Glass interval throughput / 1% low", lambda r: f"{r['on_glass']['interval_throughput_fps']:.2f} / {r['on_glass']['one_percent_low_fps']:.2f} FPS"),
        ("Callback throughput / 1% low", lambda r: f"{r['callback']['callback_throughput_fps']:.2f} / {r['callback']['one_percent_low_fps']:.2f} FPS"),
        ("Callback interval p50 / p99", lambda r: f"{r['callback']['frame_interval_ms']['p50_nearest_rank']:.2f} / {r['callback']['frame_interval_ms']['p99_nearest_rank']:.2f} ms"),
        ("E01 scene CPU p50 / encode CPU p50", lambda r: f"{r['e01']['cpu_stage_ms']['scene_ms']['p50_nearest_rank']:.3f} / {r['e01']['cpu_stage_ms']['encode_ms']['p50_nearest_rank']:.3f} ms"),
        ("E01 indexed draws p50 / updated bytes p50", lambda r: f"{r['e01']['work']['indexed_draws']['p50_nearest_rank']:.0f} / {r['e01']['work']['updated_bytes']['p50_nearest_rank']:.0f}"),
        ("On-GPU elapsed p50 / p99", lambda r: f"{r['on_glass']['on_gpu_elapsed_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['on_gpu_elapsed_ms']['p99_nearest_rank']:.3f} ms"),
        ("In-flight CPU end-to-end p50 / p99", lambda r: f"{r['on_glass']['inflight_cpu_end_to_end_ms']['p50_nearest_rank']:.3f} / {r['on_glass']['inflight_cpu_end_to_end_ms']['p99_nearest_rank']:.3f} ms"),
    ]
    lines = ["# Stage-10 per-frame portal fast-path diagnostic", "",
        "Both arms ran the same candidate binary with per-frame suppression ON. The fast arm enables population-join reuse and prepared-motion reuse together; the full arm leaves both off. This pair estimates only their combined overhead change.", "",
        "| Metric | Fast combined reuse | Full join + motion path |", "|---|---:|---:|"]
    lines.extend(f"| {name} | {fn(fast)} | {fn(full)} |" for name, fn in table)
    lines.extend(["", "Graph and mask timers are disjoint Calculate/callback stages, so their per-frame sums are reported separately from the total frame. GPU elapsed and in-flight CPU end-to-end overlap pipeline work and are not additive; E01 CPU stages and frame-upload counters are also separate diagnostics.", "",
        "On-Glass throughput is `N / sum(interval_seconds)` from selected drawable durations, not mean reciprocal FPS. Both runs stayed on AC power, reported no thermal/performance warning, and showed no swap-use change at the boundaries. The window is a single matched 20/20 sample; normal run-order and variance limits remain. It cannot isolate population reuse from prepared-motion reuse, establish motion/visual safety, or support an FPS acceptance or shipping claim.", ""])
    return "\n".join(lines)


def main() -> int:
    build = build_gate()
    env = setting_delta()
    fast, full = run_stats("fast_on"), run_stats("full_on")
    if fast["weather_signature"] != full["weather_signature"]:
        raise ValueError("committed weather state differs between paired runs")
    summary = {"schema": "portal-perframe-fast-roi-review/v1",
        "scope": "one 20/20 same-binary per-frame-ON pair; population and prepared motion reuse toggled together",
        "build_gate": build, "environment_delta_gate": env,
        "shared_pins": EXPECTED, "runs": {"fast_on": fast, "full_on": full},
        "limits": ["combined effect only; cannot assign gains to either reuse flag individually",
            "no new GPU visual/motion proof", "no FPS or shipping admission"]}
    summary_path = ANALYSIS / "perframe-fast-roi-summary.json"
    report_path = ANALYSIS / "RESULT.md"
    summary_path.write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(render(summary), encoding="utf-8")
    inputs = [Path(__file__), STAGE / "candidate-portal-perframe-10/source-manifest.json",
        GENERAL / "runtime-root-build-10/result.json"]
    for out in RUNS.values():
        inputs += [out / name for name in ("argv.txt", "environment.txt", "exit-status.txt", "phases.csv",
            "phases.csv.device.csv", "phases.csv.weather.csv", "metal-timeline.json", "portal-perframe.csv",
            "e01-frames.csv", "matched_profile.lua", "shared-state-sha256.txt")]
    inputs += [summary_path, report_path]
    receipt = {str(path.relative_to(ROOT)): sha(path) for path in inputs}
    (ANALYSIS / "analysis-sha256.json").write_text(json.dumps(receipt, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"status": "complete", "written": [str(summary_path), str(report_path)],
        "fast_mask_p50_ms": fast["portal"]["mask_cpu_ms"]["p50_nearest_rank"],
        "full_mask_p50_ms": full["portal"]["mask_cpu_ms"]["p50_nearest_rank"],
        "fast_on_glass_fps": fast["on_glass"]["interval_throughput_fps"],
        "full_on_glass_fps": full["on_glass"]["interval_throughput_fps"]}, indent=2))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, json.JSONDecodeError) as exc:
        print(f"paired runs are not ready or receipts are invalid: {exc}", file=sys.stderr)
        raise SystemExit(2)
