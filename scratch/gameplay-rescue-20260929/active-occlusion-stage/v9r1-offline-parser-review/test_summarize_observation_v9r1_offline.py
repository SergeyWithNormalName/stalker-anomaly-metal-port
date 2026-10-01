#!/usr/bin/env python3
"""Synthetic safety cases for the V9 CPU/GPU observer join."""
import importlib.util
import json
import tempfile
from pathlib import Path


SCRIPT = Path(__file__).with_name('summarize_observation_v9r1_offline.py')
SPEC = importlib.util.spec_from_file_location('summarize_observation_v9r1_offline', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)
SCHEMA = json.loads(Path(__file__).with_name('catalog-schema-v9r1_offline.json').read_text(encoding='utf-8'))


def cpu(scope, frame, batch, light, parent, face, prediction='behind', cells=1,
        kind=None, expected=None, reason='fixture'):
    kind = kind or 'single'
    expected = expected or (6 if kind == 'omni-parent' else 1)
    return ('METAL_ACTIVE_OCCLUSION_FACE '
            f'cpu_observation_scope={scope} engine_frame={frame} batch_id={batch} '
            f'light_id={light} parent_light_id={parent} face={face} '
            f'group_kind={kind} expected_faces={expected} '
            f'prediction={prediction} query_cells={cells} reason={reason}')


def gpu_face(scope, frame, serial, epoch, batch, light, parent, face, kind, expected, count, status='VALID'):
    return ('METAL_WHOLE_LIGHT_VISIBILITY_FACE '
            f'status={status} submitted_serial={serial} acquire_epoch={epoch} engine_frame={frame} '
            f'cpu_observation_scope={scope} batch_id={batch} light_id={light} parent_light_id={parent} '
            f'face={face} group_kind={kind} expected_faces={expected} deferred_receiver_samples={count}')


def gpu_group(scope, frame, serial, epoch, kind, gid, expected, count, status='complete'):
    zero = 'true' if count == 0 else 'false'
    return ('METAL_WHOLE_LIGHT_VISIBILITY status=' + status + ' '
            f'submitted_serial={serial} acquire_epoch={epoch} engine_frame={frame} '
            f'cpu_observation_scope={scope} group_kind={kind} group_id={gid} '
            f'expected_faces={expected} deferred_receiver_samples={count} zero_deferred_receiver_samples={zero}')


def tile(frame, batch, light, parent, face, kind=0):
    return ('METAL_WHOLE_LIGHT_VIS_TILE '
            f'frame={frame} batch={batch} light={light} parent={parent} face={face} kind={kind} '
            'static_rows=2 static_encoded_draws=1 dynamic_submitted_draws=3 static_transform_bytes=512')


def prep(*, status='enabled', reason='qualified-source-snapshot', bad_counts=False, mate_hints_cleared=100):
    catalog_visuals = 99 if bad_counts else 100
    return ('METAL_ACTIVE_OCCLUSION_PREP '
            f'status={status} reason={reason} catalog_visuals={catalog_visuals} catalog_triangles=1000 '
            'catalog_bytes=100000 candidate_visuals=100 candidate_triangles=1000 omitted_visuals=0 '
            f'omitted_triangles=0 mate_hints_cleared={mate_hints_cleared} '
            'excluded_materials=12 excluded_trees=8 invalid_visuals=1 '
            'cap_visuals=0 max_triangles=32768')


def grid(scope, frame, faces, *, status='ready', triangles=8, triangle_cap=0, cell_test_cap=0,
         query_invalid=0):
    groups = len({(row.split('group_kind=')[1].split()[0],
                   int(row.split('parent_light_id=')[1].split()[0]) if 'group_kind=omni-parent' in row else
                   int(row.split('light_id=')[1].split()[0])) for row in faces})
    query = sum(int(row.split('query_cells=')[1].split()[0]) for row in faces)
    draw_count = max(groups, 1)
    return ('METAL_ACTIVE_OCCLUSION_GRID '
            f'status={status} cpu_observation_scope={scope} engine_frame={frame} '
            f'selected_groups={groups} selected_faces={len(faces)} draw_receipts={draw_count} '
            f'selected_draws={groups} draws_used={groups} draws_skipped={draw_count - groups} '
            f'receipt_triangles={triangles} selected_triangles={triangles} omitted_receipt_triangles=0 '
            'selector_invalid=0 selector_duplicate=0 selector_unknown_projection=0 '
            'selector_outside_viewport=0 selector_budget_omissions=0 selection_budget=4096 '
            f'triangles={triangles} source_pairs=2 '
            f'paired_unions_accepted=1 cells_marked=12 primitive_cell_tests=32 '
            f'query_cell_tests={query} query_invalid={query_invalid} triangle_cap={triangle_cap} cell_test_cap={cell_test_cap} '
            'raster_convention=synthetic_only suppression=disabled '
            'build_ms=0.125 query_ms=0.062 reason=fixture')


