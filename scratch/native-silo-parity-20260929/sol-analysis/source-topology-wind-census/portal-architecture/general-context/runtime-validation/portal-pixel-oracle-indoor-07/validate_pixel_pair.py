#!/usr/bin/env python3
"""Read-only validation for one captured frame-700 Flora portal pixel pair."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
from pathlib import Path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def read_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def ppm_payload(path: Path) -> tuple[bytes, bytes]:
    data = path.read_bytes()
    match = re.match(rb"P6\n(\d+) (\d+)\n255\n", data)
    if not match:
        raise ValueError(f"invalid P6 PPM header: {path.name}")
    width, height = map(int, match.groups())
    payload = data[match.end():]
    if width != 1920 or height != 1200 or len(payload) != width * height * 3:
        raise ValueError(f"unexpected PPM dimensions or byte count: {path.name}")
    return data[:match.end()], payload


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_output", type=Path, help="native/output directory for portal-pixel-oracle-indoor-07")
    parser.add_argument("observer_validation", type=Path, nargs="?", help="optional independent source-observer validation JSON")
    parser.add_argument("--emit", type=Path, help="optional destination for the validation receipt")
    args = parser.parse_args()
    out = args.run_output.resolve()
    pair_dir = out / "main-flora-pixel-pair"
    summary_path = pair_dir / "main-flora-source-vs-portals-frame-700.json"
    summary = read_json(summary_path)
    observer = read_json(args.observer_validation.resolve()) if args.observer_validation else None

    expected_names = [
        *(f"r4_temporal_main-flora-source-vs-portals-copy-identity-mrt-{i}_frame_700.json" for i in range(4)),
        *(f"r4_temporal_main-flora-source-vs-portals-mrt-{i}_frame_700.json" for i in range(4)),
        "r4_temporal_main-flora-source-vs-portals-nonblank-position-delta_frame_700.json",
        "r4_depth_stencil_pair_main-flora-source-vs-portals-copy-identity-depth-stencil_frame_700.json",
        "r4_depth_stencil_pair_main-flora-source-vs-portals-depth-stencil-pair_frame_700.json",
        "r4_depth_stencil_pair_main-flora-source-vs-portals-nonblank-depth-stencil-delta_frame_700.json",
    ]
    reports = {name: read_json(pair_dir / name) for name in expected_names}
    equality_reports = [reports[name] for name in expected_names if "nonblank-" not in name]
    equality_checks = all(
        report.get("result") == "PASS" and report.get("command_completed", True) and
        report.get("readback_available", True) and
        report.get("pair_exact", report.get("expect_equal") is True) and
        report.get("pair_mismatch_byte_count", report.get("depth_mismatch_byte_count", 0)) == 0 and
        report.get("pair_mismatch_pixel_count", report.get("depth_mismatch_pixel_count", 0)) == 0 and
        report.get("width", 1920) == 1920 and report.get("height", 1200) == 1200 and
        ("readback_byte_count" not in report or report["readback_byte_count"] > 0) and
        report.get("stencil_mismatch_byte_count", 0) == 0 and
        report.get("stencil_mismatch_pixel_count", 0) == 0
        for report in equality_reports)

    delta_color = reports["r4_temporal_main-flora-source-vs-portals-nonblank-position-delta_frame_700.json"]
    delta_depth = reports["r4_depth_stencil_pair_main-flora-source-vs-portals-nonblank-depth-stencil-delta_frame_700.json"]
    delta_nonblank = (
        delta_color.get("result") == "PASS" and delta_color.get("pair_mismatch_pixel_count", 0) > 0 and
        delta_depth.get("result") == "PASS" and delta_depth.get("depth_mismatch_pixel_count", 0) > 0)

    ppm_equal = True
    ppm_hashes: dict[str, str] = {}
    for index in range(4):
        off = pair_dir / f"r4_temporal_main-flora-source-vs-portals-mrt-{index}_off_frame_700.ppm"
        on = pair_dir / f"r4_temporal_main-flora-source-vs-portals-mrt-{index}_on-control_frame_700.ppm"
        off_header, off_payload = ppm_payload(off)
        on_header, on_payload = ppm_payload(on)
        ppm_equal = ppm_equal and off_header == on_header and off_payload == on_payload
        ppm_hashes[off.name] = sha256(off)
        ppm_hashes[on.name] = sha256(on)

    topology = read_json(out / "source-topology.json")
    active_level = out / "active-engine-level.bin"
    level_geom = out.parent / "runtime/gamedata/levels/l01_escape/level.geom"
    log = (out / "engine.log").read_text(encoding="utf-8", errors="replace")
    argv = (out / "argv.txt").read_text(encoding="utf-8", errors="replace")
    exit_status = (out / "exit-status.txt").read_text(encoding="ascii").strip()
    candidate_root = out.parents[3] / "candidate-portal-pixel-oracle-07"
    candidate_executable = candidate_root / "Anomaly.app/Contents/MacOS/AnomalyDX11"
    candidate_manifest = candidate_root / "source-manifest.json"
    inventory_match = re.search(r"METAL_E08_FLORA_INVENTORY draws=(\d+)", log)
    inventory_draws = int(inventory_match.group(1)) if inventory_match else None
    reader_match = re.search(r"METAL_PORTAL_ACTIVE_READER generation=(\d+) bytes=(\d+) cursor=\d+ copy_ok=(\d+) suppression=(\d+)", log)
    with (out / "e01-frames.csv").open(newline="", encoding="utf-8") as stream:
        e01_rows = list(csv.DictReader(stream))
    e01_frame700 = [row for row in e01_rows if row.get("engine_frame") == "700"]
    source_draws = summary.get("source_draws", 0)
    candidate_draws = summary.get("candidate_draws", 0)
    actual_skips = summary.get("candidate_actually_skipped", 0)
    source_culled = summary.get("source_culled_visuals", -1)
    observer_scope_consistent = (
        topology.get("frame") == 700 and topology.get("observer_valid") is True and
        topology.get("suppressed") == 0 and topology.get("view_match") is True and
        topology.get("full_transform_source_order_match") is True and
        topology.get("csv_ssa_match") is True and topology.get("geometry_join_unknown_ids") == 0 and
        topology.get("geometry_joined_ids") == source_draws and topology.get("flora_id_count") == source_draws)
    checks = {
        "game_process_exit_zero": exit_status == "0",
        "frame700_source_vs_portals": summary.get("frame") == 700 and summary.get("mode") == "source-vs-portals",
        "source_and_candidate_accounting": source_draws > 0 and candidate_draws > 0 and actual_skips >= 0 and
            source_draws - actual_skips == candidate_draws and
            summary.get("source_culled_visuals") == summary.get("candidate_culled_visuals"),
        "all_twelve_completed_gpu_readback_checks": len(reports) == 12 and equality_checks and delta_nonblank,
        "source_vs_candidate_all_mrts_and_depth_stencil_exact": equality_checks,
        "source_flora_delta_nonblank": delta_nonblank and
            delta_color.get("pair_mismatch_pixel_count", 0) > 0 and
            delta_depth.get("depth_mismatch_pixel_count", 0) > 0,
        "ppm_rgb_renderings_exact_for_all_four_mrts": ppm_equal,
        "observer_frame_fields_consistent_with_this_oracle_capture": observer_scope_consistent,
        "source_transform_matches": summary.get("source_order_full_transform_match") is True and
            topology.get("full_transform_source_order_match") is True,
        "active_reader_generation_and_level_bytes_match": reader_match is not None and
            reader_match.group(1) == str(topology.get("generation")) and reader_match.group(3) == "1" and
            reader_match.group(2) == str(active_level.stat().st_size) and reader_match.group(4) == "0" and
            topology.get("active_level_sha256") == sha256(active_level),
        "retained_level_geom_matches_frame_receipt": level_geom.is_file() and
            topology.get("level_geom_sha256") == sha256(level_geom),
        "frame700_e01_completed_row_present": len(e01_frame700) == 1 and e01_frame700[0].get("status") == "completed",
        "pixel_oracle_frame_portal_and_flora_inputs_valid": summary.get("portal_frame") == summary.get("frame") == 700 and
            summary.get("portal_generation") == topology.get("generation") and summary.get("wind_valid") is True and
            summary.get("static_globals_present") is True,
        "prepared_inventory_matches_source_clip_accounting": inventory_draws is not None and
            inventory_draws == source_draws + source_culled,
        "same_pinned_candidate_executable_present_in_argv": str(candidate_executable) in argv and candidate_executable.is_file(),
        "portal_and_wind_unknowns_fail_open_counts_recorded": summary.get("candidate_wind_bound_unknown", -1) >= 0 and
            summary.get("source_portal_flags_are_gpu_wind_proof") is False,
    }
    if summary.get("candidate_owner_frustum_unknown") is not None:
        checks["owner_unknown_count_is_nonnegative"] = summary["candidate_owner_frustum_unknown"] >= 0

    hash_paths = [
        summary_path, out / "source-topology.json", out / "active-engine-level.bin",
        out / "main-flora-ids/main-flora-ids-frame-700.csv",
        out / "main-flora-ids/main-flora-ids-frame-700.submitted",
        out / "e01-frames.csv", out / "engine.log", out / "exit-status.txt",
        out / "runtime-manifest.json", out / "argv.txt", out / "environment.txt",
        out / "matched_profile.lua", out / "launch-epoch.txt", out / "exit-epoch.txt",
        candidate_executable, candidate_manifest,
        level_geom,
        *(pair_dir / name for name in expected_names),
    ]
    if args.observer_validation:
        hash_paths.append(args.observer_validation.resolve())
    hashes = {str(path.relative_to(out) if path.is_relative_to(out) else path): sha256(path) for path in hash_paths}
    result = {
        "schema": "flora-portal-pixel-oracle-review/v1",
        "run": out.parent.parent.name,
        "pixel_oracle_passed": all(checks.values()),
        "checks": checks,
        "source_observer_validator_passed": observer.get("passed") if observer else None,
        "source_observer_status": "passed" if observer and observer.get("passed") else
            ("incomplete" if not topology.get("complete") else "not independently validated"),
        "candidate_executable_sha256": sha256(candidate_executable),
        "candidate_source_manifest_sha256": sha256(candidate_manifest),
        "source_observer_completeness": topology.get("complete"),
        "source_observer_limitation": None if topology.get("complete") else
            "frame ID capture is smaller than the observer validator's pinned 5316-ID contract; no observer-complete claim",
        "observed": {
            "engine_frame": 700,
            "source_main_draws": source_draws,
            "candidate_main_draws": candidate_draws,
            "actually_skipped_after_normal_camera_clip": actual_skips,
            "prepared_flora_draws": inventory_draws,
            "prepared_candidate_exclusions": summary.get("candidate_excluded_id_count"),
            "ordinary_camera_clip_culls": source_culled,
            "wind_bound_unknown_kept_visible": summary.get("candidate_wind_bound_unknown"),
            "owner_frustum_unknown_kept_visible": summary.get("candidate_owner_frustum_unknown"),
            "mrt_count": summary.get("mrt_count"),
            "depth_stencil_format": summary.get("depth_stencil_format"),
            "nonblank_position_delta_pixels": delta_color.get("pair_mismatch_pixel_count"),
            "nonblank_depth_delta_pixels": delta_depth.get("depth_mismatch_pixel_count"),
        },
        "scope_limit": "one captured frame-700 pose; no general view coverage claim",
        "no_claims": ["general view coverage", "general culling safety", "shipping admission", "FPS or performance improvement"],
        "receipt_sha256": hashes,
        "ppm_rgb_sha256": ppm_hashes,
    }
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.emit:
        destination = args.emit.resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return 0 if result["pixel_oracle_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
