#!/usr/bin/env python3
"""Byte-safe receipt parser for one whole-light visibility observer run."""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path


def parse_fields(line):
    fields = {}
    for match in re.finditer(r'([A-Za-z_][A-Za-z0-9_]*)=("[^"\n]*"|[^\s]+)', line):
        value = match.group(2)
        if value.startswith('"') and value.endswith('"'):
            value = value[1:-1]
        fields[match.group(1)] = value
    return fields


def int_field(fields, name):
    try:
        return int(fields[name], 10)
    except (KeyError, ValueError):
        return None


FACE_INTEGER_FIELDS = (
    'engine_frame', 'submitted_serial', 'acquire_epoch', 'batch_id',
    'light_id', 'parent_light_id', 'face', 'expected_faces',
    'deferred_receiver_samples',
)
TILE_SOURCE_FIELDS = {
    'engine_frame': 'frame',
    'batch_id': 'batch',
    'light_id': 'light',
    'parent_light_id': 'parent',
    'face': 'face',
}
TILE_COUNTER_FIELDS = (
    'static_rows', 'static_encoded_draws', 'dynamic_submitted_draws',
    'static_transform_bytes',
)
TILE_KEY_FIELDS = ('engine_frame', 'batch_id', 'light_id', 'parent_light_id', 'face')


def normalize_face(fields):
    normalized = dict(fields)
    for name in FACE_INTEGER_FIELDS:
        normalized[name] = int_field(fields, name)
    return normalized


def normalize_tile(fields):
    normalized = dict(fields)
    for target, source in TILE_SOURCE_FIELDS.items():
        normalized[target] = int_field(fields, source)
    normalized['kind'] = int_field(fields, 'kind')
    for name in TILE_COUNTER_FIELDS:
        normalized[name] = int_field(fields, name)
    return normalized


def tile_key(fields):
    return tuple(fields.get(name) for name in TILE_KEY_FIELDS)


def has_nonnegative_integer_fields(fields, names):
    return all(isinstance(fields.get(name), int) and fields[name] >= 0 for name in names)


