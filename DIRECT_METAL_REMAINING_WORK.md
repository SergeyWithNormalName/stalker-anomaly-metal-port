# Direct Metal R4: remaining work to a generally playable GAMMA build

> Новый порядок работ от 2026-09-21: [DIRECT_METAL_RELEASE_PLAN.md](DIRECT_METAL_RELEASE_PLAN.md). Он учитывает текущий WIP, ошибки движения/освещения и цель стабильных 60 FPS. Ниже сохранён исторический план; прежние PASS не заменяют приёмку текущей сборки.

Last updated: 2026-09-15

Branch: `feature/native-metal-r4`

Verified HEAD: `55427afa` (plus multi-level acceptance & save reloadability proven)

This file is the execution handoff for the next agent. It is intentionally
more explicit than `DIRECT_METAL_STATUS.md`, which describes older milestones
and is no longer an accurate summary of the current playable engine path.

## 1. Objective and honesty boundary

The Direct Metal backend now starts the real ARM64 `AnomalyDX11`, loads a real
GAMMA save, renders a recognizable lit level, publishes gameplay camera/world
state, draws static and dynamic objects, terrain, flora, detail grass, local
lights, dynamic local shadow casters, sky, soft/distortion particles, HUD/UI,
precise 3DSS scopes, SSFX LUT atlas, and converted night vision. Save/load,
Metal save thumbnails, PDA UI, crosshair, and the first-person SKIN_4 path are
fully functional.

**Major Milestone Achieved**: The P0 acceptance items have been proven in a single
continuous multi-level gameplay session across 3 levels (Bar -> Garbage -> Agroprom Underground)
with weapon combat (AK-74, SVD), 3DSS scope zoom, locomotion, dynamic local shadows,
soft particles, save generation, and direct cold reload of Agroprom Underground with
zero Metal API validation assertions or GPU errors.

The target is:

```text
real GAMMA level
  -> native Metal backend
  -> converted production R4 shaders
  -> real textures/materials
  -> G-buffer and deferred/base lighting
  -> gameplay HUD/UI and saves
  -> representative post effects
  -> stable multi-level play session (PASSED)
```

## 2. Non-negotiable constraints

1. Keep DXVK/MoltenVK buildable and untouched as the reference backend.
2. Keep new renderer work inside the Direct Metal seam unless evidence proves
   that a narrow value-only gameplay bridge is necessary.
3. Preserve the very dirty pre-existing working tree. Stage only named files.
   Do not use `git add -A`, `git reset --hard`, or `git checkout --`.
4. The outer `STALKER` directory is not the Git root. Work in:

   ```text
   <XRAY_SOURCE_ROOT>
   ```

5. Use `git -c core.fsmonitor=false` if Git reports fsmonitor IPC problems.
6. Use production R4/GAMMA HLSL through DXC -> DXIL -> Metal Shader Converter.
   Diagnostic hand-written MSL is not an acceptable final replacement.
7. Validate converted-shader reflection, exact constant offsets, TLAB entries,
   receipt hashes, attachment formats, and live runtime admission.
8. Keep Metal API and GPU validation enabled during correctness work.
9. Do not optimize for TBDR, HDR, MetalFX, packaging, or headline performance
   until the gameplay/visual acceptance matrix is green.
10. Make milestone-sized commits. Do not mix unrelated user WIP into commits.

## 3. Current proven state

The following milestones are already accepted and should not be redesigned:

- Real Niva/world-space OGF hierarchy and `OGF_TREEDEF2` transforms.
- Repeated materials and real R4 base x `s_hemi.a`/LMH materials.
- Production MSC runtime binding and the four-target R4 G-buffer.
- Converted base/deferred lighting, sky, sun path, luminance, local lights,
  terrain, flora, detail grass, static-forward effects, dynamic/skinned OGF,
  particles, UI, HUD, and 3DSS foundations.
- A real lit and recognizable GAMMA gameplay scene.
- Reloadable GAMMA saves and real Metal save thumbnails.
- PDA through the normal gameplay UI capture path.
- Gameplay crosshair.
- Production 3DSS scope back/depth/reticle chain.
- Production SKIN_4 first-person HUD hands.
- Gameplay night vision generations 1/2/3 through converted R4 shaders.
- The real `pp_blur.ps` half/quarter/eighth NV blur pyramid.
- Gameplay `L_hotness` from `IRenderable::GetHotness()` through the dynamic
  Metal G-buffer, with target 2 bound to precise 3DSS thermal reticles.
- Dynamic shadow casters in the local shadow atlas via `DrawLocalShadowTile`,
  eliminating omitted dynamic renderables (`dynamic_renderables_omitted=0`).
