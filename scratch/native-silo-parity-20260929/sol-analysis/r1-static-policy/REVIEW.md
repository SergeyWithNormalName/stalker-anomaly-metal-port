# R1 static-light source policy: confirmed native package-admission defect

2026-09-29. Source/evidence audit and SCRATCH drafts only. No production application, build, game/GPU run or benchmark by Sol. Root owns observation, correction gates and any performance screen.

The matched native/Silo config says `r2_allow_r1_lights off` ([native user.ltx:377](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/runs/static-pso-off-01/native/runtime/appdata/user.ltx:377), [Silo matched control:377](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/runs/windows-control-01/silo/runtime/appdata/user.ltx:377)). The on/off static-PSO runs both retain this setting. The console command maps directly to `R2FLAG_R1LIGHTS` ([xrRender_console.cpp:1199](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/xrRender_console.cpp:1199)); the default flag initializer also omits that bit ([line176](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/xrRender_console.cpp:176)). The recorded matched Lua profile contains no override of it.

Windows [CLight_DB::add_light:199](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/Light_DB.cpp:199) returns for `L->flags.bStatic && !ps_r2_ls_flags.test(R2FLAG_R1LIGHTS)` **before** `L->export_()`. That prevents the complete point/spot package from entering deferred, six-face shadows and subsequent volumetric/forward routes. There is no HUD or volumetric exemption to this explicit source setting. It preserves the engine light's existence and baked lightmaps; it only excludes the renderer package. Directional sun/reflected light handling is separate.

Active native End ([MetalRenderDeviceRender.mm:1022](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm:1022)) checks active/type, then appends every parent and expands every shadowed point into six records. It never checks this switch. It copies the parent static flag correctly, so the snapshot contains enough evidence to classify the error. This is an explicit source-policy parity correction, not a geometric/occlusion inference or permission to change quality settings.

## Actual current evidence and its limits

`static-pso-off-01` normal log frame300: captured/admitted429, `static=354`, rejected frustum/fog/LOD all0. Frame600: captured/admitted441, static354, those rejection fields again0. The `static` field is an **expanded record count**; it is not a parent count. These logs are produced by the raw End path, whose dormant rejection fields do not prove the source predicates passed.

Same completed engine frame360/E01 serial360 has 21,216 indexed draws, 15,057 PSO requests and 28,146,720 updated bytes. Its existing static-PSO census has 297 actual local shadow faces and 1,270 main static-geometry indexed draws. Explicit expanded IDs/face ordinals identify 48 six-face parents plus nine spot parents (57 final parents). They do **not** identify which of those parents are source-static; normal logs lack that per-ID flag. Do not infer the exact final static intersection from `354/6`, nearby frame300 or memory addresses. The new observer records it directly.

Level loading assigns static Point/shadow lights ([Light_DB.cpp:38](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/Light_DB.cpp:38)); ordinary Create defaults static=false ([line177](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/Light_DB.cpp:177)). This makes substantial unnecessary package work plausible. Exact parent/face membership and work must come from the frame360 join. No percentage/ms/FPS benefit is claimed from nested 14ms local-shadow timing or record counts.

## Frozen independent drafts

| Artifact | Files | SHA256 |
|---|---|---|
| [r1-static-policy-observer.patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/r1-static-policy/r1-static-policy-observer.patch) | Device + EngineScene, no public metadata/API changes | `863dea6a82368b0de6be6c3cb4760edc0348dad4313051aac0a38eac1023551a` |
| [r1-static-policy.patch](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/r1-static-policy/r1-static-policy.patch) | Device only, seven added code/comment lines | `d93e89f9588c24c337c4530a026ebe5059c2f88e5d3cb45f5f4cd455f8edcf07` |

