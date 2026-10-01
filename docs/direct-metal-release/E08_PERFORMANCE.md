# E08 — Direct Metal performance (2026-09-28)

This packet covers only E08. Visual parity is the user’s provisional assumption,
and native 1080p60 remains OPEN. The first accepted core and its original matrix
are retained as a controlled milestone; a further static-shadow merge was then
qualified on S4 and promoted to the normal path.

## Controlled particle result

The four S2 runs use one binary (`c01adb97fd1ae77bba00a862d9f19fb28e133647d94f44b4d1218476fd8167ac`),
one shader receipt set (`5acd3ed64df93c0e5ca94ce12f93372d65197bd948e583789c61743fedab897f`),
and one Garbage save. All exit0, scenario OK, native 1920×1080. A/B/B/A changes
only the particle frame-arena flag. Runs use 60 seconds warm-up and 150 seconds
sampling, excluding the first 30 seconds after scenario start as in E07. No
capture, Metal validation or per-object E07 CSVs run during these measurements.

| Run | CPU encode p50 ms | GPU p50 ms | Displayed p50 ms | Median rate FPS | 1% low FPS | Buffer allocations p50 |
|---|---:|---:|---:|---:|---:|---:|
| control_a | 45.876 | 19.587 | 50.000 | 20.0 | 8.67 | 359 |
| arena_b1 | 27.048 | 18.783 | 33.333 | 30.0 | 21.72 | 11 |
| arena_b2 | 29.218 | 18.796 | 33.333 | 30.0 | 18.35 | 11 |
| control_a2 | 45.801 | 19.671 | 50.000 | 20.0 | 10.55 | 351 |

`control_a` and `arena_b2` each have one missing presentation callback; their
available interval statistics are partial and are not a complete pacing gate.
The complete `arena_b1` and reverse `control_a2` runs average **32.67 and 19.54
actually presented FPS**, respectively. The CPU result is supported by all
four runs; it does not rely on an incomplete callback being treated as displayed.
Median indexed draws remain 10,029–10,128 and uploaded bytes 14.8–15.7 MB; no
geometry, actors, animation, quality, resolution or draw order was reduced.
Requested NPC count 0 means zero added fixture NPCs; background save actors remain.

Raw evidence: `scratch/release_runs/20260928-e08-{control_a,arena_b1,arena_b2,control_a2}/`.
Summary: `scratch/release_runs/20260928-e08-analysis/particle-abba.json`.
AC power and before/after thermal-warning snapshots are retained in `20260928-e08-prestate/`.
No recorded warning is not continuous temperature/frequency telemetry.

## Correctness evidence already completed

- Frame upload/reprojection, private-upload recovery, Flora broad-phase tests,
  and particle split/payload/reuse self-tests: 4/4 PASS (`tests-particle-native.log`).
  Shadow atlas, batch planning and sun cascade source contracts: 3/3 PASS
  (`tests-shadow-core.log`). E01 analyzer tests: 9 PASS (`tests-e01-analyzer.log`).
- Actual converted local-light GPU probe: 200 lights, 16 tiles per batch,
  three camera poses; grouped atlas equals serial reference and legacy separate
  tiles, including per-tile R32 hashes, alpha and six point-face controls.
  API/shader validation enabled. `20260928-e08-gpu-final/` + `gpu-final.log`.
- Static sun culling: six completed full-list/culled-list 2048×2048 R32 map pairs
  are bit-identical, with positive nonempty coverage. Three cascades at frames
  240/480: 8,073 source static rows become 676 (13/174/489), maintaining order.
  `20260928-e08-sun-pixels/e08-pixel-comparison.json`. The native validator’s
  NO_COVERAGE label reflects zero requested off-camera test casters; only
  completed raw map comparison is used as this optimization’s oracle.
- Flora instancing experiment: synthetic trunk/leaf × wind-off/A/B, positive
  alpha controls, per-instance scale/height/rotation/clip/off-camera geometry.
  Legacy/instanced R32 outputs match. Diagnostic raster-depth shader also
  matches and observes trunk wind changes. `20260928-e08-flora-gpu4/` and
  `20260928-e08-flora-depth-gpu1/`. This is not a production-scene pixel oracle
  or a native D32 export comparison; the experiment remains off by default.

