# CPU scene/shadow audit and candidates — 2026-09-29

Status: DefVertex candidate implemented, first central build finished. Flora production oracle and opt-in dynamic shadow BVH implemented and ready for the next central build/runtime; default instancing remains experimental and dynamic BVH defaults off. No game or build launched by this worker. No ancestor or scoped AGENTS.md found. Dirty WIP preserved.

## First candidate: DefVertex shadow broad phase

Evidence: `newgame-native/engine.log:4361` admits 1146 DefVertex draws, including 222 alpha-tested. Every local-light face and sun cascade previously scanned every draw and applied the 8-corner exact test. `newgame-native/cpu-sample.txt:32-35` has 260 samples in a local-shadow call branch, 232 in DefVertex and 191 in its exact bounds function. Heavy-save sample independently has 70/62/49 in the corresponding branch. These are sampled stack counts, not milliseconds or an additive stage budget.

Patch reuses `R4StaticShadowIndex`: build once from immutable admitted level models/bounds, query each light's own matrix, retain sorted original draw order, then run the unmodified exact admission test. Invalid/missing/non-affine/uncertain bounds remain candidates. Constants/resources/draw state remain unchanged. DefObjectsLod has only 57 admitted draws here, so it is outside the first patch.

`XRAY_METAL_DEF_VERTEX_SHADOW_LINEAR=1` selects all draws without querying the BVH for a same-binary baseline. `XRAY_METAL_DEF_VERTEX_SHADOW_ORACLE=1` performs a full independent exact scan and fails on any omitted admitted caster; its bounded output gives total views and candidate count. Oracle runs are correctness diagnostics and must be excluded from performance acceptance.

The existing adapter SelfTest now compares final exact ordered caster lists for 1024 affine/sheared/scaled/reflected models across 128 arbitrary views, plus invalid/nonfinite/non-affine/missing/inverted bounds and all six Metal clip-plane contacts. `git diff --check` passes. Not compiled by this worker.

Candidate-only diff: `defvertex-candidate.diff`. Prestate SHA256: CPP `cf7cb2515a91584785c1f1fff185f4c24fc0aac960317d7245bd198a8c70e2f1`; H `111c3f2cf1de01274ae2f73d406b10c3ef22725da5f526d4b55d05b1e23286f9`.
Candidate CPP SHA256: `0671e428314c37ed4dbaa7a289eeaddd3a110eb9a1a55355f789596f417a2130`. Header unchanged.

Targeted central commands:

```sh
ninja -C build XRay.Metal.DefVertexAdapterProbe
ctest --test-dir build/src/Layers/xrRenderMetal -R '^XRay\.Metal\.DefVertexAdapter\.SelfTest$' --output-on-failure
```

## Next candidate: qualify existing Flora shadow instancing

Actual newgame inventory is 11663 Flora instances sharing 487 groups. The newgame engine log says `METAL_E08_FLORA_INSTANCING enabled=0`. The adapter already has immutable per-view model uploads and instanced leaf/trunk pipelines. It activates only when `XRAY_METAL_MSC_FLORA_INSTANCED_SHADOW_DIR` is configured and the OFF flag is absent. The shared launcher shader environment intentionally sets only ordinary Flora and shadow directories.

The bundle exists at `build/metal-runtime-shaders/flora-shadow-instanced-e08`. All six DXIL/metallib/reflection artifact hashes match its receipt, and current source tree/wind hashes also match: tree `92cc3c864db4a4eda25685c5b247f5031a0546d1241d3d6bc8ec04ce4cff60e3`, wind `f11cef099fba18f62457fb2828fab0fa662c19158404a166ebe019bea2423341`. The cooker preserves source wind/alpha math and supplies each instance's modelY to branch/trunk wind. It fails if upstream text contracts do not match; it must not silently apply to changed mod shaders.

E08_PERFORMANCE.md records synthetic leaf/trunk, wind-off/A/B, alpha controls, scale/height/rotation/clipping parity and an S2 short-run gross whole-frame improvement of ~1.5 ms (~5%). It remained off pending production pixel/order proof. Two S4 starts aborted during CModelPool blank-filename/CHangingLamp loading before useful world-render frames. Causation was not established. The heavier full-gameplay workload and current user's broader ROI authorization justify requalification, not unconditional activation.

