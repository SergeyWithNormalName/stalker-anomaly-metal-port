#!/usr/bin/env python3
"""Synthetic contract tests for E01 run aggregation."""

import contextlib
import csv
import hashlib
import io
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import aggregate_metal_e01 as aggregate


def sha(char):
    return char * 64


class E01AggregationTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.registry = self.root / "FIXTURES.json"
        self.registry_data = {"schema": "direct-metal-release-fixtures/v1", "fixtures": {}}
        for fixture in aggregate.FIXTURES:
            self.registry_data["fixtures"][fixture] = {
                "status": "ready",
                "level": f"level_{fixture.lower()}",
                "camera": "1,2,3",
                "direction": "0,0,-1",
                "weather": f"weather_{fixture}",
                "weather_weight": 0.0,
                "time": "08:00:00",
                "save_name": f"save_{fixture}",
                "save_sha256": sha("a"),
                "scenario": "static" if fixture == "S0" else fixture,
            }
        self.write_json(self.registry, self.registry_data)
        self.run_specs = []
        for fixture in aggregate.FIXTURES:
            for index in range(1, 4):
                path = self.root / f"{fixture.lower()}-r{index}"
                self.make_run(fixture, path, index)
                self.run_specs.append((fixture, path))

    def tearDown(self):
        self.temp.cleanup()

    @staticmethod
    def write_json(path, data):
        path.write_text(json.dumps(data, sort_keys=True), encoding="utf-8")

    def make_run(self, fixture, path, index):
        contract = self.registry_data["fixtures"][fixture]
        registry_hash = hashlib.sha256(self.registry.read_bytes()).hexdigest()
        path.mkdir(parents=True, exist_ok=True)
        window = {
            "start_engine_frame": 1000,
            "end_engine_frame": 1120,
            "warmup_seconds": 60,
            "sample_seconds": 120,
        }
        baseline = {
            "schema": "direct-metal-e01-baseline/v1",
            "timing_window": {key: window[key] for key in ("start_engine_frame", "end_engine_frame")},
            "rows_accepted": 121,
            "rows_rejected": 0,
            "frame_interval_ms": {"count": 120},
            "displayed_frame_interval_ms": {
                "count": 120,
                "p50": 16.6 + index / 10,
                "p95": 20.0 + index / 10,
                "p99": 24.0 + index / 10,
                "max": 28.0 + index / 10,
                "one_percent_low_fps": 35.0 + index,
            },
            "gpu_frame_ms": {"count": 121, "p50": 7.0 + index},
            "cpu_stage_ms": {
                "scene_ms": {"count": 121, "p50": 4.0 + index},
                "wait_ms": {"count": 121, "p50": 2.0},
                "drawable_ms": {"count": 121, "p50": 1.0},
                "commit_ms": {"count": 121, "p50": 0.5},
            },
            "presentation": {
                "pacing_gate": "CLOSED",
                "displayed_timestamp_samples": 121,
                "displayed_interval_samples": 120,
                "zero_timestamp_callbacks": 0,
                "serial_gap_events": 0,
                "serial_gap_frames": 0,
                "display_metrics_status": "complete",
            },
        }
        provenance = {
            "backend": "metal",
            "fixture": fixture,
            "git_head": "commit-123",
            "build_options": "build-flags",
            "dirty_patch_sha256": sha("b"),
            "untracked_list_sha256": sha("c"),
            "shader_receipt_set_sha256": sha("d"),
            "binary": {"sha256": sha("e")},
            "profile": {"manifest_sha256": sha("f"), "user_ltx_sha256": sha("0")},
            "tool_versions": {"xcode": "Xcode 1", "msc": "MSC 1"},
            "effective_env": {
                "XRAY_PARITY_FIXTURE_REGISTRY_SHA256": registry_hash,
                "XRAY_METAL_NATIVE_RETINA": "1",
                "XRAY_BENCHMARK_LOGICAL_SIZE": "960x540",
            },
        }
        telemetry_contract = {
            "registry_sha256": registry_hash,
            "expected_level": contract["level"],
            "camera_pos": contract["camera"],
            "camera_dir": contract["direction"],
            "weather": contract["weather"],
            "weather_weight": contract["weather_weight"],
            "time": contract["time"],
            "save_name": contract["save_name"],
            "scenario": contract["scenario"],
            "camera_position_tolerance_m": 0.0001,
            "camera_angle_tolerance_rad": 0.0001,
        }
        telemetry = {
            "fixture": fixture,
            "backend": "Direct Metal",
            "loading_screen_active": False,
            "fixture_scenario": {"status": "NOT_APPLICABLE" if fixture == "S0" else "OK", "failures": [], "fixture": fixture},
            "fixture_contract": telemetry_contract,
            "level_name": contract["level"],
            "save_sha256": contract["save_sha256"],
            "camera": {"pos": [1.0, 2.0, -13.0], "dir": [1.0, 0.0, 0.0]} if fixture == "S4" else
                {"pos": [1.0, 2.0, 3.0], "dir": [0.0, 0.0, -1.0]},
            "weather": {"weather_name": contract["weather"], "weather_weight": 0.0},
            "game_time": {"hours": 8, "minutes": 0, "seconds": 0},
            "material_set_sha256": sha("1"),
            "actual_drawable_width": 1920,
            "actual_drawable_height": 1080,
            "engine_width": 960,
            "engine_height": 540,
            "engine_aspect": 16 / 9,
            "engine_fov": 1.2,
            "options": {"quality": "high"},
            "tonemap_and_color_grading": {"enabled": True},
        }
        for name, value in (("baseline.json", baseline), ("provenance.json", provenance),
                            ("telemetry.json", telemetry), ("timing-window.json", window)):
            self.write_json(path / name, value)
        (path / "frames.csv").write_text(
            "serial,engine_frame\n" +
            "".join(f"{serial},{999 + serial}\n" for serial in range(1, 122)),
            encoding="utf-8",
        )

    def cli_args(self, output):
        args = ["--output", str(output), "--registry", str(self.registry)]
        for fixture, path in self.run_specs:
            args += ["--run", f"{fixture}:{path}"]
        return args

    def test_aggregates_twelve_valid_runs_to_diagnostic_budget(self):
        output = self.root / "aggregate"
        with contextlib.redirect_stdout(io.StringIO()):
            result = aggregate.main(self.cli_args(output))
        self.assertEqual(result, 0)
        baseline = json.loads((output / "baseline.json").read_text())
        budget = (output / "budget.md").read_text()
        self.assertTrue(baseline["all_fixture_groups_three_of_three"])
        self.assertEqual(baseline["classification"], "DIAGNOSTIC_BUDGET_ONLY")
        self.assertEqual(baseline["fixture_groups"]["S2"]["run_count"], 3)
        self.assertEqual(len(baseline["top_three_measured_scene_costs"]), 3)
        self.assertEqual(baseline["fixture_groups"]["S2"]["all_three_status"], "VALID")
        self.assertIn("1% low FPS", budget)
        self.assertEqual(baseline["release_60_fps_gate"], "FAIL")
        self.assertIn("60 FPS gate: FAIL", budget)
        with (output / "frames.csv").open(newline="", encoding="utf-8") as stream:
            frames = list(csv.DictReader(stream))
        self.assertEqual(len(frames), 12 * 121)
        self.assertEqual((frames[0]["fixture"], frames[0]["run_id"]), ("S0", "s0-r1"))
        self.assertEqual((frames[-1]["fixture"], frames[-1]["run_id"]), ("S5", "s5-r3"))

    def test_rejects_missing_run_count_and_contract_mismatch_before_writing(self):
        output = self.root / "should-not-exist"
        args = self.cli_args(output)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(aggregate.main(args[:-2]), 2)
        self.assertFalse(output.exists())

        provenance_path = self.run_specs[0][1] / "provenance.json"
        provenance = json.loads(provenance_path.read_text())
        provenance["binary"]["sha256"] = sha("9")
        self.write_json(provenance_path, provenance)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(aggregate.main(self.cli_args(output)), 2)
        self.assertFalse(output.exists())

    def test_accepts_open_partial_presentation_as_diagnostic_and_keeps_gate_open(self):
        output = self.root / "partial-aggregate"
        for fixture, path in self.run_specs:
            baseline_path = path / "baseline.json"
            baseline = json.loads(baseline_path.read_text())
            baseline["presentation"].update({
                "pacing_gate": "OPEN",
                "displayed_timestamp_samples": 119,
                "displayed_interval_samples": 118,
                "serial_gap_events": 1,
                "serial_gap_frames": 2,
                "zero_timestamp_callbacks": 2,
                "display_metrics_status": "partial",
            })
            baseline["displayed_frame_interval_ms"]["count"] = 118
            baseline["displayed_frame_interval_ms"]["one_percent_low_fps"] = 70.0
            self.write_json(baseline_path, baseline)
        with contextlib.redirect_stdout(io.StringIO()):
            result = aggregate.main(self.cli_args(output))
        self.assertEqual(result, 0)
        baseline = json.loads((output / "baseline.json").read_text())
        self.assertTrue(baseline["all_fixture_groups_three_of_three"])
        self.assertEqual(baseline["release_60_fps_gate"], "OPEN")

    def test_rejects_invalid_timing_window(self):
        output = self.root / "should-not-exist"
        window_path = self.run_specs[0][1] / "timing-window.json"
        window = json.loads(window_path.read_text())
        window["sample_seconds"] = 119
        self.write_json(window_path, window)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(aggregate.main(self.cli_args(output)), 2)
        self.assertFalse(output.exists())

    def test_accepts_fixture_specific_options_but_rejects_mismatch_within_fixture(self):
        output = self.root / "fixture-options"
        for fixture, path in self.run_specs:
            if fixture != "S5":
                continue
            telemetry_path = path / "telemetry.json"
            telemetry = json.loads(telemetry_path.read_text())
            telemetry["options"]["quality"] = "indoor"
            self.write_json(telemetry_path, telemetry)
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(aggregate.main(self.cli_args(output)), 0)
        fixture, path = next(spec for spec in self.run_specs if spec[0] == "S5")
        telemetry_path = path / "telemetry.json"
        telemetry = json.loads(telemetry_path.read_text())
        telemetry["options"]["quality"] = "different"
        self.write_json(telemetry_path, telemetry)
        with contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(aggregate.main(self.cli_args(self.root / "reject-options")), 2)


if __name__ == "__main__":
    unittest.main()
