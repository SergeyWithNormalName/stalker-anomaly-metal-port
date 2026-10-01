# Direct Metal release status

**E08 update, 2026-09-28:** Executed only the authorized E08. Conservative caster culling, grouped atlas encoding, MSC scratch, late drawable, sun culling, particle arena and order-preserving static shadow merge are production defaults. Controlled S2 median8.57→30 FPS; additional S4 A/B20→24 FPS. Final native1080p matrix medians24–40 FPS, all six exit0. **Native60 remains OPEN**; ordinary measured ROI is exhausted for this packet under the user's stopping condition. CPU skin oracle/quality/actors remain; E09 was not executed. [E08 evidence and limits](E08_PERFORMANCE.md).

**E07 handoff, 2026-09-28:** Visual parity provisionally accepted by the user; diagnosis selected branchB shadow/rendering costs. Exact visible-count scaling remains OPEN. [E07 evidence](E07_NPC_PERFORMANCE.md).

**Audit override, 2026-09-27:** [E02–E06 audit](E02_E06_AUDIT_2026-09-27.md) supersedes the historical E04/E05 AUTO PASS conclusions below. Confirmed: local-light cutoff at 124 stencil records, wrong previous-camera data in static/terrain motion producers, incomplete shadow casters/cascade contract, 16-tile shadow overflow, and non-neutral IL-off behavior. E04/E05 are REOPENED; E06 remains OPEN. Only diagnostics were corrected in this audit. Final Bar static run exits0 with API/GPU validation enabled; FrameUploadGpu 1/1 passes. Neither is Windows or motion acceptance.

**Packet 1 update, 2026-09-27:** A1 light cutoff is FIXED and verified: Windows non-MSAA masked stencil rollover, GPU 125/126/127/200/400 mixed records, positive tail light in both camera poses, preserved depth/geometry/accumulator, Bar motion exit0 with validation and 232..1135 submissions, and a new Bar trace with 590 lights / 1774 draws / 4 resets. See [rollover evidence](E05_LIGHT_MARKER_ROLLOVER.md). E05 remains REOPENED for A3/A4; E04 and E06 remain open.

**Packet 2 update, 2026-09-27:** A2 static producers are FIXED and verified through actual converted shaders: LMH/terrain/DefVertex/DefObjectsLod use the last accepted camera; AO/IL source ownership follows accepted submissions. Final controlled Bar S1 exits0 with API/shader validation, turn/pure-pan error ≤0.127px, cut/return, reload, retry, encoded/cold abort, second-view rejection and 1920→960→1920 resize verified. 60 G-buffer readbacks, 9 history clears and 16 position-copy/output pairs pass. The old driver froze Device time and did not produce commanded post-precache motion; S1 clock/precache handling is corrected. NPC previous bones, identity generation limits and Windows/V1 remain OPEN. [Production evidence](E04_PRODUCTION_MOTION_HISTORY.md).

**Packet 3 update, 2026-09-27:** Active Bar A3 caster/cascade contract is repaired and GPU verified. Source `(20,60,160)` footprints, ray chaining/snapping/bias/receiver cuboids and near/middle/far stencil graph replace near20 + level-sized far. Flora and conditional DetailGrass use converted source alpha/wind; off-camera static admission loads geometry without main-view instances, with CPU-skinned casters retained. Same-binary grass off/on runs exit0 with observed turn/pan/hold/cut/return, 44 production four-phase readbacks with zero extra/missed stencil writes, and actual G-buffer receiver overlap for cold item/NPC/Flora in sun and real point/spot maps. Alpha fence/Flora and same-matrix wind map pairs pass. Depth/raster uncertainty is explicitly excluded. A4, portal/HOM fallback, unobserved Flora permutations and Windows parity remain OPEN. [Packet 3 evidence](E05_SHADOW_CASTERS_AND_SUN_CASCADES.md).

**Packet 5 update, 2026-09-27:** A5 independent AO/IL controls and neutral IL-off are repaired: actual converted probe 18 states/34 readbacks PASS; native Bar exact black-control low/high pairs and stale-white sensitivity controls PASS. Packet5 adds general SSR quality 0, first RGBA16F selflightl accumulator, sun/spot/Omni volumetric and fog scattering. Accepted Bar lifecycle/motion run exits0 with 514 finite reports/659 texture reads, all25 volumes and SSR submit-owned history. The colored-texture emitter probe caught and fixed SKIN_NONE UV*1024; NONE/SKIN0 positive and rejected-family controls now PASS. Final original-save S3 exits0 at 1920×1080 with 179 finite reports/220 texture reads and actual SSR scale reset/readbacks; old38 interpolant reports and colored-artifact cause remain OPEN. [Packet 5 implementation, evidence and limits](E06_PACKET5_CONTROLS_AND_COVERAGE.md).

