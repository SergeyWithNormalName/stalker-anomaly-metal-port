# Optional R4 level portal metadata draft

- `prestate-R4LevelGeometry.h/.cpp` and `prestate-hashes.json` pin the exact dirty production parser files before this scratch draft.
- `draft/src/Layers/xrRenderMetal/R4LevelGeometry.h/.cpp` contain the bounded optional metadata extension.
- `metadata.patch` is the focused patch relative to the pinned source paths.
- `metadata_probe.cpp` is a portable standalone parser/fixture probe; `metadata_probe` is its scratch-only executable.
- `VALIDATION.md` records input receipts and results.

The copied prestate portable self-test had a tree-definition fixture of 96 bytes; the parser reads a 64-byte matrix followed by two 20-byte lighting color values, so the scratch fixture is repaired to 104 bytes. This only repairs test data and does not change parser behavior.

Portal metadata validation covers finite ordered vertices and topology references, but it does not yet prove the polygon is non-collinear/simple or that `CPortal::Setup` can safely build its fan planes and average normal. Treat portal polygons as unqualified until a later consumer performs source-compatible prevalidation; any failure must keep topology unavailable and rendering fail-open.

No gameplay, renderer, build-system, cook, or production file is included in the draft outside the two parser files; nothing in this folder is installed or wired into runtime code.
