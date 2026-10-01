# E07 — NPC performance diagnosis (2026-09-28)

## Decision and scope

E07 selects **branch B: shadow/rendering costs**, retaining the existing CPU
skinning oracle. The performance gate remains **OPEN**. No E08 optimization or
GPU skeletal migration is included in this packet.

The user explicitly authorized E07 with visual parity treated as a provisional
working assumption. This supersedes the plan's prerequisite ordering for this
execution; it does not close the outstanding E06/Windows visual findings.

The new production changes are opt-in diagnostic timers and CSV output. Skinning
math, mesh layouts, material/CROS bindings, shadow admission, AI, simulation
frequency, physics, and the DXVK fallback retain their existing behavior.

## Reproduction and proof boundaries

Build: `ninja -C build -j 4 Anomaly.DX11 XRay.Metal.DynamicWorldAdapterProbe`.
The new executable SHA-256 is
`1b7b9c0c22a527fe5064984b1cd47ebaa54bfe8c44d5802b771054d92c6c4a6b`.
Machine: Apple M1 Pro, 10 CPU / 16 GPU cores, 16 GB RAM, macOS 27.2, AC power.
The runs report a native 1920×1080 backing/render surface and a 120-Hz-capable built-in
display. Thermal snapshots report no recorded warning; they are not continuous
temperature or frequency measurements.

```sh
bash script/run_metal_release_fixture.sh --timing --fixture S2 --backend metal \
  --e07-npc-count 8 --run-id e07-s2-n8 --warmup 60 --duration 150
```

Repeat with counts 0/1/4/8 and unique run IDs. The runs use the same
Garbage save, renderer binary, weather, profile and shader receipts in isolated
runtimes. The current E07 driver holds the requested camera each update and
records its actual position/direction before correction. It snapshots the Lua
scenario into the run directory and records its SHA-256 before launch. There
are 60 seconds of initial warm-up, then 150 seconds of normal
Device-clock simulation. The analyzer excludes the first 30 seconds after the
fixture spawns, leaving approximately 120 seconds. API/shader validation and
capture are disabled in these timing runs. Requested counts refer to **added
fixture NPCs**, not all NPCs already present in the save.

Unlike historical S2, E07 unfreezes the Device clock so object updates and
animations actually run; weather and game time remain fixture-paused. The
fixture releases only its own spawned objects, allows two seconds of normal
Device time for deferred object/light deletion, then restores and verifies
the saved clock factor in `telemetry.json.cleanup.json`.
The default S2 and ordinary gameplay are unchanged.

`e07-frames.csv` attributes CPU repack, post-repack bounds and skin upload time,
vertices/bytes, cache reuse, and shadow encoding. `shadow_cpu_ms` measures only
the DynamicWorld adapter; `local_shadow_cpu_ms` and `sun_shadow_cpu_ms` include
all caster families. The adapter interval is nested in these totals. All are
nested in CPU `scene_ms`, which is nested in `encode_ms`; never add these
durations. GPU duration and displayed intervals are independent measurements.

The object sidecar joins actual captured IDs to the fixture's spawned IDs.
Frustum flags establish potential geometric visibility; successful G-buffer
submissions establish encoded draws. Neither establishes visible pixels or
absence of occlusion. Lua observes online/alive state periodically. Normal AI
moves NPCs away from their spawn stations, so the exact fixed-background,
0/1/4/8 **visible** cost curve remains OPEN. No AI freeze, removal of off-camera
casters, or teleportation is used to manufacture that gate.

## Measurements

One 150-second ordinary run per requested count, with the first 30 seconds
excluded. Values below are p50 milliseconds; the skin timer covers all captured
skinned geometry, including background actors, rather than only added NPCs.

| Added NPCs | CPU skin | Local shadow CPU | Sun shadow CPU | CPU scene | GPU frame | Drawable wait | Display interval |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 0 | 4.44 | 35.90 | 8.02 | 71.16 | 28.07 | 0.05 | 75.00 |
| 1 | 4.42 | 35.73 | 7.85 | 69.65 | 35.07 | 34.85 | 108.33 |
| 4 | 5.64 | 35.87 | 7.90 | 71.00 | 35.37 | 36.75 | 116.67 |
| 8 | 5.60 | 36.07 | 8.00 | 72.91 | 36.34 | 37.88 | 116.67 |

[Generated curve and coverage](../../scratch/release_runs/20260928-e07-analysis/curve.md),
[machine-readable summary](../../scratch/release_runs/20260928-e07-analysis/curve.json),
and [per-run raw joins](../../scratch/release_runs/20260928-e07-analysis/runs/).
All four listed ordinary processes exited 0. `n4-r1` is rejected for camera drift.
`n4-r2` has the camera guard and zero periodic camera error; its provenance was
written just before an angle-observation logging edit. The other three runs
precede that guard. These are diagnostic measurements with evolving Lua
receipts, not a strictly identical-script cost curve or a release baseline.
The current snapshot protocol prevents future source edits changing a launched
scenario. No exact incremental NPC slope is inferred from this table.

Settled actual spawned-NPC frustum ranges are 0, 0, 0–2 and 0–4; encoded
G-buffer object counts are 0, 0–1, 1–3 and 3–5. The four-NPC run has three
alive NPCs at its final observation; the eight-NPC run retains eight. Captured
NPC skeletons have 47 bones where known; uncaptured objects have unknown bone
coverage rather than zero. The recorded maximum sums are 0/47/141/235 bones.
Mixed randomized NPC models and existing save actors prevent an identical-mesh
scaling claim. Global repack vertices p50 are 75,160/69,867/97,254/95,340;
upload bytes p50 are 2,705,760/2,515,212/3,501,144/3,432,240. Pose-cache
reuse and submitted/shadow vertices are preserved in the linked JSON/CSV.

