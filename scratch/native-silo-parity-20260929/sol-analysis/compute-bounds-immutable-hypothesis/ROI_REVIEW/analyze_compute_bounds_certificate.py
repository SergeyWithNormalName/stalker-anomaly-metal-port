#!/usr/bin/env python3
"""Independent read-only analysis of the compute-bounds certificate pair."""
from __future__ import annotations

import csv
import datetime as dt
import hashlib
import importlib.util
import json
import math
import re
import statistics
from pathlib import Path
from typing import Any

ROOT = Path(__file__).resolve().parents[5]
STAGE = ROOT / "scratch/native-silo-parity-20260929"
OUT = {arm: STAGE / f"runs/compute-bounds-certificate-{arm}-12/native/output" for arm in ("off", "on")}
REVIEW = Path(__file__).resolve().parent
EXPECTED = {
    "exe_sha256": "1c21fa1e07639a0ed24f75502b6b98a1207199d0734695dd4153e77a3a8d4b87",
    "manifest_sha256": "fce83b79a1240399b0b333782ab61b3be67435dc68cb89db71213d5807f38b98",
    "profile_sha256": "41312db9a6b06ef5876bc34fe3753e8cc6480711672618dd345331773f1601fb",
    "user_ltx_sha256": "b82f466fb766cc140256ca8953f4f80768ce5f8d6ff18deb126e3abfd51b3334",
    "save_sha256": "4558915180d6fdc11d22006b4f9dff850c581fc15c57514b1f8cec39252b9225",
    "ordinary_exe_sha256": "d3d93d5bb7c5883c8a453f1f5e24d6a690da4ed769d5ab201203a8db95484fa0",
}
LEVEL, WEATHER, CALENDAR = "l01_escape", "w_clear2", "2018-10-26T08:09:47"
CAMERA = (-221.082565, -18.284866, -168.182037)
DIRECTION = (0.0, 0.0, 1.0)
CANDIDATE = STAGE / "candidate-portal-perframe-12/Anomaly.app/Contents/MacOS/AnomalyDX11"
MANIFEST = STAGE / "candidate-portal-perframe-12/source-manifest.json"
BUILD_RESULT = STAGE / "sol-analysis/source-topology-wind-census/portal-architecture/general-context/runtime-root-build-12/result.json"
BASE_ANALYZER = STAGE / "sol-analysis/static-globals-versioned-hypothesis/ROI_REVIEW/analyze_versioned_static_globals.py"
spec = importlib.util.spec_from_file_location("baseline_timeline", BASE_ANALYZER)
if spec is None or spec.loader is None:
    raise RuntimeError("could not load established timeline analyzer")
BASE = importlib.util.module_from_spec(spec)
spec.loader.exec_module(BASE)


