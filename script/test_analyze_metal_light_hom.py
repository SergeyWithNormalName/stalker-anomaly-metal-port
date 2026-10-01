from __future__ import annotations

import pathlib
import tempfile
import unittest

import analyze_metal_light_hom as hom


def frame(frame_id: int = 1, *, sample: int = 1, groups: int = 2, tested: int = 2, hidden: int = 2,
          eligible_hidden: int = 2, status: str = "ready", overhead_ms: str = "3",
          planned_batches: int = 1, planned_shadow_records: int = 2,
          unshadowed_records: int = 0) -> str:
    return (
        "METAL_LIGHT_HOM_FRAME "
        f"frame={frame_id} sample={sample} status={status} geometry_verified=0 suppressed=0 "
        f"groups={groups} tested={tested} hidden={hidden} eligible_hidden={eligible_hidden} "
        f"excluded_hud=0 excluded_volumetric=0 invalid=0 planned_batches={planned_batches} "
        f"planned_shadow_records={planned_shadow_records} unshadowed_records={unshadowed_records} "
        "build_ms=0.2 test_ms=0.3 "
        f"overhead_ms={overhead_ms} eye=0,0,0 camera_hash=abc123"
    )


def group(parent: int, *, frame_id: int = 1, sample: int = 1, state: str = "hidden", eligible: int = 1,
          reason: str = "none") -> str:
    return (
        "METAL_LIGHT_HOM_GROUP "
        f"frame={frame_id} sample={sample} parent={parent} state={state} records=1 "
        f"eligible={eligible} reason={reason} "
        "bounds_min=0,0,0 bounds_max=1,1,1"
    )


def tile(tile_id: int, *, parent: int, batch: int = 4, frame_id: int = 1, sample: int = 1,
         hidden: int = 1, eligible: int = 1, prep: str = "1", shadow: str = "1") -> str:
    return (
        "METAL_LIGHT_HOM_TILE "
        f"frame={frame_id} sample={sample} batch={batch} tile={tile_id} parent={parent} "
        f"hidden={hidden} eligible={eligible} "
        f"prep_cpu_ms={prep} shadow_cpu_ms={shadow} static_rows=1 static_draws=1 "
        "dynamic_draws=0 transform_bytes=0"
    )


def batch(batch_id: int, *, records: int, hidden: int, eligible_hidden: int,
          frame_id: int = 1, sample: int = 1, cpu_ms: str = "100") -> str:
    return (
        "METAL_LIGHT_HOM_BATCH "
        f"frame={frame_id} sample={sample} batch={batch_id} records={records} hidden_records={hidden} "
        f"eligible_hidden_records={eligible_hidden} total_cpu_ms={cpu_ms}"
    )


def accum(batch_id: int, *, records: int, hidden: int, eligible_hidden: int,
          frame_id: int = 1, sample: int = 1, cpu_ms: str = "7") -> str:
    return (
        "METAL_LIGHT_HOM_ACCUM "
        f"frame={frame_id} sample={sample} batch={batch_id} records={records} hidden_records={hidden} "
        f"eligible_hidden_records={eligible_hidden} cpu_ms={cpu_ms}"
    )


