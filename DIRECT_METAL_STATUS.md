# Direct Metal R4 status

Date: 2026-09-09

Branch: `feature/native-metal-r4`

## Milestone A: first real Metal level

Status: **PASS for standalone diagnostic static geometry**.

The native Metal backend now renders the real loose-file winner for GAMMA
`l05_bar`, rather than synthetic bootstrap geometry. The source is the active
MO2 profile override from mod 442:

```text
runtime/macos-gamma/profile/gamedata/levels/l05_bar/level
  size: 2,486,998 bytes
  sha256: 878a4f068c235635bea056007b08472e9b159a923a289bb18919aa7d1f0263fe

runtime/macos-gamma/profile/gamedata/levels/l05_bar/level.geom
  size: 64,437,196 bytes
  sha256: baf5a15015b1b981133dce44c3525ca8d1491a3ceb7ac8811f40a53b34400b51
```

The archive copy is not used for this proof because the active profile
overrides it.

## What is rendered

The portable parser reads the real X-Ray chunk layout used by
`r4_loader.cpp` and `FVisual.cpp`:

```text
fsL_VB (9):  D3DVERTEXELEMENT9 declaration, vertex count, raw vertices
fsL_IB (10): index count, raw uint16 indices
fsL_VISUALS (3) / OGF_GCONTAINER (21):
             vb id/base/count + ib id/base/count
```

The active level contains:

```text
28 vertex buffers
4 index buffers
1,765,972 vertices
3,962,202 stored indices
8,075 validated static visual ranges
0 rejected visual ranges
13,208,628 submitted indices per diagnostic frame
```

All 28 declarations expose stream-0 `POSITION0/FLOAT3`. Every selected visual
passes buffer-range, local-index, finite-position, and bounds validation.
Private Metal VB/IB uploads are staged before the render encoder in the same
command buffer.

The draw path uses a fixed camera derived from the level bounds, the original
X-Ray `indexBase`, `indexCount`, and `vertexBase`, one cached immutable PSO,
and a `Depth32Float` attachment cleared each frame with `Less` depth testing
and depth writes enabled. A deliberately misaligned index-buffer offset is
rejected by the backend before encoding.

## Runtime acceptance

Command:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./build/metal-bootstrap/AnomalyMetalBootstrap \
  --frames 600 \
  --level-dir runtime/macos-gamma/profile/gamedata/levels/l05_bar
```

Final receipt: `/private/tmp/r4-metal-level-proof-final-2.txt`, SHA-256
`38a64267c21a79dd86b98a69c018b83859183452cd863a628cf3a851cac8c609`.

The tested `AnomalyMetalBootstrap` SHA-256 is
`52c2460465201c5c1ca530ed8545243611f856b19485289d1377048ee0183d6d`.

Acceptance requires the receipt to contain both validation-enabled banners and:

```text
METAL_LEVEL_READY source=real_xray_static ... vertex_buffers=28 index_buffers=4 visuals=8075 indices=13208628 pipeline_cache=1 ... depth=Depth32Float
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_LEVEL_COMPLETE source=real_xray_static frames=600 visuals_per_frame=8075 indices_per_frame=13208628 pipeline_cache=0 depth=Depth32Float invalid_index_offset=rejected materials=not_yet_bound
```

The cache contains one live PSO at `METAL_LEVEL_READY`; it is zero in the
completion line because shutdown drains every frame and then releases the
strong PSO cache before reporting completion.

The captured frame is
[`direct-metal-evidence/l05-bar-flat-metal-milestone-a.png`](direct-metal-evidence/l05-bar-flat-metal-milestone-a.png),
SHA-256 `e39505d7cd6f2b4cefd6468e44d9734682402fcbb6b148ed346ddd824ae4e5dd`.
It visibly contains the Bar terrain and built environment with depth occlusion.

## Exact acceptance boundary

This is the first real-level static-geometry milestone, not a playable Metal
renderer. It is still launched through the standalone backend executable. The
main engine's explicit `-render_backend metal` admission gate intentionally
stops before D3D11/Vulkan window creation until the R4 device wrapper is
connected.

The fragment shader is a deliberately flat diagnostic MSL shader used only to
isolate geometry, camera, indexed-draw, and depth correctness. The production
shader direction remains existing R4 HLSL through DXC and Metal Shader
Converter. No real textures, materials, sector/frustum visibility, dynamic or
skinned objects, gameplay camera, lighting, HUD, or postprocess are claimed.
The 600-frame run is a stability receipt, not a performance comparison.

The initial Phase 8 target begins with the real `fsL_SHADERS` material mapping,
DDS lookup/format validation, UV declaration binding, and base-texture sampling
for these same static visual ranges.

## Phase 8 progress: real base-texture diagnostic

Status: **PASS for one causally selected material submesh; Phase 8 remains partial**.

The level parser now preserves all 536 `fsL_SHADERS` slots, OGF header
`shader_id`, and the complete D3D9 vertex declaration. All 8,075 drawable
visuals resolve to a non-empty material. The selected proof follows this exact
chain:

```text
l05_bar visual 6138
  -> OGF_HEADER shader_id 294
  -> def_shaders\def_vertex/prop\prop_el_shkaf_01
  -> VB 3, base 26667, count 2161
  -> IB 1, base 624087, count 6123
  -> TEXCOORD0 SHORT2 at byte 28, converted as tc / 1024
