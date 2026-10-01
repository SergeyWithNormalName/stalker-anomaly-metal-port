# V6 whole-light visibility observer result

This is one numeric-only fixed-camera observer run. It records whole-light receiver samples and matched work counters; it is not a culling result, a presentation/visual check, or a performance/FPS measurement.

## Run integrity

- Candidate executable: SHA-256 `238db09155f62caf726f53917cf82876aa12b06ee60c8ea03d157b24731c26dd`, same before/after.
- Game exit 0; watchdog timeout 0; parser exit 0; execution integrity `pass`.
- Required markers appeared exactly once: loaded state, window start, window end, drain complete. No failed marker, selection cap violation, backend cap skip, or backend disabled/config event.
- Selected acquisition epochs/frames: 300–311 (12). All 12 emitted selected records and completed observations.
- Original app, V4, and V5 executable hashes matched their before-run receipts after the run; see `preserved-app-hashes-before.txt` and `preserved-app-hashes-after.txt`.
- The copied runtime used the canonical Cordon control save. Camera stayed at `(-221.082565, -18.284866, -168.182037)`, facing +Z; device time factor 1 and world factor 6. The observer markers record `w_clear2` and calendar time 08:13 during the sampled window. Preserve those actual values; no claim is made that the requested weather/calendar pin was achieved.

## Observations

- 96 complete group observations: 88 with zero deferred receiver samples and 8 with nonzero samples; 0 incomplete groups. All 516 face records were `VALID`; all groups' face sums matched their reported group sample count, and all selected groups had complete matched tile counters.
- Kinds: 84 omni-parent observations and 12 single-light observations. This corresponds to 46 distinct omni parent IDs and 6 distinct single-light IDs (52 unique IDs total).
- Repeat coverage: 38 omni IDs occurred twice and 8 once; all 6 single-light IDs occurred twice. Thus 44 IDs were observed twice and 8 once. No repeated ID changed between zero/nonzero classification.
- The 8 nonzero observations belonged to 4 omni parents, each observed twice: IDs `514747572224` (1,893 samples each time), `514747577344` (1,862 each), `515698683904` (1,890 each), and `515698689024` (1,893 each). No single-light group had nonzero samples.

## Matched selected tile work counters

These are sums over the recorded selected face observations only. Repeated parents are counted once per observation; they are not estimates of avoided work, and nothing is inferred about groups omitted by the 8-group/frame observer quota.

| Observed class | Groups | Face observations | `static_rows` | `static_encoded_draws` | `dynamic_submitted_draws` | `static_transform_bytes` |
|---|---:|---:|---:|---:|---:|---:|
| Zero deferred receiver samples | 88 | 468 | 11,016 | 1,713 | 1,393 | 2,820,096 |
| Nonzero deferred receiver samples | 8 | 48 | 2,434 | 456 | 924 | 623,104 |
| Total selected observations | 96 | 516 | 13,450 | 2,169 | 2,317 | 3,443,200 |

Per-frame selection diagnostics and raw evidence are in `observer-summary.json`, `engine.log`, `observer-markers.csv`, and the run receipt files in this directory.