def scenario(scope, frame, serial, epoch, kind, gid, cpu_rows, gpu_counts, *,
             group_status='complete', gpu_status='VALID', gpu_scopes=None, duplicate_cpu=0,
             duplicate_gpu=0, grid_status='ready', bad_grid=False, face_batches=None,
             triangle_cap=0, cell_test_cap=0, single_parent_id=0, duplicate_grid=0,
             query_invalid=0):
    records = []
    for row in cpu_rows:
        records.append(row)
    records.extend([cpu_rows[0]] * duplicate_cpu if cpu_rows else [])
    if cpu_rows:
        grid_row = grid(scope, frame, cpu_rows, status=grid_status,
                        triangles=-1 if bad_grid else 8, triangle_cap=triangle_cap,
                        cell_test_cap=cell_test_cap, query_invalid=query_invalid)
        records.append(grid_row)
        records.extend([grid_row] * duplicate_grid)
    else:
        records.append('')
    expected = 1 if kind == 'single' else 6
    records.append(gpu_group(scope, frame, serial, epoch, kind, gid, expected, sum(gpu_counts), group_status))
    for index, count in enumerate(gpu_counts):
        face = index
        light = gid if kind == 'single' else gid * 10 + face + 1
        parent = single_parent_id if kind == 'single' else gid
        batch = face_batches[index] if face_batches and index < len(face_batches) else 0
        face_scope = (gpu_scopes[index] if gpu_scopes and index < len(gpu_scopes) else scope)
        receipt = gpu_face(face_scope, frame, serial, epoch, batch, light, parent, face,
                           kind, expected, count, gpu_status)
        records.append(receipt)
        records.append(tile(frame, batch, light, parent, face, 0 if kind == 'single' else 1))
        if index < duplicate_gpu:
            records.append(receipt)
            records.append(tile(frame, batch, light, parent, face, 0 if kind == 'single' else 1))
    return records


def run_case(records, expected_hash='fixture-hash', inject_prep=True):
    with tempfile.TemporaryDirectory(prefix='active-occlusion-v9-') as tmp:
        out = Path(tmp)
        log = [row for row in records if row]
        if inject_prep and not any('METAL_ACTIVE_OCCLUSION_PREP ' in row for row in log):
            log.insert(0, prep())
        (out / 'engine.log').write_bytes(('\n'.join(log) + '\n').encode('utf-8'))
        (out / 'observer-markers.csv').write_text(
            'observer_window_start,frame=300\nobserver_window_end,frame=311\n'
            'observer_drain_complete,frame=311\n', encoding='utf-8')
        (out / 'environment.json').write_text(json.dumps({'XRAY_METAL_ACTIVE_OCCLUSION': '1'}), encoding='utf-8')
        (out / 'input-sha256.txt').write_text(f'{expected_hash}  /candidate-v9-r1/Anomaly.app/Contents/MacOS/AnomalyDX11\n', encoding='ascii')
        (out / 'exit-status.txt').write_text('0\n', encoding='ascii')
        (out / 'timeout-flag.txt').write_text('0\n', encoding='ascii')
        (out / 'post-run-app-sha256.txt').write_text(f'{expected_hash} /candidate-v9-r1/Anomaly.app/Contents/MacOS/AnomalyDX11\n', encoding='ascii')
        status = MODULE.main(out)
        return json.loads((out / 'observer-summary.json').read_text(encoding='utf-8')), status