```

The real base DDS comes from the active profile archive, not a fabricated or
replacement texture:

```text
runtime/macos-gamma/profile/db/textures/textures.db2
  archive sha256: 190990ed559d231086f16a4d7adfe0cb7bcbe1c831d952a582ac90d333110192
  virtual path: textures\prop\prop_el_shkaf_01.dds
  archive offset: 378723073
  real bytes: 174904
  compressed bytes: 153649
  archive crc: 0x528e022b

extracted DDS sha256:
  a6040b0d0d1e812113ed9c0f3dfccaae1cb6b02b51609fd65302f3f639c7da20
```

The strict portable DDS reader identifies legacy DXT1 as BC1 RGBA UNORM,
512x512, 10 mips. It derives every block pitch from format geometry rather
than trusting the advisory DDS pitch and requires exact payload coverage.
Cube, volume, array, unknown, truncated, and overflowing inputs fail closed.

The tested command is:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./build/metal-bootstrap/AnomalyMetalBootstrap \
  --frames 600 \
  --level-dir runtime/macos-gamma/profile/gamedata/levels/l05_bar \
  --texture-visual 6138 \
  --texture-dds /private/tmp/xray-archive.wd85ag/prop_el_shkaf_01.dds
```

Receipt: `/private/tmp/r4-metal-texture-diag-proof-1.txt`, SHA-256
`b4ece153f6bf52cc87feaf31f9a45f21df989f71333356baa290a3dc948587a2`.
Tested executable SHA-256:
`0f5d229e9987f20160ebb330a838451595652eda2ea25c6cf8e2561ffff156eb`.

```text
METAL_LEVEL_TEXTURE_DIAG_READY visual=6138 shader_id=294 ... format=BC1_RGBA_UNORM size=512x512 mips=10 ... uv=SHORT2@28 scale=1/1024 bc_supported=yes
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_LEVEL_TEXTURE_DIAG_COMPLETE visual=6138 shader_id=294 ... uploaded_mips=10 uploaded_bytes=174776 sampler=repeat_linear_mip result=PASS
```

The captured material frame is
[`direct-metal-evidence/l05-bar-visual-6138-base-texture.png`](direct-metal-evidence/l05-bar-visual-6138-base-texture.png),
SHA-256 `08d1cd06999a40164e0cb6d2a741c26fbe9185232b94454386a096081c7d572f`.
The disconnected parts are expected: visual 6138 is one material submesh of a
multi-part level object, not the complete visual hierarchy.

This proves material lookup, raw UV conversion, BC capability admission,
complete mip upload, sampler binding, texture binding, and stable sampling.
It does not yet prove lightmaps, bump/detail maps, alpha-tested materials,
full multi-material objects, whole-scene texturing, MSC argument-buffer
binding, or sRGB/lighting parity. The diagnostic shader remains small MSL;
production R4 shaders remain on the DXC -> MSC route.

## Phase 8 progress: complete multi-material hierarchy

Status: **PASS for one complete world-space object hierarchy; Phase 8 remains partial**.

