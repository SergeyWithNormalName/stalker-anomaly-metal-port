# E01 — timings and diagnostic budget (2026-09-23)

## Scope and admission

E00 is closed. This stage measures Direct Metal on the unchanged fixture contracts
S0, S2, S4, S5 at 1920×1080. Each baseline fixture has three accepted runs with
60 s warm-up and a 120 s sample. All 12 runs exited 0, loaded the expected
save/level/weather/camera contract, and used binary SHA-256
`46b21ffa62681c18fb6a19df53643b4d69624ccb6556b4c965befb5d734e9e36`.
The aggregator rejects mismatched provenance, fixture state, sample duration, or
run count. Performance runs disabled `rs_stats`, capture, and diagnostic Metal
environment variables. E00 captures and ordinary user profiles were not changed.

The inspectable outputs are [`baseline.json`](../../scratch/release_runs/e01-aggregate-20260923/baseline.json),
[`frames.csv`](../../scratch/release_runs/e01-aggregate-20260923/frames.csv),
[`cpu_top.md`](../../scratch/release_runs/e01-aggregate-20260923/cpu_top.md),
[`gpu_passes.csv`](../../scratch/release_runs/e01-aggregate-20260923/gpu_passes.csv),
and [`budget.md`](../../scratch/release_runs/e01-aggregate-20260923/budget.md).
The combined `frames.csv` retains all raw frame rows with a fixture/run identity;
`timing-window.json` in each run marks which rows enter the 120 s statistics.
The three repetitions of each fixture are under `scratch/release_runs/` with
prefix `e01-base2-20260923-` and suffixes S0/S2/S4/S5 `r1`, `r2`, `r3`
(S4 uses `r2b`, S5 uses `r3b` after invalid teardown attempts).

## Measurements

| Fixture | Display interval p50 | p99 | 1% low | CPU scene p50 | GPU frame p50 |
|---|---:|---:|---:|---:|---:|
| S0 | 25.000 ms | 33.333 ms | 27.222 FPS | 20.960 ms | 14.458 ms |
| S2 | 75.000 ms | 116.667 ms | 8.188 FPS | 73.514 ms | 25.618 ms |
| S4 | 91.667 ms | 166.666 ms | 4.457 FPS | 90.295 ms | 39.608 ms |
| S5 | 175.000 ms | 249.333 ms | 3.855 FPS | 165.854 ms | 14.914 ms |

These are medians of three run-level values. CPU and GPU intervals execute
asynchronously; they cannot be added. The 60 FPS release budget **fails in every
fixture**. Display callbacks returned zero `presentedTime` for 1–3 frames in
some S4/S5 runs; those individual pacing gates remain **OPEN / partial**. Their
positive displayed intervals still establish severe failure, but do not provide
a complete on-screen pacing record. The runner records submission/completion
serials, engine and camera frame IDs, queue depth, GPU timestamp pairs, and
actual drawable presentation timestamps separately from callback arrival.

S2 CPU scene time increases in the 0/1/4/8 NPC timeline; S4 increases during
the scripted outdoor traversal as dynamic renderables and indexed draws grow.
S5 has the highest CPU scene cost and run-to-run variation. A separate early and
late S5 CPU sample repeatedly reaches particle `StageFrame` buffer creation.
This is a strong E08 candidate, after correctness and resource ownership are
addressed; E01 makes no speedup claim. See `cpu_top.md` for the evidence limits.

The separate [`Metal System Trace`](../../scratch/release_runs/e01-profile-s5-20260923/metal-system.trace)
provided 448 labelled GPU intervals across only five frames. The per-pass CSV
reports union of each pass's intervals within a frame, never their sum. In this
narrow trace, median activity is 2.364 ms for local static shadow atlas tiles,
2.073 ms for G-buffer, 1.840 ms for SSFX AO temporal base, 1.740 ms for
combine_1, and 1.722/1.639 ms for far/near shadow maps. Five traced frames are
insufficient for a global GPU ranking. The trace is intrusive and excluded from
the baseline. The full-frame GPU timestamp pairs above remain independent.

## Instrumentation and limits

