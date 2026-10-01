# Packet 3 — caster coverage and the R4 sun cascade contract

2026-09-27. **Active Bar contract repaired and GPU verified.** This is controlled
Direct Metal evidence, not a Windows image-parity verdict. E05 remains open for
A4 (atlas overflow) and the limits below.

## Production changes

- Sun now uses three source-sized maps `(20,60,160)`. These values are orthographic
  footprint sizes, **not camera-Z split distances**. `R_sun_fixed_support.h`
  contains the unchanged Windows `FixedConvexVolume`; the engine bridge carries
  its caster planes, ray chaining, source reset conditions, light translation,
  pixel snapping and receiver cuboids into Metal. Far accumulation uses the
  **middle** cascade's receiver cuboid with the far map/lookup. Near/middle use
  source GE depth and masked stencil zero; far keeps stencil and honors z-cull.
  Source per-cascade bias is `-2.5e-6 * size`.
- Sun and local shadow graphs call Flora and conditional DetailGrass alongside
  LMH, DynamicWorld, DefVertex and DefObjectsLod. Grass obeys the source option,
  cascade index, fade, light distance/frustum/decorative gates and full 3D local
  light range at the decompressed slot center. Sun fade uses squared camera
  distance; local fade uses squared XZ distance. Grounded slot bounds reproduce
  the source cache center, rather than using individual blade positions.
- `shaders.xr` tree blender properties determine alpha and Object-LOD selection.
  Bar reports `flora\leaf_wave` alpha=1 / Object-LOD=0 and
  `flora\trunk_wave` alpha=0 / Object-LOD=0. Visible regular-tree leaf/trunk
  shaders and their wind setup now match those properties. Shadow shaders keep
  the original regular-tree vertex deformation and source alpha threshold.
  The grass wrapper calls the original grass PS, retaining alpha/dither, and
  changes only its output representation to R32 shadow depth.
- Dynamic capture unions the primary view with actual sun caster frusta and
  admitted local-light range queries. Non-skinned shadow-only objects load
  geometry without allocating a primary-view instance. CPU-skinned casters and
  parent/leaf no-shadow flags remain in the producer.
- Sun `Ldynamic_color.w` now receives source `u_diffuse2s`, rather than luminance;
  the source SSFX controls are also bound. The prior value could drive the active
  PBR roughness expression to zero and produce NaN. Final captures contain no
  nonfinite sun accumulation channels. This does not close other A5 issues.

Main files: `MetalR4EngineScene.mm`, `MetalBootstrap.mm`,
`MetalR4DynamicWorldAdapter.cpp`, `MetalR4FloraAdapter.cpp`,
`MetalR4DetailGrassAdapter.cpp`, `MetalR4EngineFs.cpp`,
`MetalR4SunCascadeCapture.h`, `MetalR4FloraMaterialCapture.h`,
`r__dsgraph_render.cpp`, `r__dsgraph_build.cpp`, `dxEnvironmentRender.cpp`.

## Verification and artifacts

Evidence root: `scratch/release_runs/20260927-shadow-casters/`.
Final binary SHA-256:
`d0173dc167aa282a55e5f4d6e4105c5667a29e14dc73573996095928c8395e6a`.
`final-source-manifest.json` records the source bytes. Shader bundles are
`build/metal-runtime-shaders/flora-wind-v3` and `shadow-casters`, with their
receipted source/compiler/artifact hashes. DXC 1.9 and MSC 4.0.1 were used.

The main application and four focused CPU probes build successfully
(`build-integration-eleventh.log`). CTest: **4/4 PASS**
(`cpu-tests-eleventh.log`): shared cascade math, static shadow-only cache
admission, Flora material/vertex contract and grass slot/range/fade contract.

| Final runtime | Grass | Primary readbacks | Scenario / process | Native verifier |
|---|---|---:|---|---|
| `20260927-shadow-sixth` | on, `(3,1,100,0)` | 21 | OK / exit 0 | PASS |
| `20260927-shadow-seventh` | off, inherited profile | 23 | OK / exit 0 | PASS |

Both runs use the same binary, exact Bar save SHA
`f78e11c4ac6d7b956102cb523bf5f34507ded87a4e73c6f20a28565391ca2003`,
`w_clear1` at 12:00, observed first-eye turn/pan/hold/cut/return, and a
1280×720 drawable (640×360 logical window). API and shader validation are
on; the logs contain no detected Metal validation error. Profile/mod fallback
script errors are still recorded separately. These runs are explicitly
`DIAGNOSTIC_ONLY`, not E00-admitted comparisons.

The fixture adds a source-derived Bar wood mesh through a new-file-only overlay
in each isolated runtime; the player's profile is not edited. See
`bar_static_shadow_fixture_manifest.json` and each run's `fixture_overlay.json`.
A source CSE spawn flag is changed through a byte-preserving STATE roundtrip,
with graph/node/layout checks, to prevent AI-grid height snapping. The item is
first created outside the scene, then enters near online admission at height
8 m. NPC pose control retains actual engine meshes/current CPU skinning.
Cleanup deactivates the owned Lua lights and releases the spawned objects.

For local tests the fixture creates **real `script_light()` point and spot
lights**, then an opt-in position selector retains their captured records.
Point expansion yields six OmniFace records; spot adds one. This prevents the
known 16-tile overflow from excluding the test lamps. Ordinary production
capture is unchanged when the selector is absent. It does **not** repair A4.