The parser now retains all 10,300 visual nodes, validates `OGF_CHILDREN_L`
links, rejects cycles without recursive traversal, and reads the first 4x4
model transform from `OGF_TREEDEF2` for `MT_TREE_ST`/`MT_TREE_PM` leaves. The
diagnostic follows root visual 2933, an `MT_LOD` Niva in the real `l05_bar`, to
its complete high-detail child set:

```text
root 2933
  -> visual 2932, shader 159, veh\veh_niva2_soc,     9,372 indices
  -> visual 2931, shader 158, veh\veh_nivawheels1,  3,888 indices
  -> visual 2930, shader 157, veh\veh_nivawheels1a,   432 indices

shared TREEDEF2 translation: 241.533997, 2.040909, -67.348846
total: 3 leaves, 3 materials, 13,692 indices per frame
```

These leaves use the real `TEXCOORD0 SHORT4` declaration at byte 24. The
vertex diagnostic consumes `xy` with the same `tc / 1024` base-UV conversion
as R3, applies each leaf's X-Ray model matrix before the view-projection
matrix, and uses back-face culling. Each visual owns its uniform binding while
identical DDS references share one texture asset and one GPU upload. This was
separately exercised on root 76: two `terrain\terrain_bar` leaves correctly
share one 16,777,216-byte RGBA8 texture upload.

The three Niva DDS files were extracted from the active-profile
`textures.db3` archive (SHA-256
`0efc89eb42018c4a7dadf67ad0e9d5cf7e77b904916b4903ca4bf3e89110e137`):

```text
veh_niva2_soc.dds     BC1 1024x1024, 11 mips, SHA-256 be6532d992be21f9de2cc3687dee125e16430c983407378195665671bc347573
veh_nivawheels1.dds   BC1 1024x1024, 11 mips, SHA-256 6aad5f790e0c4c4627ba16b852209592b4fc12fb54dba17d4c1a109ae296e78c
veh_nivawheels1a.dds  BC1 1024x512,  11 mips, SHA-256 bf4199652594f672a4399d7dae90fb30501945ac5fcc8d3c70413582feee1ecf
```

Final Metal API + GPU Validation command:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./build/metal-bootstrap/AnomalyMetalBootstrap \
  --frames 600 \
  --level-dir runtime/macos-gamma/profile/gamedata/levels/l05_bar \
  --texture-root 2933 \
  --texture-dds /private/tmp/xray-archive.wd85ag/phase8-candidates/root-2933/veh/veh_niva2_soc.dds \
  --texture-dds /private/tmp/xray-archive.wd85ag/phase8-candidates/root-2933/veh/veh_nivawheels1.dds \
  --texture-dds /private/tmp/xray-archive.wd85ag/phase8-candidates/root-2933/veh/veh_nivawheels1a.dds
```

Receipt: `/private/tmp/r4-metal-niva-root-proof-1.txt`, SHA-256
`f8c5f88cd45c0a9f7fae0187fdaae280930d06f51415387641c73d590d3563d2`.
Tested executable SHA-256:
`2256c0950b5d28370381f809a77e5c840653e22c2c743236548682500c4913ca`.

```text
METAL_LEVEL_TEXTURE_ROOT_READY root=2933 leaves=3 materials=3 bc_supported=yes
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_LEVEL_TEXTURE_ROOT_COMPLETE root=2933 leaves=3 materials=3 frames=600 indices_per_frame=13692 uploaded_mips=33 uploaded_bytes=1747672 sampler=repeat_linear_mip result=PASS
```

The world-space material frame is
[`direct-metal-evidence/l05-bar-niva-root-2933-multimaterial.png`](direct-metal-evidence/l05-bar-niva-root-2933-multimaterial.png),
SHA-256 `1ab4a6504b13ca00bd409c0c822ef39aa13d3505790cc49cb0454522988aeb71`.
It visibly contains one coherent transformed vehicle with its body, wheel-face,
and tyre-tread materials rather than disconnected material geometry.

This is still an unlit base-texture diagnostic written directly in MSL. It
does not prove the low-detail imposter transition, bump/detail textures,
alpha-tested/blended families, production MSC argument buffers, lighting, or
whole-scene material coverage.

## Phase 8 progress: active R4 hemi-lightmap diagnostic

Status: **PASS for one causally selected `default` LMH visual; Phase 8 remains partial**.

The active R4 path was traced rather than inferred from the three-name texture
list. Visual 8011 resolves as follows:

```text
visual 8011, MT_NORMAL
  -> shader_id 97
  -> default/ston\ston_bordur_j03,lmap#5_1,lmap#5_2
  -> CBlender_deffer_flat -> uber_deffer -> deffer_base_lmh_flat
  -> s_base = textures[0]
  -> s_hemi = textures[2] = lmap#5_2
  -> textures[1] is not bound by this DX10/11 default path
