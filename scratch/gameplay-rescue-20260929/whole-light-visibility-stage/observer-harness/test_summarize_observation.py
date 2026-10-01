#!/usr/bin/env python3
"""Offline synthetic acceptance cases for summarize_observation.py."""
import importlib.util
import json
import tempfile
from pathlib import Path


SCRIPT = Path(__file__).with_name('summarize_observation.py')
SPEC = importlib.util.spec_from_file_location('summarize_observation', SCRIPT)
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


def make_group(frame, serial, epoch, kind, group_id, expected_faces, count, zero, faces, tiles):
    lines = [
        'METAL_WHOLE_LIGHT_VISIBILITY status=complete '
        f'submitted_serial={serial} acquire_epoch={epoch} engine_frame={frame} '
        f'group_kind={kind} group_id={group_id} expected_faces={expected_faces} '
        f'deferred_receiver_samples={count} zero_deferred_receiver_samples={zero}'
    ]
    for face, light_id, batch_id, face_count in faces:
        parent = group_id if kind == 'omni-parent' else 0
        lines.append(
            'METAL_WHOLE_LIGHT_VISIBILITY_FACE status=VALID '
            f'submitted_serial={serial} acquire_epoch={epoch} engine_frame={frame} '
            f'batch_id={batch_id} light_id={light_id} parent_light_id={parent} '
            f'face={face} group_kind={kind} expected_faces={expected_faces} '
            f'deferred_receiver_samples={face_count}'
        )
        if (face, light_id, batch_id) not in tiles.get('missing', set()):
            lines.append(
                'METAL_WHOLE_LIGHT_VIS_TILE '
                f'frame={frame} batch={batch_id} light={light_id} parent={parent} face={face} '
                f'kind={1 if kind == "omni-parent" else 0} static_rows=2 '
                'static_encoded_draws=1 dynamic_submitted_draws=3 static_transform_bytes=512'
            )
    return lines


def main():
    log_lines = []
    scenarios = []

    zero_faces = [(face, 101 + face, face // 3, 0) for face in range(6)]
    scenarios.append(make_group(300, 41, 900, 'omni-parent', 500, 6, 0, 'true', zero_faces, {}))

    scenarios.append(make_group(301, 42, 901, 'single', 600, 1, 7, 'false',
        [(0, 600, 0, 7)], {}))

    missing_face = [(face, 701 + face, face // 3, 0) for face in range(5)]
    scenarios.append(make_group(302, 43, 902, 'omni-parent', 700, 6, 0, 'true', missing_face, {}))

    duplicate_face = [(face, 801 + index, index // 3, 0)
        for index, face in enumerate((0, 1, 2, 3, 4, 4))]
    scenarios.append(make_group(303, 44, 903, 'omni-parent', 800, 6, 0, 'true', duplicate_face, {}))

    missing_tile_faces = [(0, 900, 0, 0)]
    scenarios.append(make_group(304, 45, 904, 'single', 900, 1, 0, 'true',
        missing_tile_faces, {'missing': {(0, 900, 0)}}))

    for frame, records in zip(range(300, 305), scenarios):
        log_lines.append(
            'METAL_WHOLE_LIGHT_VIS_FRAME status=selected '
            f'frame={frame} planned_groups=1 eligible_groups=1 selected_groups=1 '
            f'selected_records={6 if frame in (300, 302, 303) else 1} omitted_quota=0 '
            'incomplete_or_collision=0 hud_exempt_records=0 volumetric_exempt_records=0 '
            'invalid_exempt_records=0'
        )
        log_lines.extend(records)

    with tempfile.TemporaryDirectory(prefix='whole-light-parser-') as tmp:
        out = Path(tmp)
        (out / 'engine.log').write_bytes(('\n'.join(log_lines) + '\n').encode('utf-8'))
        (out / 'observer-markers.csv').write_text(
            'observer_window_start,frame=300\nobserver_window_end,frame=304\n'
            'observer_drain_complete,frame=304\n', encoding='utf-8')
        (out / 'environment.json').write_text(
            json.dumps({'RESCUE_EXPECTED_APP_SHA256': 'fixture-hash'}), encoding='utf-8')
        (out / 'exit-status.txt').write_text('0\n', encoding='ascii')
        (out / 'timeout-flag.txt').write_text('0\n', encoding='ascii')
        (out / 'post-run-app-sha256.txt').write_text('fixture-hash /fixture/app\n', encoding='ascii')

        assert MODULE.main(out) == 0
        summary = json.loads((out / 'observer-summary.json').read_text(encoding='utf-8'))
        assert summary['execution_integrity'] == 'pass'
        assert summary['observation_status'] == 'incomplete_or_inconclusive'
        assert summary['zero_deferred_receiver_sample_group_count'] == 1, json.dumps(summary['groups'], indent=2)
        assert summary['nonzero_deferred_receiver_sample_group_count'] == 1
        assert summary['incomplete_group_count'] == 3
        by_frame = {group['frame']: group for group in summary['groups']}
        assert by_frame[300]['classification'] == 'zero_deferred_receiver_samples'
        assert by_frame[300]['tile_counters_complete'] is True
        assert by_frame[301]['classification'] == 'nonzero_deferred_receiver_samples'
        assert by_frame[302]['classification'] == 'incomplete_or_inconclusive'
        assert by_frame[303]['classification'] == 'incomplete_or_inconclusive'
        assert by_frame[304]['classification'] == 'incomplete_or_inconclusive'
        assert by_frame[304]['tile_counters_complete'] is False

    print('OBSERVER_PARSER_FIXTURES PASS zero nonzero missing-face duplicate-face missing-tile')


if __name__ == '__main__':
    main()
