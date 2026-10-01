# CPU Flora wind-envelope prototype

## Contract review

The available per-render inputs are sufficient for a per-frame bound. `R4FloraViewState` supplies `windValid`, `windParams`, `windSetupTrees`, `bendersSetup`, and both 32x4 current/previous bender arrays on each Flora draw. It also carries both wind-animation float4 uniforms. The bound validates animation values for finiteness, but deliberately does not use their phase: the pinned analytic envelope covers every trig phase and every normalized wind-wave texture sample in `[0,1]`.

At `Prepare`, the adapter has repacked P3/C4/C4/C4/S4 vertices, local indices, and each model transform before discarding the CPU vectors. The minimum retained per visual for an eventual main-loop use is a world-space enclosing rest-sphere center/radius, transformed indexed `H` min/max (`worldY - model._24`), `max(abs(1-tc.y))`, and the material family. Compute them over unique referenced indices. The existing `Draw` retains only an AABB/model/material, so this small metadata would need to be added there for runtime use. `R4StaticVisual` does not retain an OGF source sphere in the current adapter contract; the integration center/radius can instead come from the transformed indexed-vertex AABB, which encloses all indexed vertices. That alternate sphere may be looser and can make more rows unknown or larger, so the frame replay below does not stand in for an AABB-center integration replay.

## Frame-700 replay

`FloraWindEnvelope.h` implements the previously reviewed component bounds: H-dependent trunk wind, UV-scaled branch wind, and nine sequential SSFX bender bounds per current/previous state. It fails open for invalid/nonfinite data, possible zero input to shader `normalize`, unsupported rest extrema, or overflow. It outward-rounds float radii and uses the maximum of independently bounded current and previous displaced positions.

The standalone C++17 driver replayed all 5,316 frame-700 mesh rows against the exact current/previous uniform snapshot and compared every bounded output radius with the prior Python envelope CSV. The replay used each row's original OGF sphere center plus its indexed max-distance rest radius so this comparison is apples-to-apples with that prior result.

| Check | Result |
|---|---:|
| Visual IDs processed | 5,316 (2,385 leaf; 2,931 trunk) |
| Prior source-SSA rejects in input | 1,199 |
| Known conservative bounds | 5,301 |
| Fail-open unknown bounds | 15 |
| Bounded radii matching prior Python output | 5,301 / 5,301 |
| Prior no-HOM portal geometry candidates kept excluded | 4,086 |
| Synthetic checks | 6 / 6 pass |

Because every bounded radius and every bounded/unknown status matches the prior screen, its 4,086 candidate labels reproduce exactly. This validates a compact C++ arithmetic port for the supplied frame inputs; it does not validate source draw eligibility or an integrated renderer path.

The root's fresh frame-700 wind observer produced a second valid snapshot. The JSON reader now preserves bare `-0` numeric tokens as IEEE negative zero, so callback and Flora eye/view/projection words compare **exactly** with the frozen frame camera fixture; no camera words are normalized or ignored. Current/previous benders and animations changed, as expected for another observation. The same C++ helper reports 5,303 known and 13 fail-open unknown radii. The CPU portal reclassification remains at 4,086 excluded and 341 reopened raw exclusions. Both snapshots use the same frame-700 mesh extrema and OGF sphere centers; neither establishes temporal validity.

The renderer source formula costs a median **1.27 ms per 5,316 visuals** over 50 compute-only iterations using the original frame-700 motion, and **1.30 ms** with the fresh active-input snapshot. CSV parsing, fixture parsing, output, and compilation were outside the timed loop. The checksum was consumed through a volatile sink. This measures the current validated helper contract on this host; it is not an FPS claim.

Synthetic cases cover ordinary leaf bounds, a current bender entering while the previous one leaves, the swapped current/previous union, possible zero-normalize fail-open, large UV values with negative H extrema, and nonfinite previous bender fail-open. The full replay also covers trunk-only motion and real unknown-normalize rows.

## Reproduction and limits

Run `./run_replay.sh` from this folder. It extracts float32-rounded values from the pinned `flora-wind.json`, compiles only the standalone helper/driver with the system C++17 compiler, and writes `frame700-replay.csv`. No application or production source is built or changed; the game and shader cook are not run.

The radius inputs in this replay use the proven frame-700 mesh-extrema output. Runtime integration must first compute and retain the metadata listed above, ideally a sphere from the transformed indexed AABB. Then compare that alternate bound and candidate set independently. This is an analytical per-frame geometry envelope, not a temporal/global promise: benders can move between frames, and the 15 unknowns must remain visible. The old portal labels are no-HOM per-ID candidates and still omit source hierarchy/full-visible shortcuts.

## GPU transform precision caveat

The eight-ULP outward step in the helper applies to computed displacement/radius terms. It does **not** bound the error between CPU-transformed rest vertices and the GPU's `m_xform * P`; a fixed ULP count at the final radius cannot cover arbitrary cancellation or a large matrix/position magnitude.

For an indexed local vertex `v=(x,y,z,w)` and transform row `i`, form `S_i = |M_i0*x| + |M_i1*y| + |M_i2*z| + |M_i3*w|`. The pinned R3 tree input declaration is `float4 P` with the source comment `(float,float,float,1)` (`common_iostructs.h`, `v_tree`), and the tree shaders multiply that input by `m_xform`. Thus `w=1` is the intended contract, but integration must still verify that the actual repacked P3 vertex-fetch format supplies `w=1`; if that fetch contract is not proven, use the four-component input and conservatively allow a full four multiply/add pairs. Under finite normal-range IEEE float arithmetic, use `e_i <= gamma_6*S_i` only for proven `w=1` (three products plus three sums), or `e_i <= gamma_8*S_i` for an unconstrained float4 (four products plus four sums), with `gamma_n=(nu)/(1-nu)` and `u=2^-24`. Fused multiply-add can reduce rounding steps, but the bound must not depend on that optimization. Add `max_v sqrt(e_x^2+e_y^2+e_z^2)` to the indexed rest radius. For shader `H=pos.y-M._24`, widen both H extrema by the Y dot-product error plus `u*(|pos.y|+|M._24|)` for the subtraction. Check for overflow and account for subnormal/flush-to-zero behavior or fail open. Signed S4 `tc.y` scaled by exactly `1/1024` is an exact binary fraction for admitted int16 inputs; UV conversion itself is not the uncertainty here.

This margin scales with the absolute local products and translation, not just the final radius. For example, `S_i=1000` gives `gamma_6*S_i` about `0.00036`, while eight ULPs around radius 10 are about `0.0000076`. The prototype does not calculate these per-vertex transform errors; do not claim integrated GPU-vertex enclosure until that margin is implemented and CPU-checked against the exact transform/input contract.
