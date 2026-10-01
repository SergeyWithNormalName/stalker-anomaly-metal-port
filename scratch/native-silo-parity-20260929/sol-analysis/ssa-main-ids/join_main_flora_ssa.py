#!/usr/bin/env python3
"""Join one completed main-Flora ID capture to the exact level OGF spheres.

Observe-only analysis. Shader E[4] emissive / visual optimization flags are
not in the flattened Metal capture, so numerical rejects are never reported
as safe suppression candidates.
"""

import argparse
import csv
import hashlib
import json
import math
import struct
from collections import Counter, defaultdict
from pathlib import Path

EXPECTED_LEVEL_SHA256 = "1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7"
EPS = 0.0000100
EPS_S = 0.0000001
MASK32 = 0xFFFFFFFF


def require(condition, message):
    if not condition:
        raise ValueError(message)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", value))[0]


def from_bits(value):
    require(len(value) == 8, f"invalid float32 hex word: {value}")
    return struct.unpack("<f", struct.pack("<I", int(value, 16)))[0]


def bits(value):
    return struct.unpack("<I", struct.pack("<f", value))[0]


def ulp32(value):
    value = abs(value)
    if value < 2.0 ** -126:
        return 2.0 ** -149
    return 2.0 ** (math.floor(math.log2(value)) - 23)


def chunks(data, begin, end):
    at = begin
    while at < end:
        require(at + 8 <= end, "truncated X-Ray chunk header")
        identity, size = struct.unpack_from("<II", data, at)
        require(not identity & 0x80000000, "compressed X-Ray chunk unsupported")
        body, stop = at + 8, at + 8 + size
        require(stop <= end, "X-Ray chunk overrun")
        yield identity, body, stop
        at = stop
    require(at == end, "unreconciled X-Ray chunk stream")


def one(chunks_by_id, identity):
    found = chunks_by_id.get(identity, [])
    require(len(found) == 1, f"expected one X-Ray chunk {identity}, got {len(found)}")
    return found[0]


def group(items):
    result = defaultdict(list)
    for identity, begin, end in items:
        result[identity].append((begin, end))
    return result


def load_flora_spheres(level_path):
    source = level_path.read_bytes()
    sha = hashlib.sha256(source).hexdigest()
    require(sha == EXPECTED_LEVEL_SHA256,
            f"archive-winning level mismatch: {sha}")
    data = memoryview(source)
    top = group(chunks(data, 0, len(data)))
    shader_begin, shader_end = one(top, 2)
    shader_count = struct.unpack_from("<I", data, shader_begin)[0]
    shader_cursor = shader_begin + 4
    shaders = []
    for _ in range(shader_count):
        nul = source.find(b"\0", shader_cursor, shader_end)
        require(nul >= 0, "unterminated level shader entry")
        entry = source[shader_cursor:nul]
        shaders.append(entry.split(b"/", 1)[0])
        shader_cursor = nul + 1
    require(shader_cursor == shader_end, "shader chunk has trailing bytes")
    visual_begin, visual_end = one(top, 3)
    records = {}
    for visual_id, begin, end in chunks(data, visual_begin, visual_end):
        sub = group(chunks(data, begin, end))
        header_begin, header_end = one(sub, 1)
        require(header_end - header_begin >= 44 and data[header_begin] == 4,
                f"invalid OGF_HEADER for visual {visual_id}")
        shader_id = struct.unpack_from("<H", data, header_begin + 2)[0]
        require(shader_id < len(shaders), f"shader ID out of range for visual {visual_id}")
        shader = shaders[shader_id]
        if shader not in (b"flora\\leaf_wave", b"flora\\trunk_wave"):
            continue
        require(data[header_begin + 1] == 7,
                f"Flora visual {visual_id} is not expected MT_TREE_ST")
        require(visual_id not in records, f"duplicate Flora visual {visual_id}")
        sphere_bits = struct.unpack_from("<4I", data, header_begin + 28)
        center_radius = tuple(from_bits(f"{word:08x}") for word in sphere_bits)
        require(all(math.isfinite(value) for value in center_radius) and center_radius[3] > 0,
                f"invalid OGF sphere for Flora visual {visual_id}")
        records[visual_id] = {
            "shader_id": shader_id,
            "material": "leaf" if shader == b"flora\\leaf_wave" else "trunk",
            "sphere": center_radius,
            "sphere_bits": sphere_bits,
        }
    require(len(records) == 11663,
            f"Flora source census changed: {len(records)} != 11663")
    return records, sha