| Field | Status |
|---|---|
| Active stage | E08 optimization packet complete / NATIVE60 OPEN; ROI stop authorized by user. E06 visual acceptance deferred; historical findings remain recorded |
| E00 | **PASS / CLOSED on 2026-09-22** |
| E01 baseline | **CAPTURED / DIAGNOSTIC** — 3 × 120 s after 60 s warm-up for S0, S2, S4, S5 on one binary; all 12 runs admitted and exited 0 |
| E01 60 FPS gate | **FAIL** in all four fixtures; S5 CPU scene has the largest measured cost |
| E01 overhead gate | **OPEN / NOISY** — six same-binary on/off runs cannot prove ≤2%; 12–18% within-mode throughput span |
| E01 attribution | **PARTIAL / OPEN** — frame GPU and labelled trace activity measured; several CPU domains and counters remain uninstrumented |
| E02 code | **IMPLEMENTED** — acquired-slot arena, immutable per-draw/pass slices, MSC constant snapshot, Shared vertex/index copy-on-write, AO/water submit-only history |
| E02 GPU probe | **PASS** — old Shared overwrite reproduced; new six-frame, two-draw, COW and bounds checks passed under API/shader validation |
| E02 runtime | **PASS ON CONTROLLED DIAGNOSTIC S1** — final packet 2 exit0 with observed Device turn/pan/cut and API/shader validation; earlier frozen-clock S1 records are historical and do not prove scripted movement |
| E02 gate | **PASS FOR TESTED PRIMARY-VIEW POLICY** — actual retry, discarded encode including cold uploads, reload, resize and pre-encode second-view rejection verified; asynchronous GPU device loss still uses fatal shutdown |
| E02 performance | **DIAGNOSTIC ONLY** — one matched 30 s warm-up + 60 s S0 before/after pair; no isolated or release-level speedup claim |
| Admission | `ADMITTED` for the final S0 C1/C2/C3/D manifest; zero validation errors |
| Runtime | PASS — all four final processes exited 0; loading screen inactive; 1920x1080 framebuffer captures and 15 s motion artifacts present |
| Visual | `DIAGNOSTIC_ONLY` — the admitted C/D comparison exposes material lighting differences; it is not a visual-parity PASS |
| Windows reference A | **OPEN** — no local Windows D3D11 reference is available; later stages may continue, but Windows parity cannot be claimed |
| CrossOver/GPTK reference B | Not available locally |
| Reference C | Three native DXVK/MoltenVK repetitions captured |
| Candidate D | Direct Metal capture admitted against C |
| E03 | **IN PROGRESS / GATE OPEN** — the admitted S0/S1 camera is indoors and does not reproduce the user's outdoor screenshot; see E03 evidence below |
| E04 | **STATIC PRODUCER/HISTORY FIX VERIFIED / V1 OPEN** — actual converted G-buffer turn/pan/cut and AO/IL lifecycle pass; NPC previous bones/identity generations and Windows visual parity remain open. See packet 2 evidence |
| E05 | **A1/A3/A4 REPAIRS GPU VERIFIED / VISUAL AND PERFORMANCE OPEN** — rollover, active caster/cascade graph and complete local-shadow batches pass in tested scope. Packet 4: GPU 17/25/37/125/200; final Bar 25-record grass-on camera-return run exit0 with 28 GPU snapshots. Natural Bar completed frames reach 1107 records/91 batches; full validation run interrupted for MetalTools memory pressure. Portal/permutation/Windows limits remain. See [packet 4 evidence](E05_LOCAL_SHADOW_BATCHES.md) |
| E06 prework | **E06 PREWORK COMPLETE: materials census + weather/effects census** — [`docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md`](MATERIAL_PERMUTATION_CENSUS.md) + [`docs/direct-metal-release/WEATHER_EFFECTS_CENSUS.md`](WEATHER_EFFECTS_CENSUS.md). Discovery complete; confirmed gaps incorporated into `DIRECT_METAL_RELEASE_PLAN.md`. Prework remains historical evidence; E06 implementation started 2026-09-27, not completed. |
| E06 execution | **RESUMED BY PACKET 5 REQUEST / PARTIAL IMPLEMENTATION VERIFIED** — new SSR/emissive/volume/fog paths and AO/IL controls; historical verified packet:6A water contract,6C active bloom/emitter subset+256² luminance producer/order,6D AO/IL blur-scale fix,6E rain/splash/lens; [PARITY_MATRIX.md](PARITY_MATRIX.md); two GPT-6 Luna workers, one integrator. |
| E07 | **BRANCH B / PERFORMANCE OPEN** — opt-in skin/bounds/upload/shadow telemetry, spawned-ID joins, 0/1/4/8 diagnostic runs, preserved CPU oracle. [Evidence and limits](E07_NPC_PERFORMANCE.md) |
| E08 | **IMPLEMENTED / NATIVE60 OPEN** — controlled S2 comparison, extra S4 static-merge A/B/A, native six-fixture matrix, exact shadow pixel probes, retry and scoped tests. [Evidence and limits](E08_PERFORMANCE.md) |
| Next step | No further stage executed. Native60, complete three-run pacing, cold/full-AI and long-session cache/stability evidence remain open; further work needs a measured new bottleneck and its own scope |

## E06 prework evidence (2026-09-26)

Read-only предварительное исследование завершено до начала работ сильной модели (Sol):
- **Материалы и перестановки:** [`docs/direct-metal-release/MATERIAL_PERMUTATION_CENSUS.md`](MATERIAL_PERMUTATION_CENSUS.md) (~30 категорий перестановок). Подтверждены пробелы FLOD/impostor, динамических декалей (bullet/blood wallmarks), особенности fallback bump/bumpX и базовый факт вырождения CPU skinning в SKIN_0 для E07.
- **Погода и спецэффекты:** [`docs/direct-metal-release/WEATHER_EFFECTS_CENSUS.md`](WEATHER_EFFECTS_CENSUS.md) (24 эффекта/подсистемы). Подтверждены пробелы Lens Flare (uninstantiated stub adapter), Bloom (заглушка 1×1 black), Emissive (отсутствие target/pass, только телеметрия), Sunshafts (отсутствует в Metal), Volumetrics (отсутствует в Metal), Rain ground splashes (срезаны ранним return), Rain wetness accumulation (нет screen-space прохода). Подтверждены рабочие проходы: sky2, SSFX water waves (512×512), distortion particles, hard particles, SSFX AO/IL (5+5 passes), 3DSS scope reticle, SSFX LUT atlas.
- План `DIRECT_METAL_RELEASE_PLAN.md` актуализирован: пакеты 6A–6G наполнены конкретными подтверждёнными gaps; Sol освобождён от повторного сквозного census.