Both pass `git apply --check`, independently against original restored prestate Device `81188b9f3a6aab3e3d47b606efcd8b62fc0239a05f2b2285b9324bea973a8073` and Scene `c7c3b5c77e11e41b78095c867e25fe7568b886aa70c2d2fc3eb6ee1af3c1af53`. [source-manifest.json](<XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/r1-static-policy/source-manifest.json) pins source/draft/patch hashes. They are independent proposals; initially apply only the observer and restore its two source files before considering the one-file policy gate.

Observer is strict default-off `XRAY_METAL_R1_STATIC_POLICY_OBSERVE=1`, exactly engine frame360. End logs one row per parent from the **completed value snapshot**, its captured-record count, static flag, live R1 bit, exact hypothetical predicate and HUD/volumetric fields. It does not keep light pointers or alter snapshot values. EngineScene logs each actually rendered final face after its existing tile path succeeds, using the immutable snapshot's parent/static fields and existing per-tile counters. It adds no clock, GPU command, culling query, per-draw wrapper or public field.

Proxies are explicitly separate: `source_static` labels an engine light parent; `static_geometry_draws` counts main level-geometry caster draws for its face; `dynamic_geometry_casters` is the existing submitted-caster delta; `flora_shadow_draws` is the just-rendered Flora telemetry; `material_rows*256` is the corresponding static-transform payload. DefVertex, object-LOD, detail grass, shaders/binding/culling overhead and the full GPU cost are not included. These counters must not be added to overlapping CPU timers.

Root's existing matched harness needs only the observer flag; leave `XRAY_METAL_R1_STATIC_POLICY` absent and all other suppression/face-cull/oracle controls off. Add both new flags to inherited-environment sanitization. Then:

```sh
python3 <XRAY_SOURCE_ROOT>/scratch/native-silo-parity-20260929/sol-analysis/r1-static-policy/summarize_observer.py \
  "$XRAY_R1_OUTPUT/engine.log" \
  --e01 "$XRAY_R1_OUTPUT/e01-frames.csv" \
  --device-phases "$XRAY_R1_OUTPUT/phases.csv.device.csv" \
  --output "$XRAY_R1_OUTPUT/r1-static-policy-summary.json"
```

The parser reconciles captured parent/record totals, exact predicate, parent/face IDs/static flags, unique final tiles and proxy values; it requires exactly one completed E01 acquisition and device phase1 at frame360. Reject any render error/discard/retry at that frame; observer rows have no separate acquire-epoch backend hook. Captured-source-static count and final policy-rejected face/work counts remain distinct. `parser-fixture/` is a synthetic parser arithmetic check, not game evidence. Logging makes the observed frame unsuitable for performance timing.

The candidate is strict default-off `XRAY_METAL_R1_STATIC_POLICY=1`. It uses the identical source condition after existing inactive/type filtering and before parent identity/map/append. It excludes a whole rejected parent, preserving complete groups for accepted parents and all source objects/resources. The opt-in is captured once; the console R1 bit is read live during each frame's loop. No frustum, HOM, fog, LOD, color, shader, projection, volumetric formula or geometry-instancing change is combined with it. With the R1 bit on, source-static parents follow the existing native path unchanged.

## End ownership and later shared-helper policy

The supposed filtered upstream producer, `r_dsgraph_capture_metal_lights`, is dead in current source: repository-wide caller search finds only declaration/definition and its unused copy/publication API. Its comment saying End invokes it is stale. [R_calculate.cpp:31](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/R_calculate.cpp:31) deliberately calls Lights.Update and captures world/HUD, then states lighting is taken at End after object updates to avoid newly spawned light mutation during New Game. [End:1000](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderMetal/MetalRenderDeviceRender.mm:1000) keeps registry enumeration because native level loading lacks the D3D sector/portal/octree traversal contract. Retain that ownership point; calling Calculate's published list would reintroduce stale/missing capture.

For eventual maintenance, factor the **pure static-switch predicate** into a small shared source helper and use it from Light_DB::add_light, the dormant producer and active End. Do not call add_light/export_ merely to classify: they change frame stamps, packages and queries. The first draft deliberately duplicates only the exact three-term predicate to keep the correction bounded; factor after acceptance if requested.

