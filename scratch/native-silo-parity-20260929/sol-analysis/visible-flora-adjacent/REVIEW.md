# Main Flora adjacent instancing: stop after source-order census

2026-09-29. Read-only source and exact active-level analysis. No production edits, shader cook, renderer build or GPU/game run by Sol.

The attractive global number is real but does not describe a safe main G-buffer optimization. Startup logs in `runs/r1-policy-off-01/native/output/engine.log` show 11,663/11,663 Flora visuals admitted, 487 geometry/resource shadow groups, and 103 textures. Main frame300/600 draws5,316 and culls6,347. The saved CPU sample's437/2,264 main Flora stack samples are inclusive and cannot be converted to FPS. R1 source policy does not directly change these main draws.

`MetalR4FloraAdapter.cpp:293` collects `R4LevelGeometry.visuals` in original serialized order. `:904` culls each draw against the live main camera, then encodes one draw for each survivor in that same order; there is no main sort/group. Shadow instancing instead gathers every selected model into its precomputed group before drawing (`:847`, `:1244`) and may reorder whole casters because that depth-only pass has a different output contract.

The active native runtime's exact `l01_escape/level` symlink resolves to mod442's level file (6,416,980 bytes, SHA256 `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`). [census_source_order.py](census_source_order.py) reads this one small file and reproduces the adapter's admitted source order and exact shadow grouping key: VB/vertex range, IB/index range, material and base texture. It does not read `level.geom`, cook a shader or launch the game. Its 11,663 source records, 487 geometry groups and 487 exact resource groups independently match the runtime inventory. [source-order-census.json](source-order-census.json) preserves the result.

| All source records before camera culling | Count |
|---|---:|
| Distinct exact resource groups | 487 |
| Adjacent compatible runs | 11,646 |
| Draws removable by *adjacent-only* instancing | **17 / 11,663 (0.15%)** |
| Multi-instance runs / participating instances | 12 / 29 |
| Multi-instance runs with different authored tree lighting | 12 / 12 |
| Longest run | 4 |
| Trunk multi-instance runs | 0 |
| Groups with different authored tree lighting values | 369 / 487 |

Those17 are exact for the **unculled source sequence**, not a bound on the5,316 camera-visible draws: culling may remove intervening records and create new adjacency. Existing frame logs have only survivor counts, not IDs, and no accepted main view matrix/position-bounds ledger. A live same-frame survivor-order census or an exact offline level.geom+accepted-view replay would be required to claim a visible-stream reduction. Because source adjacency is almost absent and the needed shader change is substantial, the current proposal stops without either new observation or a production candidate. Revisit only if another accepted main-view record makes a substantial adjacent reduction demonstrable.

The actual two shaders are the mod-winning `build/metal-shader-sources/r3` inputs pinned by `flora-wind-v3/flora-l05-bar-wind-msc-receipt.txt`. Main leaf VS uses per-instance `m_xform`, `m_xform_v`, authored `c_scale/c_bias/c_sun`, model height in the wind phase, current/previous wind and benders, and previous-view projection for motion. Main leaf PS has `SSFX_FLORAFIX` enabled and reads `m_WV` for its G-buffer normal, while the trunk PS uses VS-interpolated normal and motion. The shadow recipe's 64-byte model-only SRV cannot be applied unchanged to main leaf/trunk draws. The main pixel output uses four MRTs and Depth32Float_Stencil8 with strict Less depth and stencil Replace; grouping or reordering unproven equal-depth overlaps could change which tree writes the G-buffer, motion and stencil.

If a later visible-order census reverses the economics, the smallest **real-shader** gate would begin with two consecutive compatible instances, from one existing group, preserving their sequence. A generated wrapper over the active winning shader tree would provide an immutable per-instance record containing exact model, WV and authored tree coefficients; shadow instancing's validated MSC structured-buffer SRV descriptor pattern can be reused at the VS, with the reflected per-stage TLAB order checked anew. Leaf also needs a flat instance identifier reaching its fragment shader so its `m_WV` is selected per instance. Keep original current/previous wind/bender arrays frame-common; preserve alpha clip and all other FLORAFIX math around that one per-instance WV lookup. A trunk-only two-instance probe is smaller but would not exercise any of the twelve actual adjacent runs and cannot admit the leaf path.

The offscreen oracle would draw the same two admitted inputs as two legacy draws and one instanceCount=2 draw into separate production-format four-MRT plus D32S8 attachments, using the actual qualified leaf VS/PS permutation and wind-wave/bump textures. Compare raw bytes of all MRTs, depth and stencil after completed command buffers. Require nonblank alpha-tested coverage, distinct model Y/tree lighting, current/previous wind+benders and nonzero motion; include an overlapping equal-depth case with different authored lighting so a changed winner fails rather than yielding blank==blank. The old E08 Flora GPU probe uses only a shadow R32 target and cannot certify this G-buffer contract. Generated wrappers must fail on changed source/receipt hashes and stay outside installed GAMMA assets; no upstream mod file would be edited. This is an oracle plan, not a request to cook/build/run it now.
