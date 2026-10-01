# Static constant group candidate (scratch only)

This stage groups only the production MSC transform-constant triplet by the exact 64-byte `TextureMaterial::modelTransform` representation. `XRAY_METAL_STATIC_CONSTANT_GROUPS=1` is required; the default path retains the old per-material allocation/update loop. Grouped mode keeps a separate PS globals b0 for every material, keeps the VS b0, VS b1, and PS b1 buffers as three distinct buffers per group, and does not change textures, geometry, ordering, draw admission, or shaders.

Preparation builds the exact-byte group plan and all candidate group triplets plus all per-material PS globals before publishing any pointer into materials. A planning/allocation failure drops the candidate resources and runs the original per-material allocation path. The frame helper invokes the existing `multiply` function in its original WVP, world-view, previous-WVP order once per representative, and updates the three separate buffers once per group. Optional active-occlusion stamps fan out from those exact payload bytes only after that group uploaded; any grouped update failure invalidates all stamps for the active scope. Every retry rebuilds payloads from the current and previous view matrices; no frame-number-only payload cache is used.

The standalone probe calls the same grouping, payload-construction, group-update/fanout, and stamp helpers as the draft integration. It compares the initial and per-frame payload bytes with the preexisting per-material formulas for repeated identity and distinct nonidentity matrices, distinguishes signed-zero keys, checks one group-update callback per group, injects a failed group and verifies all scope stamps clear, retries on the same engine frame with changed matrices and a new scope, and verifies update-only mode skips material fanout.

The parser gives every non-tree `R4StaticVisual` the default identity transform and only reads a serialized model transform for tree visual types 7 and 11 (`R4LevelGeometry.cpp:649-674`). Root's offline admission count found 2,052 automatic Cordon material admissions with one exact matrix key. This is cardinality evidence, not performance evidence.

The native backend copies ordinary Shared constant buffers into per-draw frame-upload slices during MSC binding (`MetalR4Backend.mm:2925-2952`). This candidate reduces repeated CPU matrix construction and Shared-buffer update calls/objects; it does not remove those per-draw binding copies. The saved profile sample contains 1,163 main-thread samples: 897 in `MetalR4EngineScene::Render`, including 351 in `renderProductionScene`; the `CheckR4ComputeSkinBounds` guard accounts for 36. Flat counts include 13 `UpdateBuffer` and 2 `multiply` samples. These are sample counts, not milliseconds, and do not attribute a large share of CPU time to `updateSceneView`; no performance claim is made.

## Validation

Both commands use the renderer translation unit's relevant compiler settings from `build/compile_commands.json`: `/usr/bin/clang++ -O2 -g -DNDEBUG -std=gnu++17 -arch arm64 -ftrivial-auto-var-init=zero`, with no stricter floating-point options than the renderer.

Optimized probe:

```sh
/usr/bin/clang++ -O2 -g -DNDEBUG -std=gnu++17 -arch arm64 -ftrivial-auto-var-init=zero \
  -I scratch/gameplay-rescue-20260929/static-constant-groups-stage/draft/src/Layers/xrRenderMetal \
  scratch/gameplay-rescue-20260929/static-constant-groups-stage/draft/tests/R4StaticConstantGroupsProbe.cpp \
  -o scratch/gameplay-rescue-20260929/static-constant-groups-stage/out/R4StaticConstantGroupsProbe
scratch/gameplay-rescue-20260929/static-constant-groups-stage/out/R4StaticConstantGroupsProbe
```

Output: `STATIC_CONSTANT_GROUPS PASS exact-keys initial-and-frame-bytes group-fanout same-frame-retry-stamps`.

UBSan probe used the same flags plus `-fsanitize=undefined -fno-sanitize-recover=all`; it produced the same PASS line. Captured outputs are in `out/optimized-run.log` and `out/ubsan-run.log`. No renderer build, game run, or GPU run was performed. The scratch CMake delta registers `XRay.Metal.StaticConstantGroupsProbe` with CTest name `XRay.Metal.StaticConstantGroups.SelfTest`; CMake configure/build/CTest remains for the central runtime owner.

## Draft mapping

- `draft/src/Layers/xrRenderMetal/MetalBootstrap.mm` → production `src/Layers/xrRenderMetal/MetalBootstrap.mm`
- `draft/src/Layers/xrRenderMetal/MetalR4EngineScene.mm` → production `src/Layers/xrRenderMetal/MetalR4EngineScene.mm`
- `draft/src/Layers/xrRenderMetal/CMakeLists.txt` → production `src/Layers/xrRenderMetal/CMakeLists.txt`
- `draft/src/Layers/xrRenderMetal/R4StaticConstantGroups.h` → new production helper header at the same path
- `draft/tests/R4StaticConstantGroupsProbe.cpp` → new production test at `src/Layers/xrRenderMetal/tests/R4StaticConstantGroupsProbe.cpp`
- `MetalR4EngineScene.h` is snapshotted in `prestate/` and unchanged by this proposal.
