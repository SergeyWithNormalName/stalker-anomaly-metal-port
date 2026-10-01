# Scratch Flora rest metadata adapter

This is an unbuilt scratch-only copy based on `runtime-root-stage-04`. It adds value-owned rest records during the existing `Prepare` loop, while the parser bytes, repacked vertices, and local 16-bit indices are still available. It does not modify production sources, create a culling decision, change any draw/shadow list, or alter shader/resource bindings.

`RestRecordsForValidation()` returns one record per successfully admitted Flora draw, in exactly the adapter's main source order. Scene code can join by `visualId`. `known` and `eligible` are set only after the model, indexed range, unique index references, decoded S4 V coordinate, and all `RestAccumulator` inputs/extrema pass. Any false bit or non-`ok_indexed_rest_bounds` status means keep-visible.

The helper copies the raw OGF sphere into `sourceSphere` only as provenance. Its transform convention is not assumed. Instead, the parsed indexed-position AABB midpoint is transformed by the exact prepared draw model to select a world-space center; the accumulator starts with radius zero and expands over every unique indexed vertex. This gives the envelope helper a center/radius enclosing those indexed rest positions without relying on OGF sphere transform semantics. The `RestAccumulator` adds its conservative float-transform margin. The result's `minH`, `maxH`, and `maxAbsOneMinusUvY` come from those same indexed vertices, with signed S4 V decoded as `texcoord[1] / 1024`.

This structural metadata is not a GPU/wind enclosure proof and is not permission to skip draws. A later oracle must still use a frame-matched valid motion snapshot and every owning portal frustum; missing owners, frusta, motion, or any unknown record must remain visible. No compiled or runtime validation was performed in this stage by design.

## Files and hashes

The adapter copies are from the immutable runtime-root-stage-04 files. The two helper headers are copied unchanged from the existing wind prototype.

- `MetalR4FloraAdapter.h`: `116b00c0ebfce567ec3684e6a5829c2ed8b804c49129c1af7f9a5ba186bf6122`
- `MetalR4FloraAdapter.cpp`: `6d8a17002b9f2f37b6d991cf989e12c7f7321df36d76ad92d5dc0a4104aebedf`
- `include/RestAccumulator.h`: `560e472a1e89e587ead3c783d0ad2da2e0c75d9809bffa77722d8bcb7f9da127`
- `include/FloraWindEnvelope.h`: `8a282c96f188e51e32e2d6624c6c5395cd4f752a77430ab4520f21d22c778d26`
- `adapter.patch` is the diff from runtime-root-stage-04 for the two adapter files only.

The source base hashes are `224fa9b1d745250d71cfdae76e98ba8bd4326869ff5664dad4825e57ec67b6cf` (header) and `6a78fa6752b6431896ce5550155c82648fe066324601009dcb33adc2f548f244` (implementation).