## Packet 4 update (2026-09-27)

A4 atlas overflow is repaired with complete sequential batches and coherent six-face point groups. Each batch is consumed before atlas reuse, preserving accumulated light and depth/stencil. GPU 17/25/37/125/200, six canonical face controls, batch/serial equality, rebuilt CPU 5/5 and the final grass-on Bar A→B→A route pass. The final binary is recorded in [packet 4 evidence](E05_LOCAL_SHADOW_BATCHES.md). That report separates partial/rejected runs, individual natural-Bar GPU frames, recorder duration, and remaining visual/performance/startup limits. Historical A4 OPEN statements above are superseded for the tested scope.

## E05 stable lighting, grass, NPC and shadows evidence (2026-09-25)

### 1. Root-Cause Diagnosis & Grass Shimmering Elimination

1. **Detail Grass Shimmer & Dither Flutter:**
   - *Fade Range Fix:* In `MetalR4DetailGrassAdapter.cpp`, legacy code used `fadeRange = fadeLimit - 1.0f`, forcing linear alpha fade starting at distance = 1 m from the camera. In `deffer_grass.ps`, dither clipping (`clip(I.M1.x - DITHER_THRESHOLDS[dither_idx])`) triggered on 50% of grass pixels between 20 m and 60 m, creating violent screen-door shimmer during camera translation. Fixed by maintaining 1.0 opacity up to 75% of fade radius and fading smoothly in the outer 25% zone (`fadeStart = fadeLimit * 0.75f`).
   - *Deterministic Spatial Chunking:* In `MetalR4EngineFs.cpp`, detail grass instances were collected across `[eyeZ - Radius .. eyeZ + Radius]` into a single flat vector `instances[modelId]`. Crossing a 2 m slot boundary prepended or dropped slots, causing a global index shift for all 61-element batches across the entire map, fluttering bounding boxes and light assignment. Fixed by grouping instances into spatial 4×4 slot chunks (8×8 m) keyed by `(modelId, chunkZ, chunkX)`. Chunks iterate in monotonic order, ensuring 100% deterministic batch composition and tight bounding boxes regardless of camera translation.

2. **Flora Vertex Repacking & Material Lookup:**
   - In `MetalR4FloraAdapter.cpp`, verified that BGR packing matches MSC vertex input `unpack_D3DCOLOR` (`c.bgra`) and `unpack_bx4` (`4.0 * v - 2.0`). Preserved double-sided normal/cull/AREF, base alpha, and bumpX textures.
   - Enhanced `RunR4FloraAdapterSelfTest` in `R4FloraAdapterProbe.cpp` to explicitly verify unpacked normals match world basis (+X) and validate fallback behavior.

3. **Local Shadow Atlas Stability & Anti-Flicker:**
   - In `MetalR4LocalLightAdapter.cpp`, when active local lights exceed atlas tile capacity (16 slots), previously the loop executed `continue;`, dropping light submission and causing strobing/flashing. Replaced with fallback to unshadowed rendering (`light.bShadow = false; item.shadowed = false;`), ensuring illumination remains continuous.
   - In `MetalR4EngineScene.mm`, introduced a 25% hysteresis scoring advantage to current atlas slot holders, preventing rapid tile oscillation on edge-case priority ties.

4. **CROS Lighting & Material Identity Isolation:**
   - In `r__dsgraph_build.cpp`, implemented `resolveVisualMaterialId` to derive `xmaterial` directly per-visual without inheriting or contaminating parent/preceding NPC material slots (`item.material.w`).
   - Implemented `resolveMetalLightingFallback` for objects with null or zero ROS (e.g. dropped inventory items, dynamic props), falling back to ambient/hemi instead of rendering pitch-black.
   - For attached meshes (weapons, helmets, attachments), inherited parent NPC's `CROS_impl*` lighting while maintaining separate visual material identity. Ensured first-person legs receive CActor ROS lighting and transforms.

### 2. Automated Probe Verification

All five Stage E05 probes compiled cleanly and passed:
- `XRay.Metal.DetailGrassAdapterProbe --self-test` -> **PASS** (`parser=true v3_slots=true v4_slots=true repack_61=true validation=true`)
- `XRay.Metal.FloraAdapterProbe --self-test` -> **PASS** (`repack=true xrfs=true traversal_rejected=true auto_selection=true frustum_culling=true`)
- `XRay.Metal.EngineWorldSnapshotProbe --self-test` -> **PASS** (`cros=true attached_meshes=true legs=true material_identity=true fallback_ambient=true`)
- `XRay.Metal.LocalShadowAtlasFoundationProbe` -> **PASS** (exit 0)
- `XRay.Metal.LocalLightAdapterProbe --self-test` -> **PASS** (exit 0)

### 3. Runtime Fixture Validation (S4 Garbage Outdoor)

- **Run ID:** `20260924T180929Z-metal-S4`
- **Scenario:** S4 (outdoor vegetation, forward walk across terrain and 90° shadow-split turn, 15 seconds)
- **Level:** `l02_garbage`
- **Exit Status:** `0` (`engine_exit_status.txt` = `0`)
- **Telemetry:** 1920×1080 native retina, scenario status `OK`, failures `[]`, 971 engine frames, zero texture rejections, zero batch rejections.
- **Visual Evidence:** 16-frame contact sheet `motion-contact.png` generated from `motion.mov`. Confirms continuous terrain rendering, solid vegetation, no fluttering or light/dark alternation on camera translation/rotation, and clean shadow-split transitions.

