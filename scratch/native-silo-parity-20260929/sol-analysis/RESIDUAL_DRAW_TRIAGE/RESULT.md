# Residual draw-family triage — fast portal-on indoor run

## Decision

The corrected measurement window confirms that main Flora is materially smaller after the per-frame portal mask: the portal CSV records 5,316 post-camera-cull draws reduced to 1,260 submitted draws, with 4,056 actual mask skips. This does not expose a clear next optimization by draw count alone. I do not recommend a new draw-suppression or batching candidate from this receipt: the largest remaining family counts either already have substantial culling/merging, are spread across shadow views, or lack isolated timing attribution.

A narrow next step, if root wants another ROI, is to measure one family’s own CPU encode interval inside the existing scene callback before changing its draw path. Dynamic OGF reports 819 draws in an in-window engine frame, but also 822 cache hits, one miss, and 768 pose reuses; there is no evidence here that another cache/suppression change is needed. The log does not provide a family-specific GPU duration.

## Measurement-window evidence

`phases.csv` marks `measure_start frame=572` and `measure_end frame=1154`. E01 completed rows cover engine frames 572–1154 inclusive (583 rows; serial 570–1152). Their medians are `scene_ms` 28.395 ms, `dynamic_prewarm_ms` 5.195 ms, `encode_ms` 30.425 ms, GPU interval 22.479 ms, indexed draws 10,715, and PSO binds 7,060. These are separate scopes; do not add scene, prewarm, encode, or GPU values.

The portal CSV has the same 583-frame interval. Median values are 10,373 prepared exclusions, 4,056 actual post-ordinary-clip skips, 1,260 draws, and 6,347 ordinary-camera culls. The range is 1,260–1,261 draws; skip counts are 4,055–4,056. Wind unknowns range 37–41 and owner unknowns remain 8. This is the per-frame source for the main-Flora measured count; the `METAL_FLORA_FRAME sample=900` text log uses a telemetry sample index, not an engine frame, and is not used to align this result.

## Family counters with in-window engine-frame stamps

The following family records carry actual engine-frame stamps within 572–1154:

- At engine frame 927, detail grass reports 552 draws. Dynamic OGF reports 819 skinned draws/draws, 834 candidates, 9 frustum rejects, 822 cache hits, one miss, and 768 reused poses.
- At engine frame 927, local shadows report 81 rendered lights, 3,164 submitted static caster rows, 163,048 culled static casters, 739 submitted dynamic casters, and 44,801 culled dynamic casters. E08 static merge reports 440 encoded draws from those 3,164 static rows, across 179 keys. Dynamic caster count is not asserted to equal draw calls.
- At engine frame 1080, static sun-shadow reports 129, 342, and 518 map draws across the three cascades (989 reported static draws total). Flora shadow telemetry reports 44, 116, and 306 draws per cascade (466 Flora shadow draws). These are shadow passes, separate from main Flora.

LMH, terrain, static-forward, def-vertex, and def-objects-LOD logs use their own sample counters (the printed `frame` values are not `environment.engineFrame`). They are intentionally omitted from the in-window engine-frame comparison rather than treated as joined observations.

These counters show where work exists, not which family dominates CPU or GPU time. The existing local static-shadow merge already collapses 3,164 source rows to 440 encoded draws; sun-map counts do not show a large candidate-to-draw gap; and the high dynamic-OGF count accompanies high cache/pose reuse. No family-specific timing or isolated visual proof for a new candidate is present, so the evidence supports measurement before another behavior change.

## Scene-order anchors

In `MetalR4EngineScene.mm`, sun shadow passes draw static/dynamic/def casters and then Flora/detail-grass shadow casters before ending each cascade (around lines 4534–4548). The main G-buffer draws the level, def-vertex, def-objects-LOD, and terrain, then invokes the world callback (around lines 4617–4630). That callback draws Flora first, detail grass second, and dynamic OGF third (around lines 7231–7288). Local-shadow and merge telemetry emission is around lines 7785–7803. These source anchors explain ordering only; they do not make the differently sampled telemetry counters a same-frame sum.

## Provenance

SHA-256:

- `engine.log`: `871a020d5bf847e98753ac00c0ef4bcc8f8de588e62bbac2ef1d4da1660dbead`
- `e01-frames.csv`: `c636164aa3f329a4f501c6f89eecfa1bf89225a683bc03567b6887aa3aba010d`
- `portal-perframe.csv`: `21dc0b7a67058201a41e791b6326f35299f8ebb34ef2df88791712fde1c19886`
- `phases.csv`: `780195a1afa26cef3ce265d2b5c9a2f6484947a70fa04ad523b850636558cbf9`
- `MetalR4EngineScene.mm`: `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`
- `MetalBootstrap.mm`: `fb1b1052704eab1756bb12bdbc8bcdd46f96c5962481de1aa5e59c2b793ad27c`

No game, build, or sample was run for this correction; no source changes were made.