## Accepted core and experimental decisions

The normal launch uses a late drawable acquire, the conservative static shadow
BVH, one render encoder per local shadow atlas batch, stack scratch for small
MSC binding tables, static sun-frustum caster selection, particle frame-arena
slices, and order-preserving static local-shadow index merging. No MetalFX/scaler,
resolution, quality, distance, NPC, pose-update or simulation change is part
of this packet. The CPU skinning oracle remains.

The BVH only narrows candidates: the original eight-corner final predicate is
retained, invalid/missing bounds fail open, IDs return in original source order,
and exhaustive diagnostic scans observed zero omissions. It is rebuilt with
scene preparation; no rendered shadow map is cached across changing lights/wind.
Atlas region changes preserve clears/attachments/retention and restore the saved
region through diagnostic pass splits. Three frame slots remain bounded.

Particle staging preserves the exact vertex/index payloads and all record/chunk
ordering. Immutable ranges live until that frame slot completes. Vertex and
index offsets are applied in both color and distortion draws. The 16,383-sprite
chunk limit remains, and discard clears prepared slices without committing state.
Normal launch now uses the arena; `XRAY_METAL_E08_PARTICLE_LEGACY=1` is the control.
The older comparison binary used `XRAY_METAL_E08_PARTICLE_ARENA=1` for B instead.

A late-acquire failure or extent/format mismatch discards the unsubmitted frame
and signals a recoverable submit result instead of losing the device. Accepted
history and submission serial remain unchanged. The original bounded private
upload recovery/stop guard is preserved. Under API/shader validation, a forced
late failure at frame 40 logs one `METAL_FRAME_SUBMIT_RETRY`, followed by continued
rendering and exit0/scenario OK (`20260928-e08-submit-retry-validation`). The
first validation run’s requested frame 321 was not reached; it is not retry proof.

Short isolated opt-in profiles use the same binary, S2, 15 seconds warm-up,
65 seconds sampling and 30 seconds settle; they are diagnostic follow-ups,
not substitutes for the long comparison. All exit0 with complete presentation.

| Experiment | CPU encode p50 ms | Mean displayed FPS | Draws p50 | Decision |
|---|---:|---:|---:|---|
| resource dedup | 29.522 | 29.75 | 10,137 | Off: no material frame benefit |
| private upload encoder batching | 29.045 | 30.31 | 10,140 | Off: no material frame benefit |
| Flora shadow instancing | 27.530 | 32.12 | 8,456 | Off: modest ~5% short-run gain, no median cadence change |
| core repeat | 29.016 | 30.47 | 10,171 | Selected control |

Flora instancing reduces the far-sun CPU pass from ~2.68 to ~1.22 ms, but its
whole-frame saving is only ~1.5 ms and the live scene needs another pixel/order
qualification. It is not enabled by the shared launcher shader environment.
The optional paths/cooker/GPU oracle are retained explicitly for reproduction;
they do not count as accepted production optimizations. Visible-Flora host
vector reuse also stays off; its prior exploratory combined run did not prove
an isolated gain. Raw profiles: `20260928-e08-{resource-profile,upload-profile,
instance-profile,core-repeat-profile}/` and adjacent `*-passes/` directories;
summary `20260928-e08-analysis/optional-profiles.json`.

## Remaining budget and initial S2 ROI assessment

Sampled CPU pass encoding in the core repeat: G-buffer ~7.40 ms; all six local
atlas batches per sampled frame ~6.16 ms; far sun ~2.7 ms. Dynamic preparation is
another ~6–9 ms in ordinary long runs. CPU buckets are nested in pass/scene
intervals; do not add them. GPU frame time remains ~19 ms; observed GPU stage
spans include G-buffer vertex/fragment work and ~2.7 ms AO, may overlap, and must
not be summed. Counter timings are elapsed-time evidence, not occupancy,
bandwidth, cache-miss or Windows parity measurements.

