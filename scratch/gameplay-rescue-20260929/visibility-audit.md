# Visibility audit and bounded candidate, 2026-09-29

No game or build launched by this worker. Source-only `git diff --check` passes for EngineScene. No AGENTS.md found in the repository ancestry or src/docs scopes. Dirty WIP preserved; task prestate and task-only patch are under this directory.

## Confirmed visibility difference

`src/Layers/xrRenderPC_R4/r4_loader.cpp:35` returns early for Metal after CPU lights and optional diagnostic-only HOM load. It does not load the normal sectors/portals graph. `src/Layers/xrRender/HOM.cpp:353` rebuilds a synchronous diagnostic raster only when explicitly enabled; `visibleDiagnostic` is fail open for uncertain bounds. Ordinary Metal light capture (`r__dsgraph_render.cpp:966`) explicitly bypasses unavailable sector/HOM gates, applies frustum/fog/LOD, skips D3D OMNIPART children, and expands one point parent into exactly six immutable face records (`:1061`). No duplicate faces found in code; planner validates unique IDs/indices and a complete six-face group.

Existing whole-light diagnostics are explicitly observe-only. S4 recorded 2.677 ms p50 known hidden-tile preparation/encoding against 0.279 ms HOM/group overhead, approximately 2.40 ms potential CPU reserve, not measured frame gain. Active Expanded & Fixed Maps `level`/`level.geom` differ from the archive HOM geometry. `geometry_verified=0` correctly blocks suppression. S2 recorded zero hidden work. This historical S4 fixture does not quantify real Cordon benefit.

## Concrete redundant work

`MetalR4EngineScene.mm:2409` performs whole-parent shadow admission. Before this candidate, every OmniFace was tested using a full local [-1,+1]^3 cube scaled by 2*range, then every six-face parent was retained if any face passed. Because captured face rotations are axis aligned, these cubes cover the same world cube for all faces.

The real accumulator mesh is `du_sphere_part` (82 vertices, 160 faces), with local bounds [-0.353553,+0.353553] in X/Y and [0,0.5] in Z, transformed by the same `2*range` face model. `BuildMetalR4LocalLightVolumeBounds` in LocalLightAdapter reads those canonical vertices and proves containment against the actual mesh in its existing self-test, including rotated/translated bases. Bounds expand by 1 cm plus 1e-5 times world-coordinate magnitude. For a face whose entire real volume is outside the camera, its three stencil/accumulator mesh draws cannot produce fragments; none of that face's shadow map can be consumed. Removing that whole record avoids static BVH selection/merge/transforms, all dynamic/flora/definition/detail shadow families, and GPU shadow draws. Caster visibility remains each admitted light's own frustum.

Saved actual new-game postboot logs show 303 local shadow views / 25 batches, 8,322 static caster rows and roughly 127,000 rejected dynamic caster checks per report. First new-game logs show 111–129 views / 9–11 batches. Local-shadow CPU p50 is 31.56 ms postboot and 13.67 ms first run (inclusive scene stage). This is a material stage ceiling and an explicit multiplier; there is no saved count of frustum-invisible actual faces, so no net gain estimate is claimed.

## Implemented opt-in candidate

`XRAY_METAL_LOCAL_LIGHT_FACE_CULL=1` enables a bounded post-planner filter. Default path is identical. The full captured source is validated with existing planner before filtering; partial visible groups retain source order and stay within their existing batch, empty batches are dropped. The original snapshot is unchanged.

Eligible only: active shadowed OmniFace, current frame/view/environment, actual padded mesh bounds available, finite shadow scale/bias and finite view/projection. Entire parent is exempt if any face is HUD or can invoke either authored or global SSFX volumetrics. Spot and unshadowed Point behavior is unchanged. Shared camera inverse and each candidate's ordinary shadow view/projection and derived shadow/lightmap matrices are checked before removing its record; malformed data stays on the normal rendering path. Any unknown input is fail open.

