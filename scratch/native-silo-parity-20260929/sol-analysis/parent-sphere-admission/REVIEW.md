# Whole-parent camera sphere: one bounded observer

2026-09-29. Scratch-only proposal. Production sources were read/copied but never edited by Sol; no build/game/GPU work. Rendering and all existing settings remain unchanged by this patch.

The next measurable hypothesis is that native's loose point-parent admission retains whole six-face packages that Windows rejects with the parent's actual spatial sphere. This is distinct from the old opt-in face-cull flag: that flag clips individual faces and previously removed only7–13; this census tests the earlier parent admission boundary and performs no culling.

## Source proof

- Windows establishes `ViewBase` from current `Device.mFullTransform`, L/R/T/B/Far (`r4_R_render.cpp:113`), passes it to `GMBase.traverse(:135)`, and `r__dsgraph_manager.cpp:53` copies that same camera frustum to `i_frustum`. `r_dsgraph_capture_lights(:516)` queries light spatial spheres against it before LOD/sector/package export. HOM, portal/sector and occlusion stages are excluded from this proposal.
- `light::spatial_move(:305)` gives a point parent sphere exactly `(position, range)`; a spot has a cone-dependent shifted minimal enclosing sphere. The observer reads the live authoritative `SpatialComponent->spatial.sphere`; it does not reconstruct spot spheres or use an OMNIPART child's shifted sphere.
- Native `planLocalShadowBatches(:2581)` instead tests each OmniFace's transformed `[-1,1]^3` box and keeps all six if any box intersects. `makeSpotVolumeModel(:390)` scales this box by `2*range` on all axes. Its enclosing radius can be `2*sqrt(3)*range`, while the source parent radius is `range`.
- The actual qualified `du_sphere_part` volume is half-radius (`MetalR4LocalLightAdapter.cpp:195`). A read-only vertex census of its82 vertices finds bounds(-.353553,-.353553,0)..(.353553,.353553,.5), maximum radius.5000007623 (rounding), before the2*range model scale. This proves loose native admission, not a current-frame wasted-work count or permission to suppress.

The completed R1-OFF frame360 retained63 dynamic shadow faces; its kept work is2,572 material rows,354 main static draws,683 submitted dynamic-caster proxies,802 Flora shadow draws and658,432 transform bytes. Of these sources, three spots are volumetric and every captured source HUD flag is0. These are an upper bound for this old frame's remaining work, not the new ON run's counts. The current R1 observer JSON contains no spatial positions/radii, so an exact source-sphere intersection cannot be recovered from those saved IDs. Pointer-derived IDs are process-local.

Fog remains deferred: the measured profile freezes fog_distance638/far_plane648. Source `Light_DB.cpp:200` has a pure fog-center gate, but this view provides no measured reason to prioritize it. LOD is also separate; source `light::get_LOD(:662)` writes `distance`, so this observer never calls it. Source LOD/color semantics and the dormant collector are not reused.

## Frozen patch and provenance

`parent-sphere-observer.patch` changes only Device and EngineScene. Its pinned baseline is the currently applied R1 gate **plus** the already compiled R1 observer: Device12b16a921963c60f1b512abafddd7b4c7a37bf7f2f5c77b002136f43be085a0c, Scene3cf3d743d40aece9ffd1aa9fd61a1726840bcb8f31fd04e3fab70e7c1269322f. It adds no public field, backend hook, shader/resource update, draw wrapper or per-draw clock. The original frozen R1 patches and parser are untouched. Apply it only to the matching baseline, or have Root explicitly rebase the scratch diff.

Strict default-off controls:

```text
XRAY_METAL_PARENT_SPHERE_OBSERVE=1
XRAY_METAL_PARENT_SPHERE_OBSERVE_FRAME=700
```

Absent frame defaults700; malformed/zero/sentinel input disables the observation. Both TUs read controls once. Only that one engine frame allocates/logs parent metadata and work rows.

Device copies its current VP and cached `SetCacheXform` view/projection, requires same engine/cache/device frame and camera eye, recomposes P*V with the same `Fmatrix::mul` as `device.cpp:504`, and requires exact VP bits. Only then does a stack-local `CFrustum` classify each copied source sphere. Unsupported sphere/view input is recorded unknown, never filtered. The existing source-owner maps and pointers stay inside this same End call; only printed values cross the observation boundary.

EngineScene logs full float bits of the **accepted** view/projection/eye after view-history selection and actual successful local-shadow planning. It records current snapshot/environment frames, scope/HUD receiver presence and volumetric-frame presence. The offline parser requires those arrays to be bit-identical to the captured source arrays. Thus its source VP validation is transitive: live Device VP exactly equals current cached P*V, and those same P/V/eye are accepted by the scene. Any alternate, fixed, stale or history-modified view is unusable; there is no tolerance relaxation.

After actual tile draws, existing material/static/dynamic/Flora/transform counters are joined to immutable parent IDs. No shader/pipeline/bind/stencil/draw/order/admission change occurs. Logging before a batch's final encoder close is qualified only by a unique completed E01 acquisition for that engine frame; submit error/retry/duplicate or missing completion rejects the census.

## Root's shortest test

After the current clean R1 pair, Root can use its existing matched harness with the observer controls above, `XRAY_METAL_R1_STATIC_POLICY=1`, live `r2_allow_r1_lights off`, E01 enabled only to obtain completion/count provenance, and all other observers/culling/probes off. Choose the frame from actual phase ledgers;700 is a default, not a promise that the frame is measured. No GPU readback is needed for this cost census.

```sh
python3 <XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/parent-sphere-admission/summarize_observer.py \
  "$XRAY_PARENT_OUTPUT/engine.log" --frame 700 \
  --e01 "$XRAY_PARENT_OUTPUT/e01-frames.csv" \
  --device-phases "$XRAY_PARENT_OUTPUT/phases.csv.device.csv" \
  --require-corrected-r1 --output "$XRAY_PARENT_OUTPUT/parent-sphere-summary.json"
```

Parser admission: exactly one capture/done/accepted view; matching full matrices and eye bits; same finite current source/snapshot/environment frame; exactly one completed E01 and phase1 device row; unique final lights/tiles; complete six-face point or single-face spot packages; source-static/type joins and transform counts reconcile. An unknown sphere contributes no outside prediction. It reports all/source-inside/source-outside/unknown actual work separately and the ordinary dynamic outside subset excluding source HUD/volumetric parents and active scope receivers.

That ordinary subset is a **cost opportunity**, not a certified suppression set. Primary HUD receiver behavior is still recorded for review. Before any runtime gate, qualify deferred/HUD/3DSS/scope, volumetric, forward/translucent and any other camera consumers against the exact source policy and accepted current view. Camera transitions/tangent spheres and current frame parity need their own images. No previous-frame prediction, HOM, GPU wait, second prewarm or history cache is proposed.

Read-only `git apply --check` passes. Nine scratch synthetic parser cases pass: positive complete group arithmetic, projection mismatch, stale cache frame, unmatched VP, duplicate capture/retry, incomplete Omni group, warmup, GPU error and absent R1 gate. This verifies the evidence rejection path only; no C++ build or game observation is claimed.

If actual ordinary outside work is zero, stop. If it is only a few faces/material rows comparable to the prior face-cull opportunity, stop rather than build a new runtime gate. Material opportunity must come from the successful measured-frame join; do not turn the old nested local-shadow timer or this geometric bound into an FPS prediction. A useful census can justify a separately reviewed small current-parent camera gate; suppression is not part of this draft.