def main(out):
    raw_log = (out / 'engine.log').read_bytes()
    text = raw_log.decode('utf-8', errors='replace')
    lines = text.splitlines()
    marker_path = out / 'observer-markers.csv'
    marker_text = marker_path.read_bytes().decode('utf-8', errors='replace') if marker_path.exists() else ''
    markers = [line.split(',', 1)[0] for line in marker_text.splitlines() if line.strip()]
    marker_counts = Counter(markers)

    selection_frames = []
    group_events = []
    face_events = []
    tile_events = []
    disabled_events = []
    cap_events = []
    other_events = []
    for line_no, line in enumerate(lines, 1):
        if 'METAL_WHOLE_LIGHT_VIS_FRAME ' in line:
            fields = parse_fields(line)
            fields['_line'] = line_no
            selection_frames.append(fields)
        elif 'METAL_WHOLE_LIGHT_VISIBILITY_FACE ' in line:
            fields = normalize_face(parse_fields(line))
            fields['_line'] = line_no
            face_events.append(fields)
        elif 'METAL_WHOLE_LIGHT_VIS_TILE ' in line:
            fields = normalize_tile(parse_fields(line))
            fields['_line'] = line_no
            tile_events.append(fields)
        elif 'METAL_WHOLE_LIGHT_VISIBILITY status=' in line:
            fields = parse_fields(line)
            fields['_line'] = line_no
            group_events.append(fields)
        elif 'METAL_WHOLE_LIGHT_VIS status=' in line:
            fields = parse_fields(line)
            fields['_line'] = line_no
            disabled_events.append(fields)
        if 'METAL_WHOLE_LIGHT_VISIBILITY status=SKIPPED reason=12-acquisition-cap' in line:
            cap_events.append({'line': line_no, 'text': line})

    env_path = out / 'environment.json'
    env = json.loads(env_path.read_text(encoding='utf-8')) if env_path.exists() else {}
    exit_path = out / 'exit-status.txt'
    game_exit = int(exit_path.read_text().strip()) if exit_path.exists() else None
    timeout_path = out / 'timeout-flag.txt'
    timed_out = timeout_path.exists() and timeout_path.read_text().strip() == '1'
    post_hash_path = out / 'post-run-app-sha256.txt'
    post_hash = post_hash_path.read_text(encoding='utf-8').split()[0] if post_hash_path.exists() else None
    expected_hash = env.get('RESCUE_EXPECTED_APP_SHA256')
    # RESCUE_EXPECTED_APP_SHA256 is a runner input rather than a renderer flag;
    # if not in the filtered environment receipt, read the recorded input hash.
    if not expected_hash:
        for row in (out / 'input-sha256.txt').read_text(encoding='utf-8', errors='replace').splitlines():
            if 'candidate-v6/Anomaly.app/Contents/MacOS/AnomalyDX11' in row:
                expected_hash = row.split()[0]
                break

    complete_groups = []
    incomplete_groups = []
    zero_groups = []
    nonzero_groups = []
    face_status_counts = Counter(f.get('status', 'missing') for f in face_events)
    group_keys = []
    for group in group_events:
        kind = group.get('group_kind', 'unknown')
        group_id = int_field(group, 'group_id')
        group_keys.append((int_field(group, 'engine_frame'), int_field(group, 'submitted_serial'),
            int_field(group, 'acquire_epoch'), kind, group_id))
    group_key_counts = Counter(group_keys)
    for group in group_events:
        frame = int_field(group, 'engine_frame')
        serial = int_field(group, 'submitted_serial')
        epoch = int_field(group, 'acquire_epoch')
        group_id = int_field(group, 'group_id')
        expected_faces = int_field(group, 'expected_faces')
        kind = group.get('group_kind', 'unknown')
        receipt_key = (frame, serial, epoch, kind, group_id)
        group_metadata_ok = frame is not None and serial is not None and serial > 0 and \
            epoch is not None and epoch > 0 and group_id is not None and group_id > 0 and \
            group_key_counts[receipt_key] == 1 and kind in ('single', 'omni-parent')
        matched = []
        for face in face_events:
            if face.get('engine_frame') != frame or face.get('submitted_serial') != serial or face.get('acquire_epoch') != epoch:
                continue
            if face.get('group_kind') != kind or face.get('expected_faces') != expected_faces:
                continue
            if kind == 'omni-parent':
                if face.get('parent_light_id') == group_id:
                    matched.append(face)
            elif face.get('light_id') == group_id:
                matched.append(face)
        face_numbers = [f.get('face') for f in matched]
        if kind == 'omni-parent':
            expected_set = set(range(6))
            face_set = set(face_numbers)
            shape_ok = expected_faces == 6 and len(matched) == 6 and face_set == expected_set and \
                all(face.get('parent_light_id') == group_id for face in matched)
        else:
            shape_ok = kind == 'single' and expected_faces == 1 and len(matched) == 1 and \
                face_numbers == [0] and matched[0].get('light_id') == group_id
        face_identity_ok = len({f.get('light_id') for f in matched}) == len(matched) and all(
            all(isinstance(f.get(k), int) for k in ('engine_frame', 'submitted_serial', 'acquire_epoch',
                'batch_id', 'light_id', 'parent_light_id', 'face', 'expected_faces')) and
            f.get('batch_id', -1) >= 0 for f in matched)
        all_face_valid = shape_ok and face_identity_ok and all(
            f.get('status') == 'VALID' and isinstance(f.get('deferred_receiver_samples'), int) and
            f.get('deferred_receiver_samples') >= 0 for f in matched)
        group_count = int_field(group, 'deferred_receiver_samples')
        zero_flag = group.get('zero_deferred_receiver_samples')
        per_face_sum = sum(f['deferred_receiver_samples'] for f in matched
            if isinstance(f.get('deferred_receiver_samples'), int) and f['deferred_receiver_samples'] >= 0)
        count_consistent = group_count is not None and group_count >= 0 and \
            len(matched) > 0 and per_face_sum == group_count and (
                (zero_flag == 'true' and group_count == 0) or
                (zero_flag == 'false' and group_count > 0))
        tile_matches = []
        tile_unique = True
        for face in matched:
            matches = [tile for tile in tile_events if tile_key(tile) == tile_key(face)]
            if len(matches) != 1:
                tile_unique = False
            tile_matches.extend(matches)
        tile_counters_valid = all(has_nonnegative_integer_fields(tile, TILE_COUNTER_FIELDS)
            for tile in tile_matches)
        expected_tile_kind = 1 if kind == 'omni-parent' else 0 if kind == 'single' else None
        tile_complete = len(matched) > 0 and tile_unique and len(tile_matches) == len(matched) and \
            tile_counters_valid and expected_tile_kind is not None and all(
                tile.get('kind') == expected_tile_kind for tile in tile_matches)
        query_complete = group.get('status') == 'complete' and group_metadata_ok and \
            all_face_valid and count_consistent and tile_complete
        item = {
            'line': group.get('_line'), 'frame': frame, 'submitted_serial': serial,
            'acquire_epoch': epoch, 'group_kind': kind, 'group_id': group_id,
            'status': group.get('status'), 'expected_faces': expected_faces,
            'deferred_receiver_samples': group_count,
            'zero_deferred_receiver_samples': zero_flag,
            'per_face_sum_matches_group_count': count_consistent,
            'face_records': [{k: f.get(k) for k in ('status', 'batch_id', 'light_id', 'parent_light_id', 'face', 'deferred_receiver_samples', '_line')} for f in matched],
            'tile_counters_complete': tile_complete,
            'selected_tile_counters': [{k: t.get(k) for k in ('engine_frame', 'batch_id', 'light_id', 'parent_light_id', 'face', 'static_rows', 'static_encoded_draws', 'dynamic_submitted_draws', 'static_transform_bytes', '_line')} for t in tile_matches],
            'classification': 'incomplete_or_inconclusive'
        }
        if query_complete:
            item['classification'] = 'zero_deferred_receiver_samples' if group_count == 0 else 'nonzero_deferred_receiver_samples'
            complete_groups.append(item)
            (zero_groups if item['classification'] == 'zero_deferred_receiver_samples' else nonzero_groups).append(item)
        else:
            incomplete_groups.append(item)

    completion_count_by_frame = Counter(int_field(g, 'engine_frame') for g in group_events
        if g.get('status') in ('complete', 'invalid') and int_field(g, 'engine_frame') is not None)
    selected_without_completion = []
    for selection in selection_frames:
        selected = int_field(selection, 'selected_groups') or 0
        frame = int_field(selection, 'frame')
        outcomes = completion_count_by_frame.get(frame, 0)
        if selected > outcomes:
            selected_without_completion.append({'frame': frame, 'selected_groups': selected, 'completed_or_invalid_group_records': outcomes})

    start_markers = marker_counts.get('observer_window_start', 0)
    end_markers = marker_counts.get('observer_window_end', 0)
    drain_markers = marker_counts.get('observer_drain_complete', 0)
    failed_markers = [m for m in markers if m.startswith('FAILED_')]
    selected_frame_count = len({int_field(s, 'frame') for s in selection_frames if int_field(s, 'selected_groups') and int_field(s, 'frame') is not None})
    invalid_groups = sum(g.get('status') == 'invalid' for g in group_events)
    acquire_epochs = sorted({int_field(g, 'acquire_epoch') for g in group_events if int_field(g, 'acquire_epoch') is not None})
    selection_cap_violations = []
    for s in selection_frames:
        groups = int_field(s, 'selected_groups') or 0
        records = int_field(s, 'selected_records') or 0
        if groups > 8 or records > 48:
            selection_cap_violations.append({'line': s.get('_line'), 'frame': int_field(s, 'frame'), 'groups': groups, 'records': records})

    base_integrity = game_exit == 0 and not timed_out and start_markers == 1 and end_markers == 1 and drain_markers == 1 and not failed_markers and post_hash == expected_hash
    observer_mode = 'no_eligible_groups' if not any((int_field(s, 'selected_groups') or 0) for s in selection_frames) else 'observations_recorded'
    if cap_events or selection_cap_violations or selected_without_completion or invalid_groups or incomplete_groups:
        data_status = 'incomplete_or_inconclusive'
    elif not complete_groups:
        data_status = 'incomplete_or_inconclusive' if observer_mode == 'observations_recorded' else 'no_eligible_groups'
    else:
        data_status = 'complete_observations'

    summary = {
        'schema': 'whole-light-visibility-observer/v1',
        'execution_integrity': 'pass' if base_integrity else 'fail',
        'game_exit_status': game_exit,
        'timeout': timed_out,
        'post_run_candidate_hash_matches_preflight': bool(post_hash and post_hash == expected_hash),
        'markers': dict(marker_counts),
        'failed_markers': failed_markers,
        'observation_status': data_status,
        'observer_mode': observer_mode,
        'selection_frame_count': len(selection_frames),
        'selected_acquisition_epochs_in_receipts': acquire_epochs,
        'selection_frames': [{k: v for k, v in s.items() if k != '_line'} | {'line': s.get('_line')} for s in selection_frames],
        'complete_group_count': len(complete_groups),
        'zero_deferred_receiver_sample_group_count': len(zero_groups),
        'nonzero_deferred_receiver_sample_group_count': len(nonzero_groups),
        'incomplete_group_count': len(incomplete_groups),
        'selected_without_completion': selected_without_completion,
        'selection_cap_violations': selection_cap_violations,
        'backend_cap_skip_events': cap_events,
        'backend_disabled_or_config_events': disabled_events,
        'face_status_counts': dict(face_status_counts),
        'groups': complete_groups + incomplete_groups,
        'interpretation_limit': 'A complete zero count is zero deferred receiver samples only; it does not establish invisibility, absent other consumers, or permission to cull.',
        'no_fps_visual_or_motion_claim': True
    }
    (out / 'observer-summary.json').write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    print('execution_integrity=' + summary['execution_integrity'])
    print('observation_status=' + data_status)
    print(f"selection_frames={len(selection_frames)} groups_complete={len(complete_groups)} zero={len(zero_groups)} nonzero={len(nonzero_groups)} incomplete={len(incomplete_groups)}")
    if selected_without_completion:
        print('selected_without_completion=' + json.dumps(selected_without_completion, sort_keys=True))
    # A data-incomplete observer is still a valid preserved run receipt; only
    # execution integrity controls the shell's exit status.
    return 0 if base_integrity else 1


if __name__ == '__main__':
    if len(sys.argv) != 2:
        raise SystemExit('usage: summarize_observation.py OUTPUT_DIRECTORY')
    raise SystemExit(main(Path(sys.argv[1])))
