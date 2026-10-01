#!/usr/bin/env python3
"""Conservative CPU-prediction / submitted-GPU receipt join for one V7 run."""
import json
import math
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def parse_fields(line):
    fields = {}
    for match in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)=("[^"\n]*"|[^\s]+)', line):
        value = match.group(2)
        fields[match.group(1)] = value[1:-1] if value.startswith('"') else value
    return fields


def integer(fields, name):
    try:
        return int(fields[name], 10)
    except (KeyError, TypeError, ValueError):
        return None


def nonnegative(fields, names):
    return all(isinstance(fields.get(name), int) and fields[name] >= 0 for name in names)


def finite_nonnegative(fields, names):
    values = {}
    for name in names:
        try:
            value = float(fields[name])
        except (KeyError, TypeError, ValueError):
            return None
        if not math.isfinite(value) or value < 0:
            return None
        values[name] = value
    return values


def normalize(fields, names):
    result = dict(fields)
    for name in names:
        result[name] = integer(fields, name)
    return result


CPU_ID = ('cpu_observation_scope', 'engine_frame', 'batch_id', 'light_id', 'parent_light_id', 'face')
CPU_INTEGER_FIELDS = CPU_ID + ('expected_faces', 'query_cells')
GRID_INTEGER_FIELDS = ('cpu_observation_scope', 'engine_frame', 'selected_groups', 'selected_faces',
                       'triangles', 'paired_unions_accepted', 'cells_marked', 'primitive_cell_tests',
                       'query_cell_tests', 'triangle_cap', 'cell_test_cap')
GPU_FACE_INTEGER_FIELDS = ('submitted_serial', 'acquire_epoch', 'engine_frame', 'cpu_observation_scope',
                           'batch_id', 'light_id', 'parent_light_id', 'face', 'expected_faces',
                           'deferred_receiver_samples')
GROUP_INTEGER_FIELDS = ('submitted_serial', 'acquire_epoch', 'engine_frame', 'cpu_observation_scope',
                        'group_id', 'expected_faces', 'deferred_receiver_samples')
TILE_KEY = ('engine_frame', 'batch_id', 'light_id', 'parent_light_id', 'face')
TILE_COUNTERS = ('static_rows', 'static_encoded_draws', 'dynamic_submitted_draws', 'static_transform_bytes')
GRID_COUNTERS = ('selected_groups', 'selected_faces', 'triangles', 'paired_unions_accepted', 'cells_marked',
                 'primitive_cell_tests', 'query_cell_tests')


def valid_cpu_identity(face):
    return all(isinstance(face.get(name), int) and face[name] >= 0 for name in CPU_INTEGER_FIELDS) and \
        face['cpu_observation_scope'] > 0 and face['face'] in range(6) and \
        face.get('group_kind') in ('single', 'omni-parent') and \
        face.get('expected_faces') == (1 if face.get('group_kind') == 'single' else 6) and \
        (face.get('face') == 0 if face.get('group_kind') == 'single' else True)


def cpu_key(fields):
    return tuple(fields.get(name) for name in CPU_ID)


def tile_key(fields):
    return tuple(fields.get(name) for name in TILE_KEY)


def valid_grid(grid, face_count, group_count, query_cells):
    timings = finite_nonnegative(grid, ('build_ms', 'query_ms'))
    return grid.get('status') == 'ready' and nonnegative(grid, GRID_INTEGER_FIELDS) and \
        grid['cpu_observation_scope'] > 0 and grid['triangle_cap'] in (0, 1) and grid['cell_test_cap'] in (0, 1) and \
        grid['selected_faces'] == face_count and grid['selected_groups'] == group_count and \
        grid['query_cell_tests'] == query_cells and timings is not None


def expected_face_numbers(kind):
    if kind == 'single':
        return [0]
    if kind == 'omni-parent':
        return list(range(6))
    return []