### 4. Gate Verdict

- **Status:** **AUTO PASS / VISUAL PENDING V2**
- All automated gates (probes, deterministic batching, atlas fallback, CROS inheritance, runtime exit 0, telemetry OK) passed.
- Visual inspection of grass/lighting aligns with Checkpoint V2 as defined in Section 9 & 16 of `DIRECT_METAL_RELEASE_PLAN.md`.

## E04 motion, camera and temporal history evidence (2026-09-24)

### 1. Architectural Diagnosis & Root-Cause Fixes

Investigation of temporal history, reprojection matrices, and object motion vectors identified three distinct root causes for camera/scene lag and ghosting:
1. **Water SSR Reprojection Bug:** In `MetalR4WaterAdapter.cpp`, the legacy code copied the current composite matrix $WVP$ into both `m_current` and `m_previous` constants. Because $(current - previous) = 0$, the screen-space reflection shader never reprojected previous reflections during camera translation/rotation, freezing reflections in screen space and leaving lagging ghost trails. Fixed by computing the true previous reprojection matrix $M_{prev} = VP_{prev} \times V_{curr}^{-1}$, placing current projection at offset 304 (`m_current`) and $M_{prev}$ at offset 368 (`m_previous`), uploading `ssrGlobals` once per frame, and clearing temporal SSR history on camera cuts or invalid previous state.
2. **SSFX AO / IL Reprojection Bug:** In `MetalR4SsfxAoAdapter.cpp`, previous view/projection matrices were not tracked across frame inputs. Fixed by extending `FrameInputs` with previous view/projection, calculating synchronous $M_{prev} = VP_{prev} \times V_{curr}^{-1}$, and clearing temporal history buffers on `cameraCut || !previousValid`.
3. **Dynamic World Instance Motion Vector Bug:** In `MetalR4DynamicWorldAdapter.cpp`, dynamic instance vertex shaders were receiving identical current transforms for both `m_wvp` and `m_wvp_prev` (offsets 0 and 64). As a result, moving NPCs and dynamic meshes produced zero screen-space velocity vectors, causing massive smearing and ghosting in temporal filters. Fixed by introducing `ObjectMotionHistory` to track object transforms by `instanceIdentityKey` across consecutive frames, uploading true $WVP_{prev} = VP_{prev} \times Transform_{prev}$ and $VP_{prev}$, and invalidating cache on camera cuts.
4. **Startup & Teleport Discontinuity Contract:** At startup (frame $\le 1$), `mView_saved` and `mProject_saved` contained uninitialized zero memory in `device.cpp`. Fixed by initializing saved transforms to current transforms on frame $\le 1$ in `device.cpp` and `MetalRenderDeviceRender.mm`. Furthermore, camera teleportation ($>50\text{ m}$ eye displacement) triggers `cameraCut = true`, resetting all downstream temporal histories.

### 2. Automated Reprojection Gate Verification (Gate: $\le 0.5$ px)

An automated GPU/CPU reprojection test (`RunMetalR4ReprojectionProbe`) was integrated into `XRay.Metal.FrameUploadGpuProbe` (`--reprojection` and `--self-test`).
The probe tests 64 view points across near, mid, and far depth planes, simulating camera translation and rotation:
- **CPU Reprojection Max Error:** `0.0001 px`
- **GPU Kernel Reprojection Max Error:** `0.0003 px`
- **Dynamic Object Motion Vector Error:** `0.0001 px`
- **Stationary Object Jitter Error:** `0.000000 px`
- **Gate Verdict:** **PASS** (all errors $\ll 0.5$ px threshold).

### 3. Live Runtime Frame Stamp Monotonicity

Periodic diagnostics in `MetalRenderDeviceRender.mm` sampled frame stamps across all snapshot domains during the S1 scenario run (`e04-s1-flight3-20260924`):
- `engine=301 camera=301 world=301 light=301 env=301 hud=301 cut=0`
- `engine=601 camera=601 world=601 light=601 env=601 hud=601 cut=0`
- `engine=901 camera=901 world=901 light=901 env=901 hud=901 cut=0`
- `engine=1201 camera=1201 world=1201 light=1201 env=1201 hud=1201 cut=0`

All snapshot domains maintain exact frame stamp parity with zero systematic single- or multi-frame drift.

### 4. S1 Fixture Motion Captures

Two complete S1 scenario runs were executed on `k00_marsh` with production passes enabled:
- **Flight 3 (`e04-s1-flight3-20260924`):** Exit code 0, scenario status `OK`, failures `[]`, 15.04 s video (`motion.mov`, 21.5 MB), contact sheet `motion-contact.png` generated.
- **Flight 2 (`e04-s1-flight2-20260924`):** Exit code 0, scenario status `OK`, failures `[]`, 15.04 s video (`motion.mov`, 20.5 MB), contact sheet `motion-contact.png` generated.

Visual inspection of 16-tile temporal contact sheets confirms smooth panning, 180° rotation, strafing, and walking without lagging scene elements, lingering trails, or mesh detachments.

### 5. In-Flight Queue Latency & Frame Pacing Evaluation (2 vs 3 Frames In-Flight)

Queue latency and frame pacing were measured using high-precision E01 telemetry on fixture S0 (20 s duration, 5 s warmup):

