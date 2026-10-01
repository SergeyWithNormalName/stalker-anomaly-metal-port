from __future__ import annotations

import csv
import json
import pathlib
import tempfile
import unittest

import analyze_metal_e07 as e07
import analyze_metal_e01 as e01


def write_json(path: pathlib.Path, value: object) -> None:
    path.write_text(json.dumps(value), encoding="utf-8")


def make_run(root: pathlib.Path, count: int, *, name: str | None = None,
             duplicate_frame_key: bool = False, rendered_zero: bool = False,
             exit_status: int = 0, scenario_failures: bool = False,
             unknown_visible: bool = True) -> pathlib.Path:
    run = root / (name or f"run-n{count}")
    run.mkdir()
    spawned_ids = [str(100 + index) for index in range(count)]
    profile_hash = "a" * 64
    save_hash = "b" * 64
    shader_hash = "c" * 64
    write_json(run / "provenance.json", {
        "backend": "metal",
        "platform": "fixture-platform",
        "binary": {"sha256": "d" * 64},
        "profile": {"manifest_sha256": profile_hash, "user_ltx_sha256": "e" * 64},
        "shader_receipt_set_sha256": shader_hash,
        "effective_env": {
            "XRAY_METAL_E07_NPC_COUNT": str(count),
            "XRAY_BENCHMARK_LOGICAL_SIZE": "960x540",
            "XRAY_METAL_NATIVE_RETINA": "1",
            "XRAY_METAL_E01_SECONDS": "120",
            "XRAY_PARITY_WARMUP_SECONDS": "60",
        },
    })
    write_json(run / "e07-diagnostic.json", {
        "stage": "E07",
        "admission": "DIAGNOSTIC_ONLY_NOT_E00_ADMITTED",
        "requested_spawn_count": count,
        "device_clock": "unfrozen for NPC simulation; weather/game clock remains fixture-paused",
    })
    write_json(run / "timing-window.json", {
        "start_engine_frame": 1,
        "end_engine_frame": 4,
        "warmup_seconds": 60,
        "sample_seconds": 120,
    })
    (run / "engine_exit_status.txt").write_text(f"{exit_status}\n", encoding="utf-8")
    (run / "engine.log").write_text(
        "METAL_DRAWABLE_SIZE logical=960x540 backing=1920x1080 render=1920x1080 policy=native-retina\n",
        encoding="utf-8",
    )
    objects = [{"id": object_id, "label": f"e07_npc_{index + 1}"}
               for index, object_id in enumerate(spawned_ids)]
    events = [{"kind": "restore", "detail": "complete", "elapsed_seconds": 5.0}]
    events.append({"kind": "actual_npc_observation",
                   "detail": f"requested_spawn_count={count} actual_online={count} actual_alive={count} alive_known={count}",
                   "elapsed_seconds": 1.0})
    telemetry = {
        "fixture": "S2",
        "is_direct_metal": True,
        "save_sha256": save_hash,
        "fixture_contract": {"expected_level": "l02_garbage", "scenario": "S2"},
        "fixture_scenario": {
            "status": "FAILED" if scenario_failures else "OK",
            "failures": ["synthetic failure"] if scenario_failures else [],
            "events": events,
            "spawned": objects,
        },
        "camera": {"pos": [1, 2, 3], "dir": [0, 0, 1], "fov": 80},
        "weather": {"weather_name": "w_cloudy2_dark", "weather_weight": 0},
        "options": {"ssfx_ao": "on"},
        "tonemap_and_color_grading": {"r__gamma": "1"},
        "engine_width": 1920,
        "engine_height": 1080,
        "actual_drawable_width": 1920,
        "actual_drawable_height": 1080,
        "engine_fov": 80,
        "engine_aspect": 0.5625,
    }
    write_json(run / "telemetry.json", telemetry)

    with (run / "frames.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=e01.REQUIRED_FIELDS + ("updated_bytes", "buffer_allocations"))
        writer.writeheader()
        for serial, (frame, seconds) in enumerate(zip(range(1, 5), (1, 2, 4, 7)), 1):
            writer.writerow({
                "serial": serial, "engine_frame": frame, "camera_frame": frame, "slot": frame % 3,
                "begin_ns": seconds * 1_000_000_000, "wait_ms": 0, "drawable_ms": 1,
                "scene_ms": 5, "encode_ms": 6, "present_encode_ms": 1, "commit_ms": 1,
                "scheduled_ns": seconds * 1_000_000_000 + 100, "completed_ns": seconds * 1_000_000_000 + 200,
                "gpu_start_s": seconds, "gpu_end_s": seconds + 0.01,
                "presented_s": seconds + 0.02, "queue_depth": 1, "status": "completed",
                "updated_bytes": 0, "buffer_allocations": 0,
            })
    with (run / "frames.csv.presented.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=("serial", "presented_s"))
        writer.writeheader()
        for serial, seconds in enumerate((1, 2, 4, 7), 1):
            writer.writerow({"serial": serial, "presented_s": seconds + 0.02})

    fields = list(e07.E07_FRAME_FIELDS)
    with (run / "e07-frames.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=fields)
        writer.writeheader()
        for frame in range(1, 5):
            row = {field: 0 for field in e07.E07_METRICS}
            row.update({"engine_frame": frame, "slot": frame % 3,
                        "rendered": 0 if rendered_zero and frame == 1 else 1})
            writer.writerow(row)
            if duplicate_frame_key and frame == 1:
                writer.writerow(row)
    with (run / "e07-frames.csv.objects.csv").open("w", newline="", encoding="utf-8") as stream:
        writer = csv.DictWriter(stream, fieldnames=e07.E07_OBJECT_FIELDS)
        writer.writeheader()
        for frame in range(1, 5):
            for object_id in spawned_ids:
                writer.writerow({"object_id": object_id, "frame": frame,
                                 "view_frustum_visible": 1, "bones": "",
                                 "asset_geometry_count": 2, "submitted_to_gbuffer": 1})
            if unknown_visible:
                writer.writerow({"object_id": 999, "frame": frame,
                                 "view_frustum_visible": 1, "bones": 50,
                                 "asset_geometry_count": 4, "submitted_to_gbuffer": 1})
    return run


class AnalyzeMetalE07Tests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)

    def tearDown(self) -> None:
        self.temp.cleanup()

    def test_curve_accepts_all_zero_skinning_and_delegates_one_percent_low(self) -> None:
        paths = [make_run(self.root, count) for count in e07.COUNTS]
        result = e07.analyze_curve(paths, self.root / "out", settle_seconds=0)
        run = e07.analyze_run(paths[1], settle_seconds=0)[0]
        self.assertEqual(run["e07_metrics"]["skin_ms"]["count"], 4)
        self.assertEqual(run["e07_metrics"]["skin_ms"]["p50"], 0)
        self.assertEqual(run["e07_metrics"]["cache_hit"]["count"], 4)
        self.assertEqual(run["timing"]["frame_interval_ms"]["one_percent_low_fps"],
                         e01.frame_metrics([1000.0, 2000.0, 3000.0])["one_percent_low_fps"])
        self.assertEqual(result["schema"], "direct-metal-e07-curve/v1")
        self.assertEqual(result["exact_visibility_gate"], "OPEN_NOT_PROVEN_BY_FRUSTUM_OR_SUBMISSION_FLAGS")
        self.assertTrue((self.root / "out" / "curve.md").is_file())

    def test_settle_seconds_trims_by_begin_timestamp(self) -> None:
        run = make_run(self.root, 1)
        analyzed, _ = e07.analyze_run(run, settle_seconds=1.5)
        self.assertEqual(analyzed["timing"]["rows_accepted"], 2)
        self.assertEqual(analyzed["e07_metrics"]["skin_ms"]["count"], 2)
        self.assertEqual(analyzed["scenario_object_observations"]["coverage"], "missing")

    def test_duplicate_engine_frame_slot_join_is_rejected(self) -> None:
        run = make_run(self.root, 1, duplicate_frame_key=True)
        with self.assertRaisesRegex(ValueError, "duplicate E07 engine_frame\+slot"):
            e07.analyze_run(run, settle_seconds=0)

    def test_rendered_zero_is_rejected(self) -> None:
        run = make_run(self.root, 1, rendered_zero=True)
        with self.assertRaisesRegex(ValueError, "rendered=0"):
            e07.analyze_run(run, settle_seconds=0)

    def test_missing_drawable_log_evidence_keeps_coverage_open(self) -> None:
        run = make_run(self.root, 0)
        (run / "engine.log").write_text("no drawable record\n", encoding="utf-8")
        analyzed, _ = e07.analyze_run(run, settle_seconds=0)
        self.assertEqual(analyzed["coverage"]["drawable_evidence_status"], "OPEN_MISSING_LOG_EVIDENCE")
        self.assertEqual(analyzed["coverage_status"], "INCOMPLETE")

    def test_unknown_background_object_ids_do_not_count_as_npcs(self) -> None:
        run = make_run(self.root, 1, unknown_visible=True)
        analyzed, _ = e07.analyze_run(run, settle_seconds=0)
        self.assertEqual(analyzed["npc_per_frame"]["frustum_visible_spawned_count"]["p50"], 1)
        self.assertEqual(analyzed["npc_per_frame"]["submitted_to_gbuffer_spawned_count"]["p50"], 1)
        self.assertEqual(analyzed["visibility_curve_status"], "OPEN_VISIBILITY_NOT_PROVEN")
        self.assertEqual(analyzed["coverage"]["background_unique_object_ids_separate"], 1)

    def test_requested_spawn_count_must_match_fixture_spawned_ids(self) -> None:
        run = make_run(self.root, 1)
        telemetry = json.loads((run / "telemetry.json").read_text())
        telemetry["fixture_scenario"]["spawned"] = []
        write_json(run / "telemetry.json", telemetry)
        with self.assertRaisesRegex(ValueError, "spawned object ids for requested count 1"):
            e07.analyze_run(run, settle_seconds=0)

    def test_nonzero_exit_and_scenario_failures_are_rejected(self) -> None:
        bad_exit = make_run(self.root, 0, name="bad-exit", exit_status=1)
        with self.assertRaisesRegex(ValueError, "exited with status 1"):
            e07.analyze_run(bad_exit, settle_seconds=0)
        bad_scenario = make_run(self.root, 0, name="bad-scenario", scenario_failures=True)
        with self.assertRaisesRegex(ValueError, "fixture scenario failures"):
            e07.analyze_run(bad_scenario, settle_seconds=0)

    def test_malformed_csv_and_mismatched_curve_provenance_are_rejected(self) -> None:
        malformed = make_run(self.root, 0, name="malformed")
        (malformed / "e07-frames.csv").write_text("wrong,header\n1,2\n", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "missing columns"):
            e07.analyze_run(malformed, settle_seconds=0)
        paths = [make_run(self.root, count, name=f"curve-n{count}") for count in e07.COUNTS]
        provenance = json.loads((paths[-1] / "provenance.json").read_text())
        provenance["binary"]["sha256"] = "f" * 64
        write_json(paths[-1] / "provenance.json", provenance)
        with self.assertRaisesRegex(ValueError, "mismatches curve provenance/settings: binary_sha256"):
            e07.analyze_curve(paths, self.root / "bad-out", settle_seconds=0)


if __name__ == "__main__":
    unittest.main()