def main(out):
    raw = (out / 'engine.log').read_bytes().decode('utf-8', errors='replace')
    lines = raw.splitlines()
    marker_path = out / 'observer-markers.csv'
    marker_text = marker_path.read_bytes().decode('utf-8', errors='replace') if marker_path.exists() else ''
    marker_counts = Counter(line.split(',', 1)[0] for line in marker_text.splitlines() if line.strip())

    cpu_faces, grids, gpu_faces, gpu_groups, tiles, selection_frames = [], [], [], [], [], []
    for line_no, line in enumerate(lines, 1):
        if 'METAL_ACTIVE_OCCLUSION_FACE ' in line:
            face = normalize(parse_fields(line), CPU_INTEGER_FIELDS)
            face['_line'] = line_no
            cpu_faces.append(face)
        elif 'METAL_ACTIVE_OCCLUSION_GRID ' in line:
            grid = normalize(parse_fields(line), GRID_INTEGER_FIELDS)
            grid['_line'] = line_no
            grids.append(grid)
        elif 'METAL_WHOLE_LIGHT_VISIBILITY_FACE ' in line:
            face = normalize(parse_fields(line), GPU_FACE_INTEGER_FIELDS)
            face['_line'] = line_no
            gpu_faces.append(face)
        elif 'METAL_WHOLE_LIGHT_VISIBILITY status=' in line:
            group = normalize(parse_fields(line), GROUP_INTEGER_FIELDS)
            group['_line'] = line_no
            gpu_groups.append(group)
        elif 'METAL_WHOLE_LIGHT_VIS_TILE ' in line:
            tile = parse_fields(line)
            for name, source in (('engine_frame', 'frame'), ('batch_id', 'batch'), ('light_id', 'light'),
                                 ('parent_light_id', 'parent'), ('face', 'face'), ('kind', 'kind')):
                tile[name] = integer(tile, source)
            for name in TILE_COUNTERS:
                tile[name] = integer(tile, name)
            tile['_line'] = line_no
            tiles.append(tile)
        elif 'METAL_WHOLE_LIGHT_VIS_FRAME ' in line:
            selection = parse_fields(line)
            selection['_line'] = line_no
            selection_frames.append(selection)

    cpu_counts = Counter(cpu_key(face) for face in cpu_faces)
    grid_counts = Counter((grid.get('cpu_observation_scope'), grid.get('engine_frame')) for grid in grids)
    cpu_by_key = {cpu_key(face): face for face in cpu_faces if cpu_counts[cpu_key(face)] == 1}
    grid_by_key = {(g.get('cpu_observation_scope'), g.get('engine_frame')): g for g in grids
                   if grid_counts[(g.get('cpu_observation_scope'), g.get('engine_frame'))] == 1}
    faces_by_grid = defaultdict(list)
    for face in cpu_faces:
        faces_by_grid[(face.get('cpu_observation_scope'), face.get('engine_frame'))].append(face)
    grid_validation = []
    for key, face_rows in sorted(faces_by_grid.items(), key=lambda item: repr(item[0])):
        grid = grid_by_key.get(key)
        group_ids = {(f.get('group_kind'), f.get('parent_light_id') if f.get('group_kind') == 'omni-parent'
                      else f.get('light_id')) for f in face_rows}
        query_cells = sum(f.get('query_cells', 0) for f in face_rows
                          if isinstance(f.get('query_cells'), int) and f.get('query_cells') >= 0)
        face_lines_before_grid = grid is not None and all(grid.get('_line', 0) > face.get('_line', 0) for face in face_rows)
        ok = grid is not None and face_lines_before_grid and valid_grid(grid, len(face_rows), len(group_ids), query_cells)
        grid_validation.append({'scope': key[0], 'engine_frame': key[1], 'face_rows': len(face_rows),
                                'selected_group_rows': len(group_ids), 'query_cell_tests_sum': query_cells,
                                'grid_line': grid.get('_line') if grid else None,
                                'grid_follows_all_face_rows': face_lines_before_grid,
                                'status': grid.get('status') if grid else 'missing', 'qualified': ok,
                                'build_ms': finite_nonnegative(grid, ('build_ms', 'query_ms'))['build_ms'] if grid and finite_nonnegative(grid, ('build_ms', 'query_ms')) else None,
                                'query_ms': finite_nonnegative(grid, ('build_ms', 'query_ms'))['query_ms'] if grid and finite_nonnegative(grid, ('build_ms', 'query_ms')) else None,
                                'triangle_cap': grid.get('triangle_cap') if grid else None,
                                'cell_test_cap': grid.get('cell_test_cap') if grid else None})
    grid_validation_row = {(row['scope'], row['engine_frame']): row['qualified'] for row in grid_validation}

    gpu_face_counts = Counter((f.get('submitted_serial'), f.get('acquire_epoch'), f.get('engine_frame'),
                               f.get('group_kind'), f.get('expected_faces'), f.get('cpu_observation_scope'),
                               f.get('parent_light_id') if f.get('group_kind') == 'omni-parent' else f.get('light_id'))
                              for f in gpu_faces)
    group_counts = Counter((g.get('submitted_serial'), g.get('acquire_epoch'), g.get('engine_frame'),
                            g.get('group_kind'), g.get('group_id'), g.get('cpu_observation_scope')) for g in gpu_groups)

    # A false-hidden face is independently actionable even if another face
    # makes its parent incomplete. Require unique, full-identity CPU and GPU rows.
    false_hidden = []
    for key, cpu in cpu_by_key.items():
        if cpu.get('prediction') != 'behind' or cpu.get('cpu_observation_scope', 0) <= 0:
            continue
        matches = [gpu for gpu in gpu_faces if cpu_key(gpu) == key]
        if len(matches) != 1:
            continue
        gpu = matches[0]
        if cpu.get('group_kind') == gpu.get('group_kind') and cpu.get('expected_faces') == gpu.get('expected_faces') and \
                gpu.get('status') == 'VALID' and isinstance(gpu.get('deferred_receiver_samples'), int) and \
                gpu['deferred_receiver_samples'] > 0:
            false_hidden.append({'identity': dict(zip(CPU_ID, key)), 'cpu_line': cpu['_line'],
                                 'gpu_line': gpu['_line'], 'deferred_receiver_samples': gpu['deferred_receiver_samples'],
                                 'finding': 'CPU predicted behind but submitted GPU receipt has deferred receiver samples'})

    complete, incomplete, missed_zero = [], [], []
    assigned_gpu_face_lines = set()
    used_cpu_keys = set()
    for group in gpu_groups:
        kind = group.get('group_kind', 'unknown')
        frame, serial, epoch = group.get('engine_frame'), group.get('submitted_serial'), group.get('acquire_epoch')
        scope, group_id = group.get('cpu_observation_scope'), group.get('group_id')
        expected = expected_face_numbers(kind)
        key = (serial, epoch, frame, kind, group_id, scope)
        metadata_ok = group_counts[key] == 1 and group.get('status') == 'complete' and \
            all(isinstance(v, int) and v > 0 for v in (serial, epoch, scope)) and \
            isinstance(frame, int) and frame >= 0 and isinstance(group_id, int) and group_id > 0 and \
            group.get('expected_faces') == len(expected) and bool(expected)
        matched_gpu = []
        for face in gpu_faces:
            if (face.get('submitted_serial'), face.get('acquire_epoch'), face.get('engine_frame'),
                face.get('group_kind'), face.get('expected_faces'), face.get('cpu_observation_scope')) != \
                    (serial, epoch, frame, kind, group.get('expected_faces'), scope):
                continue
            if kind == 'omni-parent' and face.get('parent_light_id') == group_id:
                matched_gpu.append(face)
            elif kind == 'single' and face.get('light_id') == group_id:
                matched_gpu.append(face)
        matched_gpu.sort(key=lambda f: f.get('face') if isinstance(f.get('face'), int) else -1)
        assigned_gpu_face_lines.update(f['_line'] for f in matched_gpu)
        shape_ok = len(matched_gpu) == len(expected) and [f.get('face') for f in matched_gpu] == expected
        if kind == 'omni-parent':
            shape_ok = shape_ok and all(f.get('parent_light_id') == group_id and f.get('light_id') != group_id for f in matched_gpu)
        elif kind == 'single':
            shape_ok = shape_ok and len(matched_gpu) == 1 and matched_gpu[0].get('light_id') == group_id
        face_receipts_ok = shape_ok and all(f.get('status') == 'VALID' and
            isinstance(f.get('deferred_receiver_samples'), int) and f['deferred_receiver_samples'] >= 0 and
            all(isinstance(f.get(k), int) and f[k] >= 0 for k in GPU_FACE_INTEGER_FIELDS) for f in matched_gpu)
        group_samples = group.get('deferred_receiver_samples')
        face_sum = sum(f.get('deferred_receiver_samples', 0) for f in matched_gpu
                       if isinstance(f.get('deferred_receiver_samples'), int) and f['deferred_receiver_samples'] >= 0)
        zero_flag = group.get('zero_deferred_receiver_samples')
        count_ok = isinstance(group_samples, int) and group_samples >= 0 and face_sum == group_samples and \
            ((zero_flag == 'true' and group_samples == 0) or (zero_flag == 'false' and group_samples > 0))

        cpu_matches = []
        for gpu in matched_gpu:
            cpu = cpu_by_key.get(cpu_key(gpu))
            expected_faces = 1 if group.get('group_kind') == 'single' else 6 if group.get('group_kind') == 'omni-parent' else None
            if cpu is not None and cpu.get('group_kind') == kind and cpu.get('expected_faces') == expected_faces and \
                    cpu.get('prediction') in ('behind', 'visible_or_unknown') and \
                    nonnegative(cpu, CPU_INTEGER_FIELDS) and valid_cpu_identity(cpu):
                cpu_matches.append(cpu)
                used_cpu_keys.add(cpu_key(cpu))
        cpu_complete = len(cpu_matches) == len(expected) and len({cpu_key(f) for f in cpu_matches}) == len(expected) and \
            all(grid_validation_row.get((f['cpu_observation_scope'], f['engine_frame']), False) for f in cpu_matches)
        tile_matches = []
        tile_unique = True
        for gpu in matched_gpu:
            mt = [tile for tile in tiles if tile_key(tile) == tile_key(gpu)]
            if len(mt) != 1:
                tile_unique = False
            tile_matches.extend(mt)
        tile_kind = 1 if kind == 'omni-parent' else 0 if kind == 'single' else None
        tile_ok = len(matched_gpu) > 0 and tile_unique and len(tile_matches) == len(matched_gpu) and \
            all(nonnegative(t, TILE_COUNTERS) and t.get('kind') == tile_kind for t in tile_matches)
        query_complete = metadata_ok and face_receipts_ok and count_ok and cpu_complete and tile_ok
        all_behind = query_complete and all(f.get('prediction') == 'behind' for f in cpu_matches)
        item = {'line': group.get('_line'), 'scope': scope, 'frame': frame, 'submitted_serial': serial,
                'acquire_epoch': epoch, 'group_kind': kind, 'group_id': group_id,
                'status': group.get('status'), 'gpu_sample_count': group_samples,
                'gpu_zero_flag': zero_flag, 'cpu_face_predictions': [f.get('prediction') for f in cpu_matches],
                'all_cpu_faces_behind': bool(all_behind), 'metadata_complete': metadata_ok,
                'gpu_faces_complete': face_receipts_ok, 'gpu_count_consistent': count_ok,
                'cpu_faces_and_grids_complete': cpu_complete, 'tile_counters_complete': tile_ok,
                'selected_tile_counters': [{k: t.get(k) for k in TILE_KEY + TILE_COUNTERS + ('_line',)} for t in tile_matches],
                'classification': 'inconclusive'}
        if query_complete:
            item['classification'] = 'zero_deferred_receiver_samples' if group_samples == 0 else 'nonzero_deferred_receiver_samples'
            complete.append(item)
            if group_samples == 0 and not all_behind:
                item['missed_zero_reason'] = 'complete GPU zero group has at least one CPU face not predicted behind'
                missed_zero.append(item)
        else:
            incomplete.append(item)

    game_exit = None
    try:
        game_exit = int((out / 'exit-status.txt').read_text().strip())
    except (OSError, ValueError):
        pass
    timeout = (out / 'timeout-flag.txt').exists() and (out / 'timeout-flag.txt').read_text().strip() == '1'
    post_path = out / 'post-run-app-sha256.txt'
    post_hash = post_path.read_text(encoding='utf-8').split()[0] if post_path.exists() else None
    expected_hash = None
    for row in (out / 'input-sha256.txt').read_text(encoding='utf-8', errors='replace').splitlines() if (out / 'input-sha256.txt').exists() else []:
        if 'candidate-v7/Anomaly.app/Contents/MacOS/AnomalyDX11' in row:
            expected_hash = row.split()[0]
            break
    markers = [m for m in marker_counts for _ in range(marker_counts[m])]
    failed_markers = [m for m in markers if m.startswith('FAILED_')]
    base_integrity = game_exit == 0 and not timeout and marker_counts['observer_window_start'] == 1 and \
        marker_counts['observer_window_end'] == 1 and marker_counts['observer_drain_complete'] == 1 and \
        not failed_markers and post_hash == expected_hash and expected_hash is not None

    unmatched_cpu_faces = [f for f in cpu_faces if cpu_key(f) not in used_cpu_keys]
    unmatched_gpu_faces = [f for f in gpu_faces if f['_line'] not in assigned_gpu_face_lines]
    orphan_grids = [g for g in grids if (g.get('selected_groups', 0) or g.get('selected_faces', 0)) and
                    (g.get('cpu_observation_scope'), g.get('engine_frame')) not in faces_by_grid]
    matched_tile_totals = {name: sum(t.get(name, 0) for group in complete
                                      for t in group['selected_tile_counters']) for name in TILE_COUNTERS}
    semantic_gaps = unmatched_cpu_faces or unmatched_gpu_faces or orphan_grids
    completion_count = Counter(g.get('engine_frame') for g in gpu_groups
                               if g.get('status') in ('complete', 'invalid'))
    selected_without_completion = []
    selection_cap_violations = []
    for selection in selection_frames:
        frame = integer(selection, 'frame')
        selected = integer(selection, 'selected_groups') or 0
        selected_records = integer(selection, 'selected_records') or 0
        if frame is not None and selected > completion_count[frame]:
            selected_without_completion.append({'frame': frame, 'selected_groups': selected,
                                                'completion_receipts': completion_count[frame]})
        if selected > 8 or selected_records > 48:
            selection_cap_violations.append({'frame': frame, 'selected_groups': selected,
                                             'selected_records': selected_records})
    semantic_gaps = semantic_gaps or selected_without_completion or selection_cap_violations
    data_status = 'complete_observations' if complete and not incomplete and not false_hidden and not semantic_gaps else 'incomplete_or_inconclusive'
    if false_hidden:
        data_status = 'false_hidden_finding'
    summary = {
        'schema': 'active-occlusion-hybrid-observer/v1',
        'execution_integrity': 'pass' if base_integrity else 'fail',
        'game_exit_status': game_exit, 'timeout': timeout,
        'post_run_candidate_hash_matches_preflight': bool(post_hash and post_hash == expected_hash),
        'markers': dict(marker_counts), 'failed_markers': failed_markers,
        'observation_status': data_status,
        'cpu_grid_count': len(grids), 'cpu_face_row_count': len(cpu_faces),
        'gpu_face_receipt_count': len(gpu_faces), 'gpu_group_receipt_count': len(gpu_groups),
        'cpu_grid_qualification': grid_validation,
        'complete_gpu_cpu_correlated_group_count': len(complete),
        'zero_deferred_receiver_sample_group_count': sum(g['gpu_sample_count'] == 0 for g in complete),
        'nonzero_deferred_receiver_sample_group_count': sum(g['gpu_sample_count'] > 0 for g in complete),
        'incomplete_or_inconclusive_group_count': len(incomplete),
        'unmatched_cpu_face_count': len(unmatched_cpu_faces),
        'unmatched_gpu_face_count': len(unmatched_gpu_faces),
        'orphan_nonempty_grid_count': len(orphan_grids),
        'unmatched_cpu_face_lines': [f.get('_line') for f in unmatched_cpu_faces],
        'unmatched_gpu_face_lines': [f.get('_line') for f in unmatched_gpu_faces],
        'selected_without_completion': selected_without_completion,
        'selection_cap_violations': selection_cap_violations,
        'missed_zero_groups': missed_zero,
        'false_hidden_faces': false_hidden,
        'selected_tile_work_totals': matched_tile_totals,
        'cpu_build_query_costs_ms': [
            {'scope': g.get('scope'), 'engine_frame': g.get('engine_frame'), 'build_ms': g.get('build_ms'),
             'query_ms': g.get('query_ms')} for g in grid_validation],
        'groups': complete + incomplete,
        'interpretation_limit': 'CPU behind is an observation only; GPU receipts establish submitted deferred receiver samples. No unselected-work, saved-time, or FPS inference is valid.',
        'no_fps_or_visual_claim': True,
    }
    (out / 'observer-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print('execution_integrity=' + summary['execution_integrity'])
    print('observation_status=' + data_status)
    print(f"cpu_grids={len(grids)} cpu_faces={len(cpu_faces)} gpu_groups={len(gpu_groups)} correlated={len(complete)} incomplete={len(incomplete)} false_hidden={len(false_hidden)}")
    return 3 if false_hidden else 0 if base_integrity else 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: summarize_observation_v7.py OUTPUT_DIRECTORY')
    raise SystemExit(main(Path(sys.argv[1])))