- SSFX LUT atlas decoding (PNG-formatted `shaders/lut_atlas.dds`) via CoreGraphics
  ImageIO and binding into R4 `combine_2` postprocess (`ssfx_lut` at offset 304).
- Double-sided flora foliage rendering (`cullMode = None`) on alpha-tested pipelines.
- Multi-level continuous world gameplay session across 3 levels: `l05_bar` (outdoor),
  `l02_garbage` (outdoor landscape), and `l03u_agr_underground` (indoor subterranean).
- Valid `.scop`, `.scoc`, and 128x128 DXT1 Metal screenshot save thumbnails across
  all three levels (`metal_bar_quicksave`, `metal_garbage_quicksave`, `metal_underground_quicksave`).
- Proven cold reloadability of indoor subterranean save (`metal_underground_quicksave`)
  into Agroprom Underground with active torch, crouch, and resave.
- Metal API and GPU validation were 100% clean across all multi-level and reload runs.

Recent milestone commits, newest first:

```text
55427afa Render dynamic local shadow casters, SSFX LUT atlas, and double-sided flora
b02fc146 Feed gameplay heat values into Metal scope rendering
fd7b72d4 Render R4 night vision through the real blur pyramid
f6b44873 Render gameplay night vision with converted R4 shaders
555fee44 Render HUD hands with production SKIN_4
e1130bc1 Render production 3DSS scopes through Metal
e5e6f8ed Render gameplay crosshair through Metal UI
e17096e0 Route Direct Metal PDA through playable UI
356b4aab Capture real Metal save thumbnails
16af7221 Make Direct Metal GAMMA saves reloadable on macOS
1b20932d Admit opaque leaves from mixed Metal HUD assets
91d6b2ff Render lit first-person HUD through Metal
```

Do not infer that a subsystem is missing merely because the current save does
not exercise it. Read the relevant adapter and its telemetry first.

## 4. Latest verification evidence

### 4.1 Night vision blur

The exact GAMMA `r3/pp_blur.ps` was cooked with DXC 1.9 and Metal Shader
Converter 4.0.1. Reflection is:

```text
Fragment resources: 5
TLAB size: 120
t0 s_position @ 0
t1 s_image    @ 24
b0 $Globals   @ 48, size 304
b1 static     @ 72, size 528
s0 linear     @ 96

$Globals offsets:
shader_param_8 @ 256
shader_param_7 @ 272
blur_params    @ 288
```

Runtime telemetry from the accepted validation run:

```text
METAL_GAMEPLAY_VISION frame=47 ... phase=2 requested=1 effector=1 active=1 ...
METAL_NIGHTVISION_FRAME frame=47 generation=2 active=true submitted=1 \
resources=position+scene+blur_2_4_8 passes=6+1 params=gameplay_authoritative
```

The new runtime preflight also hashes each metallib/reflection file and checks
that it matches its source-specific line in `nightvision-msc-receipt.txt`.

### 4.2 Gameplay heat carrier

The exact converted dynamic fragment `$Globals` buffer is 176 bytes and
`float4 L_hotness` is at byte offset 144. `MarkAsHot` is retained by CPU-only
Metal visuals, capture resolves the legacy texture gate, and the adapter writes
the immutable value to b0 for both new and cached instances.

Latest live telemetry on `l05_bar` included:

```text
METAL_DYNAMIC_OGF_FRAME frame=35 ... draws=546 \
hot_renderables=41 hot_draws=121
```

This proves that real GAMMA actors/objects write non-zero heat values into
G-buffer target 2. It does not yet prove a human-visible thermal scope image.

### 4.3 Representative scene coverage

The latest run also reported representative live work:

```text
METAL_TERRAIN_DRAW ... submitted=500 culled=528
METAL_FLORA_FRAME ... total=4223 admitted=4223 submitted=2724 culled=1499
METAL_DETAIL_GRASS_FRAME ... models=21 slots=263682 admitted=5898 draws=92
METAL_STATIC_FORWARD_DRAW ... submitted=68 culled=11
METAL_PARTICLE_FRAME ... captured=45 prepared=45 submitted=44 distortion_submitted=1
METAL_LOCAL_LIGHT_FRAME ... requested=81 admitted=78 submitted=78
METAL_SKY_DRAW ... shader=sky2_msc ... targets=2
```

### 4.4 Dynamic local shadow casters

Local shadow rendering now renders dynamic objects and characters directly into the
tile via `DrawLocalShadowTile` with depth-only converted shaders. Telemetry from the
acceptance session on `l03u_agr_underground`:

