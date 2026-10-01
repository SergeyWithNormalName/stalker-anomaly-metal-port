#!/usr/bin/env python3
"""Derive conservative per-visual SSFX wind/bender displacement envelopes."""

import argparse
import csv
import hashlib
import json
import math
import os
import struct
import sys


EXPECTED_WIND_SHA = "7515eff2f59b25f5357fb3a02cf5e9f3b48acecb8a9dc4dd1607fe2fc8101643"
EXPECTED_TEXTURE_SHA = "4fb08cd504bb2a2e7be7f128d3eb2533b24afae8645ee977fa7321a2821b29e2"
MAX_FLOAT = struct.unpack("<f", bytes.fromhex("ffff7f7f"))[0]


def digest(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def json_int(value):
    # JSON's `-0` is tokenized as an integer by Python's default parser.
    return -0.0 if value == "-0" else int(value)


def f32(value):
    return struct.unpack("<f", struct.pack("<f", float(value)))[0]


def f32_bits(value):
    return struct.unpack("<I", struct.pack("<f", f32(value)))[0]


def saturate(value):
    if not math.isfinite(value):
        raise ValueError("nonfinite saturate input")
    return min(1.0, max(0.0, value))


def close_json_to_binary_frame(wind, topology, fixture_path):
    callback = wind["callback_view"]
    flora = wind["flora_view_state"]
    if not wind.get("available") or not flora.get("wind_valid"):
        raise ValueError("wind observation is unavailable or invalid")
    if callback["engine_frame"] != 700 or flora["environment_engine_frame"] != 700 or flora["motion_snapshot_engine_frame"] != 700:
        raise ValueError("frame provenance is not the requested frame 700")
    if callback["previous_frame"] != 699 or not callback["valid"] or not callback["previous_valid"]:
        raise ValueError("callback view provenance is incomplete")
    if flora["valid"] != callback["valid"] or flora["previous_valid"] != callback["previous_valid"]:
        raise ValueError("flora and callback view validity mismatch")
    for key in ("eye", "view", "projection", "previous_view", "previous_projection"):
        if not all(math.isfinite(float(x)) for x in callback[key] + flora[key]):
            raise ValueError(f"nonfinite callback/flora {key}")
        if [f32_bits(x) for x in callback[key]] != [f32_bits(x) for x in flora[key]]:
            raise ValueError(f"callback/flora {key} mismatch")
    if len(callback["view"]) != 16 or len(callback["projection"]) != 16:
        raise ValueError("view matrix dimensions are unsupported")

    topology_frame = topology["engine_frame"]
    if topology_frame != 700:
        raise ValueError("source-topology provenance frame mismatch")
    for key, bits_key in (("eye", "eye_bits"), ("view", "view_bits"), ("projection", "projection_bits")):
        if [f32_bits(x) for x in callback[key]] != topology[bits_key]:
            raise ValueError(f"wind JSON and source-topology {key} IEEE bits differ")

    with open(fixture_path, "rb") as f:
        raw = f.read()
    if len(raw) < 36 or raw[:8] != b"R4SSA1\0\0":
        raise ValueError("old frame fixture header is malformed")
    _, count = struct.unpack_from("<8sI", raw, 0)
    old_eye = struct.unpack_from("<3f", raw, 12)
    discard, fade_k, exponent = struct.unpack_from("<3f", raw, 24)
    if count != 5316 or len(raw) != 36 + count * 28:
        raise ValueError("old frame fixture record count or byte length mismatch")
    if [f32_bits(x) for x in callback["eye"]] != [f32_bits(x) for x in old_eye]:
        raise ValueError("wind JSON and source-sphere fixture eye bits differ")
    if f32_bits(topology["r_ssaDISCARD"]) != f32_bits(discard):
        raise ValueError("source-topology and source-sphere SSA discard differ")
    return {"eye": old_eye, "discard": discard, "fade_k": fade_k, "exponent": exponent}


def h_displacement_factor_max(h_min, h_max):
    if not (math.isfinite(h_min) and math.isfinite(h_max) and h_min <= h_max):
        raise ValueError("invalid transformed H interval")
    # H^2*saturate(1-.005H) is H^2 below zero, a cubic on [0,200],
    # and zero above 200. Include its only interior positive critical point.
    candidates = [h_min, h_max]
    if h_min <= 0.0 <= h_max:
        candidates.append(0.0)
    critical = 2.0 / 0.015
    if h_min <= critical <= h_max:
        candidates.append(critical)
    if h_min <= 200.0 <= h_max:
        candidates.append(200.0)
    return max(h * h * saturate(1.0 - 0.005 * h) for h in candidates)


def tree_wind_constants(flora):
    wind_params = [float(x) for x in flora["wind_params"]]
    tree_setup = [float(x) for x in flora["wind_setup_trees"]]
    if len(wind_params) != 4 or len(tree_setup) != 4 or not all(map(math.isfinite, wind_params + tree_setup)):
        raise ValueError("unsupported/nonfinite tree wind constants")
    wind_speed = max(tree_setup[3], saturate(wind_params[1] * 0.001))
    if wind_speed < 0.0:
        raise ValueError("negative effective wind speed")
    sqrt_speed = saturate(math.sqrt(wind_speed * 1.66))
    w_speed = saturate(sqrt_speed * 1.5)
    t_wave_abs = 1.5 * abs(tree_setup[2]) * (1.0 + 2e-6)
    # TWave=(cos(p)*sin(5p)+.5)*bend. Each trig result lies in [-1,1],
    # so abs(cos*sin+.5) <= 1.5 without assuming a favorable phase correlation.
    trunk_z_max = saturate((1.0 + t_wave_abs) * 0.5)
    return wind_speed, w_speed, t_wave_abs, trunk_z_max


def trunk_vector_bound(h_min, h_max, w_speed, t_wave_abs):
    max_factor = h_displacement_factor_max(h_min, h_max)
    # HLSL's float sin/cos and multiply have a tiny outward allowance below.
    # The sqrt(2) covers independently bounded direction components.
    value = math.sqrt(2.0) * 0.006 * w_speed * w_speed * t_wave_abs * max_factor
    return value * (1.0 + 2e-6)


def branch_vector_bound(h_min, h_max, uv_abs, wind_speed, trunk_z_max):
    if not (math.isfinite(uv_abs) and uv_abs >= 0.0):
        raise ValueError("invalid per-visual UV multiplier bound")
    # clamp(H*.1,1,2.5) is monotone, so its maximum comes from Hmax.
    branch_height_scale = min(2.5, max(1.0, h_max * 0.1))
    horizontal_raw = math.sqrt(2.0) * (trunk_z_max * branch_height_scale + 1.0)
    branch_vector = uv_abs * wind_speed * math.sqrt(horizontal_raw * horizontal_raw + 1.0)
    return branch_vector, branch_height_scale


def bender_strengths(bender, direction, setup):
    if len(bender) != 4 or len(direction) != 4 or len(setup) != 4:
        raise ValueError("invalid bender array shape")
    radius, strength = float(bender[3]), float(direction[3])
    if not all(map(math.isfinite, (radius, strength, *setup))):
        raise ValueError("nonfinite bender input")
    non_dynamic = radius <= 0.0
    effective_radius = float(setup[0]) if non_dynamic else radius
    horizontal_strength = strength * (float(setup[1]) if non_dynamic else 1.0)
    vertical_strength = strength * (float(setup[2]) if non_dynamic else 1.0)
    denominator = f32(f32(effective_radius) + f32(0.001))
    return non_dynamic, effective_radius, denominator, horizontal_strength, vertical_strength


def f32_ceil_with_margin(value, ulps=8):
    if not math.isfinite(value) or value < 0.0 or value > MAX_FLOAT:
        raise ValueError("displacement/radius overflows float range")
    if value == 0.0:
        return 0.0
    rounded = f32(value)
    bits = struct.unpack("<I", struct.pack("<f", rounded))[0]
    if rounded < value:
        bits += 1
    bits += ulps
    if bits >= 0x7F800000:
        raise ValueError("outward float32 rounding overflow")
    return struct.unpack("<f", struct.pack("<I", bits))[0]


def bender_sequence_bound(row, sphere_center, uv_abs, benders, setup, base_radius, sequence_name):
    if len(benders) != 32 or not all(len(v) == 4 for v in benders) or len(setup) != 4:
        raise ValueError("unsupported bender array shape")
    radius = base_radius
    total = 0.0
    reasons = []
    min_normalize_clearance = math.inf
    min_influence_clearance = math.inf
    for b in range(9):  # SSFX_INT_GRASS=8, shader loop is inclusive.
        pos = benders[b]
        direction = benders[b + 16]
        non_dynamic, effective_radius, denominator, strength_x, strength_y = bender_strengths(pos, direction, setup)
        center = [float(x) for x in pos[:3]]
        if not all(map(math.isfinite, center)):
            raise ValueError(f"nonfinite {sequence_name} bender {b} center")

        # At this stage every possible vertex lies in a sphere around the source
        # sphere center with `radius`. If a bender center falls in that sphere,
        # normalize(pos-center) may see zero for some referenced vertex: fail open.
        center_distance = math.dist(sphere_center, center)
        domain_guard = 2e-5 * max(1.0, radius)
        normalize_clearance = center_distance - radius - domain_guard
        min_normalize_clearance = min(min_normalize_clearance, normalize_clearance)
        if normalize_clearance <= 0.0:
            reasons.append(f"zero_normalize_possible_{sequence_name}_b{b}")

        dx = sphere_center[0] - center[0]
        dz = sphere_center[2] - center[2]
        xz_center_distance = math.hypot(dx, dz)
        # The enclosing 3D sphere projects to an XZ disk with the same radius.
        xz_min = max(0.0, xz_center_distance - radius - domain_guard)
        if denominator < 0.0:
            bend_bound = 1.0
        elif denominator == 0.0:
            if xz_min <= domain_guard:
                reasons.append(f"zero_bender_distance_denominator_{sequence_name}_b{b}")
            bend_bound = 1.0 if xz_min <= domain_guard else 0.0
        else:
            denominator_upper = denominator + 2e-6 * max(1.0, abs(denominator))
            min_influence_clearance = min(min_influence_clearance, xz_min - denominator_upper)
            bend_bound = max(0.0, 1.0 - xz_min / denominator_upper)
            if bend_bound > 0.0:
                bend_bound = min(1.0, bend_bound + 2e-6)

        # normalize(bend) has unit length when defined; dir_limit and bend are
        # saturated to [0,1]. Height limit contributes <= abs(1-tc.y).
        per_bender = uv_abs * bend_bound * math.sqrt(
            2.0 * (2.25 * abs(strength_x)) ** 2 + (0.67 * abs(strength_y)) ** 2
        )
        per_bender *= 1.0 + 2e-6
        if not math.isfinite(per_bender) or per_bender < 0.0:
            raise ValueError(f"invalid {sequence_name} bender bound at {b}")
        per_bender = f32_ceil_with_margin(per_bender)
        total += per_bender
        radius += per_bender
        if not (math.isfinite(total) and math.isfinite(radius)) or radius > MAX_FLOAT:
            raise ValueError(f"{sequence_name} bender bound overflow")
    return f32_ceil_with_margin(total), reasons, min_normalize_clearance, min_influence_clearance


def csv_float(value):
    return format(float(value), ".17g")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--wind", required=True)
    parser.add_argument("--topology", required=True)
    parser.add_argument("--mesh", required=True)
    parser.add_argument("--fixture", required=True)
    parser.add_argument("--texture", required=True)
    parser.add_argument("--texture-receipt", required=True)
    parser.add_argument("--shader-receipt", required=True)
    parser.add_argument("--wind-source", required=True)
    parser.add_argument("--leaf-source", required=True)
    parser.add_argument("--trunk-source", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    if digest(args.wind) != EXPECTED_WIND_SHA:
        raise ValueError("live wind fixture hash differs from the assigned frame-700 capture")
    if digest(args.texture) != EXPECTED_TEXTURE_SHA:
        raise ValueError("wind-wave DDS does not match the proven normalized texture receipt")
    receipt_text = open(args.texture_receipt, encoding="utf-8").read()
    if EXPECTED_TEXTURE_SHA not in receipt_text or "RGBA8Unorm" not in receipt_text or "[0,1]" not in receipt_text:
        raise ValueError("normalized texture receipt is not present/verified")
    shader_receipt = open(args.shader_receipt, encoding="utf-8").read()
    for token in ("SSFX_WIND=1", "SSFX_INT_GRASS=8", "deffer_tree_branch_bump-hq.vs", "deffer_tree_flat.vs"):
        if token not in shader_receipt:
            raise ValueError(f"pinned shader receipt misses {token}")
    wind_source = open(args.wind_source, encoding="utf-8").read()
    leaf_source = open(args.leaf_source, encoding="utf-8").read()
    trunk_source = open(args.trunk_source, encoding="utf-8").read()
    for token in ("s_waves.SampleLevel", "ssfx_wind_tree_trunk", "ssfx_wind_tree_branches", "(1.0f - tc_y) * W.speed"):
        if token not in wind_source:
            raise ValueError(f"pinned wind source misses {token}")
    for token in ("normalize(w_pos.xyz - benders_pos[b].xyz)", "SSFX_INT_GRASS + 1", "2.25f", "0.67f"):
        if token not in leaf_source:
            raise ValueError(f"pinned leaf source misses {token}")
    if "ssfx_wind_tree_trunk" not in trunk_source:
        raise ValueError("pinned trunk source does not call the proven trunk wind helper")

    json_opts = {"parse_float": float, "parse_int": json_int,
                 "parse_constant": lambda value: (_ for _ in ()).throw(ValueError(f"JSON nonfinite: {value}"))}
    with open(args.wind, encoding="utf-8") as f:
        wind = json.load(f, **json_opts)
    with open(args.topology, encoding="utf-8") as f:
        topology = json.load(f, **json_opts)
    fixture_meta = close_json_to_binary_frame(wind, topology, args.fixture)
    flora = wind["flora_view_state"]
    float_arrays = ("current_benders", "previous_benders", "benders_setup", "wind_params",
        "wind_setup_grass", "wind_setup_trees", "wind_animation_current", "wind_animation_previous")
    for key in float_arrays:
        values = flora.get(key)
        if values is None:
            raise ValueError(f"missing captured wind field {key}")
        def flatten(value):
            if isinstance(value, list):
                for child in value:
                    yield from flatten(child)
            else:
                yield float(value)
        if not all(math.isfinite(value) for value in flatten(values)):
            raise ValueError(f"nonfinite captured wind field {key}")
    if not math.isfinite(float(flora["time_seconds"])) or not math.isfinite(float(flora["tree_scale_bias_correction"])):
        raise ValueError("nonfinite captured flora scalar")
    setup = [float(x) for x in flora["benders_setup"]]
    current = flora["current_benders"]
    previous = flora["previous_benders"]
    if len(current) != 32 or len(previous) != 32:
        raise ValueError("live bender snapshot does not have 32 float4 entries")
    wind_speed, w_speed, t_wave_abs, trunk_z = tree_wind_constants(flora)

    with open(args.mesh, newline="", encoding="utf-8") as f:
        mesh_rows = list(csv.DictReader(f))
    rejected = [row for row in mesh_rows if row["ssa_reject"] == "1"]
    if len(mesh_rows) != 5316 or len(rejected) != 1199:
        raise ValueError("mesh-extrema input does not match 5316 visuals / 1199 source rejects")

    result = []
    bender_clearances = []
    for row in rejected:
        visual_id = int(row["visual_id"])
        shader = row["shader"]
        if shader not in ("flora\\leaf_wave", "flora\\trunk_wave"):
            result.append({"id": visual_id, "shader": shader, "status": "unknown", "reason": "unsupported_shader"})
            continue
        center = [float(row[k]) for k in ("sphere_x", "sphere_y", "sphere_z")]
        original_radius = float(row["sphere_radius"])
        rest_radius = float(row["max_world_vertex_distance_to_sphere"])
        h_min, h_max = float(row["min_transformed_H"]), float(row["max_transformed_H"])
        uv_abs = float(row["max_abs_1_minus_tc_y"])
        wind_bound = 0.0
        current_bender_bound = 0.0
        previous_bender_bound = 0.0
        displacement = 0.0
        if not all(map(math.isfinite, center + [original_radius, rest_radius, h_min, h_max, uv_abs])) or \
            original_radius <= 0.0 or rest_radius <= 0.0 or uv_abs < 0.0 or h_min > h_max:
            result.append({"id": visual_id, "shader": shader, "status": "unknown", "reason": "invalid_mesh_row"})
            continue
        reasons = []
        if shader == "flora\\trunk_wave":
            displacement = trunk_vector_bound(h_min, h_max, w_speed, t_wave_abs)
            current_bender_bound = previous_bender_bound = 0.0
            wind_bound = displacement
        else:
            branch_bound, _ = branch_vector_bound(h_min, h_max, uv_abs, wind_speed, trunk_z)
            trunk_bound = trunk_vector_bound(h_min, h_max, w_speed, t_wave_abs)
            wind_bound = branch_bound + trunk_bound
            if not math.isfinite(wind_bound) or wind_bound < 0.0:
                reasons.append("invalid_leaf_wind_bound")
                wind_bound = 0.0
            wind_bound = f32_ceil_with_margin(wind_bound)
            base_radius = rest_radius + wind_bound
            if not math.isfinite(base_radius) or base_radius > MAX_FLOAT:
                reasons.append("leaf_base_radius_overflow")
                base_radius = rest_radius
            try:
                current_bender_bound, current_reasons, current_norm_clear, current_influence_clear = bender_sequence_bound(
                    row, center, uv_abs, current, setup, base_radius, "current")
                previous_bender_bound, previous_reasons, previous_norm_clear, previous_influence_clear = bender_sequence_bound(
                    row, center, uv_abs, previous, setup, base_radius, "previous")
                reasons.extend(current_reasons)
                reasons.extend(previous_reasons)
                bender_clearances.append({
                    "visual_id": visual_id,
                    "current_min_normalize_clearance": current_norm_clear,
                    "previous_min_normalize_clearance": previous_norm_clear,
                    "current_min_influence_clearance": current_influence_clear,
                    "previous_min_influence_clearance": previous_influence_clear,
                })
            except (ValueError, OverflowError) as exc:
                reasons.append("bender_bound_failure:" + str(exc).replace(",", ";"))
                current_bender_bound = previous_bender_bound = 0.0
            displacement = wind_bound + max(current_bender_bound, previous_bender_bound)
        if not math.isfinite(displacement) or displacement < 0.0 or displacement > MAX_FLOAT:
            reasons.append("displacement_bound_overflow")
        try:
            adjusted = f32_ceil_with_margin(rest_radius + displacement) if not reasons else None
        except (ValueError, OverflowError):
            adjusted = None
            reasons.append("adjusted_radius_float_overflow")
        status = "unknown" if reasons or adjusted is None else "bounded"
        result.append({
            "visual_id": visual_id,
            "source_rejected": 1,
            "shader": shader,
            "sphere_x": center[0], "sphere_y": center[1], "sphere_z": center[2],
            "source_radius": original_radius,
            "rest_max_distance": rest_radius,
            "h_min": h_min, "h_max": h_max,
            "uv_abs_max": uv_abs,
            "wind_bound": wind_bound,
            "current_bender_bound": current_bender_bound,
            "previous_bender_bound": previous_bender_bound,
            "displacement_bound": displacement if math.isfinite(displacement) else "",
            "adjusted_radius": adjusted if adjusted is not None else "",
            "status": status,
            "reason": ";".join(reasons),
        })

    with open(args.out, "w", newline="", encoding="utf-8") as f:
        columns = ["visual_id", "source_rejected", "shader", "sphere_x", "sphere_y", "sphere_z",
            "source_radius", "rest_max_distance", "h_min", "h_max", "uv_abs_max", "wind_bound",
            "current_bender_bound", "previous_bender_bound", "displacement_bound", "adjusted_radius",
            "status", "reason"]
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for item in result:
            writer.writerow({k: csv_float(v) if isinstance(v, float) else v for k, v in item.items()})

    clearance_path = os.path.splitext(args.out)[0] + ".bender-clearance.csv"
    with open(clearance_path, "w", newline="", encoding="utf-8") as f:
        columns = ["visual_id", "current_min_normalize_clearance", "previous_min_normalize_clearance",
            "current_min_influence_clearance", "previous_min_influence_clearance"]
        writer = csv.DictWriter(f, fieldnames=columns)
        writer.writeheader()
        for item in bender_clearances:
            writer.writerow({k: csv_float(v) for k, v in item.items()})

    summary = {
        "frame": 700,
        "callback_previous_frame": 699,
        "topology_sector_count": topology["source_sector_count"],
        "topology_visual_count": topology["source_visual_count"],
        "topology_geometry_verified": topology["geometry_verified"],
        "source_ssa_rejected": len(rejected),
        "mesh_rows": len(mesh_rows),
        "wind_speed": wind_speed,
        "wind_wspeed": w_speed,
        "t_wave_abs_bound": t_wave_abs,
        "trunk_z_max_bound": trunk_z,
        "wind_wave_texture_sha256": digest(args.texture),
        "wind_wave_texture_normalized_contract": True,
        "safe_displacement_bounds": sum(x.get("status") == "bounded" for x in result),
        "unknown_displacement_bounds": sum(x.get("status") == "unknown" for x in result),
        "leaf_bender_visuals": len(bender_clearances),
        "min_normalize_clearance_m": min((min(x["current_min_normalize_clearance"], x["previous_min_normalize_clearance"]) for x in bender_clearances), default=None),
        "min_bender_influence_clearance_m": min((min(x["current_min_influence_clearance"], x["previous_min_influence_clearance"]) for x in bender_clearances), default=None),
        "input_wind_sha256": digest(args.wind),
        "input_mesh_sha256": digest(args.mesh),
        "input_source_topology_sha256": digest(args.topology),
        "input_old_id_fixture_sha256": digest(args.fixture),
        "ssa_settings": fixture_meta,
        "notes": [
            "This derives a same-frame analytical upper bound only; it does not validate global or temporal safety.",
            "Any possible zero argument to HLSL normalize fails open for that visual.",
            "Eight float32 ULPs are added only for outward rounding after full shader terms are bounded.",
        ],
    }
    summary_path = os.path.splitext(args.out)[0] + ".summary.json"
    with open(summary_path, "w", encoding="utf-8") as f:
        json.dump(summary, f, indent=2, allow_nan=False)
        f.write("\n")
    print(json.dumps({k: summary[k] for k in (
        "source_ssa_rejected", "safe_displacement_bounds", "unknown_displacement_bounds",
        "wind_speed", "wind_wspeed", "t_wave_abs_bound", "trunk_z_max_bound",
        "leaf_bender_visuals", "min_normalize_clearance_m", "min_bender_influence_clearance_m")}, indent=2))


if __name__ == "__main__":
    try:
        main()
    except Exception as exc:
        print(f"wind-envelope screen stopped: {exc}", file=sys.stderr)
        raise