`R4LocalLightFaceBoundsOutsideView` uses all eight world-AABB corners and homogeneous plane halfspaces, computed in double. Every corner must be strictly outside the same left/right/top/bottom/near/far plane with base margin 1e-5*max(1,abs(W)) plus 64*float-epsilon times the sum of absolute projection terms for each component. This covers large-coordinate cancellation and CPU/GPU float product reassociation. The refined helper certifies all six homogeneous halfspaces without a blanket W/near crossing exemption: one affine separating plane suffices regardless of W sign. Uncertain/touching distances stay visible. Independent Sutherland-Hodgman clipping of all 12 AABB-face triangles across 4000 deterministic dense projections verifies reject implies no clipped polygon; tests also cover eye-inside/frustum-containing boxes, negative/zero W and near crossings. This revision still awaits build/test/runtime proof. No temporal result is reused; exact current view is tested every rendered view.

`XRAY_METAL_LOCAL_SHADOW_BATCH_PROBE_DIR` expects six-face groups, so this older witness forces baseline behavior with explicit `status=bypassed reason=complete_face_batch_witness` log. This preserves its contract rather than silently qualifying a candidate through a no-op oracle. Independent `XRAY_METAL_COLOR_PROBE_DIR` covers accumulator/combine_base/scene_before_postprocess parity.

The CMake target `XRay.Metal.LocalLightFaceVisibilityProbe` and focused CTest `XRay.Metal.LocalLightFaceVisibility.SelfTest` check all clip planes, boundary touching, near crossing, invalid boxes, zero/negative W, nonfinite camera matrix, HUD, authored volumetric, SSFX volumetric, disabled volumetric mode, Spot/Point, unavailable environment, translated large-coordinate cancellation versus actual float projection, and near-plane touching after large translation. It is not run by this worker; command for leading worker:

```sh
c++ -std=c++17 -Isrc/Layers/xrRenderMetal src/Layers/xrRenderMetal/tests/R4LocalLightFaceVisibilityProbe.cpp -o /tmp/xray-face-visibility-probe
/tmp/xray-face-visibility-probe
```

The existing `XRay.Metal.LocalLightAdapterProbe --self-test` provides canonical-mesh containment proof; the engine build checks scene integration.

## Runtime acceptance gates

1. One bounded real new-game screen must show useful `METAL_LOCAL_LIGHT_FACE_CULL before/eligible/exempt/unknown/removed/remaining` counts and lower whole-frame/local-shadow work before longer testing. Diagnostics must identify exemptions, or candidate can be a no-op with globally enabled volumetrics.
2. Same binary baseline/candidate color captures at fixed state must compare accumulator, combine_base and scene-before-postprocess pixels. The six-face batch witness is unsuitable for this candidate.
3. Camera motion through point-light face edges, near plane and camera-inside range must preserve illumination/shadows. Cover campfires/lamps, doors/windows, HUD light, and volumetric sources. Off-camera shadow casters must remain correct for all retained face records.
4. Same-binary full-GAMMA gameplay A/B with current install and same save/config after ordinary warmup must show net frame CPU/GPU improvement; detailed logging/capture off for timing. This branch has no accepted performance/visual claim yet.

## Larger follow-up if face screen is small

The substantial remaining structural gap is whole-light and main-static occlusion against authoritative active opaque geometry. Use a runtime-generated conservative occluder set sourced from the installed XRFS-winning `level`/`level.geom` and admitted opaque material families, never borrowed archive HOM or bespoke mod assets. Retain exact surface coverage; omit alpha-tested/transparent/wind-moving/dynamic geometry and preserve holes/windows. Do not use camera-static results for caster admission.

Build CPU acceleration once per level (plus bounded generated cache), query a bounded set of large active opaque triangles for current camera, and construct a conservative raster with edge/depth tolerance. Start observe-only with current actual gameplay counters. Cache invalidation must include hashes of active geometry, material/shader classification and tree transforms plus algorithm version; unsupported/stale/missing data stays visible. Storage belongs to ordinary appdata cache, with a small cap, not an alternate mod tree. This can close the proven static/light visibility gap while maintaining upstream GAMMA mod/update compatibility, but requires its own economic screen and pixel/motion oracle before culling.

Depth-clip source proof: no backend setDepthClipMode override exists; Apple documents render-pass default MTLDepthClipModeClip at https://developer.apple.com/documentation/metal/mtlrendercommandencoder/setdepthclipmode%28_%3A%29?language=objc . Receipted build/metal-shader-sources/r3/accum_volume.vs writes only mul(m_WVP,P) to hpos; no volume extrusion.