Current grouping requires identical VB/IB/indexCount/material/alpha/objectLOD/base texture and preserves source order inside each group. Group order differs from the original interleaved draw sequence. Ordinary opaque/alpha-test depth shadows should be order-independent, but exact R32 production comparison is necessary for depth ties and source-specific behavior.

CPU sample supports this path: newgame local-shadow Flora branch has 142 samples out of 186 in its caller; heavy-save counterpart 26/32. The leaf shader takes models as structured-buffer SRV t1 and unchanged source CBs/waves/sampler through reflection-validated 120-byte TLAB; trunk is 72 bytes. Model descriptors use the backend's buffer GPU-address/length encoding and immutable frame-arena slices, not texture descriptors. Default remains experimental through qualification.

### Central GPU gates (no run yet)

```sh
ninja -C build XRay.Metal.FloraInstancingGpuProbe XRay.Metal.FloraAdapterProbe
ctest --test-dir build/src/Layers/xrRenderMetal -R '^XRay\.Metal\.FloraAdapter\.SelfTest$' --output-on-failure
build/src/Layers/xrRenderMetal/XRay.Metal.FloraInstancingGpuProbe --gpu build/metal-runtime-shaders/shadow-casters build/metal-runtime-shaders/flora-shadow-instanced-e08 scratch/gameplay-rescue-20260929/flora-gpu
```

The optional fifth argument to the GPU probe selects a diagnostic depth PS directory; the production compat PS currently writes post-raster depth for opaque and alpha-tested casters, so its regular run is meaningful. Use API/shader validation in a separate diagnostic run. Shader generation must finish before gameplay timing begins. Existing artifact hashes currently agree, so regeneration is not needed just to execute the gate.

### Required production pair oracle design

Existing static-merge and static-sun pixel oracles exclude Flora. Temporal pair validation accepts only RGBA8/RGBA16, so it cannot directly compare R32 maps. Reuse `ValidateSunShadowCascade` only for lifetime-safe completed R32 exports, and compare exported raw floats/bytes with an offline validator. Its receiver status must not be represented as a Flora PASS.

Add a diagnostic-only explicit Legacy/Instanced selector to the Flora draw method; normal configured behavior is unchanged. A local RAII guard restores adapter telemetry after diagnostic calls. The selected draw models, geometry, alpha textures, sampler, wind/time/benders and light matrix are the same immutable scene values for both modes. Restore configured behavior after each pair; model group scratch lists are rebuilt on each instanced draw.

EngineScene renders two isolated R32 maps with independent depth attachments after the active production pass has ended, then stages readbacks on the same command buffer. Clone actual descriptors: current local tiles are 512 in a 2048 atlas, sun maps 2048. Do not reuse production depth/atlas, modify snapshot light records, or commit temporary producer history. Local pass can use the tile's matrix at origin-zero viewport; the comparison concerns the same view and depth, and production resources remain untouched. Preserve/restore telemetry and never insert oracle passes inside an existing encoder.

Bound with `XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_DIR`, a sampling interval, explicit attempt/pair caps separately for local and sun (e.g. maximum 12 local views and 6 sun views at 240-frame interval). Produce per-pair metadata: engine frame, batch/tile or cascade, source light ID, view matrix, wind/time, legacy/instanced submitted instances and draws, resource extent and shader receipt hashes. Failure to create instanced pipelines or unequal submitted instance counts fails the oracle run.

Acceptance requires completed exports, equal size, byte-identical maps, zero nonfinite pixels, and positive pixels below clear depth 1 in both local AND sun groups. An all-clear equal pair is NO_COVERAGE, never a pass. All three sun cascades, leaf alpha geometry, varying wind times and off-camera trees must be sampled. If local views in the save have no trees, use another real save/light/camera diagnostic; do not substitute synthetic coverage for the production gate.

Central runtime sequence: full gameplay load/continue with experimental directory set; first a short bounded startup/qualification run, then production pair oracle and wind/motion inspection, then alternating same-binary/same-save A/B with `XRAY_METAL_E08_FLORA_INSTANCING_OFF=1` versus enabled directory, all pixel captures and validation disabled. Hold resolution/config/world state and thermal/power condition fixed and report whole-frame/cadence/tails rather than claim 11663/487 is the FPS multiplier. The 487 groups are whole-level inventory; each view has a different candidate set.

