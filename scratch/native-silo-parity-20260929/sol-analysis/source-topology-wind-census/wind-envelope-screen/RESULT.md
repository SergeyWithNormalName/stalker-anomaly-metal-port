# Frame-700 flora wind-envelope screen

## Verdict

The conservative same-frame interval bound leaves **773 of the 1,199 original source-SSA rejects still rejected** after radius inflation; 426 become kept and 0 fail open as unknown. This is a per-frame arithmetic screen only. It does not establish visual acceptance, whole-level coverage, or permission to enable flora culling.

The 314 rejected leaf visuals were evaluated against all nine current and previous shader bender slots using the captured bender centers, radii, strengths and setup. The closest bender center stays **52.844 m outside** the evolving 3D enclosing ball, and the closest XZ lower bound stays **52.741 m beyond** its effective `radius + 0.001` influence denominator. Therefore all these benders have zero displacement upper bound for this set/frame, and their `normalize(pos-center)` inputs cannot reach zero within the bound. The code does not assume bender strengths are zero; it proves their captured centers cannot affect these candidate visuals under the derived enclosing radii. Other visuals or frames may differ.

The survivors split into 714 trunks and 59 leaves. The displacement-bound median is 0.467 m, p90 1.749 m, maximum 6.410 m. The resulting adjusted-radius median is 2.320 m, p90 4.930 m, maximum 8.806 m. Bender and wind values are recorded per source reject in `classified-rejected.csv` and bender-center clearances in `rejected-envelopes.bender-clearance.csv`.

## Interval derivation

The pinned receipt selects `SSFX_WIND=1`, `SSFX_INT_GRASS=8`, leaf source `deffer_tree_branch_bump-hq.vs`, and trunk source `deffer_tree_flat.vs`. The frozen runtime DDS receipt proves `s_waves` samples in `[0,1]`. Current and previous animation inputs are captured at frame 700; the bound spans every possible texture sample and every trunk phase, so it does not depend on matching a CPU texture sample to a vertex.

For each visual, let `U=max(abs(1-tc.y))` from its exact indexed signed-S4/1024 mesh extrema, `H=[Hmin,Hmax]`, `S=W.speed`, `V=saturate(W.sqrt_speed*1.5)`, and `B=abs(ssfx_wsetup_trees.z)`. The shader math gives:

- `abs(TWave) <= 1.5*B`, since `abs(cos(phase)*sin(5*phase)+0.5) <= 1.5`.
- `Q=max_H H^2*saturate(1-0.005*H)` over the measured H interval, including the cubic's interior maximum at `H=133.333...`; this handles negative H explicitly.
- Trunk world displacement norm is bounded by `sqrt(2)*0.006*V^2*(1.5*B)*Q`. The `sqrt(2)` bounds the two direction components independently.
- Branch horizontal raw motion norm is bounded by `sqrt(2)*(1+Tz*C)`, where `Tz=saturate((1+1.5*B)/2)` and `C=max_H clamp(0.1*H,1,2.5)`. Branch Y raw motion is at most 1. Applying `U*S` and adding trunk XZ yields `Dwind <= U*S*sqrt(2*(1+Tz*C)^2+1) + Dtrunk`.
- For each bender, the effective radius follows the shader's `radius<=0 ? benders_setup.x : radius`, and non-dynamic strengths multiply by `benders_setup.yz`. Given lower-bounded XZ distance `dmin`, `bend <= 1-saturate(dmin/(effectiveRadius+0.001))`; if the denominator is zero/negative and a zero-distance case cannot be excluded, the visual fails open. Each displacement norm is bounded by `U*bend*sqrt(2*(2.25*abs(strengthX))^2+(0.67*abs(strengthY))^2)`. Per-loop bounds are accumulated for current and previous arrays separately.

The bender center is also checked before each loop. At that point every possible vertex is enclosed by a sphere centered on the unchanged source-sphere center with radius `rest_max_distance + Dwind + prior_bender_bounds`. If any captured center could lie in that sphere, the visual is marked unknown because `normalize(pos-center)` may receive zero. The script adds eight float32 ULPs only for outward rounding after the full terms are bounded. The inflated radius is `float32_ceil(rest_max_distance + max(DcurrentBenders,DpreviousBenders) + Dwind)`; its center and original source-sphere hash remain unchanged.

The existing exact `R4SourceStaticSsaReject` predicate then classified the 1,199 adjusted radii with the original frame-700 eye, discard/fade/exponent, and hash. It retained 773 rejects. This is only the source rule's disposition after an analytical bound; it is not a live visual-cull acceptance result.

## Inputs and limits

- Live wind fixture: frame, environment and motion snapshot are all 700; callback previous frame is 699. Its eye/view/projection IEEE bits match `source-topology.json` and the prior source-sphere fixture. Wind JSON SHA-256: `7515eff2f59b25f5357fb3a02cf5e9f3b48acecb8a9dc4dd1607fe2fc8101643`.
- `source-topology.json` SHA-256: `d4c9defb372380f87487bbc269061e3cf0f726f7d47480a1278dc4968aff44ea`. It independently reports frame 700, 1920x1200, `r_ssaDISCARD=1.68038387e-05`, and matching camera bits; it also reports `source_sector_count=0`, `source_visual_count=0`, `legacy_graph_populated=false`, `geometry_verified=false`, and suppression 0. This envelope calculation uses the separately captured 5,316 qualified visual IDs and must not be read as a global source-topology census.
- Mesh extrema input SHA-256: `3d15d90548265cb9be3ec9b01c4e45ecfae4f240dada0a157346dee858f83605`.
- The verified normalized DDS SHA-256 is `4fb08cd504bb2a2e7be7f128d3eb2533b24afae8645ee977fa7321a2821b29e2`; pinned shader source and runtime receipt hashes are recorded in `input-manifest.json`.
- Exact source-SSA settings come from `actual-frame-700.fixture` (SHA-256 `7e64394fe35749e3980d1eb9b26870f7f93651b22bd1324ad6984f81476ae080`); the classifier links the frozen header implementation rather than approximating its float decisions.

Reproduce the CPU calculation with `run_screen.sh`. Outputs and all input hashes are listed in `input-manifest.json`. No application build, game launch, GPU work, shader cook, or production edit was performed.