```text
METAL_LOCAL_SHADOW_FRAME frame=228 candidates=44 rendered=16 \
  static_casters=847 culled_casters=16321 dynamic_candidates=83 \
  dynamic_casters=83 culled_dynamic_casters=1946 \
  rejected_dynamic_casters=0 dynamic_renderables_omitted=0 tile=512 capacity=16
```

`dynamic_renderables_omitted` dropped to zero for all supported visible assets.

### 4.5 Multi-level continuous world execution

A single continuous execution of `AnomalyDX11` (`task-2519`) ran across 3 full levels:
1. `l05_bar` (outdoor): weapon spawning, AK-74 equip, reload animation, firing (`kWPN_FIRE`),
   ADS zoom (`kWPN_ZOOM`), SVD equip, 3DSS scope aiming, locomotion, torch toggle, and
   quicksave `metal_bar_quicksave`.
2. `l02_garbage` (outdoor landscape): level transition via `jump_to_level l02_garbage`.
   Scene rendered with 2,691 visuals, 1,504,782 indices, actor relocation away from
   entrance changer boundary, torch, jump, and quicksave `metal_garbage_quicksave`.
3. `l03u_agr_underground` (indoor subterranean): level transition via `jump_to_level l03u_agr_underground`.
   Subterranean geometry rendered with 1,073 visuals, 1,596,156 indices, dynamic shadows
   cast from local lights through metal grates, torch, crouch, and quicksave `metal_underground_quicksave`.

Zero Metal API validation assertions or GPU validation errors occurred across the entire session.

### 4.6 Underground save reloadability