The remaining ordinary candidates were checked rather than inferred from FPS:
there is no reusable current-frame portal/HOM-visible static ID snapshot. Metal
returns before the legacy static capture/HOM render; its world snapshot is a
separate dynamic spatial capture. New static visibility integration or broad
G-buffer/skin submission redesign needs a larger ownership and correctness
contract. MSC’s indexed helper emits two vertex-byte writes; only draw params
can be skipped for validated `needs_draw_params=false`, while the IR index-type
word must remain. Its entire measured indexed-draw bucket is ~2.24 ms, so the
saving is smaller and requires cross-adapter metadata plumbing. No broad rewrite
is accepted based on that ceiling.

The user authorizes stopping when ordinary measured returns cease to be
substantial. S2-only follow-ups first showed declining returns, but the matrix
found a heavier S4 camera: 24,933 draws/frame and 12,135 static local-shadow
caster rows over157 views. Work therefore continued through the independently
qualified static index merge below. The final matrix and final-binary profile
now define the remaining budget. No native60 closure is asserted.

## Reproduction and artifact ownership

Main build:

```sh
cmake --build build --target Anomaly.DX11 XRay.Metal.ParticleAdapterProbe \
  XRay.Metal.FrameUploadGpuProbe XRay.Metal.LocalShadowBatchGpuProbe \
  XRay.Metal.FloraAdapterProbe XRay.Metal.StaticShadowMergeProbe \
  XRay.Metal.StaticShadowMergeGpuProbe -j4
```

Normal S2 performance (zero **added** NPCs):

```sh
script/run_metal_release_fixture.sh --timing --fixture S2 --backend metal \
  --e07-npc-count 0 --no-e07-telemetry --warmup 60 --duration 150 --run-id <unique-id>
python3 script/analyze_metal_e08.py scratch/release_runs/<unique-id> \
  --output scratch/release_runs/<unique-id>/e08-summary.json
```

The complete pre-E08 algorithm control uses that same final binary/command with:

```sh
XRAY_METAL_E08_EARLY_DRAWABLE=1 XRAY_METAL_E08_FLORA_LINEAR=1 \
XRAY_METAL_E08_STATIC_LINEAR=1 XRAY_METAL_E08_SHADOW_SEPARATE_TILES=1 \
XRAY_METAL_E08_HEAP_BINDINGS=1 XRAY_METAL_E08_SUN_LINEAR=1 \
XRAY_METAL_E08_PARTICLE_LEGACY=1 XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF=1 \
<benchmark-command>
```

Keep experimental resource/upload/global-reuse flags and instanced shader directory
unset in normal/control timings. API/shader validation and hardware counters are
separate diagnostics. `<unique-id>`/`<benchmark-command>` are placeholders.
`scratch/e08_final_matrix.sh` and `scratch/e08_controlled_runs.sh` retain the exact
executed sequences. The latter reproduces the historical positive arena flag
only with its recorded comparison binary; use the negative legacy flag on final.

GPU atlas regression:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
build/src/Layers/xrRenderMetal/XRay.Metal.LocalShadowBatchGpuProbe --gpu \
  build/metal-runtime-shaders/local-lights build/metal-runtime-shaders/sun 200 <out-dir>
```

Static index merge GPU regression:

```sh
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
build/src/Layers/xrRenderMetal/XRay.Metal.StaticShadowMergeGpuProbe --gpu \
  build/metal-runtime-shaders/sun <out-dir>
