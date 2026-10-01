# V8 static constant groups: conditional same-binary screen

This is a preparation-only copy of the V7 fixed-condition performance runner. It has not been run. It deliberately contains `V8_NOT_BUILT` and `V8_MANIFEST_NOT_BUILT` sentinels, so it fails closed until the reviewed V8 build, executable hash, and source-manifest hash are available and explicitly pinned here.

## Planned comparison

Run one arm pair using the same V8 executable and the same canonical input in two fresh runtime clones:

| Arm | Compute skin | Dynamic shadow index | Flora instancing | Static constant groups |
|---|---:|---:|---:|---:|
| `off` | on | on | on | unset / default off |
| `on` | on | on | on | `XRAY_METAL_STATIC_CONSTANT_GROUPS=1` |

The corrected raster-depth and instanced-Flora shader bundles are pinned identically in both arms. The V2 canonical runtime, `rescue-cordon-control` save, `user.ltx`, weather and camera are inherited from the accepted V7 setup. Each arm gets a separate clone. The copied save and profile are hashed before launch; neither source runtime nor the original app is modified.

Use the V7 profile protocol unchanged: 30-second warmup, 60-second measured interval, 15-second drain, fixed Cordon camera, device factor 1, world factor 6, `w_clear2` with paused weather. The only intended environment delta is the static-groups flag. The runner sanitizes profiling/capture/observer/oracle experiments, then explicitly enables compute skin and dynamic shadow indexing for both arms. Verify the logged environment and actual camera, resolution, weather vectors, calendar markers, callback trace integrity, frame gaps, thermal state, swap, executable and source hashes before comparing. These are Lua callback-throughput measurements; no presented/display FPS claim follows. Start with off then on. Consider a reverse-order pair only if the first pair is directionally promising but order or run variance leaves the result unclear.

The runner and the V7 Lua source are separate from the sealed V7 harness. The cloned Lua is byte-identical to the V7 `live_profile_no_capture.lua`; its SHA is recorded per run. The V8 manifest check verifies every listed source hash against the live source tree. Once V8 is built, replace both sentinels with root-reviewed immutable hashes and add the new V8 bundle receipts/artifacts if the build changed any shader inputs. Then run `bash -n` and static preflight review again. There is no build, app launch, or game run authorized by this preparation.

## Existing native final-render readback

There is a lightweight in-engine path, but no standalone export command or environment toggle. `IR4Backend::CopyLatestCompletedSaveThumbnail` exposes the most recently completed final-color frame as a CPU-owned 128x128 BGRA8 snapshot. The Metal backend downsamples `context.finalColor` into a shared buffer and copies completed command-buffer output into this snapshot. The existing game-save screenshot path consumes it and writes a 128x128 DXT1 DDS (`SM_FOR_GAMESAVE`); ordinary OS/window capture is not involved.

This could provide a small final-scene diagnostic in a future isolated run by causing a normal save in the cloned runtime after at least one completed frame, then inspecting the generated DDS. At 128x128 it is too small for full visual acceptance. The current V7 performance Lua can save only at measure start when its save-control is enabled, so it should not be toggled for this comparison. No capture/save modification is included here. The existing color probe is unsuitable as a substitute: it emits attachment-level PPM/JSON records across multiple render stages and previously generated 123 PPM plus 102 JSON artifacts, while its scene-before-postprocess image is not the presented final image.