| Metric | 3 Frames In-Flight (`e04-flight3-timing`) | 2 Frames In-Flight (`e04-flight2-timing`) | Analysis |
|---|---|---|---|
| **Accepted Frames** | 929 frames ($\approx 46.5$ FPS) | 571 frames ($\approx 28.5$ FPS) | 3 in-flight delivers +62.7% higher throughput |
| **Displayed Interval p50** | 25.000 ms (40 FPS display cadence) | 33.333 ms (30 FPS display cadence) | 3 in-flight hits faster display refresh slot |
| **Displayed Interval p95** | 25.000 ms | 41.667 ms | 2 in-flight introduces interval jitter |
| **Drawable Wait (`drawable_ms`) p50** | **0.047 ms** | **13.266 ms** | 2 in-flight blocks CPU waiting for `nextDrawable` |
| **Drawable Wait (`drawable_ms`) p95** | 0.212 ms | 21.450 ms | Severe V-Sync backpressure stall in 2-flight mode |
| **CPU Scene / Encode p50** | 17.423 ms / 18.100 ms | 17.307 ms / 17.959 ms | CPU encode time is identical |
| **GPU Frame Duration p50** | 13.498 ms | 12.518 ms | GPU workload duration is identical |
| **Presentation Pacing Gate** | CLOSED (monotonic timestamps) | CLOSED (monotonic timestamps) | Both maintain clean presentation monotonicity |

**Conclusion on Queue Latency:**
- Under `flight=2` (double-buffered `CAMetalLayer`), the CPU blocks inside `nextDrawable` for 13.3–21.5 ms per frame while the display compositor holds the prior buffer for scanout. This causes missed VSync deadlines and drops throughput from 46.5 FPS to 28.5 FPS.
- Under `flight=3` (triple-buffered `CAMetalLayer`), `drawable_ms` drops to 0.047 ms (unblocked CPU pipeline), maximizing throughput and pacing stability.
- **Default:** Retain `MaxFramesInFlight = 3`. Opt-in `XRAY_METAL_MAX_FRAMES_IN_FLIGHT=2` remains available for latency-sensitive environments where lower throughput is acceptable.
- *Note:* Measured software queue latency is internal pipeline queue depth and is not equivalent to physical input-to-photon latency.

### 6. Gate Status

- **Status:** **AUTO PASS / VISUAL PENDING (Checkpoint V1)**
- All automated gates (CPU/GPU reprojection $\le 0.5$ px, frame stamp monotonicity, exit 0, scenario OK) passed.
- Stage completion awaits user visual evaluation of Checkpoint V1 as specified in Section 16 of `DIRECT_METAL_RELEASE_PLAN.md`.

## E03 evidence (2026-09-23)

The user screenshot [2026-09-21-user-report.png](./2026-09-21-user-report.png) shows an outdoor view with a large pale stretched shape and purple fragments around the buildings. The admitted S0 camera in [FIXTURES.json](./FIXTURES.json) is `(-181.132965, 6.035291, -272.460388)` on `k00_marsh`; [the E00 Metal no-HUD frame](../../scratch/release_runs/e00-final-admitted2-s0-d/scene_no_hud.png) and [the E02 S1 contact sheet](../../scratch/release_runs/e02-s1-normal-20260923/motion-contact.png) show an indoor room. Their manifest admission proves matching C/D inputs for that room, not reproduction or absence of the outdoor E03 defects. An S3 outdoor Metal capture [e03-repro-s3-metal-20260923](../../scratch/release_runs/e03-repro-s3-metal-20260923) exited 0 and does not visibly reproduce the reported shapes. A saved-camera candidate at `(-140.560, 2.783, -317.990)` was also indoors in [e03-exterior-candidate-d-20260923](../../scratch/release_runs/e03-exterior-candidate-d-20260923). Both are diagnostic only.

An aerial diagnostic camera at `(-160, 35, -330)`, looking toward `(-0.2, -0.4, 0.9)`, reproduced a pale stretched wedge over the base in [e03-aerial-scout-d-20260923](../../scratch/release_runs/e03-aerial-scout-d-20260923/scene_no_hud.png). Its 1920×1080 no-HUD frame has a contiguous low-chroma bright region at approximately `x=626..858, y=567..680`; `(700,625)` is an interior sample. The run exited 0. The [same-camera C capture](../../scratch/release_runs/e03-aerial-reference-c2-20260923/scene_no_hud.png) exited 0 and has no matching wedge. The paired C/D trace-run binaries both have SHA-256 prefix `b13e7b6204f86a`, and their registry, frozen save, camera and weather fields match. At `(700,625)` their final pixels are D `(35,38,44)` and C `(6,6,3)`; the local 233×114 ROI has MAE 15.15. This is diagnostic proxy evidence, not Windows A parity or the user's exact view.

The opt-in programmatic [GPU trace](../../scratch/release_runs/e03-aerial-trace-internal-d-20260923/e03-s0.gputrace) captured one complete frame and the process exited 0. `gpudebug` reports one command buffer, 63 encoders and 12,506 draws. Its [G-buffer albedo](../../scratch/release_runs/e03-aerial-trace-internal-d-20260923/gbuffer_albedo.png) already contains the white wedge. At pixel `(700,625)`, draw 4488 has RGB `(55,47,39)` and draw 4489 changes it to `(255,255,255)`; this precedes lighting, sky and forward passes. Draw 4489 is the eighth dynamic OGF submission (first dynamic draw 4482), using `deffer_model_flat SKIN_0`, 18 UInt16 indices, zero base vertex and a 36-byte vertex stride. The captured 10 vertices and all indices are finite/in range; local positions extend to z≈16.709 from a near group around z≈0.23–0.42. The GPU trace alone did not justify culling or hiding the draw.

