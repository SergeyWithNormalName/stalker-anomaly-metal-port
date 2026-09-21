# L05 Bar terrain HQ MSC receipt

Scope: structural DXC to DXIL and Metal Shader Converter conversion evidence for
the active `levels\bar_grass` terrain material. This is not a runtime-load,
pipeline-state, or rendered-image claim.

## Admission

The live L05 Bar level record assigns shader slot `1` to
`levels\bar_grass/terrain\terrain_bar,terrain\terrain_bar_lm`. Camera-ray
visuals `542`, `534`, and `515` each resolve to that slot. The extracted
production `shaders.xr` record is subchunk `78`, class `BmmDold`, version `3`.
The current R4 options have SSFX terrain installed and
`ssfx_terrain_quality.x = 8`; all three camera-ray visuals are under the
`8 * 10 = 80 m` HQ selector threshold. `CRender::rimp_select_sh_static` and
`CBlender_BmmD::Compile` therefore select `SE_R2_NORMAL_HQ` and the exact pair
below. `mid` and `low` are deliberately not substituted.

The machine-readable chain, actual BmmD detail textures, hashes, and rejected
permutations are in `terrain-l05-bar-admission.tsv`.

## Toolchain

- DXC: `dxc-3.7`, libdxcompiler 1.9.0.1 (`0d3ee6b5`), target `vs_6_0` / `ps_6_0`, entry `main`, HLSL 2016.
- Metal Shader Converter: 4.0.1, CLI SHA-256 `b80d3f61df0dc974ab96d176cd6a8903b226bec46d9adac2d08f69e50b4319ff`.
- Shared active R4 defines: `SMAP_size=2048`, `FP16_FILTER=1`, `FP16_BLEND=1`, `USE_HWSMAP=1`, `USE_HWSMAP_PCF=1`, `USE_BRANCHING=1`, `USE_VTF=1`, `SM_5=1`, `SSFX_INT_GRASS=8`, `SSFX_SSR_QUALITY=0`, `SSFX_WATER_QUALITY=1`, `SSFX_WATER_PARALLAX=2`, `SSFX_IL_QUALITY=16`, `SSFX_AO_QUALITY=2`, `SSFX_POM_REFINE=0`, `SSFX_TERRA_POM_REFINE=0`, `SSFX_SSS_DIR_QUALITY=6`, `SSFX_SSS_OMNI_QUALITY=2`, `SSFX_MODEXE=1`, and `SKIN_NONE=1`.

The source root is `/private/tmp/gamma-r4-runtime.HDQ9lq/sources/base/shaders/r3`.
The immutable converted bundle is `/private/tmp/r4-terrain-root-check`.

| Stage | HLSL SHA-256 | DXIL SHA-256 | Metallib SHA-256 | MSC reflection SHA-256 |
| --- | --- | --- | --- | --- |
| `deffer_terrain_flat_d.vs` | `9ccad5853363f1688a1aab899a22fbe35830219e23ce19b0338de1e4a2879fd8` | `fd77862233fd52fea8b90cfd40958f3b014bb1efab35b9569dee6054663f8fb7` | `d1a48335e956761c5aad3ca513c5d8aee42fa69bf9500b179006ea0e2d0e4507` | `7db013e9e7d80dc7c326ca15c2ea91df115f28367e34183050bb82a7fbe90093` |
| `deffer_terrain_high_flat_d.ps` | `2fbd968c4e4cb391ff45e80944d9c2fb47c7e27401dae465eb8995c537c2190d` | `da09eae1539494a605b337c6637c2e45da242b76a63129e8361cd7846d4dd9bd` | `0a9a6f058cb567bb621ffe25f2d22a4034407f062b59a10b8932a207a62ff915` | `5e2baa630a9fef0c5b3088b9de29b625406999e283ac3b8708a1e512abaf8822` |

## Reflected contract

The vertex TLAB has CBV `b0` at offset `0` and `b1` at `24`. DXIL reflection
reports `b0` (`$Globals`) as 160 bytes and `b1` (`dynamic_transforms`) as
256 bytes. The exact MSC vertex inputs are, in order: `NORMAL0 float4`,
`TANGENT0 float4`, `BINORMAL0 float4`, `TEXCOORD0 int2`, and `POSITION0 float4`.
They must be repacked from the level declaration; they cannot be passed as the
native packed D3D layout.

The fragment TLAB is fixed by reflection: SRVs `t0..t16` at offsets `0..384`
in 24-byte steps, CBV `b0` at `408`, CBV `b1` at `432`, sampler `s0` at `456`,
and sampler `s1` at `480`. DXIL reflection reports `$Globals` as 192 bytes and
`static_globals` as 528 bytes. SRV order is: `s_base`, `s_mask`, `s_dt_r`,
`s_dt_g`, `s_dt_b`, `s_dt_a`, `s_dn_r`, `s_dn_g`, `s_dn_b`, `s_dn_a`,
`s_puddles_normal`, `s_puddles_mask`, `s_rainsplash`, `s_height_r`,
`s_height_g`, `s_height_b`, `s_height_a`.

The fragment has exactly four `SV_Target` outputs. Runtime binding must use
MSC reflection offsets, 24-byte descriptor entries, absolute descriptor-table
GPU addresses, argument-buffer-capable samplers, and residency for every
indirect resource.