```

DXBC reflection and the active HLSL wrapper/source agree that this pixel
specialization consumes only `s_base` and `s_hemi`. The vertex path uses the
real VB17 declaration and exact R3 unpack formulas:

```text
POSITION0 FLOAT3 @0
TANGENT0 D3DCOLOR @16
BINORMAL0 D3DCOLOR @20
TEXCOORD0 SHORT2 @24 -> (tc + T.a,B.a) / 1024
TEXCOORD1 SHORT2 @28 -> tc / 32768
VB 17 base 64026 count 1618; IB 2 base 563682 count 5079
```

The diagnostic fragment deliberately outputs `s_base.rgb * s_hemi.a`; it does
not pretend to reconstruct the deferred G-buffer or final lighting. Base uses
the repeat sampler, while `s_hemi` uses a separate clamp/linear sampler.

The real assets are archive winners:

```text
runtime/macos-gamma/profile/db/textures/textures_stone.db0
  archive SHA-256: b3fc03d92004cc1c6ac08ce9bf9e27ccde823263b771b742bcdbcf4cf08067e7
  textures\ston\ston_bordur_j03.dds: BC1 512x256, 10 mips
  extracted SHA-256: 0f6b93f7bae9feb99f51bea7d50fcb004284c9634f69770b73accc5064e6e4d2

runtime/macos-gamma/profile/db/levels/level_l05_bar.db0
  archive SHA-256: c40488363b5adaf087f62e98b8c21652bc386a7b078b26f8ab2092a9bca6be7f
  levels\l05_bar\lmap#5_2.dds: BC3 1024x1024, 1 mip
  extracted SHA-256: 8bac497268c300e140461cc1b89296c1d3d7da015eddae9fd7a6f21b704ed8d4
```

Final Metal API + GPU Validation command:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./build/metal-bootstrap/AnomalyMetalBootstrap \
  --frames 600 \
  --level-dir runtime/macos-gamma/profile/gamedata/levels/l05_bar \
  --texture-visual 8011 \
  --texture-dds /private/tmp/xray-archive.wd85ag/ston_bordur_j03.dds \
  --hemi-dds '/private/tmp/xray-archive.wd85ag/lmap#5_2.dds'
```

Receipt: `/private/tmp/r4-metal-hemi-lightmap-proof-1.txt`, SHA-256
`9a55c311a3f76e99774d94d88694583cc82817fe2e943b0a1f9d5fbeb581623a`.
Tested executable SHA-256:
`2e0c94c0821be686814a83ac625341943e477c958ab402072bc4e0a2f0156aca`.

```text
METAL_LEVEL_HEMI_DIAG_READY visual=8011 texture=lmap#5_2 ... format=BC3_RGBA_UNORM size=1024x1024 mips=1 uv=SHORT2@28 scale=1/32768 sampler=clamp_linear_mip base_uv_offsets=T.a@16,B.a@20
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_LEVEL_HEMI_DIAG_COMPLETE visual=8011 frames=600 hemi_texture=lmap#5_2 hemi_mips=1 hemi_bytes=1048576 uv_scale=1/32768 sampler=clamp_linear_mip result=PASS
```

The fixed-camera/fixed-clear A/B evidence is:

- [`direct-metal-evidence/l05-bar-visual-8011-base-only.png`](direct-metal-evidence/l05-bar-visual-8011-base-only.png), SHA-256 `45fa11722f3a21152b1fa3da2bebbe8525eec8517f20be77a1c43faf0f6d9400`.
- [`direct-metal-evidence/l05-bar-visual-8011-base-times-hemi.png`](direct-metal-evidence/l05-bar-visual-8011-base-times-hemi.png), SHA-256 `5fc9dd92d9f49890cf4956adfd9a80f88ae40fedc7d7f6a99ad2755f1a3f7705`.