def main():
    prep_schema = SCHEMA['records']['METAL_ACTIVE_OCCLUSION_PREP']
    grid_schema = SCHEMA['records']['METAL_ACTIVE_OCCLUSION_GRID']
    assert set(prep_schema['nonnegative_integer_fields']) == set(MODULE.PREP_INTEGER_FIELDS)
    assert set(grid_schema['nonnegative_integer_fields']) == set(MODULE.GRID_INTEGER_FIELDS)
    assert grid_schema['limits']['selected_groups'] == 8
    assert grid_schema['limits']['selected_faces'] == 48
    assert 'mate_hints_cleared' in prep_schema['nonnegative_integer_fields']

    records = []
    # Complete CPU-behind / submitted-GPU-zero positive correlation.
    rows = [cpu(101, 300, 0, 601, 0, 0)]
    records += scenario(101, 300, 41, 901, 'single', 601, rows, [0])
    # Complete nonzero GPU work with a CPU visible-or-unknown prediction.
    rows = [cpu(102, 301, 0, 602, 82, 0, 'visible_or_unknown', kind='single')]
    records += scenario(102, 301, 42, 902, 'single', 602, rows, [7], single_parent_id=82)
    # Complete GPU-zero group missed by the CPU predictor.
    rows = [cpu(103, 302, 0, 603, 0, 0, 'visible_or_unknown')]
    records += scenario(103, 302, 43, 903, 'single', 603, rows, [0])
    # Parent is otherwise mixed; one behind face with actual submitted work must be surfaced.
    rows = [cpu(104, 303, 0, 650 * 10 + f + 1, 650, f,
                'behind' if f == 0 else 'visible_or_unknown', kind='omni-parent', expected=6) for f in range(6)]
    records += scenario(104, 303, 44, 904, 'omni-parent', 650, rows, [2, 0, 0, 0, 0, 0])
    # Mixed scope: one GPU face cannot join the parent's CPU observation.
    rows = [cpu(105, 304, 0, 660 * 10 + f + 1, 660, f, kind='omni-parent', expected=6) for f in range(6)]
    records += scenario(105, 304, 45, 905, 'omni-parent', 660, rows, [0] * 6,
                        gpu_scopes=[105, 105, 999, 105, 105, 105])
    # Missing CPU face receipt and duplicate CPU identity both stay inconclusive.
    records += scenario(106, 305, 46, 906, 'single', 670, [], [0])
    rows = [cpu(107, 306, 0, 680, 0, 0)]
    records += scenario(107, 306, 47, 907, 'single', 680, rows, [0], duplicate_cpu=1)
    # Scope zero, skipped grid, invalid group, duplicate GPU face, and malformed grid are all non-success.
    rows = [cpu(0, 307, 0, 690, 0, 0)]
    records += scenario(0, 307, 48, 908, 'single', 690, rows, [0])
    rows = [cpu(109, 308, 0, 700, 0, 0, 'unknown')]
    records += scenario(109, 308, 49, 909, 'single', 700, rows, [0], grid_status='skipped')
    rows = [cpu(110, 309, 0, 710, 0, 0)]
    records += scenario(110, 309, 50, 910, 'single', 710, rows, [0], group_status='invalid')
    rows = [cpu(111, 310, 0, 720, 0, 0)]
    records += scenario(111, 310, 51, 911, 'single', 720, rows, [0], duplicate_gpu=1)
    rows = [cpu(112, 311, 0, 730, 0, 0)]
    records += scenario(112, 311, 52, 912, 'single', 730, rows, [0], bad_grid=True)
    # The helper may preserve a proven covered subset after a cap. A ready
    # grid with an explicit cap bit can still qualify a positive query.
    rows = [cpu(113, 312, 0, 740, 0, 0)]
    records += scenario(113, 312, 53, 913, 'single', 740, rows, [0], triangle_cap=1)
    rows = [cpu(117, 316, 0, 770, 0, 0)]
    records += scenario(117, 316, 55, 915, 'single', 770, rows, [0], duplicate_grid=1)
    # Orphan CPU/GPU records and a nonempty grid with no face rows must be
    # visible in the receipt and prevent a clean complete-observations label.
    rows = [cpu(114, 313, 0, 750, 0, 0)]
    records.append(rows[0])
    records.append(grid(114, 313, rows))
    records.append(gpu_face(115, 314, 54, 914, 0, 760, 0, 0, 'single', 1, 0))
    records.append(tile(314, 0, 760, 0, 0))
    records.append(grid(116, 315, [cpu(116, 315, 0, 790, 0, 0)], triangles=8))

    summary, status = run_case(records)
    assert status == 3
    assert summary['execution_integrity'] == 'pass'
    assert summary['observation_status'] == 'false_hidden_finding'
    assert summary['complete_gpu_cpu_correlated_group_count'] == 5
    assert summary['zero_deferred_receiver_sample_group_count'] == 3
    assert summary['nonzero_deferred_receiver_sample_group_count'] == 2
    assert summary['incomplete_or_inconclusive_group_count'] == 9
    assert len(summary['false_hidden_faces']) == 1
    assert summary['false_hidden_faces'][0]['identity']['face'] == 0
    assert len(summary['missed_zero_groups']) == 1
    assert summary['missed_zero_groups'][0]['frame'] == 302
    assert summary['selected_tile_work_totals']['static_rows'] > 0
    assert summary['unmatched_cpu_face_count'] == 6
    assert summary['unmatched_gpu_face_count'] == 2
    assert summary['orphan_nonempty_grid_count'] == 1
    assert summary['cpu_build_query_costs_ms'][0]['build_ms'] == 0.125
    assert summary['cpu_build_query_costs_ms'][0]['query_ms'] == 0.062
    assert summary['no_fps_or_visual_claim'] is True
    assert summary['catalog_preparation_count'] == 1
    assert summary['catalog_qualified'] is True
    assert summary['catalog_preparation']['catalog_bytes'] == 100000
    assert summary['mate_hints_cleared'] == 100
    assert summary['selected_geometry_work_totals']['selected_draws'] > 0

    # query_invalid is a per-grid boolean OR. A single invalid omni query
    # affects that six-face group only; it must not erase other usable groups.
    unknown_rows = [cpu(250, 320, 4, 9901 + f, 990, f, 'unknown', cells=0,
                        kind='omni-parent', expected=6, reason='query-invalid-input') for f in range(6)]
    known_rows = [cpu(250, 320, 5 + i, 9910 + i, 0, 0, 'behind') for i in range(2)]
    invalid_query_records = [prep()] + unknown_rows + known_rows
    invalid_query_records.append(grid(250, 320, unknown_rows + known_rows, query_invalid=1))
    invalid_query_records.append(gpu_group(250, 320, 70, 930, 'omni-parent', 990, 6, 0))
    invalid_query_records.append(gpu_group(250, 320, 70, 930, 'single', 9910, 1, 0))
    invalid_query_records.append(gpu_group(250, 320, 70, 930, 'single', 9911, 1, 0))
    for face in range(6):
        light = 990 * 10 + face + 1
        invalid_query_records.extend((gpu_face(250, 320, 70, 930, 4, light, 990, face,
                                               'omni-parent', 6, 0),
                                      tile(320, 4, light, 990, face, 1)))
    for i in range(2):
        invalid_query_records.extend((gpu_face(250, 320, 70, 930, 5 + i, 9910 + i, 0, 0,
                                               'single', 1, 0),
                                      tile(320, 5 + i, 9910 + i, 0, 0, 0)))
    invalid_query, invalid_query_status = run_case(invalid_query_records)
    assert invalid_query['cpu_grid_qualification'][0]['qualified'] is True
    assert invalid_query['cpu_grid_qualification'][0]['query_invalid'] == 1
    assert invalid_query['selected_geometry_work_totals']['triangles'] == 8
    assert invalid_query['incomplete_or_inconclusive_group_count'] == 1
    assert invalid_query['complete_gpu_cpu_correlated_group_count'] == 2
    assert invalid_query_status == 2

    # A flag/face mismatch remains inconclusive rather than qualifying.
    mismatch_records = [prep()] + unknown_rows
    mismatch_records.append(grid(250, 320, unknown_rows, query_invalid=0))
    mismatch, mismatch_status = run_case(mismatch_records)
    assert mismatch['cpu_grid_qualification'][0]['qualified'] is False
    assert mismatch_status == 2

    # Purely nonzero, completely correlated data is valid but never claims culling.
    rows = [cpu(201, 300, 0, 801, 0, 0, 'visible_or_unknown')]
    rows = [cpu(201, 300, 0, 801, 91, 0, 'visible_or_unknown', kind='single')]
    nonzero, nonzero_status = run_case(scenario(201, 300, 61, 921, 'single', 801, rows, [3], single_parent_id=91))
    assert nonzero_status == 0
    assert nonzero['observation_status'] == 'complete_observations'
    assert nonzero['complete_gpu_cpu_correlated_group_count'] == 1
    assert nonzero['missed_zero_groups'] == []

    # One omni parent spans two draw batches; selected_groups is parent identity,
    # while every face retains its own full batch identity for receipt matching.
    split_rows = [cpu(202, 301, f // 3, 9001 + f, 900, f,
                      kind='omni-parent', expected=6) for f in range(6)]
    split, split_status = run_case(scenario(202, 301, 62, 922, 'omni-parent', 900, split_rows, [0] * 6,
                                            face_batches=[f // 3 for f in range(6)]))
    assert split_status == 0
    assert split['observation_status'] == 'complete_observations'
    assert split['complete_gpu_cpu_correlated_group_count'] == 1
    assert split['groups'][0]['all_cpu_faces_behind'] is True

    # A malformed or missing catalog snapshot prevents successful CPU/GPU correlation.
    invalid_prep_rows = scenario(203, 302, 63, 923, 'single', 901,
                                 [cpu(203, 302, 0, 901, 0, 0)], [0])
    bad_catalog, bad_catalog_status = run_case([prep(bad_counts=True)] + invalid_prep_rows)
    assert bad_catalog_status == 2
    assert bad_catalog['execution_integrity'] == 'pass'
    assert bad_catalog['catalog_qualified'] is False
    assert bad_catalog['observation_status'] == 'incomplete_or_inconclusive'
    assert bad_catalog['complete_gpu_cpu_correlated_group_count'] == 0

    undersized_prep = prep().replace('catalog_bytes=100000', 'catalog_bytes=61199')
    undersized_catalog, undersized_status = run_case(
        [undersized_prep] + scenario(210, 309, 70, 930, 'single', 908,
                                     [cpu(210, 309, 0, 908, 0, 0)], [0]))
    assert undersized_status == 2
    assert undersized_catalog['catalog_qualified'] is False
    assert undersized_catalog['complete_gpu_cpu_correlated_group_count'] == 0

    for mate_count in (0, 1000):
        bounded_mates, bounded_status = run_case(
            [prep(mate_hints_cleared=mate_count)] +
            scenario(213 + mate_count, 312, 73 + mate_count, 933 + mate_count, 'single', 911 + mate_count,
                     [cpu(213 + mate_count, 312, 0, 911 + mate_count, 0, 0)], [0]))
        assert bounded_status == 0
        assert bounded_mates['catalog_qualified'] is True
        assert bounded_mates['mate_hints_cleared'] == mate_count

    overcount_prep = prep(mate_hints_cleared=1001)
    overcount, overcount_status = run_case(
        [overcount_prep] + scenario(214, 313, 74, 934, 'single', 912,
                                    [cpu(214, 313, 0, 912, 0, 0)], [0]))
    assert overcount_status == 2
    assert overcount['catalog_qualified'] is False
    assert overcount['complete_gpu_cpu_correlated_group_count'] == 0

    missing_mate_count = prep().replace('mate_hints_cleared=100 ', '')
    missing_mate, missing_mate_status = run_case(
        [missing_mate_count] + scenario(215, 314, 75, 935, 'single', 913,
                                        [cpu(215, 314, 0, 913, 0, 0)], [0]))
    assert missing_mate_status == 2
    assert missing_mate['catalog_qualified'] is False
    assert missing_mate['complete_gpu_cpu_correlated_group_count'] == 0

    negative_mate_count = prep().replace('mate_hints_cleared=100', 'mate_hints_cleared=-1')
    negative_mate, negative_mate_status = run_case(
        [negative_mate_count] + scenario(216, 315, 76, 936, 'single', 914,
                                         [cpu(216, 315, 0, 914, 0, 0)], [0]))
    assert negative_mate_status == 2
    assert negative_mate['catalog_qualified'] is False
    assert negative_mate['complete_gpu_cpu_correlated_group_count'] == 0

    # Explicit disabled PREP is a fail-open observation, never a qualified empty catalog.
    disabled_prep_rows = scenario(204, 303, 64, 924, 'single', 902,
                                  [cpu(204, 303, 0, 902, 0, 0)], [0])
    disabled_catalog, disabled_status = run_case([prep(status='disabled', reason='catalog-unavailable')] + disabled_prep_rows)
    assert disabled_status == 2
    assert disabled_catalog['catalog_qualified'] is False
    assert disabled_catalog['observation_status'] == 'incomplete_or_inconclusive'
    assert disabled_catalog['complete_gpu_cpu_correlated_group_count'] == 0

    missing_catalog, missing_status = run_case(
        scenario(205, 304, 65, 925, 'single', 903, [cpu(205, 304, 0, 903, 0, 0)], [0]),
        inject_prep=False)
    assert missing_status == 2
    assert missing_catalog['catalog_preparation_count'] == 0
    assert missing_catalog['catalog_qualified'] is False
    assert missing_catalog['complete_gpu_cpu_correlated_group_count'] == 0

    duplicate_catalog, duplicate_status = run_case(
        [prep(), prep()] + scenario(206, 305, 66, 926, 'single', 904,
                                    [cpu(206, 305, 0, 904, 0, 0)], [0]))
    assert duplicate_status == 2
    assert duplicate_catalog['catalog_preparation_count'] == 2
    assert duplicate_catalog['catalog_qualified'] is False
    assert duplicate_catalog['complete_gpu_cpu_correlated_group_count'] == 0

    malformed_selector_rows = scenario(207, 306, 67, 927, 'single', 905,
                                       [cpu(207, 306, 0, 905, 0, 0)], [0])
    malformed_selector_rows = [row.replace('draws_used=1', 'draws_used=0')
                               if 'METAL_ACTIVE_OCCLUSION_GRID ' in row else row
                               for row in malformed_selector_rows]
    malformed_selector, malformed_selector_status = run_case(malformed_selector_rows)
    assert malformed_selector_status == 2
    assert malformed_selector['observation_status'] == 'incomplete_or_inconclusive'
    assert malformed_selector['complete_gpu_cpu_correlated_group_count'] == 0
    assert malformed_selector['active_grid_qualified_count'] == 0

    over_catalog_receipts_rows = scenario(208, 307, 68, 928, 'single', 906,
                                          [cpu(208, 307, 0, 906, 0, 0)], [0])
    over_catalog_receipts_rows = [row.replace('draw_receipts=1', 'draw_receipts=101')
                                  .replace('draws_skipped=0', 'draws_skipped=100')
                                  if 'METAL_ACTIVE_OCCLUSION_GRID ' in row else row
                                  for row in over_catalog_receipts_rows]
    over_catalog_receipts, over_catalog_receipts_status = run_case(over_catalog_receipts_rows)
    assert over_catalog_receipts_status == 2
    assert over_catalog_receipts['active_grid_qualified_count'] == 0
    assert over_catalog_receipts['complete_gpu_cpu_correlated_group_count'] == 0

    over_catalog_triangles_rows = scenario(209, 308, 69, 929, 'single', 907,
                                           [cpu(209, 308, 0, 907, 0, 0)], [0])
    over_catalog_triangles_rows = [row.replace('receipt_triangles=8', 'receipt_triangles=1001')
                                   .replace('selected_triangles=8', 'selected_triangles=1001')
                                   .replace('triangles=8', 'triangles=1001')
                                   if 'METAL_ACTIVE_OCCLUSION_GRID ' in row else row
                                   for row in over_catalog_triangles_rows]
    over_catalog_triangles, over_catalog_triangles_status = run_case(over_catalog_triangles_rows)
    assert over_catalog_triangles_status == 2
    assert over_catalog_triangles['active_grid_qualified_count'] == 0
    assert over_catalog_triangles['complete_gpu_cpu_correlated_group_count'] == 0

    invalid_cpu_rows = scenario(211, 310, 71, 931, 'single', 909,
                                [cpu(211, 310, 0, 909, 0, 0, 'behind')], [1])
    invalid_cpu_rows = [row.replace('expected_faces=1', 'expected_faces=0')
                        if 'METAL_ACTIVE_OCCLUSION_FACE ' in row else row
                        for row in invalid_cpu_rows]
    invalid_cpu, invalid_cpu_status = run_case(invalid_cpu_rows)
    assert invalid_cpu_status == 2
    assert invalid_cpu['false_hidden_faces'] == []
    assert invalid_cpu['observation_status'] == 'incomplete_or_inconclusive'

    invalid_gpu_rows = scenario(212, 311, 72, 932, 'single', 910,
                                [cpu(212, 311, 0, 910, 0, 0, 'behind')], [1])
    invalid_gpu_rows = [row.replace('submitted_serial=72', 'submitted_serial=0')
                        if 'METAL_WHOLE_LIGHT_VISIBILITY_FACE ' in row else row
                        for row in invalid_gpu_rows]
    invalid_gpu, invalid_gpu_status = run_case(invalid_gpu_rows)
    assert invalid_gpu_status == 2
    assert invalid_gpu['false_hidden_faces'] == []
    assert invalid_gpu['observation_status'] == 'incomplete_or_inconclusive'

    print('ACTIVE_OCCLUSION_V9R1_PARSER_FIXTURES PASS mate_hints_cleared zero/max/over/missing/negative; catalog ready/disabled/missing/duplicate/count-mismatch/undersized; selector arithmetic/catalog-relative bounds; zero/nonzero/missed-zero/false-hidden; malformed CPU/GPU identity; mixed-scope/split-batch; missing/duplicate-face/grid; caps/orphans')


if __name__ == '__main__':
    main()
