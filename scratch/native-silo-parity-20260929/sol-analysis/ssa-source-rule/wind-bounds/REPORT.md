# Flora SSA wind-bound review

## Decision

**No-go for wind-inflated flora SSA rejection remains.** The normalized wind texture contract and static per-visual transformed H/UV extrema are now proven. The remaining missing input is the exact live wind setup/params/animation and current bender arrays joined to these visual IDs. Keep uncertain flora drawing until a conservative bound includes that runtime state.

## Pinned shader path

`build/metal-runtime-shaders/flora-wind-v3/flora-l05-bar-wind-msc-receipt.txt` pins the r3 source tree hash `bc502abe6276487148978a7423c102079d7120a12ed8359c87eff7f1047b8494`, defines `SSFX_WIND=1` and `SSFX_INT_GRASS=8`, and names the actual inputs `deffer_tree_branch_bump-hq.vs` for leaves and `deffer_tree_flat.vs` for trunks. The receipt also pins each DXIL/metallib/reflection hash. Thus the legacy `calc_xz_wave(wind,wave,...)` branches are excluded for this admitted bundle; “wave” in `leaf_wave_hq` / `trunk_wave_hq` does not establish legacy wave motion.

The tree setup computes `speed=max(ssfx_wsetup_trees.w,saturate(wind_params.y*0.001))`; only the environment-derived term is saturated. `sqrt_speed` is saturated, but branch displacement multiplies by unsaturated `speed`. Tree trunk displacement is directed in XZ and scales with `0.006 * WSpeed^2 * TWave * H^2 * saturate(1 - 0.005H)`. For nonnegative H this has a finite maximum; for negative H the final saturate is 1 and a bound must come from the actual mesh's minimum transformed H. The leaf branch path samples `s_waves` twice, forms motion from sampled channels, multiplies it by `(1-tc.y)*speed`, and adds the trunk displacement. A per-mesh UV bound is therefore also required.


The CPU-only fixture in `mesh-extrema/RESULT.md` computes the exact transformed H and `1-tc.y` extrema for all 5,316 captured IDs using the frozen level geometry parser and the Flora adapter's signed-S4/1024 conversion. Among the 1,199 source rejects, 674 have negative H vertices, every record has some negative `1-tc.y`, and 257 have a per-visual `max(abs(1-tc.y)) > 1`. A single global displacement heuristic is therefore unsafe and likely too loose; any candidate envelope must use these per-visual extrema.

The leaf vertex shader also applies up to nine interactive-bender iterations (`b < SSFX_INT_GRASS+1`). Their final displacement depends on bender position, radius, direction and strength, plus `benders_setup` for non-dynamic entries; height-limit terms again multiply by `(1-tc.y)`. `FTreeVisual::Render` uploads these live values from `grass_shader_data`. The Metal frame snapshot copies current/previous benders and wind setup/animation, but the saved `sphere-geometry.csv` only records visual ID, source rejection, source radius and rest-geometry max distance. These shader inputs are not attached to the rejected-visual rows, so no offline join can reconstruct a bounded displacement for the measured frame.

## Why current evidence is insufficient

The geometry check found 214/5316 authored spheres fail transformed indexed rest-geometry enclosure, including 113/1199 source-SSA rejects; worst observed vertex distance is 4.199 times source radius. The new `flora-mesh-extrema.csv` confirms those same exact per-ID distances and adds H/UV extrema. This static enclosure can be computed for the captured geometry but does not include animated leaf/trunk positions or benders. The existing source policy uses the source sphere center and radius in its distance/SSA calculation, so any proposed adjusted radius must be recomputed through that policy and fail open for missing, non-finite or unsupported inputs.

Console command limits do bound the declared tree wind settings (`ssfx_wind_trees` range 0..(20,5,5,1)) and bender setup (`ssfx_int_grass_params_1` range 0..(5,5,5,60)); they do not establish the exact live dynamic bender strengths for the sampled frame. The frozen source plumbing exposes wind parameters/animation and current/previous bender arrays, but the saved parity artifacts do not join those values to each rejected visual. The only missing capture should now be that compact per-frame live state; H/UV extrema and the texture contract are already available.

## Smallest no-shader-cook screen for the remaining live-state question

1. Capture one compact CPU-only observation of current `windParams`, tree wind setup, current/previous wind animation, and current/previous bender arrays from the admitted frame, with the same frame number and 5,316 visual IDs. H/UV extrema, world rest distance, sphere, source policy inputs and the normalized `s_waves` contract are already frozen in `mesh-extrema/` and `texture-receipt.md`.
2. Apply interval bounds per visual using the captured live values plus its H/UV extrema, and sum the nine leaf bender maxima. Fail open for non-finite or unsupported state. Report the resulting envelope distribution before trying it against SSA.
3. Only if that per-visual bound retains plausible source rejects, re-run the exact source-SSA predicate for the 1,199 rejected records with inflated radii and unchanged eye/settings/hash. A count alone does not establish visual acceptance.

The H/UV fixture now exists for all 5,316 visual IDs. The missing same-frame live wind/bender values still prevent a supported retained-rejection count; do not infer one from the static CSV alone.

## Evidence pointers

- Pinned bundle and inputs: `build/metal-runtime-shaders/flora-wind-v3/flora-l05-bar-wind-msc-receipt.txt`.
- Frozen `s_waves` asset and parser/upload contract: `scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/wind-bounds/texture-receipt.md`.
- Exact per-visual rest geometry, transformed H and UV extrema: `scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/wind-bounds/mesh-extrema/RESULT.md` and `mesh-extrema/flora-mesh-extrema.csv`.
- Wind math: `build/metal-shader-sources/r3/screenspace_wind.h:35-58,91-150`.
- Leaf displacement and benders: `build/metal-shader-sources/r3/deffer_tree_branch_bump-hq.vs:39-120`.
- Trunk displacement: `build/metal-shader-sources/r3/deffer_tree_flat.vs:35-65`.
- Bender upload: `src/Layers/xrRender/FTreeVisual.cpp:189-242`.
- Declared console bounds: `src/Layers/xrRender/xrRender_console.cpp:1421-1422,1450-1451`.
- Captured comparison and geometry evidence: `scratch/native-silo-parity-20260929/sol-analysis/ssa-source-rule/RESULT.md`, `sphere-geometry.csv`, and `actual-frame-700.fixture`.
