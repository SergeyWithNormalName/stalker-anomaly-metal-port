# Scene-owned portal pixel-oracle draft

This scratch draft combines the scene-owned frame-700 observer from `runtime-root-stage-04` with the isolated four-MRT plus D32S8 OFF/ON copy/readback flow from the main-Flora pixel-pair control. It changes only copied `MetalR4EngineScene.h/.mm` files and carries the existing private `FloraWindEnvelope.h` helper. No production file, adapter file, shader, build, or game output was changed.

The oracle runs after scene terrain inside the scene-owned G-buffer pass and before the existing `RunSceneOwnedGBufferCallback`. It clones the exact pre-Flora four-MRT and depth/stencil state, draws source-order Flora into the OFF target, draws the ON target with only the prepared visual-ID mask, compares all four MRTs and D32S8, and resumes the production pass. The ordinary world callback remains untouched and executes once afterward in source order. Shadows are encoded before this call and are not part of the mask.

The mask is built only for an explicit `source-vs-portals` diagnostic mode on frame 700. A source portal result with `rawVisualVisible[id] == 0` only nominates a broad-phase candidate; it does not prove GPU wind bounds. The Scene consumes the adapter's proposed value-owned `R4FloraRestRecord` list, calls the existing `flora_wind::compute` helper using the frozen frame's wind and bender inputs, and excludes an ID only when its outward-rounded current/previous union sphere misses every recorded frustum for every resolved owner sector. Missing rest records, unknown bounds, empty owners, missing owner frusta, invalid motion, duplicate IDs, or invalid generation remain visible. IDs are emitted in stable adapter record order.

The test rejects before it can pass if the candidate ID list is empty, if the candidate branch reports zero actual skipped camera-visible draws, if source/candidate accounting does not reconcile, if the source-order full transform check fails, or if any color/depth comparison fails. The output marks `source_portal_flags_are_gpu_wind_proof=false`. A passing isolated pair would qualify only that captured frame and pose; it would not prove general GPU culling safety or FPS improvement.

The frame observer also gains two receipt fields needed by the independent validator: seven exact SSA float-bit values and SHA-256 of the already-retained `impl->scene.geometryBytes`. It validates `mFullTransform` against the engine source-order product `mProject * mView` using `Fmatrix::mul`, matching `src/xrEngine/device.cpp`'s `mFullTransform.mul(mProject, mView)`. A mismatch keeps the observer invalid and prevents the pixel mask.

This is a review draft, not a build-ready patch. The adapter agent is preparing value records with `RestRecordsForValidation()`, but the copied stage-04 adapter header does not yet contain `R4FloraRestRecord` or that method. The adapter package also does not expose the validation-only draw/filter entry referenced by the Scene draft:

```cpp
DrawIntoActiveGBufferForPortalValidation(backend, frozenView,
    excludedVisualIds, sampledTelemetry, actuallySkipped, error)
```

That call is intentionally unresolved until root joins the separately owned adapter seam. The production draw method remains untouched. The draft does not add or modify `MetalR4FloraAdapter.cpp/.h`.

`SCENE_DELTA.patch` shows the scene-only delta against the stage-04 copies. `stage/` contains those scratch draft copies and the reused wind-bound helper.
