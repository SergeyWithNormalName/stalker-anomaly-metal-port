# Scratch Flora rest metadata adapter v2

This immutable v2 package is a copy of `rest-adapter-draft` with two corrections: the helper now follows the `Matrix4` declaration, and accumulation is strictly opt-in through `XRAY_METAL_FLORA_REST_METADATA=1`. With the variable absent, Prepare does not run the rest accumulation loop, no metadata records are stored, and `RestRecordsForValidation()` returns a shared empty vector. No production source is changed.

The API is declared in `MetalR4FloraAdapter.h`. It returns value-owned records in successfully admitted Flora draw order, joinable by `visualId`; `known` and `eligible` are both true only when every unique indexed vertex passed validation and the conservative accumulator returned complete extrema. Any false bit or non-`ok_indexed_rest_bounds` status means keep-visible. There is no filtering/draw/shadow behavior in this adapter draft.

Each record retains the exact prepared model matrix and the source OGF sphere as raw provenance. The OGF sphere is not used as a transformed bound because its local/world convention has not been established here. The metadata instead uses the indexed-position AABB midpoint parsed by `R4LevelGeometry` as a local center, transforms that center by the prepared model, and expands a zero-radius sphere across all unique referenced vertices. `R4LevelGeometry` computes its `referencedPositionBounds` by traversing the visual's indexed local positions. This makes the new center/radius choice independent of the serialized OGF sphere convention.

S4 V decoding is `signed int16 / 1024`: `RepackR4FloraStaticVertices` sign-extends the four S4 values into `texcoord[0..3]`, and the actual adapter binds `consts.y = 1/1024`; the source tree shader computes `tc = (I.tc * consts).xy` and uses `tc.y` / `1-tc.y` for wind and bending. The accumulator consumes that decoded V and the prepared draw matrix.

Do not reuse the previous `RestAccumulator`/wind prototype counts (including 4077/350/26) with these records: this adapter deliberately selects an AABB-derived center rather than the earlier fixture's OGF sphere center, changing the bounds input. Any portal classification must be recomputed from these exact values and frozen frame motion. This remains structural rest metadata, not GPU wind enclosure, visual correctness, or culling authorization.

No build, game launch, or runtime validation was performed in this bounded stage.