def sha(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as f:
        return list(csv.DictReader(f))


def env(path: Path) -> dict[str, str]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        if "=" in line and not line.lstrip().startswith("#"):
            key, value = line.split("=", 1)
            result[key] = value
    return result


def normalized_env(arm: str) -> dict[str, str]:
    values = env(OUT[arm] / "environment.txt")
    prefix = str(STAGE / f"runs/compute-bounds-certificate-{arm}-12")
    for key, value in list(values.items()):
        values[key] = value.replace(prefix, "<RUN_ROOT>").replace(
            f"compute-bounds-certificate-{arm}-12", "compute-bounds-certificate-<ARM>-12")
    return values


def shared_hash(path: Path, suffix: str) -> str:
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[-1].endswith(suffix):
            return parts[0]
    raise ValueError(f"missing shared-state hash for {suffix}")


def phase_markers(path: Path) -> dict[str, dict[str, str]]:
    result = {}
    for line in path.read_text(encoding="utf-8").splitlines():
        parts = line.split(",")
        fields = dict(item.split("=", 1) for item in parts[1:] if "=" in item)
        result[parts[0]] = fields
    return result


def summarize_e07(path: Path, first: int, last: int) -> dict[str, Any]:
    selected = [r for r in rows(path) if first <= int(r["engine_frame"]) <= last]
    if not selected:
        raise ValueError("no E07 rows inside measured frame range")
    bounds_ms = [float(r["compute_bounds_ms"]) for r in selected]
    vertices = [int(r["compute_bounds_vertices"]) for r in selected]
    calls = [int(r["compute_bounds_calls"]) for r in selected]
    total_ms, total_vertices = sum(bounds_ms), sum(vertices)
    per_frame_ns_per_vertex = [float(r["compute_bounds_ms"]) * 1_000_000.0 /
        int(r["compute_bounds_vertices"]) for r in selected if int(r["compute_bounds_vertices"]) > 0]
    return {
        "rows": len(selected),
        "compute_bounds_ms": BASE.distribution(bounds_ms, "ms"),
        "compute_bounds_calls": BASE.distribution([float(v) for v in calls], "calls/frame"),
        "compute_bounds_vertices": BASE.distribution([float(v) for v in vertices], "vertices/frame"),
        "aggregate_ns_per_vertex": total_ms * 1_000_000.0 / total_vertices,
        "aggregate_time_ms": total_ms,
        "aggregate_vertices": total_vertices,
        "aggregate_calls": sum(calls),
        "per_frame_ns_per_vertex": BASE.distribution(per_frame_ns_per_vertex, "ns/vertex"),
        "bounds_ms": BASE.distribution([float(r["bounds_ms"]) for r in selected], "ms"),
        "other_e07": {key: BASE.distribution([float(r[key]) for r in selected], key)
            for key in ("skin_ms", "skin_upload_ms", "computed_vertices", "uploaded_bytes")},
    }


def oracle_receipt(path: Path, measure_first: int) -> dict[str, Any]:
    pattern = re.compile(r"METAL_COMPUTE_SKIN_ORACLE status=(\S+).*?engine_frame=(\d+).*?vertices=(\d+).*?influences=(\d+).*?mismatch_bytes=(\d+)")
    matches = []
    for line in path.read_text(errors="replace").splitlines():
        m = pattern.search(line)
        if m:
            matches.append({"status": m.group(1), "engine_frame": int(m.group(2)),
                "vertices": int(m.group(3)), "influences": int(m.group(4)), "mismatch_bytes": int(m.group(5))})
    counts = {str(i): sum(v["influences"] == i for v in matches) for i in range(1, 5)}
    passed = len(matches) == 8 and all(v["status"] == "PASS" and v["mismatch_bytes"] == 0 and
        v["engine_frame"] < measure_first for v in matches) and counts == {"1": 2, "2": 2, "3": 2, "4": 2}
    if not passed:
        raise ValueError(f"compute-skin oracle sample gate failed: count={len(matches)}, classes={counts}")
    return {"samples": len(matches), "influence_class_counts": counts, "all_pass": True,
        "all_before_measured_window": True, "min_frame": min(v["engine_frame"] for v in matches),
        "max_frame": max(v["engine_frame"] for v in matches), "details": matches}


def analyze_run(arm: str) -> dict[str, Any]:
    out = OUT[arm]
    required = ("argv.txt", "exit-status.txt", "phases.csv", "phases.csv.device.csv",
        "phases.csv.weather.csv", "metal-timeline.json", "e01-frames.csv", "e07-frames.csv",
        "matched_profile.lua", "environment.txt", "shared-state-sha256.txt", "engine.log",
        "power-before.txt", "power-after.txt", "thermal-before.txt", "thermal-after.txt",
        "swap-before.txt", "swap-after.txt", "vm-stat-before.txt", "vm-stat-after.txt")
    missing = [f for f in required if not (out / f).is_file()]
    if missing or (out / "exit-status.txt").read_text().strip() != "0":
        raise ValueError(f"{arm} run incomplete or nonzero exit: {missing}")
    if sha(out / "matched_profile.lua") != EXPECTED["profile_sha256"]:
        raise ValueError(f"{arm} profile hash mismatch")
    if shared_hash(out / "shared-state-sha256.txt", "/appdata/user.ltx") != EXPECTED["user_ltx_sha256"]:
        raise ValueError(f"{arm} user.ltx mismatch")
    if shared_hash(out / "shared-state-sha256.txt", "/savedgames/rescue-cordon-control.scop") != EXPECTED["save_sha256"]:
        raise ValueError(f"{arm} save mismatch")
    argv = Path((out / "argv.txt").read_text().splitlines()[0]).resolve()
    if argv != CANDIDATE.resolve():
        raise ValueError(f"{arm} did not use the candidate executable path")

    marker = phase_markers(out / "phases.csv")
    if not all(name in marker for name in ("weather_committed", "measure_start", "measure_end")):
        raise ValueError(f"{arm} markers incomplete")
    start, end = int(marker["measure_start"]["epoch_s"]), int(marker["measure_end"]["epoch_s"])
    first, last = int(marker["measure_start"]["frame"]), int(marker["measure_end"]["frame"])
    if end - start != 20:
        raise ValueError(f"{arm} measured marker span not 20 seconds")
    weather = marker["weather_committed"]
    if weather.get("weather") != WEATHER or weather.get("calendar") != CALENDAR:
        raise ValueError(f"{arm} committed weather/calendar mismatch")

    all_device = rows(out / "phases.csv.device.csv")
    phase1 = [r for r in all_device if r.get("phase") == "1"]
    state = BASE.validate_state(phase1)
    if not state["passed"]:
        raise ValueError(f"{arm} device-state gate failed: {state['errors'][:8]}")
    measured = [r for r in phase1 if first <= int(r["engine_frame"]) <= last]
    if len(measured) < 2:
        raise ValueError(f"{arm} lacks measured device rows")

    weather_rows = rows(out / "phases.csv.weather.csv")
    committed = [r for r in weather_rows if r.get("marker") == "weather_committed"]
    if len(committed) != 1 or committed[0].get("paused") != "true" or float(committed[0]["weight"]) != 0.0:
        raise ValueError(f"{arm} weather commit is not paused/zero weight")
    onglass = BASE.timeline_summary(out / "metal-timeline.json", start, end)
    e01 = BASE.e01_summary(out / "e01-frames.csv", first, last)
    e07 = summarize_e07(out / "e07-frames.csv", first, last)
    oracle = oracle_receipt(out / "engine.log", first)
    if onglass["metalfx_flags"] != {"MetalFX Enabled": ["False"],
        "MetalFX Frame Interpolator Enabled": ["False"], "MetalFX Auto Exposure Enabled": ["False"]}:
        raise ValueError(f"{arm} MetalFX flags not off/known")
    if onglass["output_configuration"] != [{"Name":"CAMetalLayer: SDL_cocoametalview",
        "Width (pixels)":1920,"Height (pixels)":1200,"MTL Pixel Format Name":"BGRA8Unorm"}]:
        raise ValueError(f"{arm} output drawable config mismatch")
    if onglass["output_configuration"] != [{"Name":"CAMetalLayer: SDL_cocoametalview",
        "Width (pixels)":1920,"Height (pixels)":1200,"MTL Pixel Format Name":"BGRA8Unorm"}]:
        raise ValueError(f"{arm} drawable output does not match 1920x1200 BGRA8")

    expected_cert = "0" if arm == "off" else "1"
    settings = env(out / "environment.txt")
    if settings.get("XRAY_METAL_COMPUTE_BOUNDS_SOURCE_CERTIFICATE") != expected_cert:
        raise ValueError(f"{arm} certificate flag mismatch")
    for key, value in {"XRAY_METAL_COMPUTE_SKIN": "1", "XRAY_METAL_COMPUTE_SKIN_ORACLE": "1",
        "XRAY_METAL_E07_FRAMES": str(out / "e07-frames.csv"),
        "METAL_PERFORMANCE_PER_FRAME_SIGNPOSTS": "1"}.items():
        if settings.get(key) != value:
            raise ValueError(f"{arm} required diagnostic setting mismatch: {key}")

    e01_completed = [r for r in rows(out / "e01-frames.csv")
        if first <= int(r["engine_frame"]) <= last and r.get("status") == "completed"]
    e01_extra = {
        "dynamic_prewarm_ms": BASE.distribution([float(r["dynamic_prewarm_ms"]) for r in e01_completed], "ms"),
        "indexed_draws": e01["per_frame_work"]["indexed_draws"],
    }
    system = BASE.system_snapshot(out)
    if "AC Power" not in system["power_before"] or "AC Power" not in system["power_after"]:
        raise ValueError(f"{arm} not on AC in before/after snapshots")
    if "No thermal warning" not in system["thermal_before"] or "No thermal warning" not in system["thermal_after"]:
        raise ValueError(f"{arm} thermal warning present")
    if system["swap_before_mb"]["used"] != system["swap_after_mb"]["used"]:
        raise ValueError(f"{arm} swap usage changed during the run")
    return {"arm": arm, "exit": 0, "marker_epoch_s": [start, end], "marker_frames": [first, last],
        "device_state": state, "measured_device_rows": len(measured), "on_glass": onglass,
        "callback": BASE.callbacks(measured), "e01": e01, "e01_additional": e01_extra,
        "e07": e07, "oracle": oracle, "system": system, "weather_signature": {k:v for k,v in committed[0].items()
            if k not in ("marker", "epoch_s")}, "input_sha256": {f:sha(out/f) for f in required}}


def build_gate() -> dict[str, Any]:
    result = json.loads(BUILD_RESULT.read_text())
    if result.get("build_exit") != 0 or result.get("candidate_executable_sha256") != EXPECTED["exe_sha256"] or \
        result.get("manifest_sha256") != EXPECTED["manifest_sha256"]:
        raise ValueError("candidate build receipt mismatch")
    if result.get("normal_build_exit") != 0 or sha(CANDIDATE) != EXPECTED["exe_sha256"] or sha(MANIFEST) != EXPECTED["manifest_sha256"]:
        raise ValueError("candidate current binary/manifest or restore build mismatch")
    if result.get("ordinary_exe_before") != EXPECTED["ordinary_exe_sha256"]:
        raise ValueError("ordinary executable prestate mismatch")
    ordinary = result.get("ordinary_outputs_restored", {})
    if ordinary.get("app") != EXPECTED["ordinary_exe_sha256"] or ordinary.get("bare") != EXPECTED["ordinary_exe_sha256"]:
        raise ValueError("ordinary executable restore receipt mismatch")
    if result.get("production_prestate") != result.get("restored_source_hashes"):
        raise ValueError("production source restore receipt differs")
    return {"candidate_sha256": sha(CANDIDATE), "manifest_sha256": sha(MANIFEST),
        "candidate_build_exit": 0, "ordinary_restore_build_exit": 0,
        "ordinary_executable_restored": ordinary, "production_source_restored": True,
        "restored_source_count": len(result["production_prestate"])}


def pair_gate() -> dict[str, Any]:
    a, b = normalized_env("off"), normalized_env("on")
    if set(a) != set(b):
        raise ValueError("environment keys differ between arms")
    diffs = {k: (a[k], b[k]) for k in sorted(a) if a[k] != b[k]}
    if diffs != {"XRAY_METAL_COMPUTE_BOUNDS_SOURCE_CERTIFICATE": ("0", "1")}:
        raise ValueError(f"environment delta is not single-flag: {diffs}")
    return {"only_setting_difference": diffs, "passed": True}


def render(summary: dict[str, Any]) -> str:
    off, on = summary["runs"]["off"], summary["runs"]["on"]
    rows = [
        ("On-Glass throughput (N / summed intervals)", "on_glass", "interval_throughput_fps", "{:.2f} FPS"),
        ("On-Glass interval p50 / p99", "on_glass", "frame_interval_ms", "dist"),
        ("On-Glass 1% low", "on_glass", "one_percent_low_fps", "{:.2f} FPS"),
        ("Device callback throughput", "callback", "throughput_fps", "{:.2f} FPS"),
        ("Device callback interval p50 / p99", "callback", "frame_interval_ms", "dist"),
        ("Callback 1% low", "callback", "one_percent_low_fps", "{:.2f} FPS"),
        ("E01 dynamic prewarm p50", "e01_additional", "dynamic_prewarm_ms", "p50only"),
        ("E01 indexed draws p50", "e01_additional", "indexed_draws", "p50count"),
        ("E07 certified bounds compute p50 / mean", "e07", "compute_bounds_ms", "p50mean"),
        ("E07 certified bounds per-frame ns / vertex p50", "e07", "per_frame_ns_per_vertex", "p50only"),
        ("E07 certified bounds aggregate ns / vertex", "e07", "aggregate_ns_per_vertex", "{:.3f} ns/v"),
        ("E07 certified bounds vertices / frame p50", "e07", "compute_bounds_vertices", "p50count"),
        ("E07 existing bounds routine p50 / p99", "e07", "bounds_ms", "dist"),
        ("GPU elapsed p50 / p99", "on_glass", "on_gpu_elapsed_ms", "dist"),
        ("In-flight CPU end-to-end p50 / p99", "on_glass", "inflight_cpu_end_to_end_ms", "dist"),
    ]
    def fmt(run: dict[str, Any], section: str, key: str, kind: str) -> str:
        val = run[section][key]
        if kind == "dist":
            if key.endswith("_ms") or key in ("frame_interval_ms", "on_gpu_elapsed_ms", "inflight_cpu_end_to_end_ms"):
                return f"{val['p50_nearest_rank']:.3f} / {val['p99_nearest_rank']:.3f} ms"
            return f"{val['mean']:.0f}"
        if kind == "p50mean":
            return f"{val['p50_nearest_rank']:.3f} / {val['mean']:.3f} ms"
        if kind == "p50only":
            return f"{val['p50_nearest_rank']:.3f} {val['unit']}"
        if kind == "p50count":
            return f"{val['p50_nearest_rank']:.0f}"
        return kind.format(val)
    lines = ["# Compute-bounds source-certificate pair: independent review", "",
        "The normalized run environments differ only in `XRAY_METAL_COMPUTE_BOUNDS_SOURCE_CERTIFICATE` (0 off, 1 on). Both arms used the same candidate executable/source manifest, matched profile/save/user.ltx, R4 scene, 20-second marker window, fixed camera, weather/clock, 1920×1200 drawable, compute-skin and compute-skin oracle diagnostics. The eight pre-window GPU oracle samples passed in each arm (two samples per influence count 1–4).", "",
        "| Metric | Certificate off | Certificate on |", "|---|---:|---:|"]
    for label, section, key, kind in rows:
        lines.append(f"| {label} | {fmt(off,section,key,kind)} | {fmt(on,section,key,kind)} |")
    normalized_off = off["e07"]["per_frame_ns_per_vertex"]["p50_nearest_rank"]
    normalized_on = on["e07"]["per_frame_ns_per_vertex"]["p50_nearest_rank"]
    reduction = 100.0 * (1.0 - normalized_on / normalized_off)
    fps_change = 100.0 * (on["on_glass"]["interval_throughput_fps"] / off["on_glass"]["interval_throughput_fps"] - 1.0)
    lines.extend(["", "For the bounds helper, normalized cost is the median of per-frame `compute_bounds_ms × 1,000,000 / compute_bounds_vertices` across measured E07 rows. It fell from {:.3f} to {:.3f} ns per vertex ({:.1f}% lower), while median processed vertices rose from {:,.0f} to {:,.0f} per frame. The ratio-of-sums is also retained in the JSON. This supports a reduction in the instrumented source-bounds helper cost in this pair.".format(normalized_off,normalized_on,reduction,off["e07"]["compute_bounds_vertices"]["p50_nearest_rank"],on["e07"]["compute_bounds_vertices"]["p50_nearest_rank"]), "",
        "Whole-frame On-Glass throughput changed by {:.2f}% (off {:.2f} FPS, on {:.2f} FPS); the 1% low moved from {:.2f} to {:.2f} FPS. Indexed draw work also differed (median {:.0f} vs {:.0f}), so scene workload was not identical. This single pair does not establish that the helper change caused the whole-frame rate movement or a repeatable FPS gain. E01 stage CPU, GPU elapsed, and in-flight CPU end-to-end timings are separate and non-additive.".format(fps_change,off["on_glass"]["interval_throughput_fps"],on["on_glass"]["interval_throughput_fps"],off["on_glass"]["one_percent_low_fps"],on["on_glass"]["one_percent_low_fps"],off["e01_additional"]["indexed_draws"]["p50_nearest_rank"],on["e01_additional"]["indexed_draws"]["p50_nearest_rank"]), "",
        "Both arms recorded AC power and no thermal warning. Swap snapshots were unchanged within each arm; their absolute starting values are context, not controls. The measured phases include E01/E07 instrumentation and CSV logging. The source certificate is an immutable-source reuse hypothesis; these runtime results do not establish broader data-quality equivalence or shipping admission.", ""])
    return "\n".join(lines)


def main() -> None:
    gate = build_gate()
    environment = pair_gate()
    off, on = analyze_run("off"), analyze_run("on")
    if off["weather_signature"] != on["weather_signature"]:
        raise ValueError("committed weather payload differs")
    if off["on_glass"]["output_configuration"] != on["on_glass"]["output_configuration"] or \
        off["on_glass"]["metalfx_dimensions"] != on["on_glass"]["metalfx_dimensions"]:
        raise ValueError("Metal drawable configuration differs")
    summary = {"schema": "compute-bounds-certificate-roi-review/v1",
        "scope": "one matched 20/20 diagnostic pair; helper cost reduction separated from whole-frame FPS causality",
        "build_restore_gate": gate, "environment_gate": environment,
        "expected_pins": EXPECTED, "runs": {"off": off, "on": on}}
    json_path, md_path = REVIEW / "summary.json", REVIEW / "RESULT.md"
    json_path.write_text(json.dumps(summary, indent=2) + "\n")
    md_path.write_text(render(summary))
    input_paths = [Path(__file__), BASE_ANALYZER, BUILD_RESULT, CANDIDATE, MANIFEST]
    for output in OUT.values():
        input_paths.extend(output / name for name in
            ("argv.txt", "exit-status.txt", "phases.csv", "phases.csv.device.csv", "phases.csv.weather.csv",
             "metal-timeline.json", "e01-frames.csv", "e07-frames.csv", "matched_profile.lua",
             "environment.txt", "shared-state-sha256.txt", "engine.log", "power-before.txt", "power-after.txt",
             "thermal-before.txt", "thermal-after.txt", "swap-before.txt", "swap-after.txt"))
    receipt = {str(p.relative_to(ROOT)): sha(p) for p in input_paths if p.exists()}
    receipt[str(json_path.relative_to(ROOT))] = sha(json_path)
    receipt[str(md_path.relative_to(ROOT))] = sha(md_path)
    (REVIEW / "analysis-sha256.json").write_text(json.dumps(receipt, indent=2) + "\n")
    print(json.dumps({"status":"complete", "off_fps":off["on_glass"]["interval_throughput_fps"],
        "on_fps":on["on_glass"]["interval_throughput_fps"],
        "off_ns_per_vertex_p50":off["e07"]["per_frame_ns_per_vertex"]["p50_nearest_rank"],
        "on_ns_per_vertex_p50":on["e07"]["per_frame_ns_per_vertex"]["p50_nearest_rank"],
        "written":[str(json_path),str(md_path),str(REVIEW / 'analysis-sha256.json')]},indent=2))


if __name__ == "__main__":
    main()
