# CPU flora mesh extrema

## Result

Processed all **5,316 exact visual IDs** from `actual-frame-700.fixture` against the active `l01_escape` geometry used by the run. The frozen `R4LevelGeometry` parser accepted every ID, with zero unsupported layouts, bad indices, non-finite source values, or missing records. The new CSV recomputes the old indexed world-distance check exactly: all 5,316 `max_world_vertex_distance_to_sphere` values and all 1,199 source-reject decisions match the prior `sphere-geometry.csv` by visual ID.

The helper additionally verifies the exact P3/C4/C4/C4/S4 declaration, repacks signed S4 UVs as `RepackR4FloraStaticVertices` does, and applies the admitted flora `consts.xy=(1/1024,1/1024)`. It emits material shader/base texture, source sphere, referenced indexed-vertex counts, maximum world rest-vertex distance, transformed `H=pos.y-m_xform._24` extrema, and per-visual extrema/max-absolute of `1-tc.y`.

| Population | H range across visuals | `1-tc.y` range across visuals | Max absolute `1-tc.y` |
|---|---:|---:|---:|
| All 5,316 | -9.954598 to 45.052227 | -18.999023 to 19.394531 | 19.394531 |
| 1,199 source rejects | -1.729906 to 25.310616 | -7.963867 to 8.890625 | 8.890625 |

Among source rejects, 674 have at least one negative H vertex, all 1,199 have at least one `1-tc.y < 0` vertex, and 257 have `max(abs(1-tc.y)) > 1`. Across all IDs, 2,262 have negative H somewhere and 1,824 have `max(abs(1-tc.y)) > 1`. The 5,316 visuals resolve to the two admitted shader names (`flora\\leaf_wave`, `flora\\trunk_wave`) and 50 distinct base textures.

This closes the static per-visual H/UV geometry uncertainty for this captured frame. These extrema are rest geometry only; they do not include branch/trunk wind, current interactive benders, or any frame-to-frame bender movement. The results are **not** a safe displacement bound and do not admit SSA rejection.

## Provenance and reproducibility

- Fixture: `../actual-frame-700.fixture`, SHA-256 `7e64394fe35749e3980d1eb9b26870f7f93651b22bd1324ad6984f81476ae080`.
- The matching run summary says `level=l01_escape`; the runtime level path resolves through the active GAMMA profile to mod `442- Expanded & Fixed Maps - HailTheMonolith`.
- `level`: SHA-256 `1800a451d0bba9af1da69fae8683b36b6e6346709c6b9bf14385f4a48d19f7`.
- `level.geom`: SHA-256 `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`.
- Frozen parser source SHA-256 `ee261880fd761340178c0de0b946ddc9abdb3a077fd798c9a2446f85100125e4`; parser header, Flora repack/constant source, and SSA predicate hashes are recorded in `manifest.json`.
- CSV SHA-256 `3d15d90548265cb9be3ec9b01c4e45ecfae4f240dada0a157346dee858f83605`.

The row-level output is `flora-mesh-extrema.csv`; exact file and source hashes plus the standalone compile/run commands are in `manifest.json`. The helper was compiled and run as a standalone CPU utility. No application build, game launch, shader cook, or production source edit was performed.
