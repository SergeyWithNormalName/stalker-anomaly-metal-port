#!/usr/bin/env python3
"""Validate one fresh frame-700 portal-observer run; never changes inputs."""
from __future__ import annotations

import argparse
import csv
import hashlib
import json
import re
import sys
from pathlib import Path

EXPECTED_OFFLINE_SHA256 = "f882ab4e87da7fc8a384e615b703e9e8cceb54e69e50946fb3ea66935cf3397f"
EXPECTED_LEVEL_GEOM_SHA256 = "a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a"
EXPECTED_IDS = 5316


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def regular_file(path: Path) -> bool:
    return path.is_file() and not path.is_symlink()


def csv_rows(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8") as stream:
        return list(csv.DictReader(stream))


def parse_id_capture(path: Path) -> tuple[dict[str, str], list[tuple[int, str]], dict[str, list[int]]]:
    metadata: dict[str, str] = {}
    bit_rows: dict[str, list[int]] = {}
    ids: list[tuple[int, str]] = []
    seen: set[int] = set()
    header = footer = False
    with path.open(newline="", encoding="utf-8") as stream:
        for row in csv.reader(stream):
            if not row:
                continue
            if row == ["visual_id", "material"]:
                if header:
                    raise ValueError("duplicate ID header")
                header = True
                continue
            if row[0] in {"view_bits", "projection_bits", "eye_bits", "ssa_settings_bits"}:
                if row[0] in bit_rows or any(not re.fullmatch(r"[0-9a-fA-F]{8}", v) for v in row[1:]):
                    raise ValueError(f"malformed or duplicate {row[0]}")
                bit_rows[row[0]] = [int(v, 16) for v in row[1:]]
                continue
            if row[0] == "encode_complete":
                if footer or len(row) != 2 or row[1] != str(EXPECTED_IDS) or len(ids) != EXPECTED_IDS:
                    raise ValueError("missing, duplicate, or non-5316 terminal footer")
                footer = True
                continue
            if not header:
                if len(row) < 2 or row[0] in metadata:
                    raise ValueError("malformed or duplicate capture metadata")
                metadata[row[0]] = ",".join(row[1:])
                continue
            if footer or len(row) != 2 or row[1] not in {"leaf", "trunk"}:
                raise ValueError("malformed ID row or data after footer")
            try:
                visual_id = int(row[0], 10)
            except ValueError as exc:
                raise ValueError("noninteger visual ID") from exc
            if visual_id < 0 or visual_id in seen:
                raise ValueError("negative or duplicate visual ID")
            seen.add(visual_id)
            ids.append((visual_id, row[1]))
    if not header or not footer or len(ids) != EXPECTED_IDS:
        raise ValueError("ID capture is incomplete")
    if metadata.get("schema") != "main-flora-id-observer/v1":
        raise ValueError("unexpected ID capture schema")
    return metadata, ids, bit_rows


def emit(result: dict, destination: Path | None, status: int) -> int:
    rendered = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if destination:
        destination = destination.resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(rendered, encoding="utf-8")
    print(rendered, end="")
    return status


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("run_dir", type=Path, help="fresh output directory for this one run")
    parser.add_argument("--offline", type=Path, help="defaults to the pinned root-build-03 per-ID CSV")
    parser.add_argument("--emit", type=Path, help="optional result JSON destination outside run_dir")
    args = parser.parse_args()
    run = args.run_dir.resolve()
    offline = args.offline or (Path(__file__).resolve().parents[2] / "cpp-walker/root-build-03/cpp-frame700-per-id.csv")
    paths = {
        "launch_epoch": run / "launch-epoch.txt",
        "topology": run / "source-topology.json",
        "ids": run / "main-flora-ids/main-flora-ids-frame-700.csv",
        "active_level": run / "active-engine-level.bin",
        "engine_log": run / "engine.log",
        "e01": run / "e01-frames.csv",
        "level_geom": run.parent / "runtime/gamedata/levels/l01_escape/level.geom",
        "offline": offline.resolve(),
    }
    checks: dict[str, bool] = {}
    unknown: list[str] = []
    errors: list[str] = []
    hashes: dict[str, str] = {}
    if (run / "run-start.json").exists() and not regular_file(run / "run-start.json"):
        errors.append("run-start.json is not a regular non-symlink file")
    for key, path in paths.items():
        if key == "launch_epoch" and regular_file(run / "run-start.json"):
            continue
        checked_path = path.resolve() if key == "level_geom" else path
        if not checked_path.is_file() or (key != "level_geom" and path.is_symlink()):
            errors.append(f"missing, symlinked, or non-regular {key} input: {path}")
        else:
            hashes[key] = sha256(checked_path)
    if errors:
        result = {"schema": "runtime-portal-validation/v1", "passed": False,
                  "checks": checks, "unknown_fields": unknown, "errors": errors, "input_sha256": hashes}
        return emit(result, args.emit, 2)

    try:
        topo = json.loads(paths["topology"].read_text(encoding="utf-8"))
        optional_start_path = run / "run-start.json"
        if regular_file(optional_start_path):
            start = json.loads(optional_start_path.read_text(encoding="utf-8"))
            checks["run_start_schema"] = start.get("schema") == "portal-runtime-run-start/v1"
            checks["run_directory_precleared"] = start.get("output_directory_precleared") is True
            started_ns = start.get("started_unix_ns")
        else:
            start = {}
            try:
                started_ns = int(paths["launch_epoch"].read_text(encoding="ascii").strip()) * 1_000_000_000
            except ValueError:
                started_ns = None
            checks["fresh_launch_epoch_present"] = isinstance(started_ns, int) and started_ns > 0
            hashes["launch_epoch"] = sha256(paths["launch_epoch"])
        checks["start_time_valid"] = isinstance(started_ns, int) and started_ns > 0
        checks["all_run_outputs_newer_than_start"] = isinstance(started_ns, int) and started_ns > 0 and all(
            paths[k].stat().st_mtime_ns >= started_ns
            for k in ("topology", "ids", "active_level", "engine_log", "e01"))

        checks["offline_csv_sha_pinned"] = hashes["offline"] == EXPECTED_OFFLINE_SHA256
        offline_rows = csv_rows(paths["offline"])
        off: dict[int, dict[str, str]] = {}
        for row in offline_rows:
            visual_id = int(row["visual_id"])
            if visual_id in off:
                raise ValueError("offline per-ID CSV has duplicate IDs")
            off[visual_id] = row
        checks["offline_5316_unique_ids"] = len(off) == EXPECTED_IDS
        checks["offline_all_source_matches"] = all(row.get("match") == "1" for row in off.values())
        checks["offline_expected_889_visible"] = sum(int(r["cpp_source_visible"]) for r in off.values()) == 889

        id_meta, capture_ids, capture_bits = parse_id_capture(paths["ids"])
        checks["capture_frame_700"] = id_meta.get("engine_frame") == "700"
        checks["capture_submitted_5316"] = id_meta.get("submitted") == str(EXPECTED_IDS)
        checks["capture_extent_matches_receipt"] = id_meta.get("extent") == f"{topo.get('width')},{topo.get('height')}"
        capture_map = dict(capture_ids)
        checks["capture_ids_exact_offline_set_and_material"] = (
            len(capture_map) == EXPECTED_IDS and set(capture_map) == set(off) and
            all(capture_map[i] == off[i]["material"] for i in off))

        bit_contract = {
            "view_bits": ("view_bits", 16),
            "projection_bits": ("projection_bits", 16),
            "eye_bits": ("current_eye_bits", 3),
            "ssa_settings_bits": ("ssa_settings_bits", 7),
        }
        for capture_name, (receipt_name, count) in bit_contract.items():
            value = topo.get(receipt_name)
            if not isinstance(value, list) or len(value) != count:
                if capture_name == "ssa_settings_bits" and topo.get("csv_ssa_match") is True:
                    unknown.append("ssa_settings_bits_external_recheck (receipt only reports csv_ssa_match=true)")
                    checks["ssa_settings_internal_assertion"] = True
                else:
                    unknown.append(receipt_name)
                checks[f"{capture_name}_receipt_match"] = False
            else:
                try:
                    checks[f"{capture_name}_receipt_match"] = capture_bits.get(capture_name) == [int(x) for x in value]
                except (TypeError, ValueError):
                    checks[f"{capture_name}_receipt_match"] = False

        checks["receipt_frame_700"] = topo.get("frame") == 700
        checks["receipt_schema"] = topo.get("schema") == "r4-source-topology-runtime-v1"
        checks["receipt_complete"] = topo.get("complete") is True
        checks["observer_valid"] = topo.get("observer_valid") is True
        checks["view_match"] = topo.get("view_match") is True
        checks["csv_view_projection_eye_assertions"] = all(
            topo.get(k) is True for k in ("csv_view_match", "csv_projection_match", "csv_eye_match"))
        checks["csv_ssa_assertion"] = topo.get("csv_ssa_match") is True and len(capture_bits.get("ssa_settings_bits", [])) == 7
        checks["suppression_zero"] = type(topo.get("suppressed")) is int and topo.get("suppressed") == 0
        checks["flora_ids_valid"] = topo.get("flora_ids_valid") is True
        checks["geometry_join_complete"] = topo.get("geometry_join_complete") is True
        checks["geometry_join_counts"] = topo.get("geometry_joined_ids") == EXPECTED_IDS and topo.get("geometry_join_unknown_ids") == 0
        if "geometry_join_scope" not in topo:
            unknown.append("geometry_join_scope")
        else:
            checks["geometry_join_scope_limited"] = "structure-only" in topo["geometry_join_scope"]
        checks["receipt_id_count"] = topo.get("flora_id_count") == EXPECTED_IDS
        checks["receipt_width_height"] = isinstance(topo.get("width"), int) and topo["width"] > 0 and \
            isinstance(topo.get("height"), int) and topo["height"] > 0

        checks["active_reader_sha_matches_bytes"] = topo.get("active_level_sha256") == hashes["active_level"]
        checks["active_level_size_nonzero"] = paths["active_level"].stat().st_size > 0
        log = paths["engine_log"].read_text(encoding="utf-8", errors="replace")
        records = re.findall(
            r"METAL_PORTAL_ACTIVE_READER generation=(\d+) bytes=(\d+) cursor=(-?\d+) copy_ok=(\d+) suppression=0", log)
        checks["active_reader_copy_record_present"] = bool(records)
        if records:
            gen, byte_count, _, copy_ok = map(int, records[-1])
            checks["latest_active_reader_copy_succeeded"] = copy_ok == 1
            checks["reader_generation_matches_receipt"] = gen > 0 and topo.get("generation") == gen
            checks["reader_copy_byte_count_matches"] = copy_ok == 1 and byte_count == paths["active_level"].stat().st_size
        else:
            checks["latest_active_reader_copy_succeeded"] = False
            checks["reader_generation_matches_receipt"] = False
            checks["reader_copy_byte_count_matches"] = False

        geom_hash = start.get("active_level_geom_sha256")
        if not isinstance(geom_hash, str):
            geom_hash = hashes["level_geom"]
            checks["runtime_level_geom_file_matches_pin"] = geom_hash == EXPECTED_LEVEL_GEOM_SHA256
        else:
            checks["run_manifest_level_geom_sha_pinned"] = geom_hash == EXPECTED_LEVEL_GEOM_SHA256 and hashes["level_geom"] == geom_hash
        if "level_geom_sha256" not in topo:
            unknown.append("receipt_level_geom_sha256_not_bound_to_cache")
            checks["receipt_level_geom_sha_matches_pin"] = False
        else:
            checks["receipt_level_geom_sha_matches_pin"] = topo.get("level_geom_sha256") == EXPECTED_LEVEL_GEOM_SHA256 and hashes["level_geom"] == topo.get("level_geom_sha256")

        raw_rows = topo.get("flora_ids")
        checks["runtime_per_id_count"] = isinstance(raw_rows, list) and len(raw_rows) == EXPECTED_IDS
        runtime_map: dict[int, dict] = {}
        duplicate_runtime = False
        if isinstance(raw_rows, list):
            for row in raw_rows:
                if not isinstance(row, dict) or not isinstance(row.get("id"), int):
                    duplicate_runtime = True
                    continue
                if row["id"] in runtime_map:
                    duplicate_runtime = True
                runtime_map[row["id"]] = row
        checks["runtime_ids_unique"] = not duplicate_runtime and len(runtime_map) == EXPECTED_IDS
        per_id_ok = len(runtime_map) == len(off) and set(runtime_map) == set(off)
        if per_id_ok:
            for visual_id, row in runtime_map.items():
                expected = off[visual_id]
                per_id_ok &= row.get("material") == expected["material"]
                per_id_ok &= int(row.get("raw_visible", -1)) == int(expected["cpp_source_visible"])
                per_id_ok &= int(row.get("owner_tests", -1)) == int(expected["owner_frustum_tests"])
        checks["runtime_raw_flags_and_owner_tests_match_offline"] = per_id_ok
        checks["runtime_visible_count_889"] = sum(int(r.get("raw_visible", 0)) for r in runtime_map.values()) == 889

        reached = topo.get("reached")
        nonempty = [s for s in reached if isinstance(s, dict) and s.get("frusta")] if isinstance(reached, list) else []
        sectors = [s.get("sector") for s in nonempty]
        frustum_count = sum(len(s.get("frusta", [])) for s in nonempty)
        checks["start_sector_25"] = topo.get("start_sector") == 25
        checks["reached_six_nonempty_sectors"] = len(nonempty) == 6 and len(set(sectors)) == 6
        checks["reached_eight_frusta"] = frustum_count == 8

        e01 = csv_rows(paths["e01"])
        frame_rows = [row for row in e01 if row.get("engine_frame") == "700"]
        checks["e01_frame700_completed"] = len(frame_rows) == 1 and frame_rows[0].get("status") == "completed" and frame_rows[0].get("serial") == "700"
    except Exception as exc:
        errors.append(f"input parse/validation error: {type(exc).__name__}: {exc}")

    passed = bool(checks) and all(checks.values()) and not unknown and not errors
    result = {
        "schema": "runtime-portal-validation/v1",
        "passed": passed,
        "checks": checks,
        "unknown_fields": sorted(set(unknown)),
        "errors": errors,
        "input_sha256": hashes,
        "counts": {
            "runtime_ids": len(topo.get("flora_ids", [])) if "topo" in locals() and isinstance(topo.get("flora_ids"), list) else None,
            "offline_ids": len(off) if "off" in locals() else None,
        },
        "interpretation": "Source observer integrity only; no GPU visibility, visual-quality, or FPS proof.",
    }
    return emit(result, args.emit, 0 if passed else 2)


if __name__ == "__main__":
    sys.exit(main())