The other bypassed policies require separate gates:

| Dormant producer / Windows policy | Active End difference and later constraint |
|---|---|
| Non-null SpatialComponent; parent sphere camera-frustum (L/R/T/B/Far) | End only checks source/active/type; native planner uses a much looser receiver cube and whole-parent retention. A later observer must copy sphere values at End and compare Device.mFullTransform to the accepted ViewState VP; don't reuse a stale Calculate frustum. |
| Static R1 switch | Confirmed explicit mismatch; independent of view/projection and all consumer geometry. Selected first candidate. |
| Parent-center fog distance | End has no gate. Preserve current environment/camera frame, HUD coordinate conversion and possible separate view/volumetric consumers before any later correction. |
| `get_LOD > ps_r2_shadow_lod_min` | End has no parent threshold. `get_LOD` writes `light.distance`, so an observe-only classifier should evaluate a pure value formula instead of mutating live light state. Current console inputs include threshold0.02/slight-fade0.5. |
| LOD-scaled deferred color/specular | End copies raw RGB and raw-derived specular. Dormant producer multiplies parent RGB by parent LOD, but this is not fully identical to Windows: Point accumulation uses raw RGB; Spot/OmniFace accumulation multiplies the **actual rendered light/face** LOD then computes specular ([r4_rendertarget_accum_spot.cpp:131](<XRAY_SOURCE_ROOT>/src/Layers/xrRenderPC_R4/r4_rendertarget_accum_spot.cpp:131)). OmniFace source sphere is offset by direction*range/sqrt2 with radius range/sqrt2. Native volumetric capture already reconstructs those values/LOD separately and must not be faded twice. Do not blindly reuse the whole dormant producer's color policy. |
| Decorative source / cameraDistance | Dormant producer computes them; End's MetalLocalLight defaults remain false/zero. This affects local detail-grass admission/fade gating. It is a separate possible cost/behavior discrepancy, not part of the static-switch patch. |
| Sector/HOM / occlusion visibility | Deliberately unsupported/unqualified native graph. Excluded from all proposed reuse. |

Device builds primary `mFullTransform = mProject*mView` immediately before SetCacheXform ([device.cpp:504](<XRAY_SOURCE_ROOT>/src/xrEngine/device.cpp:504)). HUD/alternate camera RAII temporarily swaps/restores Device matrices ([CHudInitializer.cpp:45](<XRAY_SOURCE_ROOT>/src/Layers/xrRender/CHudInitializer.cpp:45)). A future geometric classifier must require valid current engine frame, finite matching accepted matrices and restored primary view, or retain the parent. No such camera-dependent policy is implemented here.

## Consumer/correctness admission for the chosen switch

The source switch prevents all package export before HUD projection application, volumetric invocation or forward shadow graph selection. Therefore this explicit policy has **no source HUD/volumetric exemption**, unlike geometric accumulator-only face culling. The observer retains their labels so this can be reviewed rather than assumed. Native direct local-atlas consumer is Volumetric; it only receives records from the accepted batch. HUD/3DSS deferred receivers, static forward/water/particles downstream accumulation and unknown future consumers cannot demand a package that the source setting explicitly disables, but future alternate admission owners must share the predicate. Sun, reflected lights, static geometry and lightmap shading remain separate.

This changes erroneous native illumination intentionally. Old-native byte-identical output is not an acceptance target when the R1 setting is off. Qualify package membership against the Windows source policy and matching config, then compare lighting/scene pixels with Windows at the same save, weather, camera and settings. Keep a switch-on control that retains source-static records and a dynamic-light control that is unaffected. Exercise reload/first-frame/empty static package and no stale atlas contribution after omission. Only after those gates may one observer-free same-binary off/on screen report performance, explicitly describing the source-setting correction rather than an unchanged-quality micro-optimization. No FPS advantage is established by these drafts.
