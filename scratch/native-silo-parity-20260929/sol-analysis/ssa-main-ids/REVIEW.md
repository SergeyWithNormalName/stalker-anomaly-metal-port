# Main Flora visual-ID observer — scratch draft

This is an observe-only patch against the five exact source hashes in
`source-manifest.json`. It has **not** been applied to production source, built,
or run. It changes no draw admission, shader, binding, resource, or shadow path.

## Scope and source of truth

`XRAY_METAL_MAIN_FLORA_IDS_DIR` selects an existing, empty, run-unique output
directory. `XRAY_METAL_MAIN_FLORA_IDS_FRAME` selects one engine frame, at least
300. Both must be set before process launch. All other frames do no allocation
or I/O for this observer. The selected main G-buffer Flora call appends each
visual ID and leaf/trunk material only **after** its `DrawIndexed` succeeds.
It writes one buffered CSV after the loop, then renames `.partial` to `.csv`.
One status line goes to stderr. No per-draw logging is introduced.

The CSV records the accepted `R4SubmittedViewHistory` view, projection, and eye
as exact float32 hex bits, as well as engine-frame ID, extent, and live engine
SSA inputs: `fovDegrees`, `ps_r__LOD`, `ps_r__ssaDISCARD`,
`ps_r__ssaDISCARD_exp`, `ps_r__ssaDISCARD_fade_k`, `g_fSCREEN`, and the
authoritative `r_ssaDISCARD`, in that order in `ssa_settings_bits`. Its last
row is `encode_complete,<submitted>`. After `SubmitFrame` succeeds, the device
writes a separate `.submitted` marker with frame and submitted serial. The
CSV by itself proves only completed Flora encoding, not a submitted frame or
GPU/presentation completion.

## Minimal admission check for a saved run

Require a unique empty output directory, the CSV completion row, a matching
`.submitted` marker, and a completed E01 row for the same engine frame and
serial. Require `admitted == submitted + culled`, unique submitted visual IDs,
and the existing `METAL_FLORA_FRAME` count to agree for the fixed view. Reject
if the recorded engine frame, camera matrices/eye, or effective SSA threshold
disagree with the same-frame accepted scene/device record. The frame must be
in measured phase; a fast run can reach frame 360 during warmup, so frame 700
is a better initial selection for this harness.

The offline join must load the **archive-winning** `level` file and map every
recorded `visual_id` to its exact OGF_HEADER sphere and shader ID. Apply source
`CalcSSA` and `GetFvectorHash` from `r__dsgraph_build.cpp` and `_vector3d.h`
using the recorded float32 eye and runtime settings. Count hard rejects, fade
rejects, and keeps separately by leaf/trunk. `R4StaticVisual` does not carry
legacy `eIgnoreOptimization` or selected shader E[4] emissive flags; the
offline classifier must establish these from the actual source/shader state
or mark those IDs **unknown/exempt**. Unknown cannot become a culling action.
Do not infer an FPS or GPU saving from these counts.

The first possible implementation gate, if the join shows enough work, is
main G-buffer only. Local/sun shadow casters, forward, HUD, volumetric,
secondary views, and mod-authored material behavior stay outside this patch.
There is no suppression code here.

## Review limits

The patch adds `R4FloraSsaObserverSettings` to the existing view bridge so the
standalone Metal adapter does not link engine globals. The engine captures
settings in `SetCacheXform` after `CRender::Calculate`; the accepted scene view
copies them unchanged. If the engine order or camera provenance differs in a
run, discard that observation. An opt-in sample incurs a buffered write during
the selected frame and is diagnostic only, never a performance A/B arm.

`git apply --check` passed against the shared source at draft time. No build,
game, GPU probe, or runtime test has been performed by this subtask.