The background pixel is identical between A and B; exactly 18,022 pixels
(0.823374% of the cropped frame) change, confined to the selected geometry.
Both variants use the same exact position and base-UV math, so the only shader
data-path difference is `s_hemi.a` modulation.

This proves the active R4 hemi texture selection, strict asset admission,
exact packed UV decode, independent sampler/texture binding, complete upload,
and stable sampling. It does not prove normals/material BRDF, the full G-buffer,
deferred light accumulation, or whole-scene LMH coverage. Next target: expand
the diagnostic across a controlled set of `default` LMH materials, then begin
the production converted-shader binding path before any lighting-parity claim.

## Phase 9 milestone: production MSC runtime binding and first real G-buffer

Status: **PASS for visual 8011 through the exact converted R4 LMH shader pair;
deferred lighting and wider material coverage remain pending**.

The diagnostic LMH shader has been replaced for this selected draw by the real
active GAMMA wrappers:

```text
deffer_base_lmh_flat.vs -> DXC vs_6_0 -> MSC 4.0.1 metallib
deffer_base_lmh_flat.ps -> DXC ps_6_0 -> MSC 4.0.1 metallib
```

`script/compile_r4_lmh_msc.sh` reproduces both offline artifacts from a
materialized active shader tree. Exact output receipt:
`/private/tmp/r4-lmh-runtime-proof-1/lmh-msc-receipt.txt`, SHA-256
`155ea65f41d033f0ffbf8e23a712e24da817da0ea3248f61183525fe032d2985`.
The VS metallib SHA-256 is
`06415f649e216a95acf82a2940c7c9523332559705e644f5649b4a8bbde3d605`;
the PS metallib SHA-256 is
`7c8078287baf8842389301b350681c9bd445067449bce314515a26aef296ebab`.

The runtime consumes MSC automatic-linear reflection exactly:

```text
VS TLAB 48 bytes:  b0 $Globals (160), b1 dynamic_transforms (256)
PS TLAB 144 bytes: t0 s_base, t1 s_hemi,
                   b0 $Globals (176), b1 dynamic_transforms (256),
                   s0 smp_rtlinear, s1 smp_base
vertex attributes 11..16, vertex buffer index 6
```

Every indirect resource is made resident for its stage and retained through
command-buffer completion. The draw writes all four real non-MSAA R4 targets
(`RGBA16Float`, `RGBA8Unorm`, `RGBA8Unorm`, `RGBA16Float`) plus stored
`Depth32Float`. A separate MSL inspection pass displays only the albedo/gloss
target and is not counted as lighting.

Admission requires the adjacent MSC reflection JSON to match the complete LMH
TLAB and six-element static vertex-input ABI, and SHA-256 checks bind both JSON
files and both metallibs to their conversion receipt. Substituting the already-converted
non-LMH `deffer_base_bump` pair is rejected before pipeline creation with
`production MSC shader reflection does not match deffer_base_lmh_flat ABI`.
The diagnostic bootstrap was also rebuilt successfully with the MSC include
path deliberately absent; only production MSC admission becomes unavailable,
so the established MSL diagnostics and reference build remain buildable.

Final 600-frame Metal API + GPU Validation receipt:
`/private/tmp/r4-metal-msc-gbuffer-proof-1.txt`, SHA-256
`70e8b017624133396e58da04d5b4b7819c02e13022a62601d636f20c90c75ec9`.
Tested executable SHA-256:
`e2af114afe6dd7c10b94c195730bc7cd294e5dab671816a7630047f460f2fb1d`.

```text
METAL_MSC_RUNTIME_READY visual=8011 shader=deffer_base_lmh_flat binding=automatic_linear reflection=admitted vs_tlab=48 ps_tlab=144 ... render_targets=4
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_GBUFFER_COMPLETE visual=8011 shader=deffer_base_lmh_flat binding=msc_automatic_linear render_targets=4 ... displayed=albedo_gloss result=PASS
```

