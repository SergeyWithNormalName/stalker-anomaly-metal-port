# E06 Bar light-circle regression — stopped at user request, 2026-09-27

**State:** confirmed AO/IL binding defects corrected; user reports the circles seem gone but other problems appeared. This is a provisional observation, not final visual acceptance. The user explicitly requested stopping. No further diagnostics/builds/game runs are scheduled. Full E06, Windows reference and V2 remain OPEN.

## Corrected contract

- AO raw: t1 is history, t2 motion; s0 nofilter, s1 linear. These pairs were crossed.
- AO blur: t0 is position/depth, t1 AO image; s0 nofilter, s1 linear. Both texture and sampler pairs were crossed. As a result, the old blur read packed G-buffer material/hemi data as AO alpha. `combine_1` uses AO `.www` to multiply ambient/hemi while additive lighting remains visible; this can produce bright pools against erroneous dark surrounds.
- IL raw: t0 position, t1 accumulator, t2 IL history, t3 motion. The former order was accumulator/position/motion/history.
- AO blur sampling scale is scale/1/1/1, matching the literal Windows phases. The earlier review's all-four-scale statement was corrected; current profile scale1 made that discrepancy dormant.
- Combine globals at288/304/320/336 now use the complete live frame-stamped AO/IL control vectors. The IL consumer previously used scale1 while the producer uses6.666667.

Only `MetalR4SsfxAoAdapter.cpp` and `MetalBootstrap.mm` changed for this regression. No HLSL/artifact was changed, and AO/IL/local lights/bloom remain enabled. [Independent GPT-6 Luna binding review](../../scratch/release_runs/20260927-e06-circles/review-luna.md); [focused delta](../../scratch/release_runs/20260927-e06-circles/circle-fix.patch).

## Evidence and limits

The corrected source built and linked. The relevant `FrameUploadGpu.SelfTest` passed with API/shader validation ([build](../../scratch/release_runs/20260927-e06-circles/build-final.log), [test](../../scratch/release_runs/20260927-e06-circles/tests-final.log)). The corrected [Bar courtyard run](../../scratch/release_runs/20260927-e06-circles-Courtyard-final/) exited0, enabled both Metal API and GPU Validation, exercised5+5 AO/IL passes, and had zero validation reports. Its actual camera matches [before](../../scratch/release_runs/20260927-e06-circles-Courtyard-before/): `(128.175995,5.176,8.666)` / `(-0.005999,-0.335950,0.941861)`. These courtyard frames show changed shading, but do not reproduce the precise reported circles.

The user clarified the screenshot was near the new-game Bar spawn exit. Local `new_game_start_locations.ltx` places the stalker lodging spawn at `(115.59,1,65.61)`. The existing `stalker - autosave` was frozen; SHA256 `f78e11c4ac6d7b956102cb523bf5f34507ded87a4e73c6f20a28565391ca2003`. The historical user-session camera log established the actual walk to `(162.458,1.656,62.772)`. The [Street-before diagnostic](../../scratch/release_runs/20260927-e06-circles-Street-before/) exited0 and reached the correct pictured buildings, but the user stopped work before a same-camera corrected Street capture. Therefore no isolated final-image proof for the exact reported rings is claimed. Earlier saved-camera, POI and Duty scouts are locator diagnostics only, not acceptance evidence.

## Safe stop / resumption

A/B experiments temporarily restored the two pre-fix sources. On stop, both corrected sources and the already-built corrected executable were restored, including `Anomaly.app/Contents/MacOS/AnomalyDX11`. Both executable copies were hash-verified: `4282715a54a91b89b3df7b9b98bb419f50cd2e1dd60333cbe976506e7b142e0e`. Source/artifact stop-state hashes are [recorded](../../scratch/release_runs/20260927-e06-circles/stopped-candidate-source-hashes.json). No game process remains; both Luna workers finished. No commit, staging, reset or push occurred; unrelated WIP and live saves were preserved.

Build intermediates may still contain the temporary before-source objects. Before any resumed work, run the normal main/probe build against the restored source; do not assume a clean incremental build. The remaining exact Street comparison uses [BAR-NEWGAME-STREET.json](../../scratch/release_runs/20260927-e06-circles/BAR-NEWGAME-STREET.json) and its frozen save. First collect the newly reported problems when the user resumes. Do not resume SSR or another stage automatically.