The one-shot [draw census](../../scratch/release_runs/e03-draw-census-d-20260923/engine.log) maps ordinal 7 to `dynamics\light\projector`, geometry 0, object 16817, source shader `models\lightplanes`, base `lights\lights_cone_handing_lamp`, 10 vertices and 18 indices. Local [models_lightplanes.s](../../build/metal-shader-sources/r3/models_lightplanes.s) specifies `model_def_lplanes`/`base_lplanes`, depth test without depth write, `SourceAlpha, One` additive blending and sorting. The exact MO2-winning [DDS](../../runtime/macos-gamma/profile/gamedata/textures/lights/lights_cone_handing_lamp.dds) has SHA-256 `407b4346c79391f096eaff2a9ec723ea58816da48ff0a18162d423d9f1debfe9`; all 64 BC3 texels decode with alpha 0. The old dynamic path ignored the source family and routed it to opaque, depth-writing `deffer_model_flat` because `_bump` is absent. This is a material/pass error, not a proven vertex/index corruption.

The E03 fix compiles the exact active model lightplane VS/PS into [dynamic-world-e03](../../build/metal-runtime-shaders/dynamic-world-e03), validates MSC reflection, and routes `models\lightplanes` through a sorted, additive, depth-read-only pass after combine. The projector's physical/model components remain in their existing paths; only its lightplane geometry leaves the G-buffer and shadow caster path. The pre-existing six dynamic metallibs and reflection JSON files are byte identical in the new bundle. Post-fix [S0 D frame](../../scratch/release_runs/e03-lightplane-fix-s0-d-20260923/scene_no_hud.png) exited 0 with production passes enabled; the local ROI MAE versus the same-camera C proxy fell from 15.15 to 2.57. Post-fix [trace](../../scratch/release_runs/e03-lightplane-fix-trace-d-20260923/e03-s0.gputrace) shows the G-buffer sample `(700,625)` changed from white `(255,255,255)` to background `(55,47,39)` and a separate dynamic lightplane forward encoder with 11 real draws, including four six-triangle cone draws. Its PSO uses `src*SourceAlpha + dst*One`; depth state is `LessEqual`, write disabled. The 18-index cone still submits in that pass; it is not removed to make the image pass. The projector cone draw makes zero RGB change in the captured ROI, consistent with the source DDS alpha of 0. This closes the identified wedge mechanism on this diagnostic view, not the entire E03 stage.

A second, near-original courtyard camera gives a direct A/B: the [fixed frame](../../scratch/release_runs/e03-courtyard2-scout-d-20260923/scene_no_hud.png) has no wedge, while an opt-in [legacy-route probe](../../scratch/release_runs/e03-courtyard2-legacy-d-20260923/scene_no_hud.png) with the same fixture hash renders the pale stretched cone. Sky pixels `(500,300)` change from `(170,168,167)` to `(113,127,146)`. Both runs exited 0. Neither frame contains candidate high-magenta pixels, so this A/B establishes the lightplane wedge cause only. The legacy probe switch was then removed and the full game rebuilt; the final binary SHA-256 `8affdfdc94c77bb73ba898d44fd28d7c69abf5314159b379e3b48952d9dfe4ca` exactly matches the binary used for the fixed courtyard frame. No diagnostic legacy path remains in final code.

The player's current `player - autosave.scop` no longer matches E00's SHA-256. E03 diagnostic runs use the frozen E00 run copy through `XRAY_PARITY_SAVE_SOURCE_DIR`; the current save is untouched. The [separate diagnostic registry](../../scratch/e03_fixture_registry.json) and opt-in `--fixture-registry`/`--gpu-capture` runner switches record their own registry hash and mark these runs diagnostic, without changing E00's admitted fixture registry. The original screenshot is a whole-window image; the diagnostic ROI is not the exact original pixel, and no GPU draw/pass ID has yet been established. Missing `_bump` files and fallback receipts in the E00 log are material-census evidence only, not a causal explanation for purple pixels.

Follow-up diagnostics: [daylight aerial S0](../../scratch/release_runs/e03-bright-rapid-s0-d3-20260923/scene_no_hud.png) and [daylight aerial S1](../../scratch/release_runs/e03-bright-full-s1-d-20260923/scene_no_hud.png) exited 0 with production passes. S1's [15 s motion](../../scratch/release_runs/e03-bright-full-s1-d-20260923/motion.mov) reports scenario `OK` with zero failures; sampled start/middle/end frames show no return of the large wedge. This is a separate aerial fixture, not the admitted indoor S0/S1 or the exact player view. Two [ground](../../scratch/release_runs/e03-ground-projector-d-20260923/scene_no_hud.png) [candidates](../../scratch/release_runs/e03-ground-north-d-20260923/scene_no_hud.png) also exited 0 but do not reproduce the purple fragments. The frozen E00 save's [saved camera](../../scratch/release_runs/e03-saved-camera-d-20260923/scene_no_hud.png) and the current autosave's [saved camera](../../scratch/release_runs/e03-current-save-d-20260923/scene_no_hud.png) are both indoors. A simple high-magenta pixel census finds 1,384 candidate pixels in the original whole-window screenshot and zero in the daylight aerial and first ground candidate frames; it is a localization aid, not a material diagnosis. The original camera/save is still needed to establish a purple draw/pass ID. No purple fix or E03 gate PASS is claimed.

The near-original courtyard [full S1 motion](../../scratch/release_runs/e03-courtyard2-full-s1-d-20260923/motion.mov) also exited 0 with scenario `OK`, zero failures and 15 s of video. Fifteen frames sampled one second apart show neither the pale wedge nor any pixels passing the original high-magenta threshold; start/middle/end were inspected visually. This is strong negative evidence for the current diagnostic state, but it cannot identify the original purple draw or prove the original screenshot was captured with the same save, camera and material state.

