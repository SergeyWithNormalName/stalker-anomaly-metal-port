# Fixed-ID leaf LQ ROI draft

This is a review artifact only. `MetalR4FloraAdapter.base.cpp` is a byte-for-
byte copy of the production adapter at the start of this draft:

```text
3cd57e6f84069698619358337d660bd6e159357ed455c3769ee5b584c47d58dc
```

`MetalR4FloraAdapter.cpp` is the candidate copy; `leaf-lq-fixed-id-roi.patch`
is its unified diff against the preserved copy. Nothing in `src/`, `build/`,
the runtime, or the game was modified, and this candidate was not built or
run.

The candidate is inactive unless `XRAY_METAL_FLORA_LEAF_LQ_ROI=1`. It reads
the LQ metallibs directly from the private `XRAY_METAL_MSC_FLORA_LQ_DIR` and
reads the fixed allowlist from `XRAY_METAL_FLORA_LEAF_LQ_ID_FILE`. The fixture
format is deliberately exact and has 1,856 leaf visual IDs:

```text
schema,main-flora-leaf-lq-fixed-id/v1
visual_id_count,1856
view_bits,<16 float32 hex words>
projection_bits,<16 float32 hex words>
eye_bits,<3 float32 hex words>
visual_id,material
<decimal visual ID>,leaf
... exactly 1856 unique rows ...
```

Every listed ID must be present as an admitted leaf in the current Flora
inventory. A missing environment value, malformed fixture, changed count,
unknown ID/material, missing metallib/reflection, reflection mismatch, or
pipeline creation failure disables the optional path and leaves all foliage
on its current HQ pipeline. At draw time the exact view, projection, and eye
float bits must also match, and the adapter must have valid wind state;
otherwise the whole frame uses HQ. Only IDs in
that fixture can select LQ, only in the main G-buffer draw. The ordinary
source order, main visual set, draw count, wind texture, and every shadow
draw remain as before.

The LQ reflection gate is pinned to the cooked ABI: VS TLAB 48 bytes with
CBVs at offsets 0/24 (ResourceCount 2, the same five vertex inputs), and PS
TLAB 120 bytes with SRVs at 0/24, CBVs at 48/72, and sampler at 96
(ResourceCount 5, four G-buffer targets, alpha discard). It relies on the
private cook's reflection receipt; no compiler is run by this patch.

The vertex upload uses the exact LQ DXIL assembly layout: `$Globals` is 464
bytes, `m_xform`/`m_xform_v` are at 160/224, tree constants at 288-368, and
SSFX wind parameters/setups/current/previous animation are at 384/400/416/
432/448. The second VS buffer is the existing 528-byte `static_globals`.
Unlike the opaque flat-trunk path, the LQ tree's fields after byte 384 have
different meanings, so this candidate writes the LQ offsets directly and
does not extend the upload to the trunk path's 496-byte buffer. The alpha LQ
PS uses the existing 176-byte fragment globals and 256-byte dynamic
transforms, with only base and bump textures bound at its reflected SRV
slots.

The activation line reports the number of actual LQ draws once, after the
first exact camera match. LQ intentionally changes distant leaf shading,
parallax, foliage variation, and interactive branch bending; the ROI has no
pixel-equality expectation. Its measurement must keep the observer's main
draw IDs and count, view bits, current/previous wind fields, sector/frustum
capture, and live parallax range pinned. A rendered visual review is still
needed before treating this as a player-facing distance policy.

The draft fail-opens during optional bundle and fixture admission. As with
the current adapter, unexpected backend failures during an already admitted
draw still return the draw error; automatic mid-draw reconstruction of an HQ
branch upload is outside this minimal ROI patch.
