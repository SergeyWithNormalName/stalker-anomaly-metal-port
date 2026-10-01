# Portable parser metadata draft validation

## Scope

The draft extends only the scratch copy of `R4LevelGeometry.h/.cpp`. The production prestate was copied before editing and pinned at:

- header SHA-256 `2996269c5ed5d2dab13a313db0cdf2180409db5c8b4fe73fe373c26ec9aebec6`
- source SHA-256 `1aff3bda26194596a0d855c19810349fa457b08625c7896f25935e839a3ba3f8`

No production source, build tree, cook output, or game state was changed.

## Behavior

`R4VisualNode` now retains its OGF six-float box and four-float sphere plus a `boundsValid` bit. Bounds are metadata only: nonfinite, unordered, or negative-radius values set `boundsValid=false` without changing geometry admission.

After the existing geometry parser finishes its original visual/buffer/index checks, an optional metadata pass reads `fsL_SECTORS` chunk 8 and `fsL_PORTALS` chunk 4 using the same chunk scanner and the OGF fields already parsed. It returns value-owned sectors, portal endpoints/ordered vertices, and a visual-ID-to-sector-owner map over linked-child roots. It validates sector/root IDs, portal counts/endpoints, unique incident links at both sectors, bounds, child references, and per-root hierarchy cycles. Any metadata problem clears that metadata and records `available=false` plus a reason; `ParseR4LevelGeometry` still succeeds with the same drawable-visual result.

The copied prestate included an existing portable selftest fixture whose tree definition was 96 bytes although the parser requires a 64-byte transform plus two 20-byte lighting color values. The scratch test fixture is corrected to 104 bytes so the existing geometry selftest can run; this is only a fixture-length repair, not a parser behavior change. The metadata extension adds 28 header lines and 158 non-test parser lines (186 total, excluding 127 added selftest lines).

The selftest also adds a third sector that lists a portal whose endpoints are sectors 0 and 1. Metadata must become unavailable with a nonincident-sector reason, while the same static visual remains admitted. The topology reader now validates incidence in both directions: every sector-listed portal must name that sector as an endpoint, and both portal endpoints must list the portal.

Polygon qualification remains deliberately incomplete. Finite 3–6 vertex records do not by themselves prove a valid fan or averaged portal normal; collinear or self-intersecting polygons could still make later `CPortal::Setup` plane construction unsafe. Do not consume these polygons in a walker until source-compatible polygon prevalidation is added; such uncertainty must disable metadata use and preserve fail-open rendering.

## Validation result

Compiled only the draft parser and scratch probe as a standalone portable C++17 executable, then read:

- `active-engine-level.bin`, generation-1 winning-reader copy, SHA-256 `1800a451d0bba9af1da69fae8683b36b6e6346709c0d6b9bf14385f4a48d19f7`
- matching pinned `level.geom`, SHA-256 `a4552d34eec6f87ed7eb461327c8a331d1f7a3b9911a2b51342ca4d377f8d78a`
- the 5,316-row frame-700 Flora ID fixture, SHA-256 `dca66b5c94dffae895bc33ba77a4cdc57881cce7c5252b07ba823b2dd6bdcdbc`

Output:

```text
geometry_ok=1 topology=32/128 owners=5316 unique=1 sector3=8 sector30=5308 selftest=pass
```

For this revision, the draft compiled again with `-std=c++17 -Wall -Wextra -pedantic`, `metadata_probe --self-test` returned `selftest=pass`, and the real-input probe was rerun against the three SHA-pinned inputs above. It returned `geometry_ok=1 topology=32/128 owners=5316 unique=1 sector3=8 sector30=5308 selftest=pass` after the reverse-incidence fix.

The built-in selftest covers missing optional topology while preserving the previously admitted geometry, valid multi-root ownership, malformed portal endpoints while geometry remains admitted, an extra nonincident sector reference, unordered root bounds while geometry remains admitted, and a linked-root cycle while geometry remains admitted. The real fixture has 32 sectors, 128 portals, and exactly one owner for every captured Flora ID (8 in sector 3 and 5,308 in sector 30).

This is metadata parsing only. It creates no `CPortal`, `CDB::MODEL`, `CSector`, frustum, or renderer binding; the separate C++ integration outline remains unimplemented pending review.