def read_capture(path):
    lines = list(csv.reader(path.open(newline="")))
    require(len(lines) > 12 and lines[0] == ["schema", "main-flora-id-observer/v1"],
            "invalid capture schema")
    keys = ["engine_frame", "extent", "admitted", "submitted", "culled",
            "view_bits", "projection_bits", "eye_bits", "ssa_settings_bits"]
    metadata = {}
    for row, key in zip(lines[1:10], keys):
        require(row and row[0] == key, f"missing or unordered capture key {key}")
        metadata[key] = row[1:]
    require(lines[10] == ["visual_id", "material"], "missing capture ID header")
    require(lines[-1][0] == "encode_complete" and len(lines[-1]) == 2,
            "capture lacks encode completion marker")
    identifiers = []
    for row in lines[11:-1]:
        require(len(row) == 2 and row[1] in ("leaf", "trunk"),
                f"invalid Flora ID/material row: {row}")
        identifiers.append((int(row[0]), row[1]))
    require(len(identifiers) == len({identity for identity, _ in identifiers}),
            "duplicate submitted Flora visual ID")
    frame = int(metadata["engine_frame"][0])
    admitted = int(metadata["admitted"][0])
    submitted = int(metadata["submitted"][0])
    culled = int(metadata["culled"][0])
    require(submitted == len(identifiers) == int(lines[-1][1]) and
            admitted == submitted + culled == 11663,
            "capture counts disagree with ID rows")
    require(len(metadata["extent"]) == 2 and
            len(metadata["view_bits"]) == len(metadata["projection_bits"]) == 16 and
            len(metadata["eye_bits"]) == 3 and len(metadata["ssa_settings_bits"]) == 7,
            "capture view/setting ABI length mismatch")
    return metadata, identifiers, frame


def read_submitted(path, frame):
    rows = list(csv.reader(path.open(newline="")))
    require(len(rows) == 2 and rows[0] == ["engine_frame", str(frame)] and
            len(rows[1]) == 2 and rows[1][0] == "submitted_serial",
            "submission marker/frame mismatch")
    return int(rows[1][1])


def read_e01(path, frame, serial):
    rows = [row for row in csv.DictReader(path.open(newline=""))
            if int(row["engine_frame"]) == frame and int(row["serial"]) == serial]
    require(len(rows) == 1 and rows[0]["status"] == "completed" and
            int(rows[0]["camera_frame"]) == frame,
            "E01 completed frame/serial/camera witness missing")
    return {"serial": serial, "engine_frame": frame,
            "status": rows[0]["status"], "indexed_draws": int(rows[0]["indexed_draws"]),
            "pso_binds": int(rows[0]["pso_binds"])}


def read_phase(path, frame, eye, extent):
    rows = [row for row in csv.DictReader(path.open(newline=""))
            if int(row["engine_frame"]) == frame]
    require(len(rows) == 1 and rows[0]["phase"] == "1", "measured device phase missing")
    row = rows[0]
    require((int(row["width"]), int(row["height"])) == extent,
            "device/capture extent mismatch")
    reported_eye = tuple(float(row["camera_" + axis]) for axis in "xyz")
    require(all(abs(a - b) <= 0.0001 for a, b in zip(eye, reported_eye)),
            "device/capture camera eye mismatch")
    return {"phase": int(row["phase"]), "level": row["level"],
            "weather": row["weather"], "calendar": row["calendar"],
            "eye_decimal": reported_eye}


def hash_vector(sphere_bits):
    x, y, z = sphere_bits[:3]
    value = ((x * 73856093) & MASK32) ^ ((y * 19349663) & MASK32) ^ ((z * 83492791) & MASK32)
    value ^= value >> 16
    value = value * 0x85EBCA6B & MASK32
    value ^= value >> 13
    value = value * 0xC2B2AE35 & MASK32
    value ^= value >> 16
    return value