### Actual cascade boundaries

`MetalR4SunShadowValidation.mm` blits production packed G-buffer position,
Depth32/Stencil8, R32 shadow maps and RGBA16F accumulation after the sun mask
and each near/middle/far accumulation pass. It reconstructs packed view
position with the actual `pos_decompression_params`; it does not treat the
packed normal XY as position XY.

The coverage oracle uses the **exact uploaded float4 cuboid vertices**, source
triangle indices, current VP, Metal CW/back culling and depth/stencil rules.
Homogeneous winding handles negative-W vertices when the camera is inside the
cube. No G-buffer volume-membership predicate is added to the GPU rules:
these PS variants do not discard fragments. Both extra stencil writes and
missing expected writes are required to be zero; far must preserve stencil.
All 44 final four-phase reports pass. Source size/bias are recorded per phase.
Shadow-map nonfinite pixels and sun accumulation nonfinite channels are zero.

Raster edges and the explicitly reported ±2e-6 depth comparison uncertainty
band are excluded from this numerical oracle; those boundary pixels are not
classified. Aggregate ambiguity and detailed samples are retained, so a PASS
is not a claim of exact classification of every edge pixel.

### Casters outside the primary camera

An independent adapter starts with **empty geometry cache**, prewarms only
actual off-camera non-skinned source objects and never draws a primary view.
Logs show `outside_objects=1 cache_before=0 cache_after=1 main_draws=0`.
Isolated maps use production converted draw paths and are projected onto the
actual primary G-buffer receivers. Thus these are GPU caster/receiver overlap
checks, not admission counts or synthetic matrix tests.

Maximum sampled shadowed receiver pixels across the two final runs:

| Family | Sun | Local spot | Local OmniFace |
|---|---:|---|---|
| Off-camera Flora | 6,107 | PASS | PASS |
| Cold non-skinned item | 33,460 | PASS | PASS |
| Off-camera CPU-skinned NPC | 1,888 | PASS | PASS |
| Grass, option on | 2,195 | PASS | PASS |

Local item/NPC/Flora witnesses are required to have positive receiver overlap.
Grass witnesses require actual finite covered map pixels; positive receiver
samples are also present for both spot and OmniFace. Each family has zero
nonfinite map pixels. A face/cascade with no relevant caster or receiver is
reported as `NO_COVERAGE`, not promoted to PASS. See `verified-sixth.json`,
`verified-seventh.json`, raw reports, and `final-summary.json` for all counts.
These isolated witnesses do not attribute a final composited RGB pixel to one
caster when other production casters overlap the same receiver.

### Alpha and wind

Same-pose pairs retain original AREF VS and differ only in alpha discard PS.
The fence pair restricts actual level batches to `mtl_fence` textures (17
batches), separate from Flora. At frame 110:

| Pair | Changed depth pixels | Removed covered pixels | Added covered pixels |
|---|---:|---:|---:|
| Flora opaque → alpha | 1,419,403 | 158,712 | 0 |
| Fence opaque → alpha | 17,314 | 17,309 | 0 |

During the 20–40 s camera hold, frames 110 and 115 have bit-identical camera and
light matrices. Flora changes **1,263,217** depth pixels; near grass changes
**33,159**. All maps are finite. The original wind inputs advance while the
camera/light transforms stay fixed. See `alpha-flora-sixth.json`,
`alpha-fence-sixth.json`, `wind-flora-sixth.json`, `wind-grass-sixth.json` and
corresponding `.r32` exports. Grass shadow passes and witness exports are absent
in the off run, as required by the inherited option.

## Limits and remaining release work

- The current profile has grass shadows **off**. Their absence in the original
  audit frame was not itself a defect. The on run is a controlled option test.
- Windows same-fixture images remain unavailable; brightness, full PCF/filter
  appearance and visual parity are not accepted by these native checks.
- A4 atlas capacity/selection remains open. Local verification deliberately
  stays at seven real shadow records; it does not establish >16 completeness.
- Metal's existing portal/HOM sector-visibility fallback remains. The Windows
  detail draw additionally checks base/light outdoor-sector visibility; the
  Metal backend has no loaded D3D portal graph to supply those flags. This
  outdoor Bar evidence does not verify indoor portal filtering.
- The observed regular-tree alpha leaf / opaque trunk permutation is admitted.
  Object-LOD tree variants, opposite blend combinations, other quality/detail
  permutations and `ssfx_branches` off remain outside this cook's acceptance.
- NPC previous bones/identity generations remain packet 2 gaps. This packet
  verifies current CPU-skinned shadow geometry, not temporal bone history.
- Standalone `AnomalyMetalBootstrap` remains outside acceptance; its pre-existing
  Engine/Core/Theora link failure was not repaired. The production main target
  was built and executed.
- No FPS/performance conclusion is made. Validation/readback/isolated exports
  intentionally add substantial work.

Earlier attempts are retained. First run was stopped during diagnosis and is
not a clean-exit PASS. Runs 2–4 used earlier oracle/admission fixes; run 5 had a
failed static-fixture Lua API call despite passing primary stencil checks.
Only runs 6/7 meet the final native family and scenario gates.