CPU monotonic stages now cover frame acquisition/wait, drawable acquisition,
scene build/render, grass staging, dynamic prewarm, encoding, presentation
encoding, and commit. Nested scene/encode/grass/prewarm times are marked as
such in the parser. Per-frame counters include bytes updated, buffer/texture
allocations, indexed draws, and PSO binds. The Metal Trace provides labelled
passes for scene, shadows, local light, AO/IL, water, rain, combine, HUD, and
presentation where active. The frame timestamp pair is a GPU duration, not CPU
encoding time.

`pso_binds` counts bind calls, not distinct PSO switches; `indexed_draws`
excludes non-indexed draws such as final presentation. Existing dynamic-world
cache and skinning counters are emitted in periodic engine logs, not joined to
the E01 per-frame CSV, so they cannot be ranked as per-frame costs here.

The CSV does **not** separately time input/update, AI, physics, scripts,
skeletons, CPU skinning, or individual uploads, and it does not count every
requested vertex/NPC/caster/cache/readback/compilation/residency dimension.
`scene_other_ms` is unattributed. These gaps remain **OPEN**; no nonexistent
measurement is inferred from the trace or CPU sample.
The early-frame timing seams are `CInput::OnFrame`, `CRenderDevice::FrameMove`
and `CSheduler::Update`; source-local seams include `CKinematics::CalculateBones`,
the DynamicWorld vertex repack, and backend `UpdateBuffer`/`UploadTexture2D`.
The current `updated_bytes` counts only `UpdateBuffer`, and allocation counters
exclude internal native staging buffers. Accurate parallel AI/script costs,
whole-step physics, and per-cascade casters need wider thread-safe hooks.
New hooks would change the measured binary, so their data cannot be spliced
into this baseline or the current overhead comparison.

`CTimerBase::GetElapsed_ticks()` in `src/xrCore/FTimer.h` subtracts overhead
from an unsigned tick count, which can underflow for a very short interval.
`CStatTimer::End()` also has no explicit paired-Begin guard, while its constructor
does initialize `accum/result/count`. These are static risks relevant to giant
HUD statistics, **not a verified root cause**. E01 leaves physical timestep and
stats behavior untouched.

Several discarded pre-fix runs aborted at teardown. Crash reports placed the
faulting freeze-monitor thread in `xrLogger::FlushLog` after logger teardown.
An Apple-only lifecycle repair now joins that thread before core destruction;
the normal monitoring cadence remains 500 ms. A complete S5 60+120 s rerun
on new binary SHA-256
`bdc80f37900264bd28b711ea5cde7331c6da621d6dec36d806d234fb97474b58`
exited 0, with 150.000 ms displayed p50 and 146.841 ms CPU scene p50.
It is a reliability/performance sanity run, **not** merged into the 12-run
baseline from the earlier SHA. Its presentation record is OPEN / partial because
four callbacks had zero displayed timestamp; see its
[`baseline.json`](../../scratch/release_runs/e01-post-freeze-s5-20260923/baseline.json).

## Telemetry overhead

Six additional S5 runs used that same post-repair binary, fixture/profile/save,
1920×1080 drawable, 60 s warm-up and 30 s sample. All exited 0. Throughput is
the engine-frame delta divided by 30 s; the telemetry-off runs have no display
timestamps, so this comparison makes no on-screen pacing claim. The exact
per-run provenance and calculation are in
[`overhead.json`](../../scratch/release_runs/e01-aggregate-20260923/overhead.json).

| Mode | Three sample rates | Median | Relative run span |
|---|---|---:|---:|
| Telemetry on | 7.767, 6.833, 8.200 FPS | 7.767 FPS | 17.60% |
| Telemetry off | 7.100, 8.000, 7.267 FPS | 7.267 FPS | 12.39% |

The median point estimate implies −6.88% slowdown (instrumented was faster),
which is not a credible speedup: both groups vary much more than the 2% gate.
**Overhead ≤2% is OPEN / unproven.** Treat this collection as potentially
intrusive for precise performance comparisons. The five-frame Metal Trace and
CPU samples are separately intrusive diagnostics and were never mixed into
these six runs or the 12-run baseline.

## Stage gate

The measured budget is diagnostic and fails the 60 FPS goal. The baseline is
repeatable enough to order investigation: S5 particle buffer creation and scene
build first, then S4 outdoor scene and S2 NPC scene. Per-pass GPU work remains
secondary evidence with low five-frame confidence. E01 does not establish
visual parity (Windows reference A remains unavailable) or a finished release.