def classify(sphere, sphere_bits, eye, threshold, fade_k, exponent):
    x, y, z, radius = sphere
    # Emulate the source float operands and expose a wider double witness at
    # thresholds. MSVC lowering / powf may differ by a few last bits.
    dx, dy, dz = (f32(a - b) for a, b in zip(eye, (x, y, z)))
    distance = f32(f32(f32(dx * dx) + f32(dy * dy)) + f32(dz * dz))
    distance = f32(distance + f32(EPS))
    ssa = f32(f32(radius * radius) / distance)
    double_distance = sum((a - b) ** 2 for a, b in zip(eye, (x, y, z))) + EPS
    double_ssa = radius * radius / double_distance
    fade_start = f32(threshold * fade_k)
    uncertain = []
    if abs(ssa - threshold) <= 64 * ulp32(threshold) or (ssa < threshold) != (double_ssa < threshold):
        uncertain.append("hard_threshold")
    if abs(ssa - fade_start) <= 64 * ulp32(fade_start) or (ssa < fade_start) != (double_ssa < fade_start):
        uncertain.append("fade_start")
    if ssa < threshold:
        return "hard_reject", uncertain, ssa
    if ssa >= fade_start:
        return "keep_above_fade", uncertain, ssa
    survival = f32(f32(ssa - threshold) / f32(fade_start - threshold))
    hash_value = hash_vector(sphere_bits)
    value = f32(hash_value * 2.0 ** -32)
    power = f32(math.pow(survival, exponent))
    double_survival = (double_ssa - threshold) / (fade_start - threshold)
    if 0.0 <= double_survival <= 1.0:
        double_power = math.pow(double_survival, exponent)
        if (value > power) != (hash_value * 2.0 ** -32 > double_power):
            uncertain.append("fade_hash")
    if abs(value - power) <= max(64 * ulp32(power), 1.0e-6):
        uncertain.append("fade_hash")
    return ("fade_reject" if value > power else "keep_in_fade"), sorted(set(uncertain)), ssa


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--level", type=Path, required=True)
    parser.add_argument("--capture", type=Path, required=True)
    parser.add_argument("--marker", type=Path, required=True)
    parser.add_argument("--e01", type=Path, required=True)
    parser.add_argument("--phase", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--upper-bound-file", type=Path,
                        help="write the fixed-frame numerical-reject ID list for a non-shipping ROI test")
    args = parser.parse_args()
    records, level_sha = load_flora_spheres(args.level)
    metadata, captured, frame = read_capture(args.capture)
    serial = read_submitted(args.marker, frame)
    e01 = read_e01(args.e01, frame, serial)
    view = tuple(from_bits(value) for value in metadata["view_bits"])
    projection = tuple(from_bits(value) for value in metadata["projection_bits"])
    eye = tuple(from_bits(value) for value in metadata["eye_bits"])
    settings = tuple(from_bits(value) for value in metadata["ssa_settings_bits"])
    extent = tuple(int(value) for value in metadata["extent"])
    fov, geometry_lod, discard_pixels, exponent, fade_k, screen_factor, threshold = settings
    require(all(math.isfinite(value) for value in view + projection + eye + settings),
            "non-finite captured view or settings")
    require(extent[0] > 0 and extent[1] > 0 and fov > 0 and geometry_lod > 0 and
            discard_pixels > 0 and fade_k > 1 and screen_factor > 0 and threshold > 0,
            "invalid captured SSA settings")
    require(bits(f32(f32(discard_pixels * discard_pixels) / screen_factor)) == bits(threshold),
            "effective r_ssaDISCARD differs from source pixels/screen factor")
    fov_factor = f32(f32(90.0 / fov) ** 2)
    expected_screen = f32(f32(f32(extent[0] * extent[1]) * fov_factor) *
                          f32(f32(EPS_S) + geometry_lod))
    require(bits(expected_screen) == bits(screen_factor),
            "g_fSCREEN differs from source width/height/FOV/geometry LOD formula")
    projected_eye = tuple(sum(view[row + column * 4] * eye[column]
                              for column in range(3)) + view[row + 12]
                          for row in range(3))
    require(all(abs(value) <= 0.001 for value in projected_eye),
            "accepted view does not map its recorded eye to the origin")
    expected_y_scale = 1.0 / math.tan(math.radians(fov * 0.5))
    require(abs(projection[5] - expected_y_scale) <= 0.0001 and
            abs(projection[0] - expected_y_scale * extent[1] / extent[0]) <= 0.0001,
            "accepted projection disagrees with recorded FOV/extent")
    phase = read_phase(args.phase, frame, eye, extent)
    counts = defaultdict(Counter)
    uncertain = Counter()
    uncertain_examples = []
    numerical_rejects = []
    for visual_id, material in captured:
        require(visual_id in records, f"submitted visual {visual_id} absent from winning level")
        record = records[visual_id]
        require(record["material"] == material,
                f"submitted visual {visual_id} has mismatched leaf/trunk material")
        decision, boundary_reasons, _ = classify(
            record["sphere"], record["sphere_bits"], eye, threshold, fade_k, exponent)
        counts[material][decision] += 1
        if decision in ("hard_reject", "fade_reject"):
            numerical_rejects.append((visual_id, material, boundary_reasons))
        for reason in boundary_reasons:
            uncertain[reason] += 1
        if boundary_reasons and len(uncertain_examples) < 20:
            uncertain_examples.append({"visual_id": visual_id, "material": material,
                                       "decision": decision, "reasons": boundary_reasons})
    result = {
        "schema": "main-flora-exact-id-ssa-join/v1",
        "scope": "actual successfully encoded main G-buffer Flora IDs, one submitted frame; no shadow/forward suppression",
        "provenance": {
            "capture": str(args.capture), "capture_sha256": hashlib.sha256(args.capture.read_bytes()).hexdigest(),
            "level": str(args.level.resolve()), "level_sha256": level_sha,
            "engine_frame": frame, "submitted_serial": serial,
            "e01": e01, "phase": phase,
            "accepted_view_bits": metadata["view_bits"],
            "accepted_projection_bits": metadata["projection_bits"],
            "accepted_eye_bits": metadata["eye_bits"],
            "extent": extent,
            "ssa_settings_bits": metadata["ssa_settings_bits"],
            "ssa_settings_order": ["fov_degrees", "geometry_lod", "discard_pixels",
                                   "discard_exponent", "discard_fade_k", "screen_factor",
                                   "effective_discard"],
            "ssa_settings_values": settings,
        },
        "source_flora_visuals": len(records),
        "actual_submitted": len(captured),
        "actual_culled_by_native_clip": int(metadata["culled"][0]),
        "by_material": {key: dict(value) for key, value in counts.items()},
        "all_materials": dict(sum(counts.values(), Counter())),
        "arithmetic_boundary": {"reason_counts": dict(uncertain),
                                "example_ids": uncertain_examples,
                                "guard": "64 float32 ULP at SSA thresholds; 1e-6 or 64 ULP at hash decision; double witness"},
        "exception_status": {
            "visual_eIgnoreOptimization": "unknown_in_flattened_capture",
            "selected_shader_E4_emissive": "unknown_in_flattened_capture",
            "safe_to_suppress": False,
            "rule": "All numerical reject IDs fail open until runtime/source shader exceptions are proven absent per visual/material.",
        },
        "claim_limit": "Work-count opportunity only. No CPU, GPU, displayed FPS, or pixel-equivalence claim.",
    }
    if args.upper_bound_file:
        capture_sha = result["provenance"]["capture_sha256"]
        require(frame == 700 and capture_sha ==
                "dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc" and
                len(numerical_rejects) == 1199 and
                not any(boundary for _, _, boundary in numerical_rejects),
                "upper-bound file requires the exact frame-700 evidence and 1199 unambiguous IDs")
        rows = sorted(numerical_rejects)
        content = ("schema,main-flora-ssa-upper-bound/v1\n"
                   f"source_level_sha256,{level_sha}\n"
                   f"capture_sha256,{capture_sha}\n"
                   "frame,700\ncount,1199\nvisual_id,material\n")
        content += "".join(f"{visual_id},{material}\n" for visual_id, material, _ in rows)
        args.upper_bound_file.parent.mkdir(parents=True, exist_ok=True)
        args.upper_bound_file.write_text(content)
        result["upper_bound_file"] = {
            "path": str(args.upper_bound_file),
            "sha256": hashlib.sha256(content.encode()).hexdigest(),
            "ids": len(rows),
            "note": "Numerical ROI upper bound only; ignores unproven shader/visual exceptions.",
        }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"frame": frame, "serial": serial, "submitted": len(captured),
                      "by_material": result["by_material"],
                      "uncertain": result["arithmetic_boundary"]["reason_counts"]}))


if __name__ == "__main__":
    main()