The [S5 short diagnostic](../../scratch/release_runs/e03-lightplane-rapid-s5-d-20260923/scene_no_hud.png) and [S5 GPU capture](../../scratch/release_runs/e03-lightplane-trace-s5-d-20260923/e03-s0.gputrace) both exited 0 in another level. The active `lights\\lights_cone_handing_lamp.dds` was loaded there, and the forward encoder contains 28 actual lightplane draws across instances (30/8 triangles in pairs). A six-triangle projector cone is not visible in that captured S5 pass, so the exact projector instance is not yet verified as drawn in another scene. The repaired S0 capture independently contains four six-triangle cone draws. Earlier long S1/S5 attempts failed during window screenshot capture; refreshing the live window ID before each still image allowed subsequent runs to complete. Their termination stacks are not evidence of a renderer crash.

## E02 ownership evidence (2026-09-23)

The [frame-data contract](./FRAME_DATA_CONTRACT.md) defines the acquired-slot and immutable-range rules. Direct Metal's `UploadFrameData` now gives every differing AO/IL/blur draw, Flora/grass/local-light draw, water pass, rain/static-forward/LOD draw, HUD/scope/particle draw and mutable vertex stream its own slice. Legacy Shared MSC constants are copied at bind; Shared vertex/index bindings retain an exact native version and copy on later write. DynamicWorld selects the acquired backend slot on every `Prewarm`, even when its engine-frame ID repeats. AO and water history advance only after successful submission. The fallback DXVK path was not changed.

The GPU probe's `--old-path` exited 1 as expected: six reads from one overwritten Shared address did not retain their distinct AO/IL/blur patterns. Its `--self-test` passed with Metal API and shader validation; this includes six frames queued in waves of three, forced completed-slot reuse, two actual draws with different transform/color pixels, COW vertex/index versions, and bounds/alignment rejection. The correct CTest directory ran one registered GPU probe: **1/1 passed**. The full game target `Anomaly.DX11` also built. Existing adapter probe link dependencies were not treated as a pass; an attempted full-backend lifecycle executable failed to link against engine globals and was removed.

The final game link after a test-only GPU-probe extension and removal of an unused particle field has SHA-256 `134ff4288582e8ec9954ab1bda29addb80908b3a6b4264a5168569eb222b9292`. Runtime S1 and the after timing run used the earlier `fc73a030e9d6e38e855b7601dc468d8a907519619b893694fc10fe9052306075` binary. No production draw/ownership logic changed between those links; exact-final-binary runtime verification remains open.

The first S1 validation attempt exposed an existing final-water PSO mismatch (RGBA16Float pipeline on an RGBA8Unorm attachment) and exited 134. After matching the production attachment format, [`e02-s1-validation2-20260923`](../../scratch/release_runs/e02-s1-validation2-20260923) exited 0 with no Metal assertion/backend error; its TLAB report was 3 blocks / 48 MiB. Validation slowed the scene enough that this recording did not exercise the intended motion. The separate normal [`e02-s1-normal-20260923`](../../scratch/release_runs/e02-s1-normal-20260923) exited 0, reported scenario `status: OK` with zero failures, and recorded [15.09 s of motion](../../scratch/release_runs/e02-s1-normal-20260923/motion.mov). Start/mid/end frames and the [contact sheet](../../scratch/release_runs/e02-s1-normal-20260923/motion-contact.png) show camera movement without an obvious new Shared-buffer overwrite. The scene remains dark; this is diagnostic E02 evidence, not visual parity or E03/E04 acceptance.

One same-fixture S0 timing pair used 30 s warm-up and 60 s sample, with validation/capture disabled and exit 0 on each binary: [`before`](../../scratch/release_runs/e02-before-s0-20260923) SHA `bdc80f37900264bd28b711ea5cde7331c6da621d6dec36d806d234fb97474b58`, [`after`](../../scratch/release_runs/e02-after-s0-20260923) SHA `fc73a030e9d6e38e855b7601dc468d8a907519619b893694fc10fe9052306075`. Display p50 stayed 41.667 ms; p95 was 145.208 → 100.000 ms, CPU scene p50 28.108 → 27.744 ms, GPU frame p50 20.928 → 14.446 ms, buffer allocations p50 62 → 57, and updated bytes p50 19.581 → 22.979 MB (decimal). This is a noisy single pair with multiple correctness changes and does not establish a causal performance gain. The 60 FPS gate remains failed/open.

## E00 exit evidence

The authoritative manifest is
[`scratch/release_runs/e00-final-admitted2-s0-manifest/parity_manifest.json`](../../scratch/release_runs/e00-final-admitted2-s0-manifest/parity_manifest.json).
It records:

- `comparison_admission: ADMITTED`, `visual_status: DIAGNOSTIC_ONLY`, and an empty `validation_errors` list;
- exact agreement for camera position/direction, view/projection matrices, FOV/aspect, weather parameters and sun direction;
- fixture `S0`, level `k00_marsh`, save SHA-256 `24d870ac646aadd09dac577da726153225be35513daefae048b37709b3058d97`;
- material/profile SHA-256 `af6b5eba213ed391f5d37c3de0ea7e72763729d0efe392cdc43fe08b7ea7bfaa`;
- final fixture-registry SHA-256 `17583d9d9adb8c100fb2a5d775ad146e3027539ca52d08f07190215ed915f873`;
- binary SHA-256 `38d652e2e686109fc40360379d162820d614bb640308c717053b97f7c6a6e87b` in every run;
- `engine_exit_status.txt = 0` for C1, C2, C3 and D.

Final runs:

