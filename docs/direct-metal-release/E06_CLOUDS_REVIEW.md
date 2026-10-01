# E06 6B clouds configuration review — 2026-09-27

This is a bounded, read-only review of the active materialized profile and the existing S6 weather evidence. It does not change weather data or claim cloud visual parity.

The winning materialized profile is `runtime/macos-gamma/profile` (`profile-manifest.json` selects MO2 profile `MyTree`). The tested weather descriptors in `profile/gamedata/configs/environment/weathers/` are byte-identical to the corresponding `profile-top-wins-probe` copies:

| Weather | Descriptor records | `clouds_color` alpha | Non-empty `clouds_texture` |
|---|---:|---|---|
| `w_storm1` | 30 | always `0` | none |
| `w_clear1` | 28 | always `0` | none |
| `w_cloudy3_dark` | 28 | always `0` | none |

A bounded scan of all `.ltx` descriptors in the active materialized weather directory found no `clouds_color` alpha greater than zero. `w_storm2` and `w_storm3` do author `sky\\sky_oblaka` in some time sections, but those sections also have alpha zero, so they are not a usable positive cloud fixture. The current profile therefore offers no existing active nonzero-alpha cloud preset; clouds are disabled by authored weather configuration in the tested fixtures.

The S6 run (`scratch/release_runs/20260927-e06-S6-rain`) records `w_clear1` with empty cloud texture and zero cloud color. Its `METAL_CLOUD_CAPTURE` at frame 27 is `valid=false textures=, vertices=0 indices=0 alpha=0.000`. This is consistent with the descriptor fields; it is not evidence that a configured cloud layer was lost during rendering. The S6 script transitions to `w_storm1`, whose active-profile descriptor also has empty cloud texture and zero alpha.

Source gates agree with that result. `CEnvironment::RenderClouds` exits when `CurrentEnv->clouds_color.w` is zero (`src/xrEngine/Environment_render.cpp`). `dxEnvironmentRender::OnFrame` exports a cloud snapshot only when both current descriptors have cloud texture names and the CPU cloud geometry is non-empty (`src/Layers/xrRender/dxEnvironmentRender.cpp`). Metal's legacy `RenderSky` and `RenderClouds` entry points return early; the active scene path prepares and draws clouds from the snapshot. `drawCloudScene` also exits without drawing if the snapshot/resources/view are invalid or `cloudsColor[3] <= 0` (`src/Layers/xrRenderMetal/MetalR4EngineScene.mm`). Thus the observed skip is explained by inputs before a Metal cloud draw could be admitted.

`runtime/macos-gamma/appdata/user.ltx` has no cloud enable switch; its `r2_dof_sky` setting is unrelated. No weather/profile edits, build, cook, game run, or capture were made for this review. `implemented`, `runtime exercised`, `reference matched`, and `human accepted` cloud status remain distinct; runtime draw and visual acceptance remain OPEN. A valid cloud-positive fixture requires an existing authored weather descriptor with both non-empty texture identities and positive blended alpha; none was found in the active materialized profile, so clouds must not be fabricated to manufacture a pass.
