#!/usr/bin/env python3
"""Synthetic safety cases for the V7 CPU/GPU observer join."""
import importlib.util
import json
import tempfile
from pathlib import Path


SCRIPT = Path(__file__).with_name('summarize_observation_v7.py')
SPEC = importlib.util.spec_from_file_location('summarize_observation_v7', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def cpu(scope, frame, batch, light, parent, face, prediction='behind', cells=1,
        kind=None, expected=None):
    kind = kind or 'single'
    expected = expected or (6 if kind == 'omni-parent' else 1)
    return ('METAL_ACTIVE_OCCLUSION_FACE '
            f'cpu_observation_scope={scope} engine_frame={frame} batch_id={batch} '
            f'light_id={light} parent_light_id={parent} face={face} '
            f'group_kind={kind} expected_faces={expected} '
            f'prediction={prediction} query_cells={cells} reason=fixture')


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


def grid(scope, frame, faces, *, status='ready', triangles=8, triangle_cap=0, cell_test_cap=0):
    groups = len({(row.split('group_kind=')[1].split()[0],
                   int(row.split('parent_light_id=')[1].split()[0]) if 'group_kind=omni-parent' in row else
                   int(row.split('light_id=')[1].split()[0])) for row in faces})
    query = sum(int(row.split('query_cells=')[1].split()[0]) for row in faces)
    return ('METAL_ACTIVE_OCCLUSION_GRID '
            f'status={status} cpu_observation_scope={scope} engine_frame={frame} '
            f'selected_groups={groups} selected_faces={len(faces)} triangles={triangles} '
            f'paired_unions_accepted=1 cells_marked=12 primitive_cell_tests=32 '
            f'query_cell_tests={query} triangle_cap={triangle_cap} cell_test_cap={cell_test_cap} '
            'build_ms=0.125 query_ms=0.062 reason=fixture')


def scenario(scope, frame, serial, epoch, kind, gid, cpu_rows, gpu_counts, *,
             group_status='complete', gpu_status='VALID', gpu_scopes=None, duplicate_cpu=0,
             duplicate_gpu=0, grid_status='ready', bad_grid=False, face_batches=None,
             triangle_cap=0, cell_test_cap=0, single_parent_id=0, duplicate_grid=0):
    records = []
    for row in cpu_rows:
        records.append(row)
    records.extend([cpu_rows[0]] * duplicate_cpu if cpu_rows else [])
    if cpu_rows:
        grid_row = grid(scope, frame, cpu_rows, status=grid_status,
                        triangles=-1 if bad_grid else 8, triangle_cap=triangle_cap,
                        cell_test_cap=cell_test_cap)
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


def run_case(records, expected_hash='fixture-hash'):
    with tempfile.TemporaryDirectory(prefix='active-occlusion-v7-') as tmp:
        out = Path(tmp)
        log = [row for row in records if row]
        (out / 'engine.log').write_bytes(('\n'.join(log) + '\n').encode('utf-8'))
        (out / 'observer-markers.csv').write_text(
            'observer_window_start,frame=300\nobserver_window_end,frame=311\n'
            'observer_drain_complete,frame=311\n', encoding='utf-8')
        (out / 'environment.json').write_text(json.dumps({'XRAY_METAL_ACTIVE_OCCLUSION': '1'}), encoding='utf-8')
        (out / 'input-sha256.txt').write_text(f'{expected_hash}  /candidate-v7/Anomaly.app/Contents/MacOS/AnomalyDX11\n', encoding='ascii')
        (out / 'exit-status.txt').write_text('0\n', encoding='ascii')
        (out / 'timeout-flag.txt').write_text('0\n', encoding='ascii')
        (out / 'post-run-app-sha256.txt').write_text(f'{expected_hash} /candidate-v7/Anomaly.app/Contents/MacOS/AnomalyDX11\n', encoding='ascii')
        status = MODULE.main(out)
        return json.loads((out / 'observer-summary.json').read_text(encoding='utf-8')), status


def main():
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
    records.append('METAL_ACTIVE_OCCLUSION_GRID status=ready cpu_observation_scope=116 engine_frame=315 '
                   'selected_groups=1 selected_faces=1 triangles=0 paired_unions_accepted=0 cells_marked=0 '
                   'primitive_cell_tests=0 query_cell_tests=0 triangle_cap=0 cell_test_cap=0 '
                   'build_ms=0 query_ms=0 reason=fixture')

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

    print('ACTIVE_OCCLUSION_V7_PARSER_FIXTURES PASS zero nonzero missed-zero false-hidden mixed-scope split-batch missing duplicate-face duplicate-grid scope-zero skipped invalid malformed capped-subset orphan-records')


if __name__ == '__main__':
    main()