Displayed p99 intervals are approximately 91.67/125/125/133.33 ms, with
1% low 10.27/7.50/7.91/6.70 FPS. Missing or zero presentation callbacks
leave pacing coverage incomplete. These results cannot pass 60 FPS, but are
not the required three repetitions. No quality/resolution setting was reduced.

Individual skeleton/AI durations, GPU shadow/G-buffer pass durations and
per-frame GPU residency remain **OPEN**. No inference that AI costs zero, that
the GPU is bandwidth-bound, or that the added NPCs alone explain the drawable
wait is supported.

## Attribution

The separate 8-second normal-simulation CPU sample in the four-NPC run is taken
at fixture seconds 8–16 and excluded by the 30-second settlement window. It
records 2,060 main-thread samples: 1,826 in `MetalRenderDeviceRender::End`,
1,382 in scene `Render`, and 561 in `renderLocalShadowBatch`. Of the latter,
549 enter Flora shadow drawing and 459 reach its clip-volume test. These are
nested statistical samples, not additive elapsed times or exact percentages
of total application CPU usage. The sample reports an 8.0-GB physical footprint;
this is not a per-frame GPU residency measurement or a leak result.

Source explains the sampled work: `MetalR4FloraAdapter::DrawShadowCasters`
iterates the full prepared Flora draw list for each shadow view, multiplies
each model/light matrix and tests its bounds. `renderLocalShadowBatch` invokes
this for every admitted local shadow tile. The current skin buffer is already
reused across G-buffer/emissive/shadow passes. A different bone pose invalidates
the relevant instance/geometry/frame-slot cache; skinning is not repeated for
each shadow pass.

The measurements do not justify replacing this CPU oracle with a GPU path as
the first optimization. Even eliminating its measured repack time would leave
the much larger scene cost and the measured GPU/presentation limits. No
allocation-cache, bandwidth, occupancy, or GPU shader bottleneck is inferred
solely from elapsed duration.

## Validation and exclusions

- Main executable and DynamicWorld probe build succeeded.
- Rebuilt `XRay.Metal.DynamicWorldAdapter.SelfTest`: 1/1 PASS, preserving the
  existing 1/2/3/4-weight CPU oracle cases.
- Analyzer: 9/9 focused tests PASS, including zero-duration samples, failed
  exits/scenarios, ambiguous frame joins, count identity, provenance mismatch,
  unknown/background IDs, and missing drawable evidence.
- Shell/Lua syntax and scoped diff whitespace checks pass.
- There is no claimed speedup, isolated NPC-count acceptance, completed
  on-screen pacing gate, GPU migration, or new Windows visual acceptance.

`20260928-e07-initial-s2` is excluded: it used the old frozen-clock timeline,
was sampled intrusively, and its running shell file was edited while executing.
The resulting shell error triggered its own cleanup SIGTERM; the engine's
signal handler reported an abort in the interrupted render stack. This is not
counted as a successful run or attributed to a new renderer crash. Its earlier
CPU sample was taken during filesystem initialization and is also excluded.

`20260928-e07-gpu-final` is an intrusive capture excluded from the curve. It
saved one frame, then exited 134 after the measured window with repeated
`invalid captured shadowed local light` errors during spawned-object cleanup.
The E07 fixture now delays clock restoration until deferred deletion has had
its two-second grace period. The separate `20260928-e07-gpu-cleanup` retest
exits 0, scenario OK,
with zero such errors and a completed clock receipt (requested/actual factor
0/0). Metadata records one captured frame. [Capture receipt](../../scratch/release_runs/20260928-e07-gpu-cleanup/e07-capture-receipt.json),
[trace](../../scratch/release_runs/20260928-e07-gpu-cleanup/e07-s2.gputrace),
[cleanup receipt](../../scratch/release_runs/20260928-e07-gpu-cleanup/telemetry.json.cleanup.json).
The private helper `scratch/run_metal_release_fixture_e07_capture.sh` reproduces
this intrusive run:

```sh
bash scratch/run_metal_release_fixture_e07_capture.sh --timing --fixture S2 \
  --backend metal --e07-npc-count 4 --warmup 10 --duration 60 --run-id <unique-id>
```

Capture/validation runs must never be fed into a release performance comparison.

The macOS `gpudebug` CLI hangs before producing session/trace information,
including for `-l`; the attempted GPU-tools service restart was denied by SIP.
No SIP bypass or fabricated GPU pass timing is used. Trace inspection remains
OPEN. Metadata proving one captured frame is distinct from GPU-pass analysis.

Original versions of all edited implementation files and a patch containing
only this stage's changes are preserved in
`scratch/release_runs/20260928-e07-prestate/`. The large unrelated dirty checkout
is neither reset nor staged.

## E08 handoff, not implemented here

Start with the measured shadow-caster cost: bounded conservative spatial
selection/reuse of unchanged geometry and world bounds, followed by the
existing exact light/cascade tests. Preserve wind/animated bounds, off-camera
casters, alpha/wind semantics, all point-light faces and invalidation keys.
Measure one change against the same fixture before accepting it.

Then investigate GPU pass timings and the NPC-dependent drawable wait using
the E01 serial/queue/presentation records. A reported drawable wait is not proof
of a particular queue or driver defect. Do not increase queue depth or reduce
NPCs, shadow distance/resolution, or native resolution to claim 60 FPS.

After those costs change, repeat the E07 curve with a fixture that independently
proves its on-screen count, different poses and object identities, and the
three 120-second S2 repetitions required for release acceptance. CPU skinning,
individual skeleton/AI timing and the optional GPU path can then be re-ranked
from the new bottleneck, rather than assumed in advance.
