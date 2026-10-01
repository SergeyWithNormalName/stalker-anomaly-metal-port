# Main Flora HQ/LQ shader selection: bounded source ROI

## Finding

The “Metal always HQ, Windows chooses LQ by distance” hypothesis is supported
by the current R4 source and the current Metal adapter. It is a plausible
source of main-flora cost worth a small isolated follow-up, but there is no
LQ Metal bundle or runtime A/B yet, so this is not a performance result.

Windows R4 selects `SE_R2_NORMAL_HQ` when
`distance(camera, sphere.center) - sphere.radius < r_dtex_range`, otherwise
`SE_R2_NORMAL_LQ` (`src/Layers/xrRenderPC_R4/r4.cpp:85-104`). For non-landscape
static visuals—including the `MT_TREE_ST` Flora records in this frame—the
threshold is `r_dtex_range = ps_r2_df_parallax_range * g_fSCREEN / (1024*768)`
(`src/Layers/xrRender/R_calculate.cpp:24-33`). The checked-out default
`ps_r2_df_parallax_range` is 75 (`src/Layers/xrRender/xrRender_console.cpp:210`);
the old console command that could set it is commented out at line 1512.
The normal static insertion path calls `rimp_select_sh_static` for the main
shader element after admission (`src/Layers/xrRender/r__dsgraph_build.cpp:958-960`).

The `r2_ssa_lod_a/b` values (64/48 in the observer run) are not this selector.
They become projected-area thresholds in `R_calculate.cpp:28-29` and govern
`MT_LOD` scene-graph admission/fade and impostor work in
`src/Layers/xrRender/r__dsgraph_build.cpp:1335-1357` and
`src/Layers/xrRender/r__dsgraph_render_lods.cpp:16-134`. The exact 11,663
archive-winning Flora records used by the frame are `MT_TREE_ST` according to
the existing exact-ID source join, so their ordinary main draw takes the
tree-visual insertion path rather than the `MT_LOD` branch. `FTreeVisual_PM`
also has its own progressive slide-window mesh LOD when an OGF is progressive
(`src/Layers/xrRender/FTreeVisual.cpp:312-350`), but it is not the source type
of these records. Therefore SSA A/B is not a proxy for the HQ/LQ split at
issue.

The R4 tree blender compiles HQ with `uber_deffer(..., true, ...)` and LQ with
`uber_deffer(..., false, ...)` in `src/Layers/xrRender/Blender_tree.cpp:218-279`.
`uber_deffer.cpp:53-94` shows what changes: HQ may add detail variants and,
for bump textures, steep parallax and `-hq` shader permutations; LQ uses the
base bump/flat route. Both tree elements retain the wind-wave texture binding
in the tree blender, so this is not evidence that switching to LQ removes
wind. It also changes visual detail, so an LQ arm must be assessed separately
for intended distance quality and exact HQ pixel parity.

Metal's current flora setup resolves exactly four fixed paths named
`leaf_wave_hq.{vs,ps}.metallib` and `trunk_wave_hq.{vs,ps}.metallib`
(`src/Layers/xrRenderMetal/MetalR4EngineScene.mm:6485-6491`); its adapter
validates those exact HQ filenames and creates one leaf and one trunk main
pipeline (`src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp:515-560`). The
main candidate collector picks only the leaf/trunk material class and does
not classify distance (`MetalR4FloraAdapter.cpp:300-313, 673-700`); encoding
binds the corresponding fixed leaf/trunk pipeline. The available
`build/metal-runtime-shaders/flora/` and `flora-wind*` receipts and directories
contain HQ leaf/trunk pairs only. No LQ main Flora metallib, reflection file,
or cook receipt was found in those targeted folders. Thus Metal currently
does not reproduce R4's distance-selected LQ branch for these draws.

## Same-ID screen