class AnalyzeMetalLightHomTests(unittest.TestCase):
    def setUp(self) -> None:
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.log = self.root / "engine.log"

    def tearDown(self) -> None:
        self.temp.cleanup()

    def analyze(self, *lines: str) -> dict:
        self.log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        return hom.analyze_log(self.log)

    def test_uses_weighted_tile_cpu_and_only_full_batch_accum(self) -> None:
        result = self.analyze(
            group(11), group(12), frame(),
            tile(0, parent=11, prep="0.25", shadow="0.75"),
            tile(1, parent=12, prep="2", shadow="3"),
            batch(4, records=2, hidden=2, eligible_hidden=2, cpu_ms="999"),
            accum(4, records=2, hidden=2, eligible_hidden=2, cpu_ms="4.5"),
        )
        self.assertEqual(result["sample_known_savings_ms"]["1:1"], {
            "tile_prep_shadow_ms": 6.0,
            "full_batch_accum_ms": 4.5,
            "fully_hidden_unshadowed_accum_ms": 0.0,
            "estimated_upper_bound_ms": 10.5,
            "record_coverage_complete": True,
            "full_attribution_complete": True,
            "planned_batches": 1,
            "planned_shadow_records": 2,
            "unshadowed_records": 0,
            "eligible_hidden_tile_rows": 2,
            "mixed_unshadowed_accumulation_unknown": False,
        })
        self.assertEqual(result["comparison"]["optimistic_known_savings_ms_p50"], 10.5)
        self.assertEqual(result["comparison"]["overhead_ms_p50"], 3.0)
        self.assertEqual(result["comparison"]["margin_ms_p50"], 7.5)
        self.assertEqual(result["comparison"]["full_attribution_coverage_status"], "complete")
        self.assertEqual(result["known_tile_work_diagnostic"]["eligible_hidden_tile_prep_shadow_ms_p50"], 6.0)
        self.assertEqual(result["known_tile_work_diagnostic"]["known_only_margin_ms_p50"], 3.0)
        self.assertFalse(result["comparison"]["batch_total_cpu_ms_used"])
        self.assertFalse(result["claims"]["automatic_acceptance"])
        self.assertEqual(result["claims"]["fps_claim"], "not_made")

    def test_mixed_batch_accumulation_is_excluded_as_unknown(self) -> None:
        result = self.analyze(
            frame(hidden=1, eligible_hidden=1), group(11),
            group(12, state="visible"),
            tile(0, parent=11, prep="2", shadow="3"),
            tile(1, parent=12, hidden=0, eligible=1, prep="80", shadow="90"),
            batch(4, records=2, hidden=1, eligible_hidden=1, cpu_ms="200"),
            accum(4, records=2, hidden=1, eligible_hidden=1, cpu_ms="50"),
        )
        self.assertEqual(result["sample_known_savings_ms"]["1:1"], {
            "tile_prep_shadow_ms": 5.0,
            "full_batch_accum_ms": 0.0,
            "fully_hidden_unshadowed_accum_ms": 0.0,
            "estimated_upper_bound_ms": None,
            "record_coverage_complete": True,
            "full_attribution_complete": False,
            "eligible_hidden_tile_rows": 1,
            "planned_batches": 1,
            "planned_shadow_records": 2,
            "unshadowed_records": 0,
            "mixed_unshadowed_accumulation_unknown": False,
        })
        self.assertEqual(result["comparison"]["mixed_batch_accumulation"], "unknown_and_excluded")
        self.assertIsNone(result["comparison"]["optimistic_known_savings_ms_p50"])
        self.assertEqual(result["diagnostic_coverage"]["samples_with_required_record_kinds_present"], 1)
        self.assertEqual(result["diagnostic_coverage"]["samples_with_full_attribution"], 0)
        self.assertEqual(result["comparison"]["full_attribution_coverage_status"], "partial")
        self.assertIn("including mixed accumulation", result["comparison"]["selection_bias_warning"])

    def test_unavailable_and_missing_logs_are_no_evidence(self) -> None:
        missing = hom.analyze_log(self.root / "missing.log")
        self.assertEqual(missing["evidence_status"], "no_evidence")
        self.assertIsNone(missing["comparison"]["overhead_ms_p50"])
        unavailable_frame = frame(status="unavailable", groups=0, tested=0,
                                  hidden=0, eligible_hidden=0, planned_batches=0,
                                  planned_shadow_records=0, unshadowed_records=0)
        unavailable_frame = unavailable_frame.replace(" eye=0,0,0 camera_hash=abc123", "")
        unavailable = self.analyze(
            "METAL_LIGHT_HOM_LOAD path=/example/GAMMA profile with spaces",
            unavailable_frame,
        )
        self.assertEqual(unavailable["evidence_status"], "no_evidence")
        self.assertEqual(unavailable["unavailable_frames"], 1)
        self.assertEqual(unavailable["record_counts"]["LOAD"], 1)
        self.assertIsNone(unavailable["comparison"]["optimistic_known_savings_ms_p50"])

    def test_unavailable_tile_costs_are_baseline_only(self) -> None:
        result = self.analyze(
            frame(status="unavailable", groups=0, tested=0, hidden=0, eligible_hidden=0,
                  planned_batches=0, planned_shadow_records=0, unshadowed_records=0),
            tile(0, parent=99, hidden=0, eligible=0, prep="2", shadow="3"),
        )
        self.assertEqual(result["evidence_status"], "no_evidence")
        self.assertEqual(result["record_counts"]["TILE"], 1)
        self.assertEqual(result["sample_known_savings_ms"], {})
        self.assertIsNone(result["comparison"]["optimistic_known_savings_ms_p50"])

    def test_partial_diagnostics_are_reported_as_missing_coverage(self) -> None:
        result = self.analyze(frame(), group(11))
        self.assertEqual(result["diagnostic_coverage"]["missing_group_rows"], 1)
        self.assertEqual(result["diagnostic_coverage"]["ready_frames_without_tile_rows"], 1)
        self.assertEqual(result["diagnostic_coverage"]["ready_frames_without_batch_rows"], 1)

    def test_missing_tile_ordinal_marks_sample_incomplete(self) -> None:
        result = self.analyze(
            frame(), group(11), group(12), tile(0, parent=11),
            batch(4, records=2, hidden=2, eligible_hidden=2),
            accum(4, records=2, hidden=2, eligible_hidden=2),
        )
        sample = result["sample_known_savings_ms"]["1:1"]
        self.assertEqual(sample["eligible_hidden_tile_rows"], 1)
        self.assertIsNone(sample["estimated_upper_bound_ms"])
        self.assertFalse(sample["record_coverage_complete"])
        self.assertFalse(sample["full_attribution_complete"])
        self.assertEqual(result["diagnostic_coverage"]["batches_with_incomplete_tile_rows"], 1)
        self.assertIsNone(result["comparison"]["optimistic_known_savings_ms_p50"])

    def test_unshadowed_accum_is_included_only_when_entirely_eligible_hidden(self) -> None:
        full = self.analyze(
            frame(hidden=2, eligible_hidden=2, planned_batches=0,
                  planned_shadow_records=0, unshadowed_records=2),
            group(11), group(12),
            accum(-1, records=2, hidden=2, eligible_hidden=2, cpu_ms="8"),
        )
        full_sample = full["sample_known_savings_ms"]["1:1"]
        self.assertEqual(full_sample["fully_hidden_unshadowed_accum_ms"], 8.0)
        self.assertEqual(full_sample["estimated_upper_bound_ms"], 8.0)
        self.assertTrue(full_sample["record_coverage_complete"])
        self.assertTrue(full_sample["full_attribution_complete"])

        mixed = self.analyze(
            frame(hidden=1, eligible_hidden=1, planned_batches=0,
                  planned_shadow_records=0, unshadowed_records=2),
            group(11), group(12, state="visible"),
            accum(-1, records=2, hidden=1, eligible_hidden=1, cpu_ms="8"),
        )
        mixed_sample = mixed["sample_known_savings_ms"]["1:1"]
        self.assertEqual(mixed_sample["fully_hidden_unshadowed_accum_ms"], 0.0)
        self.assertTrue(mixed_sample["mixed_unshadowed_accumulation_unknown"])
        self.assertIsNone(mixed_sample["estimated_upper_bound_ms"])
        self.assertTrue(mixed_sample["record_coverage_complete"])
        self.assertFalse(mixed_sample["full_attribution_complete"])

    def test_no_planned_work_is_a_complete_zero_scheduled_work_sample(self) -> None:
        result = self.analyze(
            frame(groups=1, tested=1, hidden=0, eligible_hidden=0,
                  planned_batches=0, planned_shadow_records=0, unshadowed_records=0),
            group(11, state="visible"),
        )
        sample = result["sample_known_savings_ms"]["1:1"]
        self.assertEqual(sample["estimated_upper_bound_ms"], 0.0)
        self.assertTrue(sample["record_coverage_complete"])
        self.assertTrue(sample["full_attribution_complete"])
        self.assertEqual(result["comparison"]["optimistic_known_savings_ms_p50"], 0.0)

    def test_window_filters_outside_records_and_keeps_same_frame_samples(self) -> None:
        lines = []
        for frame_id, sample_id, overhead in ((1, 1, "1"), (2, 2, "2"), (2, 3, "4")):
            lines.extend((
                group(100 + sample_id, frame_id=frame_id, sample=sample_id, state="visible"),
                frame(frame_id, sample=sample_id, groups=1, tested=1, hidden=0,
                      eligible_hidden=0, overhead_ms=overhead, planned_batches=0,
                      planned_shadow_records=0, unshadowed_records=0),
            ))
        self.log.write_text("\n".join(lines) + "\n", encoding="utf-8")
        result = hom.analyze_log(self.log, {"start_engine_frame": 2, "end_engine_frame": 2})
        self.assertEqual(result["selection"]["window"], {
            "start_engine_frame": 2, "end_engine_frame": 2,
        })
        self.assertEqual(result["selection"]["samples_excluded"], 1)
        self.assertEqual(result["selection"]["records_excluded_by_type"], {"FRAME": 1, "GROUP": 1})
        self.assertEqual(result["frame_count"], 2)
        self.assertEqual(result["engine_frame_count"], 1)
        self.assertEqual(result["record_counts"]["FRAME"], 2)
        self.assertEqual(result["record_counts"]["GROUP"], 2)
        self.assertEqual(result["comparison"]["overhead_ms_p50"], 3.0)

    def test_window_with_no_selected_samples_returns_no_evidence(self) -> None:
        result = self.analyze(
            frame(groups=0, tested=0, hidden=0, eligible_hidden=0,
                  planned_batches=0, planned_shadow_records=0, unshadowed_records=0),
        )
        result = hom.analyze_log(self.log, {"start_engine_frame": 9, "end_engine_frame": 10})
        self.assertEqual(result["evidence_status"], "no_evidence")
        self.assertIsNone(result["comparison"]["overhead_ms_p50"])
        self.assertIsNone(result["comparison"]["optimistic_known_savings_ms_p50"])
        self.assertEqual(result["selection"]["samples_excluded"], 1)
        self.assertEqual(result["record_counts"]["FRAME"], 0)
        self.assertEqual(result["selection"]["records_excluded_by_type"], {"FRAME": 1})

    def test_rejects_malformed_window_contract(self) -> None:
        invalid_values = (
            [],
            {"start_engine_frame": 1},
            {"start_engine_frame": True, "end_engine_frame": 2},
            {"start_engine_frame": -1, "end_engine_frame": 2},
            {"start_engine_frame": 2.0, "end_engine_frame": 2},
            {"start_engine_frame": 3, "end_engine_frame": 2},
        )
        for value in invalid_values:
            with self.subTest(value=value), self.assertRaises(ValueError):
                hom._validate_window(value)

        path = self.root / "bad-window.json"
        path.write_text('{"start_engine_frame": true, "end_engine_frame": 2}', encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "start_engine_frame"):
            hom.read_window(path)

    def test_tolerates_legacy_bytes_outside_hom_records(self) -> None:
        self.log.write_bytes(
            b"ordinary game log: \xff\xfe\xfa\n" +
            frame(groups=0, tested=0, hidden=0, eligible_hidden=0,
                  planned_batches=0, planned_shadow_records=0, unshadowed_records=0).encode("ascii") +
            b"\nlocalized text \x80\x81\n"
        )
        result = hom.analyze_log(self.log)
        self.assertEqual(result["evidence_status"], "diagnostic_data_present")
        self.assertEqual(result["frame_count"], 1)

    def test_replacement_characters_in_diagnostic_values_remain_invalid(self) -> None:
        valid = frame(groups=0, tested=0, hidden=0, eligible_hidden=0,
                      planned_batches=0, planned_shadow_records=0, unshadowed_records=0)
        malformed = (
            valid.replace("status=ready", "status=rea\ufffdy"),
            valid.replace("overhead_ms=3", "overhead_ms=1\ufffd2"),
        )
        expected_errors = ("invalid frame status", "finite nonnegative number")
        for line, error in zip(malformed, expected_errors):
            with self.subTest(line=line):
                self.log.write_bytes(line.encode("utf-8"))
                with self.assertRaisesRegex(ValueError, error):
                    hom.analyze_log(self.log)

    def test_same_engine_frame_can_have_distinct_samples(self) -> None:
        result = self.analyze(
            frame(sample=1), group(11, sample=1), group(12, sample=1),
            frame(sample=2), group(11, sample=2), group(12, sample=2),
            tile(0, parent=11, sample=2), tile(1, parent=12, sample=2),
            batch(4, records=2, hidden=2, eligible_hidden=2, sample=2),
            accum(4, records=2, hidden=2, eligible_hidden=2, sample=2),
        )
        self.assertEqual(result["frame_count"], 2)
        self.assertEqual(set(result["sample_known_savings_ms"]), {"1:1", "1:2"})

    def test_rejects_invalid_durations_suppression_and_geometry_claim(self) -> None:
        for line in (
            frame(overhead_ms="nan"),
            frame(overhead_ms="-0.1"),
            frame().replace("suppressed=0", "suppressed=1"),
            frame().replace("geometry_verified=0", "geometry_verified=1"),
        ):
            with self.subTest(line=line), self.assertRaises(ValueError):
                self.analyze(line)

    def test_rejects_non_boolean_tile_visibility(self) -> None:
        bad_tile = tile(0, parent=11).replace("hidden=1", "hidden=2")
        with self.assertRaisesRegex(ValueError, "TILE hidden and eligible"):
            self.analyze(frame(groups=1, tested=1, hidden=1, eligible_hidden=1),
                         group(11), bad_tile)

    def test_rejects_stale_joins_and_missing_parent_group(self) -> None:
        with self.assertRaisesRegex(ValueError, "stale or missing frame"):
            self.analyze(frame(), group(11, frame_id=2, sample=2))
        with self.assertRaisesRegex(ValueError, "missing parent group"):
            self.analyze(frame(groups=0, tested=0, hidden=0, eligible_hidden=0),
                         tile(0, parent=11))
        with self.assertRaisesRegex(ValueError, "sample IDs"):
            self.analyze(frame(sample=2), frame(sample=1))

    def test_rejects_duplicate_keys_for_each_record_kind(self) -> None:
        valid = [frame(), group(11), group(12), tile(0, parent=11),
                 batch(4, records=2, hidden=2, eligible_hidden=2),
                 accum(4, records=2, hidden=2, eligible_hidden=2)]
        duplicates = [frame(), group(11), tile(0, parent=11),
                      batch(4, records=2, hidden=2, eligible_hidden=2),
                      accum(4, records=2, hidden=2, eligible_hidden=2)]
        for duplicate in duplicates:
            with self.subTest(duplicate=duplicate), self.assertRaisesRegex(ValueError, "duplicate"):
                self.analyze(*(valid + [duplicate]))


if __name__ == "__main__":
    unittest.main()