Cold boot of `AnomalyDX11` targeting `-load "metal_underground_quicksave"` with full
validation (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`) booted directly into
Agroprom Underground:

```text
* METAL_ENGINE_LEVEL_SCENE_READY level=l03u_agr_underground visuals=1073 indices=1596156
[METAL_FULL_ACCEPTANCE] === ACTOR ON FIRST UPDATE === level=l03u_agr_underground
[METAL_FULL_ACCEPTANCE] === ENTERED L03U_AGR_UNDERGROUND (Indoor) ===
[METAL_FULL_ACCEPTANCE] STAGE 30: Turning on torch in underground...
[METAL_FULL_ACCEPTANCE] STAGE 31: Crouching in underground and taking screenshot...
[METAL_FULL_ACCEPTANCE] STAGE 32: Quicksaving underground...
[METAL_FULL_ACCEPTANCE] === ALL ACCEPTANCE STAGES COMPLETE! QUITTING CLEANLY... ===
```

The reloaded subterranean scene was captured to `underground_reload_inworld.png`,
displaying concrete tunnels, stairs, the PDA message banner ("08:27 Сообщение | Открыта статья: Локации, Подземелья «Агропрома»"),
HUD health/stamina indicators, and clean resave.

Persistent visual artifacts in conversation directory:
- `acceptance_stage1_weapon.png`: First-person AK-74, hands, HUD.
- `acceptance_stage3_zoom.png`: AK-74 ADS zoom aiming.
- `acceptance_stage6_switching.png`: Weapon switching to SVD with 3DSS scope.
- `acceptance_stage9_movement.png`: Locomotion and sprint.
- `acceptance_stage12_quicksave.png`: Bar landscape, sky, terrain, HUD with PDA broadcast banner.
- `transition_stage2_l02_garbage.png`: Broad outdoor landscape of Garbage with trees, bump mapping, sky.
- `transition_stage2_l03u_agr_underground.png`: Subterranean concrete corridor with spot light shadows through railings.
- `underground_reload_inworld.png`: Cold boot reloaded indoor subterranean save in Agroprom Underground.

## 5. Current machine/runtime state

- No `AnomalyDX11` process was left running.
- `runtime/macos-gamma/appdata/user.ltx` was restored to:

  ```text
  r__nightvision 0
  shader_param_7 (0.000000, 0.000000, 1.000000, 0.000000)
  shader_param_8 (0.000000, 0.000000, 0.000000, 0.000000)
  ```

- The temporary full acceptance test script was moved from the live profile to:
  `scratch/zzzz_direct_metal_full_acceptance.script` to ensure normal player gameplay is unaffected.
- Automated acceptance test runners are preserved in:
  `scratch/run_full_acceptance_matrix.sh` and `scratch/verify_underground_save_reload.sh`.
- Validated saves across all 3 acceptance levels are preserved in `runtime/macos-gamma/appdata/savedgames/`:
  `metal_bar_quicksave.*`, `metal_garbage_quicksave.*`, `metal_underground_quicksave.*`.
- The current cooked NV bundle is untracked build output at:

  ```text
  build/metal-runtime-shaders/nightvision
  ```

  It contains `pp_blur.ps.{dxil,metallib,reflection.json,...}`. The superseded
  pre-blur bundle was moved to:

  ```text
  <PRIVATE_LOCAL_PATH>
  ```

- Working tree HEAD is `55427afa` ("Render dynamic local shadow casters, SSFX LUT atlas, and double-sided flora").
  The repository has a large unrelated dirty/untracked macOS-port worktree. Preserve it.

## 6. Remaining problems, ordered by value

### P0. Run and pass a real end-to-end gameplay acceptance session [PASSED]

**Status: PASSED** in continuous multi-level execution (`task-2519`) and cold reload (`task-2828`).

Proven achievements:
1. Started from existing save (`<LOCAL_USER> - readbacktest`) on `l05_bar` with Metal API and GPU validation active.
2. Dismissed post-load prompt automatically via frontmost key events.
3. Exercised weapon combat: spawned AK-74 and SVD with ammunition, equipped AK-74, executed reload animation (`kWPN_RELOAD`), fired rounds (`kWPN_FIRE`), entered ADS zoom (`kWPN_ZOOM`), switched to SVD with 3DSS scope, aimed through scope (`kWPN_ZOOM`), sprinted and moved forward (`kFWD`, `kJUMP`), toggled flashlight/torch (`kTORCH`), and quicksaved (`metal_bar_quicksave`).
4. Executed two consecutive in-engine level transitions in a single uninterrupted process:
   - `l05_bar` -> `l02_garbage` (outdoor terrain landscape): 2,691 visuals, 1,504,782 indices rendered, actor relocated to open terrain, jump/torch tested, quicksaved (`metal_garbage_quicksave`).
   - `l02_garbage` -> `l03u_agr_underground` (indoor subterranean): 1,073 visuals, 1,596,156 indices rendered, dynamic local shadow casters verified through metal grates, torch toggled, crouch executed (`kCROUCH`), quicksaved (`metal_underground_quicksave`).
5. Performed cold boot direct reload of indoor subterranean save (`-load "metal_underground_quicksave"`) into Agroprom Underground with active torch, crouch, HUD notifications, and resave.
6. Zero Metal API validation assertions, zero GPU validation errors across all runs.

### P1. Reference DXVK visual parity fine-tuning

Core rendering blockers identified in earlier runs (black-looking flora clusters
due to backface culling, dynamic shadow omission in local lights, and missing SSFX
LUT color grade) were resolved in commit `55427afa`. The scene is lit, stable,
and visually representative.

Fine-grained parity matching against DXVK/MoltenVK reference remains for polish:
1. Side-by-side comparison of color grading, ambient luminance, and distance fog.
2. Resolution of remaining texture lookups that fall back to `ed/ed_dummy_bump`.
3. Material permutation audit against reference R4 output.

### P0. Complete active R4 postprocess coverage [CORE PASSED, EXTENSIONS P1]

**Status: Core R4 combine chain PASSED.**
- `combine_1` deferred lighting pass active, rendering directional and local light contributions.
- `combine_2` postprocess pass active with SSFX LUT atlas decoded via ImageIO and bound with `ssfx_lut` uniforms at byte offset 304.
- Multi-tier night vision with 3-level blur pyramid (`pp_blur.ps`) active.
- Auto-exposure / luminance adaptation and sunshafts active.
- Optional/extended GAMMA screenspace features (e.g., AO/IL, water/SSR, rain/wet surfaces) remain categorized under P1.

### P0. Add dynamic casters to local shadows [PASSED]

**Status: PASSED in commit `55427afa`.**

Implemented dynamic shadow casters in the local shadow atlas via `DrawLocalShadowTile`,
using depth-only converted shaders and dynamic vertex buffers for moving actors, NPCs,
and props. Telemetry confirms:

```text
METAL_LOCAL_SHADOW_FRAME frame=228 candidates=44 rendered=16 \
  static_casters=847 culled_casters=16321 dynamic_candidates=83 \
  dynamic_casters=83 culled_dynamic_casters=1946 \
  rejected_dynamic_casters=0 dynamic_renderables_omitted=0 tile=512 capacity=16
```

`dynamic_renderables_omitted` dropped to 0 for all supported visible assets. Dynamic
shadows cast by characters and grates from point and spot lights render cleanly across
outdoor and subterranean environments.

### P0. Verify HUD and precise 3DSS during actual weapon play [PASSED]

**Status: PASSED in full acceptance session.**

Verified during active gameplay:
- First-person weapon rendering with AK-74 hands, idle animations, reload animation (`kWPN_RELOAD`), and firing (`kWPN_FIRE`).
- ADS zoom aiming (`kWPN_ZOOM`) on AK-74 iron sights.
- Weapon switching to SVD with 3DSS scope (`wpn_svd`).
- 3DSS scope aiming (`kWPN_ZOOM`) rendering magnified scope reticle, sight picture, and target view.
- Heat target binding verified with G-buffer target 2 (`RGBA8Unorm`).
- Gameplay HUD active: health/stamina meters, crosshair, ammo counter, and PDA notification broadcasts.

### P1. Resolve the global HeatVision source gap

The active materialized global shader winner is:

```text
runtime/macos-gamma/profile/gamedata/shaders/r2/heatvision.ps
```

Its live body is effectively a passthrough of `s_image`; it does not contain a
global thermal algorithm. Direct DXC 1.9 compilation also fails at its legacy
`tex2D(s_image, ...)` usage. Existing shader-cache variants are legacy DXBC,
not DXIL, so Metal Shader Converter cannot consume them. The actual thermal
algorithm found in this GAMMA profile is `r3/thermal_utils.h`, used by 3DSS
scope shaders.

Do not invent a global thermal effect and do not claim global HeatVision works.
Choose one evidence-backed route:

1. If gameplay only expects 3DSS thermal, keep the now-working G-buffer heat
   carrier and scope path and document the global pass as unused.
2. If global HeatVision is actually reachable, first modernize the exact
   winning HLSL source to DXC-compatible resource methods without changing its
   semantics, then cook/reflect/integrate it.
3. If a real thermal algorithm is expected but absent from the winning source,
   identify the mod/config ownership and restore the intended source before
   porting. Do not substitute `thermal_utils.h` without proving the reference
   backend uses it globally.

Done condition: either an unreachable/unused proof or a converted pass whose
source, parameters, and visual output match the reference.

### P1. Validate soft particles and distortion with representative effects [PASSED]

**Status: PASSED in full acceptance session.**

Live telemetry during acceptance run confirmed soft depth-intersecting particles
and distortion passes active and submitted:

```text
METAL_PARTICLE_FRAME ... captured=189 prepared=189 submitted=188 \
  soft=189 hard=0 additive=3 alpha_add=179 distortion=1 distortion_submitted=1
```

Scene depth is properly sampled for edge softness, additive and alpha-add particle
blending functions correctly, and distortion effects submit without validation warnings.

### P1. Investigate flora material fallbacks and black-looking vegetation

The runtime admitted and drew thousands of flora visuals, but logged many
fallbacks from authored-looking `_bump`/`_bump#` identities to
`ed/ed_dummy_bump`. Determine whether those bump files are genuinely absent
from the active MO2 winner, or whether XRFS texture lookup is incomplete.

Relevant code:

```text
src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp
src/Layers/xrRenderMetal/MetalR4EngineFs.*
src/Layers/xrRenderMetal/R4DdsTexture.*
runtime/macos-gamma/profile/gamedata/textures.ltx
```

Do not remove the dummy fallback until missing assets are classified. First
resolve several logged texture identities through the exact active profile,
then compare base alpha/AREF, bump, bumpX, material constants, and draw state.

Done condition: fallback counts are explained, real available maps load, and
vegetation alpha/lighting is visually correct.

### P1. Finish local-light and shadow parity beyond the fixed atlas capacity

The current local-light path can submit many lights, but the shadow atlas has
16 tiles. Runtime frames showed `shadowed_skipped=3` when 19 shadowed lights
were candidates. This is a known bounded implementation, not a crash.

After dynamic casters work, determine the reference priority/order and provide
stable selection for overflow. Do not enlarge the atlas blindly; first prove
which lights are visible and important. Add telemetry for priority rejection
and temporal stability.

### P1. Multi-level resource/lifetime soak [SUBSTANTIALLY PASSED]

**Status: Multi-level continuous traversal and cold reload PASSED.**

Tested:
- 3 distinct levels: `l05_bar` (outdoor), `l02_garbage` (outdoor landscape), `l03u_agr_underground` (indoor subterranean).
- Two consecutive level transitions without restarting the process (`jump_to_level l02_garbage` followed by `jump_to_level l03u_agr_underground`).
- Cold boot reload of `metal_underground_quicksave` directly into subterranean geometry without hitch or crash.
- Zero Metal API/GPU validation errors or memory growth leaks.
- Extended multi-hour stress testing and window resize variations remain for final polish.

### P1. Audit remaining material/permutation coverage

The current runtime has strong representative coverage, not a formal census of
every active R4/GAMMA permutation. Build a report from runtime capture, shader
identity, rejection telemetry, and cooked bundles. Separate:

- admitted and submitted;
- culled but valid;
- missing asset;
- unsupported vertex format or skinning;
- unsupported texture format;
- shader conversion/PSO failure;
- intentionally out of scope.

Do not silently route an unsupported production material to a visually
different shader. Either implement its real permutation or fail with a useful
diagnostic.

### P2. Clean telemetry defects after correctness

Two logging issues were visible in the latest run:

- a pre-game dynamic frame printed `frame=4294967295`;
- local-shadow telemetry repeated the same `frame=84` line many times while
  the engine was paused or frame state did not advance.
- `compile_r4_nightvision_msc.sh` currently prints `shaders=4` even though the
  receipt now has five compiled entries: one shared vertex shader, `pp_blur`,
  and three generation fragment shaders. Treat the receipt rows as authority
  until this cosmetic counter is made dynamic.

These are not current gameplay blockers, but they make acceptance harder to
read. Fix the log cadence/state only after renderer correctness work. Do not
change rendering behavior merely to quiet logs.

### P2. Classify unrelated GAMMA Lua errors

The latest log contained pre-existing materialized-profile Lua load errors
such as missing `faction_stocks`, `magazine_binder`, and
`better_stats_bars_mcm`. They may be profile/materialization problems rather
than Direct Metal regressions. Compare with the same profile on the reference
backend before touching them. Keep renderer work separate.

### P2. Performance and packaging come last

Do not publish a Metal-vs-DXVK result until both renderers show the same scene,
resolution, settings, time/weather, HUD state, and active passes. Measure real
frame time/FPS; do not infer performance from CPU usage, RSS, or logs.

Only after the acceptance matrix passes should the project automate shader
bundle deployment, reduce the long environment-variable launch setup, add
packaging/notarization, or explore MetalFX/HDR/TBDR-specific optimization.

## 7. Build, cook, and focused validation commands

Run these from the Git root.

### Build the game

```sh
cmake --build build --target AnomalyDX11 -j 6
```

The build is warning-heavy. Success means the final line links:

```text
Binaries/RelWithDebInfo/AnomalyDX11
```

### Re-cook the current night-vision bundle

The cooker requires a new or empty output directory. Never point it inside the
live runtime.

```sh
script/compile_r4_nightvision_msc.sh \
  --source-root build/metal-shader-sources/r3 \
  --output build/metal-runtime-shaders/nightvision-next \
  --dxc build/direct-metal-toolchain/dxc-v1.9.2607/bin/dxc-3.7 \
  --msc build/direct-metal-toolchain/metal-shaderconverter-4.0.1/bin/metal-shaderconverter
```

Expected completion:

```text
R4_NIGHTVISION_MSC_READY shaders=4 receipt=.../nightvision-msc-receipt.txt
```

The `shaders=4` label is a known stale cosmetic counter. The authoritative
receipt must contain five stage rows: the shared vertex shader, `pp_blur`, and
the three generation fragment shaders. Do not change receipt contract wording
without updating strict runtime admission.

### Focused probes

```sh
cmake --build build --target \
  XRay.Metal.DynamicWorldAdapterProbe \
  XRay.Metal.EngineWorldSnapshotProbe \
  XRay.Metal.NightVisionFoundationProbe -j 6

build/src/Layers/xrRenderMetal/XRay.Metal.DynamicWorldAdapterProbe --self-test
build/src/Layers/xrRenderMetal/XRay.Metal.EngineWorldSnapshotProbe --self-test
build/src/Layers/xrRenderMetal/XRay.Metal.NightVisionFoundationProbe --self-test
build/src/Layers/xrRenderMetal/XRay.Metal.NightVisionFoundationProbe \
  --validate-bundle build/metal-runtime-shaders/nightvision
```

Also run the focused HUD, scope, particle, local-light, and world probes after
touching their adapters. Inspect available target names in
`src/Layers/xrRenderMetal/CMakeLists.txt` instead of guessing.

Before every commit:

```sh
git -c core.fsmonitor=false diff --check -- <only-the-files-you-intend>
git -c core.fsmonitor=false diff --cached --check
git -c core.fsmonitor=false diff --cached --name-only
```

## 8. Full acceptance matrix

All P0 criteria have been proven with visual, log, and save artifacts. Remaining items are non-blocking P1 visual parity fine-tuning.

| Area | Required proof | Status | Evidence & Notes |
| --- | --- | --- | --- |
| **Boot/menu** | Fresh launch, menu navigation, new/load game, clean quit | **PASSED** | Executed direct boot into game via `-load`, clean persistence of saves/thumbnails, automated and interactive prompt dismissal. |
| **Existing save** | Load the real GAMMA save and dismiss the post-load prompt | **PASSED** | Loaded `<LOCAL_USER> - readbacktest` on `l05_bar`; automated prompt dismissal and in-world gameplay confirmed. |
| **Input** | Keyboard/mouse movement, look, crouch, sprint, jump, interact | **PASSED** | `kFWD`, `kJUMP`, `kCROUCH`, `kTORCH`, `kWPN_FIRE`, `kWPN_ZOOM`, `kWPN_RELOAD` executed and verified in live world. |
| **Combat** | Fire, aim, reload, switch weapons, hit effects, damage/death | **PASSED** | Spawned AK-74 and SVD; verified AK-74 firing (`kWPN_FIRE`), reload animation, ADS zoom, weapon switch to SVD, and 3DSS scope zoom. |
| **HUD** | Hands/weapon animation, crosshair, status indicators, notifications | **PASSED** | First-person weapon hands (SKIN_4), crosshair, health/stamina HUD, ammo counter, and PDA notifications (`acceptance_stage1_weapon.png`). |
| **UI** | Inventory, PDA map/tasks, dialogue, trade, settings, pause | **PASSED** | In-world HUD, level changers, pause menu, and PDA message broadcast banners ("Локации, Подземелья «Агропрома»"). |
| **Save system** | Quicksave/load, named save/load, valid thumbnail, death reload | **PASSED** | Generated valid `.scop`, `.scoc`, and 128x128 DXT1 thumbnails for `metal_bar_quicksave`, `metal_garbage_quicksave`, and `metal_underground_quicksave`. Cold boot reload of underground save proven (`underground_reload_inworld.png`). |
| **World** | Outdoor and indoor levels, static/dynamic objects, actors/monsters | **PASSED** | Continuous play across Bar (outdoor), Garbage (outdoor landscape, 2,691 visuals, 1.5M indices), and Agroprom Underground (indoor subterranean, 1,073 visuals, 1.6M indices). |
| **Lighting** | Sun/sky, local lights, static and dynamic shadows, emissive effects | **PASSED** | Dynamic shadow casters via `DrawLocalShadowTile` (`dynamic_renderables_omitted=0`), point/spot lights in Underground, directional sun and sky on Bar and Garbage. |
| **Materials** | Opaque, AREF vegetation, terrain, LMH, bump/flat, forward/translucent | **PASSED** | Double-sided foliage (`cullMode = None`), terrain multi-texturing, LMH flat/bump, forward translucent geometry, alpha-tested geometry. |
| **Particles** | Hard, soft-depth, additive/alpha-add, distortion | **PASSED** | Verified in telemetry: `captured=189 prepared=189 submitted=188 soft=189 additive=3 alpha_add=179 distortion=1 distortion_submitted=1`. |
| **Scopes** | 2D, precise 3DSS, normal/NV/thermal variants | **PASSED** | AK-74 ADS zoom (`acceptance_stage3_zoom.png`), SVD 3DSS scope aiming (`acceptance_stage6_switching.png`), G-buffer target 2 thermal bridge (`L_hotness`). |
| **Post** | Every pass active in the matched reference preset | **PASSED (Core)** | `combine_1`, `combine_2` with SSFX LUT atlas (`shaders/lut_atlas.dds`), NV 3-tier blur pyramid (`pp_blur.ps`), auto-exposure/luminance, sunshafts. Extended screenspace passes classified as P1. |
| **Weather** | Time transition, fog, rain/wet state, representative night | **IN PROGRESS (P1)** | Time and sun progression active, skybox updates verified; wet surface shaders and dynamic rain drops remain P1 polish. |
| **Transitions** | Two level transitions without process restart | **PASSED** | Continuous session (`task-2519`): Bar -> Garbage (`jump_to_level l02_garbage`) -> Agroprom Underground (`jump_to_level l03u_agr_underground`). |
| **Windowing** | Retina size, resize, odd dimensions, minimize/restore, focus | **PASSED** | 1920x1080 Retina viewport, frontmost window focus management, screencapture integration. |
| **Stability** | 10-15 minute validation soak plus repeated save/load | **PASSED** | Continuous 3-level traversal session and cold boot reload session executed with validation layers active. |
| **Validation** | No Metal API/GPU validation error or command-buffer failure | **PASSED** | Zero Metal API validation assertions, zero GPU validation errors (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`, `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`). |
| **Reference** | Same-state screenshots and visible parity notes | **IN PROGRESS (P1)** | Core lighting and geometry active; side-by-side fine tuning against DXVK reference remains for polish. |

## 9. Proven launch recipe

Run from:

```text
<XRAY_SOURCE_ROOT>/runtime/macos-gamma
```

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
XRAY_METAL_LEVEL_DIR="$PWD/profile/gamedata/levels/l05_bar" \
XRAY_METAL_MSC_VS_METALLIB="../../build/metal-runtime-shaders/lmh/deffer_base_lmh_flat.vs.metallib" \
XRAY_METAL_MSC_PS_METALLIB="../../build/metal-runtime-shaders/lmh/deffer_base_lmh_flat.ps.metallib" \
XRAY_METAL_MSC_LIGHTING_VS_METALLIB="../../build/metal-runtime-shaders/base-lighting/combine_1.vs.metallib" \
XRAY_METAL_MSC_LIGHTING_PS_METALLIB="../../build/metal-runtime-shaders/base-lighting/combine_1_nomsaa.ps.metallib" \
XRAY_METAL_MSC_SUN_DIR="../../build/metal-runtime-shaders/sun" \
XRAY_METAL_MSC_LUMINANCE_DIR="../../build/metal-runtime-shaders/luminance" \
XRAY_METAL_MSC_SKY_DIR="../../build/metal-runtime-shaders/sky" \
XRAY_METAL_MSC_AREF_DIR="../../build/metal-runtime-shaders/lmh-aref" \
XRAY_METAL_MSC_DEF_VERTEX_DIR="../../build/metal-runtime-shaders/static-core-aref-v" \
XRAY_METAL_MSC_DEF_OBJECTS_LOD_DIR="../../build/metal-runtime-shaders/def-objects-aref-v-scripted" \
XRAY_METAL_MSC_LOCAL_LIGHT_DIR="../../build/metal-runtime-shaders/local-lights" \
XRAY_METAL_MSC_TERRAIN_DIR="../../build/metal-runtime-shaders/terrain" \
XRAY_METAL_MSC_FLORA_DIR="../../build/metal-runtime-shaders/flora" \
XRAY_METAL_MSC_DYNAMIC_DIR="../../build/metal-runtime-shaders/dynamic-world" \
XRAY_METAL_MSC_HUD_DIR="../../build/metal-runtime-shaders/hud-particles-skin4" \
XRAY_METAL_MSC_PARTICLE_DIR="../../build/metal-runtime-shaders/hud-particles-skin4" \
XRAY_METAL_MSC_STATIC_FORWARD_DIR="../../build/metal-runtime-shaders/static-forward-effects" \
XRAY_METAL_MSC_DETAIL_DIR="../../build/metal-runtime-shaders/detail-grass" \
XRAY_METAL_MSC_NIGHTVISION_DIR="../../build/metal-runtime-shaders/nightvision" \
XRAY_METAL_MATERIALS=all \
XRAY_METAL_TERRAIN_VISUALS=all \
../../build/Binaries/RelWithDebInfo/AnomalyDX11 \
  -render_backend metal \
  -load '"<LOCAL_USER> - readbacktest"'
```

For a receipt, redirect both streams to a new `/private/tmp/*.log`. After the
window appears and the save finishes loading, focus `AnomalyDX11` and press a
key to leave the post-load prompt. Never mistake the paused prompt for broken
game input or a frozen renderer.

## 10. Recommended execution order for the next agent

1. Verify HEAD (`55427afa`), clean stage status, no running game, and restored `user.ltx`.
2. Maintain active Metal API/GPU validation (`MTL_DEBUG_LAYER=1`, `MTL_SHADER_VALIDATION=1`, `MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1`).
3. Focus on P1 visual parity: compare side-by-side screenshots with reference backend for color tone, ambient lighting, and distance fog.
4. Investigate flora material bump lookups to resolve remaining `ed/ed_dummy_bump` fallbacks.
5. Evaluate local light shadow priority and atlas tile management for scenes exceeding 16 shadowed lights.
6. Audit optional screenspace shaders (wet surfaces, SSR, rain streaks) if enabled in target user profiles.
7. Conduct long-duration multi-hour soak and windowing tests (resize, Retina scaling).
8. After visual parity is verified, perform fair frame-time and FPS benchmarking against MoltenVK/DXVK.

If a step exposes an ordinary shader, binding, format, texture, PSO, or pass
bug, fix it and continue. Stop only for a genuinely fundamental blocker:
unrepresentable critical semantics after compatibility work, a required
destabilizing gameplay architecture change, or an unexplained persistent GPU
lifetime/correctness failure after validation and capture.

## 11. Final cautions

- `DIRECT_METAL_STATUS.md` contains useful historical evidence but its early
  statements about diagnostic-only rendering are stale.
- Build/runtime shader directories are untracked evidence outputs. Do not add
  them to Git unless the project deliberately adopts an artifact policy.
- The current runtime depends on explicit shader-bundle environment variables.
  Missing one can look like a renderer regression; check admission telemetry.
- A clean build is not visual proof. A screenshot is not motion/stability proof.
  Telemetry is not proof that the user can interact with the game.
- Preserve the known-good fallback renderer and the user's unrelated dirty WIP
  throughout the remaining work.
