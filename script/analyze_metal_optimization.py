#!/usr/bin/env python3
"""Bounded optimization diagnostics. Never promotes short/battery runs to release proof."""
from __future__ import annotations
import argparse
import csv
import json
import math
from pathlib import Path
import statistics
from analyze_metal_e08 import summarize_run


def distribution(values):
    values = sorted(values)
    if not values:
        raise ValueError('no admitted samples')
    def percentile(q):
        index = (len(values) - 1) * q
        a = int(index)
        b = min(a + 1, len(values) - 1)
        return values[a] + (values[b] - values[a]) * (index - a)
    return {'count': len(values), 'p50': statistics.median(values),
            'p95': percentile(.95), 'p99': percentile(.99)}


def binding_summary(profile: Path, run: Path, settle: float):
    result = summarize_run(run, settle)
    if result['engine_exit_status'] != 0 or result['scenario_result'] != 'OK':
        raise ValueError('failed/incomplete run is not optimization evidence')
    with (run / 'frames.csv').open() as stream:
        frames = [r for r in csv.DictReader(stream) if r['status'] == 'completed']
    window = result['original_window']
    frames = [r for r in frames if window['start_engine_frame'] <= int(r['engine_frame']) <= window['end_engine_frame']]
    start = min(float(r['begin_ns']) for r in frames) + settle * 1e9
    serials = {r['serial'] for r in frames if float(r['begin_ns']) >= start and int(r['indexed_draws']) > 14000}
    with profile.open() as stream:
        rows = [r for r in csv.DictReader(stream) if r['status'] == 'completed' and r['serial'] in serials]
    fields = ['bind_ms', 'prepare_ms', 'snapshot_ms', 'declaration_ms', 'retain_ms',
              'tlab_alloc_ms', 'fill_ms', 'encoder_bind_ms', 'calls', 'snapshot_bytes', 'entries']
    buckets = {}
    for field in fields:
        values = [float(r[field]) for r in rows]
        if any(not math.isfinite(v) or v < 0 for v in values):
            raise ValueError(f'invalid {field}')
        buckets[field] = distribution(values)
    residual = [float(r['prepare_ms']) - sum(float(r[f]) for f in ['snapshot_ms', 'retain_ms']) for r in rows]
    # declaration_ms also contains the final TLAB declaration, outside prepare_ms.
    # Do not subtract it or add nested bucket medians into a fabricated frame budget.
    return {'schema': 'direct-metal-optimization-bindings/v1', 'run': result,
            'buckets': buckets, 'prepare_minus_snapshot_retain_ms': distribution(residual),
            'note': 'Sampled CPU diagnostics with clock overhead; declaration includes final TLAB use. '
                    'Buckets overlap. >14000 actual indexed draws selects heavy S4 camera. '
                    'Short/battery/instrumented runs are not production or native60 acceptance.'}


def compare_runs(paths, settle):
    runs = [summarize_run(p, settle) for p in paths]
    for run in runs:
        if run['engine_exit_status'] != 0 or run['scenario_result'] != 'OK':
            raise ValueError(f"failed run {run['run']}")
    fields = ['binary_sha256', 'shader_receipt_set_sha256', 'save_sha256', 'engine_extent', 'fixture']
    for field in fields:
        if not runs[0][field] or any(r[field] != runs[0][field] for r in runs):
            raise ValueError(f'incomparable {field}')
    provenance = [json.loads((p / 'provenance.json').read_text()) for p in paths]
    configs = [p['profile']['user_ltx_sha256'] for p in provenance]
    if not configs[0] or len(set(configs)) != 1:
        raise ValueError('incomparable user configuration')
    if all('effective_env' in p for p in provenance):
        ignored = {'XRAY_METAL_OPT_SHADOW_WORKERS', 'XRAY_METAL_E01_FRAMES', 'XRAY_METAL_E01_WINDOW_PATH'}
        settings = [{k: v for k, v in p['effective_env'].items()
                     if k.startswith('XRAY_METAL') and k not in ignored} for p in provenance]
        if any(s != settings[0] for s in settings):
            raise ValueError('incomparable Metal environment or instrumentation')
    return {'schema': 'direct-metal-optimization-comparison/v1', 'runs': runs,
            'note': 'Matched binary/shaders/save/extent/fixture/config. Review ordering, power, thermal '
                    'conditions and completeness separately. No automatic performance PASS.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('runs', nargs='+', type=Path)
    parser.add_argument('--bindings', type=Path)
    parser.add_argument('--settle', type=float, default=30)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    if not math.isfinite(args.settle) or args.settle < 0:
        parser.error('settle must be finite and nonnegative')
    if args.bindings:
        if len(args.runs) != 1:
            parser.error('--bindings requires one run')
        result = binding_summary(args.bindings, args.runs[0], args.settle)
    else:
        result = compare_runs(args.runs, args.settle)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    print(args.output)

if __name__ == '__main__':
    main()
