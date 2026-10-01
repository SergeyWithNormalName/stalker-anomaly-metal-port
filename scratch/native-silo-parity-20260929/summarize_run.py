#!/usr/bin/env python3
"""Summarize a short parity run; callback and on-glass metrics stay separate."""
import argparse
import csv
import datetime
import json
import math
from pathlib import Path


def distribution(values):
    values = sorted(v for v in values if math.isfinite(v) and v >= 0)
    if not values:
        return None
    def pct(p):
        x = (len(values) - 1) * p
        i = int(x)
        return values[i] + (values[min(i + 1, len(values) - 1)] - values[i]) * (x - i)
    worst = values[-max(1, math.ceil(len(values) * 0.01)):]
    return dict(count=len(values), p50=pct(.5), p95=pct(.95), p99=pct(.99),
                maximum=values[-1], one_percent_low_fps=1000 / (sum(worst) / len(worst))
                if sum(worst) > 0 else None)


def epoch(value):
    return datetime.datetime.fromisoformat(value.replace('Z', '+00:00')).timestamp()


def summarize(out):
    with (out / 'phases.csv.device.csv').open() as stream:
        device = list(csv.DictReader(stream))
    measured = [r for r in device if int(r['phase']) == 1]
    if len(measured) < 20:
        raise ValueError('measurement did not produce at least 20 callbacks')
    # os.time has one-second granularity. Trim both edges before comparing with
    # runtime timestamps rather than assigning false subsecond precision.
    start = int(measured[0]['epoch_s']) + 2
    end = int(measured[-1]['epoch_s']) - 1
    selected = [r for r in measured if start <= int(r['epoch_s']) < end]
    frames = [int(r['engine_frame']) for r in selected]
    wall = [int(r['wall_ms']) for r in selected]
    if len(selected) < 20 or end <= start or any(b <= a for a, b in zip(wall, wall[1:])):
        raise ValueError('measurement timestamps are short or nonmonotonic')
    result = dict(metric_scope='Short fixed-camera diagnostic; not whole-game acceptance.',
                  selection_epoch=dict(start=start, end=end),
                  callback=dict(samples=len(selected),
                                mean_fps=1000 * (len(wall) - 1) / (wall[-1] - wall[0]),
                                intervals_ms=distribution([b-a for a,b in zip(wall,wall[1:])]),
                                frame_gap_count=sum(b-a != 1 for a,b in zip(frames,frames[1:]))),
                  conditions={k: sorted({r[k] for r in selected}) for k in
                              ('camera_x','camera_y','camera_z','dir_x','dir_y','dir_z',
                               'device_time_factor','game_time_factor','width','height','level','weather','calendar')})
    pid = int((out / 'owned-pid.txt').read_text())
    timeline = out / 'metal-timeline.json'
    if timeline.exists():
        layers = [l for p in json.loads(timeline.read_text()) if int(p.get('PID',-1)) == pid
                  for l in p.get('Layers', [])]
        admitted = []
        configs = []
        for layer in layers:
            configs.extend(layer.get('Configuration Timeline',[]))
            rows = [r for r in layer.get('On-Glass Drawable Timeline', [])
                    if epoch(r['Start Date']) >= start and epoch(r['End Date']) <= end
                    and r.get('Duration MCT Seconds',0) > 0]
            if rows:
                admitted.append(rows)
        if len(admitted) != 1:
            result['displayed'] = dict(valid=False, reason='expected one measured game layer', layers=len(admitted))
        else:
            rows = sorted(admitted[0], key=lambda r: epoch(r['Start Date']))
            durations = [r['Duration MCT Seconds'] for r in rows]
            span = epoch(rows[-1]['End Date']) - epoch(rows[0]['Start Date'])
            gaps = [epoch(b['Start Date']) - epoch(a['End Date']) for a,b in zip(rows,rows[1:])]
            infos = [r['In-Flight Drawable Info'] for r in rows]
            flags = sorted({(i.get('MetalFX Enabled'),i.get('MetalFX Frame Interpolator Enabled'),
                             i.get('MetalFX Input Width'),i.get('MetalFX Input Height'),
                             i.get('MetalFX Target Width'),i.get('MetalFX Target Height')) for i in infos},key=str)
            valid = len(rows) >= 20 and max(gaps,default=0) <= .003
            result['displayed'] = dict(valid=valid, samples=len(rows),
                                       mean_fps=len(rows)/sum(durations),
                                       intervals_ms=distribution([d*1000 for d in durations]),
                                       coverage_seconds=sum(durations), span_seconds=span,
                                       maximum_timeline_gap_ms=max(gaps,default=0)*1000,
                                       metalfx_flags=flags, configurations=configs)
    else:
        result['displayed'] = dict(valid=False, reason='no Metal runtime timeline')
    e01 = out / 'e01-frames.csv'
    if e01.exists():
        admitted_frames = set(frames)
        with e01.open() as stream:
            rows = [r for r in csv.DictReader(stream)
                    if int(r['engine_frame']) in admitted_frames and r['status'] == 'completed']
        result['native_telemetry'] = {k: distribution([float(r[k]) for r in rows]) for k in
                                     ('encode_ms','scene_ms','dynamic_prewarm_ms','wait_ms',
                                      'drawable_ms','updated_bytes','indexed_draws','pso_binds')}
        for k,v in result['native_telemetry'].items():
            if v:
                v.pop('one_percent_low_fps', None)
        result['native_telemetry']['gpu_command_buffer_elapsed_ms'] = distribution(
            [(float(r['gpu_end_s'])-float(r['gpu_start_s']))*1000 for r in rows
             if float(r['gpu_end_s']) > float(r['gpu_start_s']) > 0])
        if result['native_telemetry']['gpu_command_buffer_elapsed_ms']:
            result['native_telemetry']['gpu_command_buffer_elapsed_ms'].pop('one_percent_low_fps', None)
        result['native_telemetry']['note'] = 'Nested CPU scopes must not be summed; GPU elapsed overlaps CPU work.'
    return result


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output',type=Path)
    args = parser.parse_args()
    result = summarize(args.output)
    (args.output/'summary.json').write_text(json.dumps(result,indent=2)+'\n')
    concise = {k:v for k,v in result.items() if k != 'native_telemetry'}
    if 'displayed' in concise:
        concise['displayed'] = {k:v for k,v in concise['displayed'].items() if k != 'configurations'}
    print(json.dumps(concise,indent=2))
