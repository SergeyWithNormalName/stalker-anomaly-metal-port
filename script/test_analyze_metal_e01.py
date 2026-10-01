#!/usr/bin/env python3
"""Focused contract tests for the E01 analyzer."""

import csv
import json
import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import analyze_metal_e01 as e01


FIELDS = list(e01.REQUIRED_FIELDS)


def make_row(serial, begin_ms, **overrides):
    row = {field: "" for field in FIELDS}
    row.update({
        "serial": str(serial),
        "engine_frame": str(serial + 100),
        "camera_frame": str(serial + 200),
        "slot": str(serial % 3),
        "begin_ns": str(begin_ms * 1_000_000),
        "status": "ok",
    })
    row.update({key: str(value) for key, value in overrides.items()})
    return row


class E01AnalyzerTests(unittest.TestCase):
    def write_csv(self, path, rows, fields=FIELDS):
        with path.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=fields)
            writer.writeheader()
            writer.writerows(rows)

    def test_frame_percentiles_thresholds_and_one_percent_low(self):
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "frames.csv"
            # Frame deltas are 20, 30, 60 ms. Slowest 1% rounds up to one.
            self.write_csv(source, [make_row(1, 100), make_row(2, 120), make_row(3, 150), make_row(4, 210)])
            report = e01.analyze(source)
        frame = report["frame_interval_ms"]
        self.assertEqual(frame["count"], 3)
        self.assertEqual(frame["p50"], 30)
        self.assertEqual(frame["max"], 60)
        self.assertEqual((frame["over_25ms"], frame["over_33ms"], frame["over_50ms"]), (2, 1, 1))
        self.assertAlmostEqual(frame["one_percent_low_fps"], 1000 / 60)

    def test_missing_nan_zero_gpu_and_display_timestamps_stay_unattributed(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "frames.csv"
            rows = [
                make_row(1, 100, scene_ms="0", encode_ms="NaN", gpu_start_s="0", gpu_end_s="1", presented_s="1"),
                make_row(2, 125, scene_ms="4", encode_ms="2", gpu_start_s="2", gpu_end_s="2.012", presented_s=""),
                make_row(3, 150, scene_ms="", encode_ms="Infinity", gpu_start_s="3", gpu_end_s="2", presented_s="0"),
            ]
            self.write_csv(source, rows)
            report = e01.analyze(source)
            e01.write_outputs(root / "out", report)
            out = root / "out"
            passes = (out / "gpu_passes.csv").read_text()
            budget = (out / "budget.md").read_text()
        self.assertEqual(report["cpu_stage_ms"]["scene_ms"]["count"], 1)
        self.assertEqual(report["cpu_stage_ms"]["encode_ms"]["count"], 1)
        self.assertEqual(report["gpu_frame_ms"]["count"], 1)
        self.assertAlmostEqual(report["gpu_frame_ms"]["p50"], 12)
        self.assertEqual(report["presentation"]["pacing_gate"], "OPEN")
        self.assertEqual(report["displayed_frame_interval_ms"]["count"], 0)
        self.assertIn("UNAVAILABLE", passes)
        self.assertIn("No per-pass cost is inferred", budget)

    def test_rejects_non_success_rows_and_duplicate_or_backward_clock_deltas(self):
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "frames.csv"
            self.write_csv(source, [
                make_row(1, 100),
                make_row(2, 100),
                make_row(3, 90),
                make_row(4, 120, status="error"),
                make_row(5, 140),
            ])
            report = e01.analyze(source)
        self.assertEqual(report["rows_rejected"], 1)
        self.assertEqual(report["rows_accepted"], 4)
        self.assertEqual(report["frame_interval_ms"]["count"], 1)
        self.assertEqual(report["frame_interval_ms"]["max"], 40)

    def test_fails_clearly_when_contract_columns_are_missing(self):
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "frames.csv"
            self.write_csv(source, [], fields=["serial", "begin_ns"])
            with self.assertRaisesRegex(ValueError, "missing required columns"):
                e01.analyze(source)

    def test_inclusive_engine_frame_window_and_empty_window_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            source = pathlib.Path(temp) / "frames.csv"
            self.write_csv(source, [make_row(1, 100), make_row(2, 125), make_row(3, 160)])
            window_path = pathlib.Path(temp) / "timing-window.json"
            window_path.write_text(json.dumps({"start_engine_frame": 102, "end_engine_frame": 103}))
            window = e01.read_window(window_path)
            report = e01.analyze(source, window)
            self.assertEqual(report["rows_accepted"], 2)
            self.assertEqual(report["rows_filtered_outside_window"], 1)
            self.assertEqual(report["frame_interval_ms"]["count"], 1)
            self.assertEqual(report["frame_interval_ms"]["p50"], 35)
            with self.assertRaisesRegex(ValueError, "contains no valid frames"):
                e01.analyze(source, {"start_engine_frame": 900, "end_engine_frame": 910})

    def test_complete_presentation_csv_computes_displayed_interval_metrics(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "frames.csv"
            presentation_path = root / "frames.presented.csv"
            self.write_csv(source, [make_row(1, 100), make_row(2, 120), make_row(3, 150)])
            with presentation_path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=["serial", "presented_s"])
                writer.writeheader()
                writer.writerows([
                    {"serial": 1, "presented_s": 1.0},
                    {"serial": 2, "presented_s": 1.020},
                    {"serial": 3, "presented_s": 1.070},
                ])
            report = e01.analyze(source, presentation=e01.read_presentation(presentation_path))
        displayed = report["displayed_frame_interval_ms"]
        self.assertEqual(report["presentation"]["pacing_gate"], "CLOSED")
        self.assertEqual(displayed["count"], 2)
        self.assertAlmostEqual(displayed["p50"], 35)
        self.assertAlmostEqual(displayed["p95"], 48.5)
        self.assertAlmostEqual(displayed["p99"], 49.7)
        self.assertAlmostEqual(displayed["max"], 50)
        self.assertEqual(displayed["over_25ms"], 1)
        self.assertEqual(displayed["over_33ms"], 1)
        self.assertEqual(displayed["over_50ms"], 0)
        self.assertAlmostEqual(displayed["one_percent_low_fps"], 20)

    def test_partial_presentation_keeps_contiguous_intervals_and_reports_zero_gaps(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "frames.csv"
            presentation_path = root / "frames.presented.csv"
            self.write_csv(source, [make_row(serial, 100 + 16 * serial) for serial in range(1, 6)])
            with presentation_path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=["serial", "presented_s"])
                writer.writeheader()
                writer.writerows([
                    {"serial": 1, "presented_s": 1.000},
                    {"serial": 2, "presented_s": 1.016},
                    {"serial": 3, "presented_s": 0},
                    {"serial": 4, "presented_s": 1.048},
                    {"serial": 5, "presented_s": 1.064},
                ])
            report = e01.analyze(source, presentation=e01.read_presentation(presentation_path))
        self.assertEqual(report["presentation"]["pacing_gate"], "OPEN")
        self.assertEqual(report["presentation"]["display_metrics_status"], "partial")
        self.assertEqual(report["presentation"]["zero_timestamp_callbacks"], 1)
        self.assertEqual(report["presentation"]["serial_gap_events"], 1)
        self.assertEqual(report["presentation"]["serial_gap_frames"], 1)
        self.assertEqual(report["displayed_frame_interval_ms"]["count"], 2)
        self.assertAlmostEqual(report["displayed_frame_interval_ms"]["p50"], 16)

    def test_nested_cpu_stages_are_grouped_in_budget_not_counted_independently(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "frames.csv"
            optional = list(e01.OPTIONAL_STAGE_FIELDS)
            fields = FIELDS + optional
            row = make_row(1, 100, scene_ms=12, encode_ms=6, wait_ms=3)
            row.update({"grass_ms": "4", "dynamic_prewarm_ms": "2", "scene_other_ms": "6"})
            self.write_csv(source, [row], fields=fields)
            baseline = e01.analyze(source)
            e01.write_outputs(root / "out", baseline)
            budget = (root / "out" / "budget.md").read_text()
        self.assertEqual(baseline["cpu_stage_ms"]["grass_ms"]["p50"], 4)
        self.assertEqual(baseline["cpu_stage_ms"]["dynamic_prewarm_ms"]["p50"], 2)
        self.assertEqual(baseline["cpu_stage_ms"]["scene_other_ms"]["p50"], 6)
        self.assertIn("nested-work overlap group", budget)
        self.assertNotIn("| 2 | encode_ms |", budget)
        self.assertIn("durations are never summed", budget)

    def test_optional_frame_counters_preserve_zero_and_have_no_timing_attribution(self):
        with tempfile.TemporaryDirectory() as temp:
            root = pathlib.Path(temp)
            source = root / "frames.csv"
            fields = FIELDS + list(e01.COUNTER_FIELDS)
            row = make_row(1, 100, updated_bytes=0, buffer_allocations=2,
                           texture_allocations="NaN", indexed_draws=0, pso_binds=3)
            self.write_csv(source, [row], fields=fields)
            baseline = e01.analyze(source)
            e01.write_outputs(root / "out", baseline)
            cpu_top = (root / "out" / "cpu_top.md").read_text()
        counters = baseline["frame_counters"]
        self.assertEqual(counters["updated_bytes"]["count"], 1)
        self.assertEqual(counters["updated_bytes"]["p50"], 0)
        self.assertEqual(counters["buffer_allocations"]["p50"], 2)
        self.assertEqual(counters["texture_allocations"]["count"], 0)
        self.assertIn("no timing attribution", cpu_top)


if __name__ == "__main__":
    unittest.main()