- [`C1`](../../scratch/release_runs/e00-final-admitted2-s0-c1)
- [`C2`](../../scratch/release_runs/e00-final-admitted2-s0-c2)
- [`C3`](../../scratch/release_runs/e00-final-admitted2-s0-c3)
- [`D`](../../scratch/release_runs/e00-final-admitted2-s0-d)

The three C no-HUD repetitions establish a proxy noise floor of MAE 0.4524–1.0141
(PSNR 41.98–47.01 dB). Direct Metal versus C is materially different:
no-HUD MAE 16.2245 / PSNR 18.43 dB and with-HUD MAE 16.4557 / PSNR 18.35 dB.
Those values are diagnostic evidence for E03–E06, not a parity verdict.

## Fixture registry and scripted scenarios

[`FIXTURES.json`](./FIXTURES.json) contains ready contracts for S0–S7, exact save hashes,
expected levels, camera policy, weather/time, warm-up, scope-cache requirement and A/B/C/D
evidence-source state. [`e00_fixture_scenarios.script`](../../scratch/e00_fixture_scenarios.script)
implements seconds-based, fail-closed S1–S7 timelines with restoration and spawned-object cleanup.

Validated scenario captures include:

- S1 pan/turn/strafe/forward/back: [`e00-validate-s1b-c`](../../scratch/release_runs/e00-validate-s1b-c)
- S2 0/1/4/8 online NPCs plus cleanup: [`e00-validate-s2c-c`](../../scratch/release_runs/e00-validate-s2c-c)
- S3 water/shore turn and step: [`e00-validate-s3c-c`](../../scratch/release_runs/e00-validate-s3c-c)
- S4 outdoor/shadow-split traversal: [`e00-validate-s4-c`](../../scratch/release_runs/e00-validate-s4-c)
- S5 underground torch and existing online NPC movement/restoration: [`e00-validate-s5e-c`](../../scratch/release_runs/e00-validate-s5e-c)
- S6 clear-to-storm-to-clear transition: [`e00-validate-s6-c`](../../scratch/release_runs/e00-validate-s6-c)
- S7 visible first-person weapon plus fire/reload/ADS/NV/thermal event path: [`e00-validate-s7f-c`](../../scratch/release_runs/e00-validate-s7f-c)

All accepted scenario telemetry reports `status: OK`, an empty `failures` list, and a completed
restore event. S7 also exposes a follow-up visual defect: the initial with-HUD capture contains the
weapon, while some post-switch motion frames omit it. That is retained as visual-correctness evidence
for later renderer stages and does not weaken the E00 reproducibility contract.

## Resolved E00 blockers

The missing scope bytecode blocker is closed. An offline MinGW helper loads the local Microsoft
`d3dcompiler_47.dll`, compiles with exact row-major flags/defines and verifies DXBC/reflection/source
CRC before materializing cache records. Provenance is stored under `tools/e00_dxbc/out-*/` for:

- `model_scope_lense.ps` (SKIN_2), cache SHA-256 `b27c2b8d024d3ee68494e99a739c4b0da25d46acbbfb99031359b60d8b7344ac`;
- `models_reflex_reticle_simple_3db.ps`, `305c699b5bdfba75d1f7c63133207629a9ff444749babcad73c281807e4f2c4b`;
- `models_laserbeam.ps`, `dafb0ece06470c5652d882f501259b996cb61c58d5766ab3fc159d5aa308d927`;
- `models_scope_reticle.vs`, `86bfced069b147c8f221f2f2a0e5010e5d6a3928d00c50f4a0fa81ed92f4851d`.

The intermittent exit-134 failure is also closed. The engine no longer starts a Discord worker after
Discord initialization fails, and logger shutdown now waits for its detached worker to acknowledge
exit before destroying mutex-backed event state. The runner rejects any nonzero engine exit.

## Reproduction

```sh
script/run_metal_release_fixture.sh --execute --fixture S0 --backend dxvk --run-id <unique-c1>
script/run_metal_release_fixture.sh --execute --fixture S0 --backend dxvk --run-id <unique-c2>
script/run_metal_release_fixture.sh --execute --fixture S0 --backend dxvk --run-id <unique-c3>
script/run_metal_release_fixture.sh --execute --fixture S0 --backend metal --run-id <unique-d>

python3 script/assemble_release_parity_manifest.py \
  --ref1 scratch/release_runs/<unique-c1> \
  --ref2 scratch/release_runs/<unique-c2> \
  --ref3 scratch/release_runs/<unique-c3> \
  --metal scratch/release_runs/<unique-d> \
  --output scratch/release_runs/<unique-manifest> \
  --expected-level k00_marsh --expected-fixture S0
```

Validation gates run for this closeout:

```sh
cmake --build build --target Anomaly.DX11 -j 4
python3 script/test_parity_manifest_contract.py
python3 script/e00_fixture_contract.py --registry docs/direct-metal-release/FIXTURES.json --check
bash -n script/run_metal_release_fixture.sh script/e00_compile_scope_dxbc.sh
luajit -b scratch/e00_fixture_scenarios.script /tmp/e00_fixture_scenarios.luac
luajit -b scratch/zzzz_visual_parity_harness.script /tmp/zzzz_visual_parity_harness.luac
```


## E06 safe stop — 2026-09-27

The user reports the Bar circles seem gone, but other problems appeared, and requested stopping. Corrected AO/IL bindings and live combine controls are saved; main/bundle restored to the verified corrected binary. Main build, relevant GPU probe and Bar API/GPU-validation run passed; exact new-game Street before/after comparison remains unfinished. No further runs are scheduled; full E06/reference/V2 stay OPEN. [Regression and resumption state](E06_CIRCLES_FIX.md).