The fixed-camera inspection frame is
[`direct-metal-evidence/l05-bar-visual-8011-production-msc-gbuffer-albedo.png`](direct-metal-evidence/l05-bar-visual-8011-production-msc-gbuffer-albedo.png),
SHA-256 `68eab295d2fde22adf655cc0b6cc020906bc25e2fe3d7a29a810987f07442768`.
It visibly contains the same real barrier geometry and base texture carried by
the production pixel shader's RT1 output. This is a production shader binding
and G-buffer milestone, not a final-lighting or parity claim. Next target:
converted base/deferred lighting over this G-buffer, followed by a controlled
set of additional real R4 material permutations.

## Phase 10 milestone: converted R4 base lighting and first recognizable lit scene

Status: **PASS for a real `l05_bar` LMH material through the converted
`combine_1` base/deferred pass; direct sun, shadows, and whole-level coverage
remain pending**.

The first real lighting consumer now runs after the production G-buffer:

```text
deffer_base_lmh_flat.vs/.ps -> four-target R4 G-buffer
clear-only RGBA16Float accumulator
combine_1.vs + combine_1_nomsaa.ps -> DXC -> MSC 4.0.1
                                    -> two RGBA8Unorm LDR outputs
diagnostic MSL copy of combine_1 low output -> drawable
```

`script/compile_r4_base_lighting_msc.sh` reproducibly emits the exact
non-MSAA `combine_1` pair. Its receipt is
`/private/tmp/r4-base-lighting-runtime-proof-final/base-lighting-msc-receipt.txt`,
SHA-256 `79f035c448a69a4cf531fb98f1c1004a0078eb5c865625dafb2382cbde3c8b15`.
The output hashes are deterministic across two clean conversions:

```text
combine_1.vs.dxil                 fb3c59c2daeef5bf6036fe2827e51d2ef845904504f3467c48c7a8bfc4947de3
combine_1.vs.metallib             248fab91462cb15ff82e1249cedfc2058c629a0b154d364063e4a02ec7201311
combine_1.vs.json                 f19aaa48f0d8826105de17460b082533accb7a879e7f7346838d7a71031364a3
combine_1_nomsaa.ps.dxil          57d93a60997103f5f123fa77fe319b1a191ffb507d938fc82bff9bed4f64aafc
combine_1_nomsaa.ps.metallib      b2ec7dcbe69124b02a362b4b1f735555e163794e10450afb19590e14b0fc9e48
combine_1_nomsaa.ps.json          d8a1c31fc7483097a393ceb212c1f060d130c1f0888e4a66c254274c1faa4a29
```

Reflection admission fixes the runtime ABI before PSO creation:

```text
VS TLAB 24 bytes:  t0 s_tonemap
PS TLAB 336 bytes: t0 position, t1 albedo, t2 accumulator,
                   t3 env0, t4 env1, t5 AO, t6 IL, t7 motion,
                   b0 $Globals, b1 $Globals, b2 Ldynamic,
                   s0 smp_nofilter, s1 smp_rtlinear, s2 smp_base
vertex attributes 11..12, vertex buffer index 6
two fragment outputs
```

All table-reached textures, buffers, samplers, and TLABs are retained through
command completion and declared resident for their stage. The environment is
a real six-face RGBA32Float cube with seven mips; AO is controlled white, IL
is controlled black, and the accumulator is deliberately cleared to zero.
These controlled inputs isolate the base/hemi/environment term. They are not a
claim that the sun or shadow cascade is implemented. Cleared G-buffer pixels
are zero, preventing `combine_1` from classifying the background as geometry.

The representative draw is real GAMMA `l05_bar` visual 7364:

```text
shader=default
texture_list=mtl\mtl_big_konteyneri,lmap#12_1,lmap#12_2
base=mtl_big_konteyneri.dds BC1 2048x1024 12 mips
base SHA-256=ee610c70fc41249ddae7b58eeb916cfbe10b1e0ced9e4ec1e18dee2a7062aca8
hemi=lmap#12_2.dds BC3 1024x1024 1 mip
hemi SHA-256=ba296fbfbd906b2ccab8888c24a6ee557fa0e51969fe3faa33d98d16343dd6c3
VB=16 vertices=3059 indices=6144
```

Final Metal API + GPU Validation command:

