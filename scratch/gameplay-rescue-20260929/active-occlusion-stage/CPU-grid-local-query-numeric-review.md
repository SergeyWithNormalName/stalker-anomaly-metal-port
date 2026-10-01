# Local-volume CPU-query transform error review

Read-only review of `CPU-grid-local-query-wiring.md` and the current
`MetalR4LocalLightAdapter.cpp`. No code or build changes. This is a conservative
diagnostic bound, not evidence of GPU coverage equivalence.

The local-volume shader uses `O.hpos = mul(m_WVP, P)` and no clip-space jitter
(`build/metal-shader-sources/r3/accum_volume.vs:11-18`). The adapter forms
`A = projection * view`, then `W = A * M`, where `M` is `localLightModel(light)`;
it uploads `W` to vertex transforms and `A` to fragment globals
(`MetalR4LocalLightAdapter.cpp:443-444, 481-491`). `BuildMetalR4LocalLightVolumeBounds`
uses that same model form, transforms the exact point/spot/canonical-omni local
box corners in double, narrows extrema to float, then adds per-axis
`d_i = 0.01 + 1e-5 * max(|lo_i|, |hi_i|)` world padding
(`629-687`).

The padding covers the geometric local box and its double-to-float world-bound
rounding: binary32 narrowing is at most about `2^-24 * |x|` plus subnormal
slack, while the relative pad is `1e-5 * |x|` and the absolute pad is 1 cm.
It does **not** by itself certify clip-space equality between the draw path
`fl(W * p)` and the query path `fl(A * (M * p))`; nor does it replace the
query helper's error bound for its own `A * worldCorner` projection.

For an admitted draw, use the exact uploaded matrices `A` (fragment globals
offset 64), `M` (vertex transforms offset 128), and `W` (vertex transforms
offset 0), all from the same draw/scope. Do not reconstruct them from a later
view or engine frame. Let `p=(x,y,z,1)` range over the actual local shape box:
point `[-1,1]^3`, spot `[-.5,.5] x [-.5,.5] x [0,1]`, or the exact
`du_sphere_part` vertex extrema for an omni face. Define `pmax_j` as the maximum
absolute local component over that box, with `pmax_3=1`.

With strict binary32 arithmetic and `gamma8 = 8u/(1-8u)`, `u=2^-24`, a
conservative per-row/column bound for the CPU matrix product is
`E_W[r,j] = gamma8 * sum_k |A[r,k] M[k,j]| + 8*FLT_MIN`.
The GPU vertex dot then gives
`E_assoc[r] = gamma8 * sum_j |W[r,j]| pmax_j + 8*FLT_MIN +
sum_j pmax_j * E_W[r,j]`, rounded upward. This bounds the association and
four-term dot difference from exact `A*M*p`; fused execution is within the
bound. Use the actual column-major uploaded values when evaluating the row
sums. Reject non-finite terms/results or a bound that reaches clip W/near/far
uncertainty; do not clamp it to an arbitrary epsilon.

Keep the current padded world AABB shape. For each of its eight world corners,
project with exact uploaded `A`; `clipAbsDotTerms` must be an upward-safe sum of
absolute row products. Put the worst-case `E_assoc[r]` (plus any CPU evaluation
error not already covered by the query dot bound) in each query corner's
`upstreamAbsError[r]`, rounded upward. The helper then expands the projected
query rectangle and nearest normal depth for the possible draw/query transform
discrepancy. Its built-in dot/division and `8*FLT_MIN` bounds cover the
query-corner projection itself. The scene must carry the local p component
bounds and the exact three matrices with the receipt; without them the CPU
query is visible/unknown. If the current 1-cm/relative world padding is ever
used as the sole association margin instead, first prove for every clip row
that its projected margin dominates `E_assoc` after homogeneous division; the
current source does not make that proof.

The local-volume transform is distinct from the static G-buffer transform, and
the current native passthrough raster probe does not qualify either matrix
association bound or this CPU projection path. The query remains observation-
only and unqualified until a local-volume-specific numerical/native boundary
check passes.