Prestate before Flora oracle: CPP `291983dd37f53f64e99f84566819544a73cfd72542f2ffbfc95998417eb44757`; H `6d76ad7f422a9df4554c1937219bf734c01e985d0a20275dacb5f34d006d3d0c`; EngineScene including the other worker's completed face visibility patch `c78bbaadfd4cbbc861beefd4ad24b751bb4510a273cab9f2ee3396369cbaf8cf`. Copies saved in `prestate/`.

Implemented oracle: diagnostic `DrawShadowCastersForValidation` in Flora cpp/h and isolated pair helper/hooks in EngineScene. The wrapper snapshots and zeroes telemetry for exact diagnostic counts, then restores configured instancing and telemetry with RAII, including failure paths. Separate targets live in scene-owned local/sun oracle state; caps are 12 local and 6 sun pairs, sampling default 240 frames or `XRAY_METAL_FLORA_SHADOW_PIXEL_ORACLE_EVERY`. Local samples are limited to four per sampled frame, rotating view ordinals 0–3 / 4–7 / 8–11 over at least three frames; sun samples all three cascades over two times. Repeated scene submissions for the same engine frame/view are deduplicated. `flora-pairs.jsonl` records the shared time/wind/matrix, counts and extent. Default instancing behavior is unchanged. Readbacks compare through completed SunShadow exports, not unsupported temporal R32 conversion. Pair helper runs after production encoders end and does not mutate production attachments or light records. All three sun map descriptors are currently 2048 squared; extent drift fails the diagnostic rather than silently changing raster conditions.

Offline comparator: `verify_flora_shadow_pairs.py`. It accepts only production `gameplay-flora-local-*`/`gameplay-flora-sun-*` paired exports at the same engine frame, requires matching recorded provenance, verifies matrices/extents/completion/size, compares every R32 pixel byte and rejects absent/nonfinite/missing pairs. Positive completed local and sun coverage is mandatory. It does not infer receiver validation from SunShadow exporter report labels. Eight controls passed: positive, all-clear, nonfinite, pixel mismatch, changed matrix, incomplete GPU, missing paired report and missing provenance.

```sh
python3 scratch/gameplay-rescue-20260929/verify_flora_shadow_pairs.py <completed-oracle-directory> --output <comparison.json>
```

Candidate-only diff: `flora-oracle-candidate.diff`; `git diff --check` passes. Post-first-build EngineScene prestate (including completed face telemetry changes) is `prestate/MetalR4EngineScene-before-flora-oracle2.mm`, SHA256 `5e4864912929d11516daff77b8584aea641e149872cc4149aad9147bd56428a9`.
Final post-review Flora candidate CPP SHA256 `3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc`; H `041e7b0e80d6f6ca63512bcccad33336670268d4d9428812ae83bf9d0022cc52`; EngineScene `39a0b51b08e3c161a2528bf2d4be6a40e39cf3bcd2e1c854a858ec5a4debcc20`. Source edit hold lifted by root only after first build; no source edits overlapped it. Runtime/GPU/build requests handed to runtime_lead.

## Compute skin once feasibility (after Flora)

The current CPU skin cache works: pose hash + geometry/object identity + acquired backend context slot, three slots. The same output buffer is reused by G-buffer/emissive/lightplane/shadow draws. Pose reuse does not eliminate repacking changed animations. Gross CPU skin budget is 5.1–6.7 ms fresh world and ~9.9 ms heavy save, not expected net gain.

A compute path can replace the cache fill, retain exactly the 36-byte SKIN_0 packed output and use the existing pose/slot cache. Immutable validated source vertex and index data can be uploaded once per geometry, and immutable bone palettes once per changed pose/frame. Output must be retained per acquired slot until GPU completion; no overwrite from simulation frame modulo. Encode compute before any G-buffer/shadow draw consuming the output on the same command buffer, without CPU readback/wait.

Large missing contracts: IR4Backend currently has graphics APIs only; native compute exists for save thumbnails, so a narrow explicit compute operation is needed. Multiweight ABIs are 64/70/76 bytes; last 3W/4W weight is signed residual exactly as engine CPU skin. CPU currently validates source indices and bones each repack, normalizes/quantizes BGR normals/tangents and repairs whole deformed tangent basis if source T/B are nonfinite. Hoist immutable validation and classify repair geometry once; preserve CPU fallback for repair cases initially. Bounds sanity currently scans deformed vertices, so use conservative per-bone transformed envelopes with signed-weight treatment or maintain CPU bounds work until separately proven. Unconditional GPU readback would erase the critical-path gain. Retain current motion contract (previous object/camera transform); adding previous bone deformation is a distinct correctness scope.