```sh
MTL_DEBUG_LAYER=1 \
MTL_SHADER_VALIDATION=1 \
MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
./build/metal-bootstrap/AnomalyMetalBootstrap \
  --frames 600 \
  --level-dir runtime/macos-gamma/profile/gamedata/levels/l05_bar \
  --texture-visual 7364 \
  --texture-dds /private/tmp/xray-archive.wd85ag/mtl_big_konteyneri.dds \
  --hemi-dds '/private/tmp/xray-archive.wd85ag/lmap#12_2.dds' \
  --msc-vs-metallib /private/tmp/r4-lmh-runtime-proof-1/deffer_base_lmh_flat.vs.metallib \
  --msc-ps-metallib /private/tmp/r4-lmh-runtime-proof-1/deffer_base_lmh_flat.ps.metallib \
  --msc-lighting-vs-metallib /private/tmp/r4-base-lighting-runtime-proof-final/combine_1.vs.metallib \
  --msc-lighting-ps-metallib /private/tmp/r4-base-lighting-runtime-proof-final/combine_1_nomsaa.ps.metallib
```

Receipt: `/private/tmp/r4-metal-base-lighting-proof-1.txt`, SHA-256
`6a60e1f7638a48ff9d4cc7fd256f7d0de958f98c18a964d75df654494b595045`.
Tested executable SHA-256:
`6e9f9c68a6919826d0a4536ad43e3a42de9ab8089f3eb3039e257e2e04e0f3b6`.

```text
METAL_BASE_LIGHTING_READY visual=7364 shader=combine_1_nomsaa binding=automatic_linear reflection=admitted vs_tlab=24 ps_tlab=336 ... outputs=2xRGBA8Unorm ... direct_sun=pending
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_GBUFFER_COMPLETE visual=7364 shader=deffer_base_lmh_flat ... displayed=combine_1_low result=PASS
METAL_BASE_LIGHTING_COMPLETE visual=7364 frames=600 shader=combine_1_nomsaa ... displayed=combine_1_low environment=controlled_neutral_cube direct_sun=pending result=PASS
```

The validated inspection frame is
[`direct-metal-evidence/l05-bar-visual-7364-production-msc-base-lit.png`](direct-metal-evidence/l05-bar-visual-7364-production-msc-base-lit.png),
SHA-256 `a7046b610b9e2a9601a057bc661588f1cffc177e74d2d71e2d0ef2dc6d43cd1b`.
It visibly resolves the real wooden-container cluster with the material carried
through the production G-buffer into `combine_1`. A mismatched G-buffer VS
substituted for `combine_1.vs` is rejected before pipeline creation. The
standalone no-MSC-header bootstrap and the unchanged `AnomalyDX11` target both
remain buildable. This is the first recognizable base-lit GAMMA geometry
scene, not yet a whole-level, direct-sun, shadow, gameplay, or parity milestone.
The next target is the real stencil/sun-mask/cascade accumulation sequence,
then controlled widening to scene material permutations.

## Phase 11 milestone: converted direct sun and shader-readable R32 shadow map

Status: **PASS for the exact non-MSAA sun mask and far-cascade shader path,
including a rendered shadow map; near/middle cascade partitioning and
whole-level caster coverage remain pending**.

The Metal pass contract now preserves R4's depth/stencil and accumulator data
across encoders. Render passes have explicit drawable ownership, per-attachment
load/store actions, independent depth/stencil actions, viewport/scissor, and
debug labels. PSOs include color write masks plus front/back stencil compare,
operations, masks, and a dynamic stencil reference. Production G-buffer draws
write geometry marker 1 into `Depth32Float_Stencil8`; the converted sun mask
promotes admitted pixels to light marker 5 without writing color; the converted
far cascade consumes marker 5 and writes `RGBA16Float` accumulator lighting.

`script/compile_r4_sun_msc.sh` reproducibly converts and receipts seven shaders:

```text
stub_notransform_t.vs + accum_sun_mask_nomsaa.ps
accum_sun.vs + accum_sun_near_nomsaa_nominmax.ps
accum_sun.vs + accum_sun_far_nomsaa.ps
shadow_direct_base.vs + shadow_direct_base_color.ps
```

