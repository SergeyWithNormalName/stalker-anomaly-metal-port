# Frame-700 main Flora geometry-group screen

This read-only count reconciles the captured 5,316 visible main Flora IDs with the archive-winning level source and the same-frame native vertex/index fetch observer. It does not edit production, build, cook, or launch the game.

Run `python3 count_visible_groups.py` from any directory. The script reuses `visible-flora-adjacent/census_source_order.py`, verifies its pinned active-level SHA, checks every visible ID/material against source, checks ordinal material against native PSO label, and requires a bijection between source geometry/material/base-texture groups and live PSO/buffer-version/index-range signatures. It writes `visible-main-group-census.json` deterministically.

Pinned inputs:

| Input | SHA-256 |
| --- | --- |
| Active source `level` | `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7` |
| Frame-700 main Flora IDs | `dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc` |
| Frame-700 native fetch TSV | `see visible-main-group-census.json` |
| Reproducible script | `af482422d170e71a144020d25e9eb44617b409c74cb4a22f6232d9cfb2ebd118` |
| Per-group CSV | `47ffb66650b3c1c4bbe96d75a1c6d534e6143b8455914da6f260928a4283456b` |
| JSON receipt | `f21f119d2bbd6c5aab590842a968006d303cfbaf606c15a3d1f4ea5d6e7b7b71` |

Result: 5,316 draws partition into 384 source geometry/material/base-texture keys and 384 matching live PSO/VB-version/IB-version/range signatures. The source-to-live grouping is bijective. The raw combinatorial ceiling is 4,932 fewer submits (92.7765%) if all same-key draws could be combined. It is not a draw-reduction result or a frame-time/FPS estimate.

There are 286 of 384 groups with multiple authored tree-lighting payloads. Current main draws remain separate and source ordered. Main leaf shading also uses per-visual WV in the fragment path; both families need per-instance transform data for any batch. Nonadjacent regrouping can change strict Less depth/stencil winners at overlaps. The count therefore supports only a small same-frame main-G-buffer OFF/OFF infrastructure control first, followed by an OFF/ON exact-pixel oracle before any batching proposal.

The fetch observer records that backend draw records do not carry a visual ID directly. The join relies on ordinal order: the observer covered the complete `R4 engine scene G-buffer` pass with 5,316 records, its material/PSO label agrees with each ordinal ID material, and the source/live equivalence partitions form a bijection. The receipt records these checks. Texture identity is the source level's shader material plus base texture; bump resources are resolved by the existing Flora adapter from that identity.
