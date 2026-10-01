#!/usr/bin/env python3
"""Synthetic negative cases for the fail-closed E00 parity admission contract."""

import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "scratch"))
from parity_manifest_contract import evaluate_runs

EXPECTED_LEVEL = "k00_marsh"
EXPECTED_FIXTURE = "S0"


def telemetry(backend, is_direct_metal):
    return {
        "backend": backend, "is_direct_metal": is_direct_metal,
        "save_sha256": "a" * 64, "fixture": EXPECTED_FIXTURE,
        "material_set_sha256": "b" * 64, "level_name": EXPECTED_LEVEL,
        "loading_screen_active": False,
        "loading_screen_dismissed": True,
        "fixture_scenario": {"status": "NOT_APPLICABLE", "failures": []},
        "game_time_hours": 8, "game_time_minutes": 0, "game_time_seconds": 0,
        "engine_width": 960, "engine_height": 540,
        "actual_drawable_width": 1920, "actual_drawable_height": 1080,
        "camera": {
            "pos": [1.0, 2.0, 3.0], "dir": [0.0, 0.0, 1.0], "top": [0.0, 1.0, 0.0], "right": [1.0, 0.0, 0.0],
            "fov": 55.0, "aspect_ratio": 16 / 9, "z_near": 0.2, "z_far": 409.5,
            "view_matrix": [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]],
            "projection_matrix": [[1.0, 0.0, 0.0, 0.0], [0.0, 1.0, 0.0, 0.0], [0.0, 0.0, 1.0, 0.0], [0.0, 0.0, 0.0, 1.0]],
        },
        "weather": {
            "weather_name": "clear", "sky_texture": "sky", "clouds_texture": "clouds", "weather_weight": 0.0,
            "fog_density": 1.0, "fog_distance": 400.0, "far_plane": 409.5, "sky_rotation": 0.0,
            "rain_density": 0.0, "wind_velocity": 0.0, "wind_direction": 0.0, "sun_shafts_intensity": 0.0,
            "water_intensity": 0.0, "bloom_threshold": 7.0, "bloom_exposure": 3.3,
            "sun_color": [1.0, 1.0, 1.0], "ambient_color": [1.0, 1.0, 1.0], "hemisphere_color": [1.0, 1.0, 1.0],
            "fog_color": [1.0, 1.0, 1.0], "sky_color": [1.0, 1.0, 1.0], "clouds_color": [1.0, 1.0, 1.0], "sun_dir": [0.0, 1.0, 0.0],
        },
        "tonemap_and_color_grading": {"r__exposure": "1.0", "r2_tonemap": "on"},
        "options": {"r2_soft_water": "on", "vid_mode": "1920x1080"},
    }


def runs():
    reference = "DXVK Native / MoltenVK (proxy)"
    return {
        "ref_run1": {"telemetry": telemetry(reference, False)},
        "ref_run2": {"telemetry": telemetry(reference, False)},
        "ref_run3": {"telemetry": telemetry(reference, False)},
        "metal_run": {"telemetry": telemetry("Direct Metal", True)},
    }


def admission(value_runs):
    return evaluate_runs(value_runs, EXPECTED_LEVEL, EXPECTED_FIXTURE)["comparison_admission"]


def expect_rejected(name, mutate):
    value_runs = runs()
    mutate(value_runs)
    assert admission(value_runs) == "INVALID_COMPARISON", name


assert admission(runs()) == "ADMITTED"
retina_units = runs()
retina_units["metal_run"]["telemetry"].update(engine_width=1920, engine_height=1080)
assert admission(retina_units) == "ADMITTED", "backend-specific Retina window units must not invalidate equal drawables"
backend_window_mode = runs()
backend_window_mode["metal_run"]["telemetry"]["options"]["rs_screenmode"] = "borderless"
backend_window_mode["ref_run1"]["telemetry"]["options"]["rs_screenmode"] = "windowed"
backend_window_mode["ref_run2"]["telemetry"]["options"]["rs_screenmode"] = "windowed"
backend_window_mode["ref_run3"]["telemetry"]["options"]["rs_screenmode"] = "windowed"
assert admission(backend_window_mode) == "ADMITTED", "backend presentation mode is provenance, not scene state"
expect_rejected("empty save hash", lambda r: r["metal_run"]["telemetry"].update(save_sha256=""))
expect_rejected("malformed material hash", lambda r: r["metal_run"]["telemetry"].update(material_set_sha256="not-a-sha"))
expect_rejected("wrong fixture", lambda r: r["metal_run"]["telemetry"].update(fixture="S1"))
expect_rejected("wrong level", lambda r: r["metal_run"]["telemetry"].update(level_name="l05_bar"))
expect_rejected("loading screen active", lambda r: r["metal_run"]["telemetry"].update(loading_screen_active=True))
expect_rejected("loading screen unacknowledged", lambda r: r["metal_run"]["telemetry"].update(loading_screen_dismissed=False))
expect_rejected("missing scenario", lambda r: r["metal_run"]["telemetry"].pop("fixture_scenario"))
expect_rejected("wrong drawable extent", lambda r: r["metal_run"]["telemetry"].update(actual_drawable_width=960))
expect_rejected("invalid hour", lambda r: r["metal_run"]["telemetry"].update(game_time_hours=24))
expect_rejected("seconds differ", lambda r: r["metal_run"]["telemetry"].update(game_time_seconds=59))
expect_rejected("missing seconds", lambda r: r["metal_run"]["telemetry"].pop("game_time_seconds"))
expect_rejected("all DXVK", lambda r: r["metal_run"]["telemetry"].update(backend="DXVK Native / MoltenVK (proxy)", is_direct_metal=False))
expect_rejected("misidentified reference", lambda r: r["ref_run2"]["telemetry"].update(backend="Direct Metal", is_direct_metal=True))
expect_rejected("empty weather", lambda r: r["metal_run"]["telemetry"]["weather"].update(weather_name=""))
expect_rejected("weather preset differs", lambda r: r["metal_run"]["telemetry"]["weather"].update(weather_name="rain"))
expect_rejected("weather differs", lambda r: r["metal_run"]["telemetry"]["weather"].update(weather_weight=0.1))
expect_rejected("tonemap differs", lambda r: r["metal_run"]["telemetry"]["tonemap_and_color_grading"].update(r__exposure="2.0"))
expect_rejected("option differs", lambda r: r["metal_run"]["telemetry"]["options"].update(r2_soft_water="off"))
expect_rejected("missing options", lambda r: r["metal_run"]["telemetry"].pop("options"))
expect_rejected("camera position", lambda r: r["metal_run"]["telemetry"]["camera"]["pos"].__setitem__(0, 1.001))

def tiny_clip_and_wrong_matrix(r):
    camera = r["metal_run"]["telemetry"]["camera"]
    camera["z_far"] += 1.0e-6
    camera["view_matrix"][0][0] = 99.0


expect_rejected("matrix cannot be bypassed by clip delta", tiny_clip_and_wrong_matrix)
print("parity manifest contract: PASS")