The prior frame-700 exact-ID join establishes 5,316 actually encoded main
Flora IDs from the archive-winning `l01_escape/level`, each uniquely joined to
its source sphere and material, at eye `(-221.082565, -18.284866, -168.182037)`
and `g_fSCREEN=2,916,000.25` (`ssa-main-ids/FRAME_700_EXACT_ID_JOIN.md`).
Using that same ID set and camera with the source default range 75 gives
`r_dtex_range=278.0915` and this source-rule estimate:

| Material | HQ within range | LQ beyond range | Total |
|---|---:|---:|---:|
| Leaf | 529 | 1,856 | 2,385 |
| Trunk | 675 | 2,256 | 2,931 |
| Total | 1,204 | 4,112 | 5,316 |

This is an exact same-ID classification **conditional on the source default
75**. The ID capture records the view, screen factor, and SSA settings, but not
a frame-time `ps_r2_df_parallax_range`/`r_dtex_range` readback. The run's
`user.ltx` and startup log contain no override, and the source command is
commented out, which supports the default assumption; it is still not an
actual Windows draw census. The frame capture is a native diagnostic and
does not establish the Windows rendered set.

The recovered stock DX11 static-insert PDB evidence is a separate control:
its hard SSA cutoff rejected 261 of these same 5,316 IDs, versus 1,199 under
the newer generic source SSA rule, so stronger Windows static SSA does not
explain the 4,112 distance-LQ candidate IDs. The audited
`static_insert_disasm.txt` proves static admission arithmetic, not the R4
HQ/LQ selector; the selector finding above is source-backed, not an
independent machine-code audit of the stock selector.

## Smallest useful next test

Keep source and build products unchanged while preparing a scratch-only LQ
shader pair from the exact R4 tree `SE_R2_NORMAL_LQ` permutations for each
archive-winning leaf/trunk material. Save DXIL, metallibs, reflection, and
the full compile receipt together; confirm matching input/resource contracts
before attempting pipeline admission. Then run one fixed-camera main-Flora
pair that routes the same captured IDs by the R4 distance rule, with the HQ
arm as control. Record selected HQ/LQ counts and on-glass/frame metrics, and
separately review distant foliage detail, wind motion, and alpha behavior.
Until that candidate pair exists and passes those gates, keep this as a
plausible hypothesis only; do not claim FPS savings or claim exact HQ pixels.

No production, build, cook, game, or runtime files were changed for this
review. The only new file is this scratch report.


## Later evidence: actual range and stock inline selector

The native frame-700 observer completed exit 0 with actual `ps_r2_df_parallax_range=75` and `r_dtex_range=278.091461`. Its eye/view/projection float bits match the earlier main-ID capture. The fixed fixture now contains exactly 1,856 LQ leaf IDs (529 HQ leaf; 2,256 LQ and 675 HQ trunk), with minimum distance-to-threshold margin 0.0212098 m. Float32 source arithmetic and double classification agree for all 5,316 IDs. Receipt: `fixed-id-roi/fixture-receipt.json`.

The already pinned stock PE/PDB also contains the same inline distance selector: sqrt at VA `0x140c9aa65`, radius subtraction `0x140c9aa7a`, non-landscape range load `0x140c9aaad`, compare/setbe at `0x140c9aab5/0x140c9aab8`, and E[0]/E[1] fetch at `0x140c9aabd`. Its range-load target resolves exactly to public `r_dtex_range` at `0x14154e4ac`. Stock Calculate multiplies g_fSCREEN by literal 75 then divides by literal 786432 and writes that same global. Receipt: `../windows-ssa-controls/effective-policy/stock-lq-range-code.json`; original PE-only disassembly is retained. This strengthens source-policy parity but still does not establish actual Windows material flags or draw counts.

Scratch LQ cook/reflection and fixed-ID draft are complete. Runtime/quality/performance acceptance remains pending. The isolated harness pins level/level.geom, metallib/reflection and ID fixture hashes; these are launch provenance gates, not general in-app shipping admission.