Mod compatibility: consume immutable captured OGF/raw bone data and source shader trees from the user's actual installation, no replacement of GAMMA assets or giant redistributable. Fingerprint cooked shaders and invalidate/recompile/fail-open on mod updates. Scope is engine/runtime binary + small shader cache; source geometry and mod files retain their original ownership.

## Other bounded CPU findings

Actual Ninja flags are RelWithDebInfo `-O2 -g -DNDEBUG`, ARM64, C++ adapter ThinLTO. EngineScene ObjC++ has -O2 without ThinLTO. No general Debug/-O0 defect established.

Pose hashing, main-view AABB test and VP/WVP/WV computation occur inside the geometry loop and can be hoisted per renderable; CPU staging arrays are allocated per changed multiweight mesh. These are real repeated operations but smaller ROI than the shader/caster paths. The ~0.33–0.46 ms world bounds pass cannot explain the whole gameplay gap. Dynamic shadow culling already occurs before vertex geometry lookup/upload; Flora and DefVertex also upload only after exact per-view admission.

Implemented next opt-in candidate is a dynamic per-frame shadow BVH: heavy report has roughly 459 objects times 303 shadow views and ~127k rejected caster checks. `XRAY_METAL_DYNAMIC_SHADOW_INDEX=1` builds once at the successful end of Prewarm from the old local-sphere cube, queries each light view, preserves sorted snapshot order, and applies the old exact test before the unchanged geometry/draw loop. Invalid/nonfinite/non-affine/overflow bounds remain unbounded. No skin pose/hash/cache data is used to shrink the old contract. Assetless/rejected and broad-phase culled counts preserve the previous per-view accounting.

Prewarm entry invalidates the retained prepared owner before all early exits. A new owner is published only after successful Prewarm. Pointer identity with a retained immutable shared owner, captured engine-frame stamp and acquired backend slot must all match before indexing; otherwise a full linear scan runs. `XRAY_METAL_DYNAMIC_SHADOW_INDEX_ORACLE=1` compares the exact ordered admitted renderable list to a full scan for indexed views and reports indexed/fallback view counts and total candidate checks separately. A fallback-only log cannot qualify the index. The oracle is diagnostic and excluded from performance timing.

The existing CPU SelfTest adds 512 randomized transformed spheres over 128 views, including assetless/noncaster cases, signed scales/shear, invalid/negative spheres, nonfinite/non-affine/overflow input. It compares ordered admitted lists and culled/rejected accounting, proves a reduction exists, and checks retained owner, same-frame different-owner, slot/null/frame mismatch and explicit early invalidation. No backend was fabricated. Tests are not compiled by this worker yet.

Dynamic CPP prestate SHA256 `ba17b521335d2369cb29f6924c575d6e1b4ecbd827f148e6b6971461e2571b81`; unchanged H `06d11038afb2363accf0606037712b59c99870425bc2b6ac5e64eb63888bafa5`; candidate CPP `c16124a0fea5024896c3450fb2fd455f51791f7219b52789c74bfc86be6e151a`. Scoped diff `dynamic-shadow-index-candidate.diff`, diffcheck passes. Runtime/build requests sent to root and runtime_lead. Isolate Flora qualification with both dynamic flags unset, then test this candidate independently. Postboot has no completed CPU sample, so these counts do not establish milliseconds or an FPS prediction.

```sh
ninja -C build XRay.Metal.DynamicWorldAdapterProbe
ctest --test-dir build/src/Layers/xrRenderMetal -R '^XRay\.Metal\.DynamicWorldAdapter\.SelfTest$' --output-on-failure
```

Do not interpret the render callback sample stack splitting as missing ~25 ms: separate branch addresses of the same Render method represent disjoint call sites (prewarm/production/etc). CPU stage medians are nested, not additive. General AI/ray-query/Lua hot spots are present but there is no matched Windows/native per-stage proof for an engine/AI rewrite. First qualify the concrete shadow and skin work above.
