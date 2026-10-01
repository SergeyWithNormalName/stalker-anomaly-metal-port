# Flora HQ/LQ source and permutation screen

Read-only supplement to `RESULT.md`. No shader was compiled or cooked.

## Exact R4 source names

The active leaf material is alpha blended and not object LOD (`METAL_FLORA_BLENDER`
lines 843-844 in the frame-700 log, read from the winning `shaders.xr`). R4's
`CRender::` startup sets `o.ssfx_branches` by checking for
`r3\deffer_tree_branch_aref_bump-hq.ps` (`src/Layers/xrRenderPC_R4/r4.cpp:499`);
that winning profile overlay file exists. With the regular-tree setting,
`CBlender_Tree::Compile` therefore sets `tvs=tps=tree_branch` for the alpha
leaf in HQ (`Blender_tree.cpp:201-226`). The HQ call requests hq=true and
alpha=true (`:229-254`); with an ordinary bump texture, no detail suffix,
and non-steep parallax, `uber_deffer` generates:

| HQ leaf stage | Generated source | Current cook |
|---|---|---|
| VS | `deffer_tree_branch_bump-hq.vs` | present; cooked as `leaf_wave_hq.vs`, with `USE_PARALLAX=1` |
| PS | `deffer_tree_branch_aref_bump-hq.ps` | present; cooked as `leaf_wave_hq.ps`, with `USE_PARALLAX=1` |

The LQ switch enters a separate case that keeps default `tvs="tree"` and
`tps="base"`, calls `uber_deffer(hq=false, aref=true)`, and still binds the
wind-wave texture (`Blender_tree.cpp:257-279`). For the same bump leaf, the
derived names are:

| LQ leaf stage | Exact generated source | Winning source exists? |
|---|---|---|
| VS | `deffer_tree_bump.vs` | yes |
| PS | `deffer_base_aref_bump.ps` | yes |

There is no `-lq` suffix. `_aref` is part of the PS name because the material
is alpha blended; `-hq` is added to VS and PS only for bump+HQ
(`uber_deffer.cpp:51-95`). With steep parallax enabled, HQ PS would instead be
`deffer_tree_branch_aref_steep-hq.ps`; the frame-700 log says
`r2_steep_parallax off`, and the current cook receipt uses the bump-hq PS.
If `ssfx_branches` were false, the HQ names would be
`deffer_tree_bump-hq.vs` and `deffer_base_aref_bump-hq.ps`; the existing
startup detection plus the present branch-alpha source selects the branch
family in this profile.

For the opaque, flat trunk, current HQ receipt names are
`deffer_tree_flat.vs` and `deffer_base_flat.ps`. `uber_deffer` only adds `_d`
for a flat HQ permutation when a detail feature is active. Since the current
receipt has no `_d`, the equivalent LQ source names are identical. Thus the
trunk's 2,256 conditional LQ IDs from `RESULT.md` do not imply a cheaper
shader in this exact material permutation; the meaningful HQ/LQ shader
candidate is the leaf subset (1,856 IDs conditional on the same distance
default).

## Winning source availability and provenance

All six exact HQ/LQ source paths above exist under
`runtime/macos-gamma/profile/gamedata/shaders/r3/` as symlinks into the active
`190- Screen Space Shaders 23 - Ascii1457` mod. Their resolved SHA-256 values
match byte-for-byte with the materialized archive-plus-overlay copies under
`build/metal-shader-sources/r3/`:

| File | SHA-256 |
|---|---|
| `deffer_tree_branch_bump-hq.vs` | `7cf845ab1bbf707f70e98eb1999ef8308832cf712896e5ccf71be1c92e204840` |
| `deffer_tree_branch_aref_bump-hq.ps` | `e803b8cb98e3a7d52512c7d61e1fceed548751ce2b92a0b8ecdeb964a759eed7` |
| `deffer_tree_bump.vs` | `7b037a5dcb24d1e09b15bcde29641188330b1a140ba649cd9668d110e6c62958` |
| `deffer_base_aref_bump.ps` | `f4e839972cb95087391587a6fce2b1563a5c8dd3d1bc18b283e86388e1c4dfc1` |
| `deffer_tree_flat.vs` | `27f5ef8c1c6176e0df0e110363349acf57927898ec67a4cade136b2b19589527` |
| `deffer_base_flat.ps` | `a3c3c7755f646b3b75be8bb9be014825eb599a5dd68324f740381c074d972326` |

`materialize_r4_shader_sources.sh` extracts the profile's linked
`shaders.db0`, then applies the profile `gamedata/shaders` overlay using
`rsync -aL` before compiling. These are therefore present in the effective
winning shader source set; an absent-LQ-source fallback is not needed to
explain route selection. This does not prove that every unrelated Lua/MCM
runtime option has no effect. The checked candidate log contains the
archive-authored Flora material flags and no shader-fallback report.

## HLSL and binding implications

The difference is more than the filename:

- HQ branch VS declares current/previous 32-float4 bender arrays, bender
  settings, and `ssfx_floravariation`; with `SSFX_INT_GRASS=8` it runs current
  and previous interactive-bending loops over nine entries. It calls
  `ssfx_wind_tree_branches`, produces `sss_extra`, and uses a branch wind path.
- LQ tree VS has none of those bender/variation arrays or loops. It calls
  `ssfx_wind_tree_trunk` for current and previous motion, so ordinary wind and
  motion vectors remain, while interactive branch/bush bend and foliage
  variation are absent. Its vertex outputs are correspondingly smaller in
  source (`v2p_bumped` has no written `sss_extra` field in this path).
- HQ branch PS includes the screen-space flora path, can apply `SSFX_FLORAFIX`,
  changes flora color from `ssfx_floravariation`, and the current cook enables
  `USE_PARALLAX`. `sload.h`'s `USE_PARALLAX` branch samples the bump-height
  channel to offset texture coordinates. LQ uses the general alpha-tested
  base PS, has no branch color-variation/FLORAFIX code, and its base bump
  route has no `-hq` suffix. The current helper's extra `USE_PARALLAX=1`
  belongs to the HQ cook; a future LQ artifact must be generated from the
  exact selected LQ contract and record its defines rather than inheriting
  that flag by filename alone.
- Flat trunk HQ and LQ resolve to the same two source files for the current
  no-detail material, so no HLSL reduction follows from selecting LQ there.

The existing HQ reflection records provide its actual Metal bindings (leaf
HQ VS: four TLAB resources; leaf HQ PS: seven; trunk flat VS/PS: four each).
There is no LQ DXIL, metallib, reflection, or cook receipt in the targeted
flora output directories. Therefore exact LQ resource/constant-buffer slot
counts and Metal ABI compatibility remain unknown without a cook. Source
declares the extra branch arrays and uniforms, but a compiler may pack or
strip bindings; those counts must not be invented from declarations.

## Decision

The source-based cost candidate is real for alpha leaf Flora and carries
intentional quality/behavior differences; it is not a label-only route.
The same source screen rejects “LQ saves work on all Flora”: current flat
trunks compile to the same HLSL pair. Existing runtime symlinks also reject
“generic fallback is needed because LQ HLSL is absent.” No cook or app edit is
justified until the planned observer captures live `r_dtex_range`, selected
HQ/LQ counts, and branch wind inputs in one matching frame. An LQ compile can
then be a scratch-only artifact task with explicit reflection/resource
admission followed by visual review of branch motion, alpha edges, color
variation, and distance detail.