```

Late-submit recovery diagnostic uses `XRAY_METAL_E08_FORCE_SUBMIT_RETRY=27` with
`--execute --metal-validation --fixture S4 --backend metal`. Value 1 fires on
first submit; a larger value waits for the actual world snapshot frame threshold.
The variable is consumed once inside the child process. This is not a performance run.

The original E07 executable and source snapshots are retained in
`scratch/release_runs/20260928-e08-prestate/`. Ordinary renderer WIP is preserved;
there is no broad stage/reset, commit, push, E09 implementation or memory update.
Stage-only diff and hashes compare the owned files against those prestate
copies rather than treating the entire dirty repository diff as E08. See
`20260928-e08-prestate/e08-only.patch` and `e08-source-manifest.json`;
`scratch/package_e08_evidence.py` regenerates them without staging any files.

## Exclusions and proof limits

All nonzero-exit runs are excluded from accepted performance samples: B1 exited139
at shutdown (spatial removal/CPS instance); C1 exited134 on the existing captured
shadow-light guard; I1 completed sampling then exited139 at shutdown. The guard
and source geometry are not weakened to conceal a failure. The file
`i1-crash-lookup-unmatched.ips` is an older C1 incident duplicate, not an I1 crash
report. No matching I1 crash report was obtained. These exploratory failures
remain stability evidence for later work; final successful runs do not prove
long-session stability.

Two later S4 instancing follow-ups on the selected binary are also rejected:
`20260928-e08-production-instance-profile` and `production-instance2-profile`
exit134 during model loading before the first useful world-render sample.
Both logs show a blank model filename through `CModelPool::Instance_Load` /
`CHangingLamp::net_Spawn`; no scenario telemetry completes. The cause is not
isolated, and no matching new OS crash report was found. Enabled instancing is
correlated with these rejected starts, not proven to cause the model-load defect.
Earlier successful S2 synthetic/runtime probes do not make this S4 attempt
qualified. The experimental shader directory stays unset in the accepted path;
guards/source models are not weakened and no E09 fix is attempted.
An S4 resource-dedup follow-up (`production-resource-profile`) also exits134
with the same blank model-load stack before useful sampling. Its performance
is unknown on that heavy camera, rather than zero-cost or a successful gain.
The standard-flag return (`production-return-profile`) starts and renders the
heavy scene again, exit0/scenario OK: CPU38.757/GPU23.366 ms p50 and median24 FPS
in its short intrusive diagnostic. No causal startup diagnosis is inferred from
this correlation.

Earlier A2/D1/P1/I2/U1 runs are exploratory, with different binary/feature/log
conditions. They are not used for the final total speedup. Short intrusive CPU/GPU
profiles are not timing acceptance. CPU sampling via `sample` was unavailable
because Developer mode is disabled; no CPU-stack or occupancy conclusion is
claimed. The rejected first counter capture was allocation-limited, and diagnostic2
had an incorrect old timestamp scale. Its raw calibration pairs are retained;
final profiling uses CPU/GPU timestamp pair ratios without a second timebase factor.
See Apple's [GPU timestamp conversion](https://developer.apple.com/documentation/metal/converting-gpu-timestamps-into-cpu-time).

The standalone Bootstrap target had an unrelated unresolved-symbol link failure;
it is not a successful target in this packet. The production executable and named
probes above build successfully. A top-level `ctest --test-dir build` discovered
zero tests; only the explicit renderer-directory runs count as test evidence.

Visual parity remains the user’s provisional assumption. Exact pixel probes cover
specified shader/fixture states; source/self-tests and green builds are not whole
scene/Windows visual acceptance. Native60 requires the full three-run matrix and
pacing conditions; a few complete presentation CSVs do not close that gate.

## Accepted-core total comparison before static merge

Executable SHA-256 for this controlled milestone:
`b232f660c1c5fcf0bbf7199469721850dba757587a944844b5b5cc795439e16d`.
The normal / complete legacy control / normal return sequence uses this one
binary, the unchanged shader receipt set and save/config inputs, native
1920×1080, the same S2 diagnostic scenario, 60+150 seconds and 30 seconds settle.
All three exit0 with scenario OK.

| Final run | CPU encode p50 ms | GPU p50 ms | Displayed p50 ms | Median rate FPS | 1% low FPS |
|---|---:|---:|---:|---:|---:|
| final-s2 (normal) | 27.479 | 19.450 | 33.333 | 30.0 | 16.36 |
| final-legacy (all original algorithm controls) | 77.602 | 31.567 | 116.666 | 8.57 | 6.73 |
| final-s2-return (normal) | 27.891 | 18.731 | 33.333 | 30.0 | 18.50 |

The observed displayed median improves about **3.5×**; CPU encode falls about
**65%**, GPU duration about **38–41%**. These are same-binary packet-level results,
not individual attribution of every accepted component. Presentation callback
completeness and percentiles remain in `final-comparison.json`; partial interval
statistics are labelled, not converted into a complete average FPS or native60 PASS.
The median cadence is not a claim of stable 30-FPS tails: 1% lows remain lower.
Raw three-run directories and
`20260928-e08-analysis/final-comparison.json` retain the full distributions.

Marsh S0 (and S6, which shares that save) initially fails preflight because the
current runtime autosave hash differs from the registered canonical save.
An exact-hash prior copy was recovered from `p5-s3/runtime/appdata/savedgames/`
into `20260928-e08-canonical-marsh-save/`, with `source.json` recording origin
and hashes. The registry and user autosave are unchanged. S0/S6 recovered-input
runs used `XRAY_PARITY_SAVE_SOURCE_DIR` and remain clearly identified. Both
recovered-input runs exit0; initial preflight failures never launch the engine.

## Representative fixture matrix, accepted core b232

One uncapped native 1920×1080 run per fixture, 60 seconds warm-up, 150 seconds
sample window and 30 seconds settle. This is a representative budget matrix,
not the required three-run native60 gate or cold/full-AI acceptance. S2 requests
zero added NPCs; background actors remain. Scenario drivers differ and some keep
the device clock paused, so cross-fixture differences are not isolated A/B.

| Fixture | CPU encode p50 ms | GPU p50 ms | Displayed p50 ms | Median rate FPS | 1% low FPS | Presentation |
|---|---:|---:|---:|---:|---:|---|
| S0 canonical Marsh | 43.894 | 22.843 | 50.000 | 20 | 16.51 | partial |
| S2 Garbage | 27.479 | 19.450 | 33.333 | 30 | 16.36 | partial |
| S4 moving camera | 43.132 | 24.040 | 50.000 | 20 | 16.68 | partial |
| S5 | 22.481 | 17.677 | 25.000 | 40 | 28.85 | complete |
| S6 canonical Marsh | 29.381 | 18.569 | 33.333 | 30 | 22.80 | partial |
| S7 | 25.602 | 17.842 | 25.000 | 40 | 20.08 | partial |

All six exit0. S0 scenario is NOT_APPLICABLE; the other five are OK. Each partial
run has one missing displayed timestamp: available intervals remain useful but
do not close pacing acceptance. S5 has complete mean displayed FPS 41.19.
No 60-cap run is claimed because the uncapped prerequisite is not met.
Raw summary: `20260928-e08-analysis/matrix-core.json`; commands in
`scratch/e08_final_matrix.sh` and `scratch/e08_recovered_marsh_matrix.sh`.

## Static local-shadow index merge, additional S4 investigation

S4's final camera submits 12,135 static caster rows across 157 light views and
13 batches. The additional candidate combines only consecutive admitted opaque
triangle-list rows with identical vertex buffer, model bytes and WVP bytes.
It preserves every absolute vertex ID and triangle order by rebasing UInt16
indices; values above 65,535, negative rebase, alpha, model/VB changes and
non-triangle remainders split to original draws. No shader or light/quality
selection changes. Fresh light matrices are used every frame.

The CPU contract test passes (order, compatibility barriers, 65,535/65,536,
negative rebase, bounds/error clearing). A converted production SUN shader GPU
probe compares three VPs with coplanar/opposite-winding triangles, nonzero index
offsets and the UInt16 edge: full R32 output matches in all three with positive
coverage 7,350 / 6,586 / 6,428 under API/shader validation. The initial probe had
an incorrect test-buffer prefix; its failed result is retained, not accepted.
`20260928-e08-static-merge-gpu2/` and `static-merge-gpu2.log` record the passing run.

Six real S4 local-atlas legacy/merged pairs at frame120 are bit-identical:
five are nonempty, one is empty on both sides; no nonfinite pixels; all 12 GPU
commands complete without error. Raw atlas size is 2048×2048 R32 (16,777,216
bytes each). `20260928-e08-static-merge-pixels/e08-pixel-comparison.json` records
hashes and coverage. The generic off-camera validator reports NO_COVERAGE
because this diagnostic requests zero off-camera casters; completed raw map
comparison is the evidence, not that validator's PASS label.

The first 32-MiB cache variant fills during the moving route. At the final camera
12,135 rows become 8,137 encoded calls; its intrusive short profile yields
CPU40.596/GPU23.567 ms and median24 FPS. This is diagnostic, not accepted A/B.
The selected variant removes unused cached CPU index copies and admits up to64 MiB
of retained GPU indices/CPU run-and-ID payload. Cache keys use exact ordered
material IDs in the scene's immutable owner; Prepare/Reset replaces them.
No eviction occurs while current frames can pin entries: once saturated, existing
keys remain reusable and new keys use full original draws. Source index decoding
is separately bounded by the loaded scene's static index spans, and upload
staging remains separately owned until completion/retry. The budget denotes
estimated retained cache payload, not exact Metal residency or total process
memory. S4 route coverage is 47 MiB/260 keys in the selected run. At the heavy
camera, the source 12,135 static rows encode as 1,889 draws; `encoded_draws`
counts all remaining run draws including alpha/singletons, not merged runs only.

The same-binary S4 A/B/A-return below uses executable
`fc3fe4b84feb70e3d0b8e219d44f7d20e6222cdac4420a51077b90f4f2b468b3`,
the same shader receipts and save, 60+150 seconds/30-second settle, native
1920×1080, with no GPU readback, counters or validation. All exit0/scenario OK.

| Run | Merge | CPU encode p50 ms | GPU p50 ms | Displayed p50 ms | Median rate FPS | 1% low FPS | Complete displayed mean FPS |
|---|---|---:|---:|---:|---:|---:|---:|
| static-merge-a | off | 42.975 | 24.251 | 50.000 | 20 | 13.22 | 21.19 |
| static-merge-b | on | 39.303 | 23.733 | 41.667 | 24 | 16.47 | 23.16 |
| static-merge-a2 | off | 43.308 | 24.283 | 50.000 | 20 | 9.77 | partial; no mean |

The complete mean improves 9.3% in A→B, while the CPU and median return to
control levels in A2. A2 has one missing presentation callback: its available
median/1%-low is shown but must not be interpreted as a complete pacing run.
The 1%-low is a noisy tail indicator, not proof of stable 24 FPS. Input/camera,
actors, resolution, light count, shadow quality and index triangle order are
unchanged. Raw `20260928-e08-static-merge-{a,b,a2}/`, summary
`20260928-e08-analysis/static-merge-{a,b,a2}.json`, and exact command
`scratch/e08_static_merge_ab.sh` reproduce the sequence. The final default uses
the negative control `XRAY_METAL_E08_MERGE_STATIC_SHADOWS_OFF=1`.

## Selected production binary and matrix

The promoted default executable is
`d9ccd643ba6d83b27e83a2e50141a96c7c0cb40323fa74b8f1b1f9a2d7ea2e55`.
S4 on that binary independently repeats the selected behavior: exit0/scenario
OK, CPU39.463/GPU23.708 ms p50, displayed41.667 ms p50 (24 FPS median),
14.61 FPS 1%-low and **23.08 actually presented mean FPS** with complete
presentation callbacks. It submits 12,135 source static rows as 1,889 draws.
The final representative matrix uses this one binary, unchanged shader receipts,
60-second warm-up +150-second window, 30-second settle, native1920×1080, no
capture/validation/counters. S0/S6 use the recovered exact-hash canonical save.

| Fixture | CPU encode p50 ms | GPU p50 ms | Displayed p50 ms | Median rate FPS | 1% low FPS | Presentation |
|---|---:|---:|---:|---:|---:|---|
| S0 canonical Marsh | 40.160 | 22.309 | 41.667 | 24 | 17.68 | partial |
| S2 Garbage | 27.250 | 18.754 | 33.333 | 30 | 17.59 | partial |
| S4 moving camera | 39.463 | 23.708 | 41.667 | 24 | 14.61 | complete |
| S5 | 20.375 | 17.627 | 25.000 | 40 | 32.40 | partial |
| S6 canonical Marsh | 28.253 | 18.546 | 33.333 | 30 | 23.05 | partial |
| S7 | 23.590 | 17.986 | 25.000 | 40 | 25.38 | partial |

All six exit0; S0 scenario NOT_APPLICABLE, five others OK. Each partial run has
one missing displayed timestamp, so its mean displayed FPS is withheld. The
matrix is one representative run per fixture, not three repetitions/native60
acceptance or cold/full-AI acceptance. The final normal fixture medians span
24–40 FPS; no 60-cap run is claimed because the uncapped prerequisite fails.
S4's same-binary A/B above attributes the static-merge benefit; this matrix is
additional coverage, not a six-fixture isolated A/B attribution. Raw
`20260928-e08-merged-{s0,s2,s4,s5,s6,s7}/`, summary
`20260928-e08-analysis/matrix-merged.json`, commands
`scratch/e08_merged_matrix.sh`. Cache payload approaches64 MiB on Marsh;
new uncached light/geometry selections after saturation safely use original
draws and can lose this extra speedup. This route evidence is not a long-session
performance/cache-memory plateau claim.

## Final checks and ROI stop

Final renderer-directory contracts: **8/8 PASS** (`tests-production-final.log`),
including signed absolute-vertex boundary and failure-output clearing in the
static merge test. Rebuilt production-shader index-merge GPU oracle: PASS under
API/shader validation (`static-merge-gpu-final.log`,
`20260928-e08-static-merge-gpu-final/`). The final normal S4 recovery run forces
late-submit failure at actual world frame27, when the first cached merged IBs
are prepared: one retry, continued rendering, exit0/scenario OK, API/shader
validation enabled and zero reported Metal errors. Full Flora/static scans pass
7,000 views with zero omissions. `20260928-e08-merge-retry-validation/` records
the binary/env, retained input provenance and runtime log. This is bounded
recovery/culling proof, not E09 stability acceptance.

The final-binary S4 short diagnostic uses15-second warm-up/65-second sampling,
GPU counters and no validation/readback. It exits0/scenario OK; the whole-run
settled CPU/GPU p50 is39.193/23.675 ms, median24 FPS. Its opt-in counters make
it diagnostic rather than accepted pacing. Eleven sampled heavy-camera frames
(`engine_frame>=600`, actual indexed_draws>14,000) give these budgets:

| Measured final heavy-camera work | CPU encode ms p50 | GPU vertex ms p50 | GPU fragment ms p50 | Draws p50 |
|---|---:|---:|---:|---:|
| All13 local shadow atlas batches | 15.473 | 8.167 | 1.288 | 6,047 |
| G-buffer | 12.429 | 6.398 | 2.437 | 5,128 |
| AO temporal base | 0.013 | 0.013 | 2.959 | 1 |
| Local light accumulation | 1.126 | 2.307 | 0.302 | 505 |

Aggregate CPU binding buckets are nested in those pass times: MSC9.947 ms,
indexed-draw3.455 ms, upload2.120 ms, PSO0.755 ms, VB0.587 ms. The lighter S2
ceilings above must not be substituted for this heavier camera. No entire
bucket is assumed removable: indexed MSC still needs its index-type word,
resource binds carry changed matrices/textures, and GPU geometry/AO work remains
necessary at the accepted quality. GPU stages overlap and the table must not be
summed into a frame wall time; it is not occupancy/bandwidth/cache evidence.
Raw `20260928-e08-production-passes/{passes.csv,passes.csv.cpu.csv}` and
`heavy-camera-budget.json` retain the data and admission rule.

Flora instancing was reconsidered on this heavier residual (~3,483 non-static,
non-dynamic local-atlas draws), but both native starts above fail before useful
sampling, so it is not adopted. Resource dedup also has no accepted S4 result;
its completed S2 isolated result already lacked material benefit. Upload batching
and host-vector reuse have no measured substantial gain. A global prepared-index
selection API was drafted, then removed from the accepted source because no FPS
return over the measured cache was shown; its uncompiled draft remains only in
the prestate evidence directory. New portal/HOM visibility, broad G-buffer
submission work, detailed MSC binding attribution or shader/GPU redesign would
need a fresh measured contract, not extrapolation from these aggregate intervals.

**Stop:** the accepted changes deliver the measured gains above; no next ordinary
correctness-preserving optimization with a demonstrated material return remains.
The user's ROI stopping condition is applied. Native1080p60, full three-run pacing,
cold traversal/full-AI behavior and long-session cache/stability remain **OPEN**.
E09/E10/E11, GPU skinning, quality cuts, commit/push and memory updates were not
performed. The source diff/manifest preserve the original dirty WIP separately.

Packaging check: `e08-only.patch` applies to an isolated copy of the saved
pre-E08 dirty-worktree files, and all22 result hashes match
`e08-source-manifest.json` (`e08-patch-check.json`: PASS). E01 analyzer final
tests9/9 PASS; Python syntax and fixture/matrix shell syntax checks pass. No
source is staged and the live workspace is not replaced by this patch check.
