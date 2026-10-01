# Local-light MSC bind reuse: independent source review

Scope: read-only review of Sol's frozen patches `local-light-bind-reuse.patch` (SHA-256 `28fb7a09c5669e0f489606b15da5d8ab8bfcf1d83f0e572267a7a279a4b2f308`) and `local-light-bind-reuse-oracle.patch` (SHA-256 `b556aac070c995f599666121290e6e0d10a61fdcff62339968b6ef497669f550`). No patch application, build, runtime, or production edit performed.

## Implementation review: PASS, bounded to one light triplet

- In `src/Layers/xrRenderMetal/MetalR4LocalLightAdapter.cpp:645-676`, first draw binds VS+mask PS, second may reuse both, final may reuse VS but binds accumulation PS. Every per-light upload (`:551-578`) is new, so no cross-light cache is valid.
- Pipeline bind changes PSO/depth/cull/winding and retains the PSO only (`MetalR4Backend.mm:2788-2809`); it does not write the stage TLAB slots. `BindMscResources` writes vertex or fragment buffer separately at MSC root index 2 (`:3067-3072`). Vertex fetch remains independent at slot 6, configured in the local-light pipeline (`MetalR4LocalLightAdapter.cpp:341-345`) and rebound every draw.
- Mask-front/back use the same unshadowed PS library for each shader kind (`MetalR4LocalLightAdapter.cpp:335-342,381-383`). Shadowed accumulation uses a distinct PS library (`:337-339,387-389`), so preserving its final PS bind is required and the patch does so.
- Marker reference is fixed-function encoder state, set on each draw (`:648-650`); rollover reset follows accumulation and remains in the same encoder (`:678-695`). Reuse does not span encoders: scene accumulation begins/ends a render pass for each unshadowed/shadow batch (`MetalR4EngineScene.mm:5145-5164`), and backend encoder creation clears indirect-read declarations and resets MSC pipeline state (`MetalR4Backend.mm:1721-1743`).
- Backend bind calls also validate descriptors, declare indirect reads, retain resource owners, and allocate TLAB (`MetalR4Backend.mm:2882-3072`). The first same-encoder bind already covers each reused resource; reuse must remain local to the triplet and frame. Acquisition retry exits before `beginAcquiredContext` (`:1397-1437`), while every ready acquisition resets arena/TLAB state (`:1361-1394`).
- This covers Point, Spot, and OmniFace volume accumulation (distinct `volumeKind` and slot-6 buffers at `MetalR4LocalLightAdapter.cpp:495-499,642-644`). `MetalR4VolumetricAdapter::DrawSpot` is a separate path (`MetalR4EngineScene.mm:5187-5201`) and is outside this flag.

## Oracle review: comparison structure is sound; acceptance gaps remain

- The compare path constructs separate baseline and reuse adapters, runs two poses and four bulk/reference/prefix/tail modes, and compares raw RGBA16F bits plus copied depth/stencil bytes (`local-light-bind-reuse-oracle.patch`, hunks around `Captured`, readback, and pair checks). It keeps prefix/tail contribution checks from the original probe. Snapshot and synthetic shadow-record frame IDs are both 501, so stale-record validation is satisfied.
- The retry prelude discards an encoded reuse frame, forces the production acquire Retry gate, then proceeds. The compare loop submits 16 frames and reacquires between them; source backend has triple frame slots. This is a credible discard/retry/slot-wrap exercise, subject to runtime proof.
- Added bind/reuse counters match the intended per-submitted-light accounting: off = 3 VS + 3 PS calls; on = 1 VS + 2 PS calls, with 2 VS and 1 mask-PS reuse. Count checks are per capture and include the chunked reference aggregation.
- **Blocker for claiming the requested shadow-sampling positive oracle:** shadow fixture uses one constant `.95` depth texture plus identity matrices, and only checks nonzero accumulation and off/on equality. It does not compare a known-lit and known-occluded atlas or otherwise prove that the shadow PS actually sampled/used the atlas. Add an atlas-perturbation signal (lit vs blocked, same PS/resources) or label this as shadowed-PSO bind coverage only.
- **Type coverage must be explicit:** source supports `family=point|spot|omni`, but the default `mixed` invocation's prefix/tail witness proves positive contribution only for the tail record's family. Run and retain per-family compare reports, or add per-family positive pixel gates in the oracle.
- There is no deliberately dropped-stage-binding negative control and no backend hook to drop one; state this limitation. Equality plus bind counters is useful but does not demonstrate that the pixel witness fails if reuse accidentally omits the necessary first bind.
- Small harness hygiene issue: non-compare `--gpu` does not force `XRAY_METAL_LOCAL_LIGHT_BIND_REUSE=0`; an inherited `=1` can make the legacy run silently use the candidate path. Set it to `0` before baseline adapter preparation for both modes.
- The 933 figure is only 3 fewer host `BindMscResources` calls per actually submitted light (e.g. 311 submitted lights); no frame-time or FPS claim follows from this count.

## Disposition

Source-level reuse change: **PASS** for the exact per-light sequence described above. Oracle exact-output/readback and lifecycle structure: **PASS by inspection**, unexecuted. For the requested comprehensive positive oracle, close the shadow-sampling and per-family witness gaps and force baseline env off. Keep `XRAY_METAL_LOCAL_LIGHT_BIND_REUSE` default-off pending actual-PSO GPU evidence.