The last fragment shader is a narrow HLSL compatibility bridge rather than
diagnostic MSL. Metal cannot expose a `Depth32Float` attachment as the ordinary
`Texture2D<float>` SRV expected by the converted R4 lighting shader, so the same
shadow draw writes post-raster depth to a 2048x2048 `R32Float` color attachment.
The original `shadow_direct_base.vs` supplies the real transformed caster
geometry; the bridge has no resources and only returns `SV_Position.z`.

Conversion receipt:
`/private/tmp/r4-sun-shadow-runtime-proof-1/sun-msc-receipt.txt`, SHA-256
`30bfb53a65ea0c58d40e85472f28fd57b4259fed898d5f426a25b48c103a51b5`.
Important metallib SHA-256 values:

```text
stub_notransform_t.vs                 0eaf740d94955e0f753894e300dbdeda77b06e2e40aff06e08a8a575750707c6
accum_sun_mask_nomsaa.ps              57adf3c03f8ad1bbc05f3ad2417eff0f3d34da8870f5d38d1a002e2f35b99bb4
accum_sun.vs                          8c570a04d307eea059a5000eb693ed2d1e6a31f7ba1a0cf212ad961c607afc93
accum_sun_near_nomsaa_nominmax.ps     b2b391505512f4afb08fb7aab100cbfd59f5978c953142a8e956d968726c0183
accum_sun_far_nomsaa.ps               c74c9407d7b641bfe087322fe6057c9e0230f2453d8de25a48f75fd1ebc7e2fa
shadow_direct_base.vs                 b839395b932633f52aaecbefad572529b9282f4c97496e614540336af11b81a0
shadow_direct_base_color.ps            e6498441be07820d0540e5d5a69a801ab8fcb9efb3467962f0091306c6fb6679
```

The runtime admits every metallib against its adjacent reflection and shared
receipt. This also required multi-record receipt lookup because the bundle has
several VS and PS records. All TLAB resources are retained and declared
resident, including the comparison sampler and generated R32 shadow map.

An autonomous one-frame GPU capture was recorded at
`/private/tmp/r4-sun-shadow-final.gputrace` (182 MiB). `gpudebug` confirms seven
ordered encoders: shadow map, G-buffer, accumulator clear, stencil mask, far
cascade, `combine_1`, and presentation. It also confirms the 6144-index real
caster draw into 2048x2048 R32/Depth32 targets, the 48-index cascade cuboid,
culling disabled as in `accum_direct_cascade`, marker reference 5, the complete
264-byte fragment TLAB,
and the generated R32 texture bound to the cascade shader.

Final 600-frame Metal API + GPU Validation receipt:
`/private/tmp/r4-metal-sun-shadow-proof-final.txt`, SHA-256
`89d1230223a38c8439b8174d92fc28bdb3deb7f4273656efc4e248e5e45625d6`.
Tested executable SHA-256:
`a2c86a8170ebe153b7072d97a261cfaf443a519175c8423c16fc76137f779daf`.

```text
METAL_DIRECT_SUN_READY ... stencil=geometry_1_light_5 accumulator=RGBA16Float shadow=rendered_r32_2048 direct_shadowmap=converted
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
METAL_DIRECT_SUN_COMPLETE ... shadow=rendered_r32_2048 direct_shadowmap=converted result=PASS
```

Evidence images:

- [`direct-metal-evidence/l05-bar-visual-7364-r32-shadow-map.png`](direct-metal-evidence/l05-bar-visual-7364-r32-shadow-map.png), SHA-256 `5e3988e352f5fc63dc01eb14a8a726bbc7ecd6c747e6fb56aa85d045f0ce7c68`.
- [`direct-metal-evidence/l05-bar-visual-7364-production-msc-sun-shadow.png`](direct-metal-evidence/l05-bar-visual-7364-production-msc-sun-shadow.png), SHA-256 `58b15660666d60fea5fe1a3d7032c89b92ce29a985114f915e22cfbeb7faa513`.

The shadow map visibly contains the selected real GAMMA caster geometry, and
the post-cascade accumulator capture changes relative to the admitted white
shadow control. This proves a real producer-to-consumer shadow resource path.
It does not yet claim complete near/middle/far cascade coverage, alpha-tested
casters, moving casters, or gameplay integration.
