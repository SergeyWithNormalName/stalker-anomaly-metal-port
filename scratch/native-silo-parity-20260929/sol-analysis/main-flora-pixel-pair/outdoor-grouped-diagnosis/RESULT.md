# Outdoor main-Flora grouped-order screen

The frame-700 grouped-order trial is rejected by the strict pixel gate. It kept all 5,308 submitted legacy draws and reported 487 candidate pre-clip exact-key groups; it did not instance or merge draws. The candidate and source both submitted 5,308 visuals. The Flora position attachment showed a meaningful 1,030,088-pixel delta, so the outdoor camera exercised a substantial Flora footprint.

MRT0, MRT2, MRT3, depth, and stencil matched exactly. MRT1 failed at 35 pixels / 56 bytes. Comparing its P6 PPMs shows 35 RGB pixels changed across 56 channel values. The largest absolute RGB deltas are R=4, G=4, B=3 in 8-bit PPM values; 25 changed pixels have a maximum channel delta of 1, five have 2, four have 3, and one has 4. The changed pixels span x=109..1716 and y=548..1073. This is sparse, low-amplitude RGB disagreement, not an alpha-only mismatch. PPM omits alpha, so this review does not establish raw alpha magnitude or identity.

The pixel pair rejects this grouped ordering under the current strict MRT gate. The attachment is not an order-independent proof: it does not identify a unique cause for the disagreement. Equal depth/stencil and sparse low-amplitude MRT1 changes do not prove equal-depth winner ordering or establish a safe tolerance. Preserve source order pending a better qualified candidate and oracle.

As a conservative key screen, the existing frame-700 source/material/base-texture census records 384 groups across 5,316 draws. Splitting every group by its recorded authored tree-lighting payload yields 5,316 payload variants; all 384 groups have as many payload variants as draws. That census therefore offers no theoretical submit reduction when this payload is part of the key. It is not an exact join to the outdoor trial, which submitted 5,308 draws and has no per-ID CSV in this run; no trial-specific bound is claimed.

The trial JSON, MRT1 comparison JSON, two PPMs, and group-census inputs are pinned by SHA-256 in `NUMERIC_RECEIPT.json`. No code, build, or game changes were made for this diagnosis.
