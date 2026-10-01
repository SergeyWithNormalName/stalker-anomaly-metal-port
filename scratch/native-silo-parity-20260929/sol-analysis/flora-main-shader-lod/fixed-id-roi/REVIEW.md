# Fixed-ID leaf LQ ROI source review

## Decision

**Conditional pass for the isolated fixed-view ROI**, with cook and level provenance enforced by the launch harness. The reviewed patch changes only the selected main G-buffer pipeline for the 1,856 fixture leaf IDs when the exact view/projection/eye bits match and current wind state is valid. It leaves source order, ID set, indexed draw loop/count, immutable frame uploads, and shadow drawing unchanged. This is a scratch quality/performance experiment, not a shipping LOD policy: LQ deliberately drops branch interaction, HQ flora variation, and the HQ parallax path.

## ABI and resource checks

The cooked LQ artifacts agree with the candidate binding setup:

| Stage | Reflected ABI | Candidate binding |
|---|---|---|
| VS | `$Globals` 464 bytes; CBV TLAB entries at byte offsets 0 and 24; 5 inputs | 48-byte TLAB: per-draw globals at 0, existing 528-byte `static_globals` at 24 |
| PS | `$Globals` 176 bytes, `dynamic_transforms` 256 bytes; 2 SRVs, 2 CBVs, sampler; 4 MRTs; alpha discard | 120-byte TLAB: base/bump at 0/24, per-draw fragment globals/dynamic transforms at 48/72, sampler at 96 |

The DXIL assembly/reflection names the VS inputs as POSITION0 Float4, NORMAL0 Float4, TANGENT0 Float3, BINORMAL0 Float3, and TEXCOORD0 Int4. These match the existing Flora vertex layout at slot 6, stride 72, attribute offsets 0/16/32/44/56. Its ten VS outputs (TEXCOORD0/1/2/3/4/7, VEL_CURRENT0, VEL_PREVIOUS0, SSS_EXTRA0, SV_Position) match the PS input semantics. The PS writes four G-buffer targets and retains the alpha discard. The LQ PS samples only T0 base and T1 bump; `ResolveR4FloraTexturePaths` orders those first, before the HQ-only bumpX resource at T2.

The candidate uses the existing pipeline descriptor: same four G-buffer formats, depth format/compare/write, cull mode, front face, and stencil state as HQ. `DrawIntoActiveGBuffer` still uploads fresh immutable slices for vertex globals, fragment globals, and dynamic transforms before binding them. It does not share or mutate an HQ slice.

## Transform and wind review

The VS assembly lays out `m_wvp_prev` at byte 0 and `m_vp_prev` at byte 64. The LQ tree shader uses `m_vp_prev` to project the previous wind-deformed position; DXIL loads the previous-view-projection registers beginning at CB register 4 and has no loads from registers 0–3, so the candidate's current `wvp` write at byte 0 is unused in this VS. Its `previousVp` write at byte 64 is the field that supplies prior-frame motion. The current tree model and view-model transforms land at offsets 160/224, matching the 64-byte column-major storage of each `float3x4` cbuffer member.

The remaining VS offsets match reflection: constants/c_scale/c_bias/c_sun at 288/304/320/368 and LQ wind params, grass/tree setups, current animation, and prior animation at 384/400/416/432/448. The source LQ shader calls the ordinary tree wind function for both current and previous deformation; unlike HQ it has no interactive branch-bender arrays. This is an intentional visual distinction. The main draw still uses the live current/previous wind snapshot; the shader variant is selected per fixed ID only when the view bit check and `windValid` pass.

## Fixed-ID scope and provenance

The checked fixture has 1,856 unique IDs, all marked `leaf`, and each is present as a leaf in the saved 5,316-ID frame-700 main-Flora capture. Its view and projection rows match that capture bit-for-bit, and its eye bits are `c35d1523,c1924768,c3282e9a`. The in-app gate compares these camera bits and only admits those IDs; it intentionally has no engine-frame test, so it applies across frames with the same fixed camera while the motion fields continue updating.

The adapter's reflection gate checks filenames and ABI strings, but does not hash the loaded metallibs, reflection JSON, or level files. The current `luna-harness/run_matched_pair.sh` gate pins the fixture plus both metallib and reflection hashes before ON launch. Root reports that the separate `run_leaf_lq_roi.sh` gate also pins the active `l01_escape/level` and `level.geom` receipts before each prepared run; that level identity guarantee belongs to the harness, not this adapter. Treat the candidate as eligible for an ON run only when those harness receipts match. Without those gates, camera bits and reused visual IDs alone would not identify the intended level or exact shader bytes.

## Limits

This is source/data review only. I did not build the adapter or run the game. The cook manifest reports successful DXC/MSC output, and the inspected reflection/assembly support the declared ABI; runtime pipeline creation, visual output, alpha-edge behavior, and performance remain unverified by this review. The source change intentionally selects the LQ base bump PS, so HQ parallax/branch coloration and interactive bending are expected to differ and need visual review before any product-policy claim.
