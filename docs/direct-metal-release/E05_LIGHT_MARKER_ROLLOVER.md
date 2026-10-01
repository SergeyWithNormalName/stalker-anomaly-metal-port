# E05 A1 — non-MSAA local-light stencil rollover

Task: packet 1 of `E02_E06_AUDIT_2026-09-27.md`. The rest of E05 remains open.

## Implementation

Windows `CRenderTarget::increment_light_marker()` permits marker 255, then
increments to 257 and calls `reset_light_marker(true)`. The non-MSAA
`blender_light_occq.cpp` E[2] draw clears only `0xFE`, preserving geometry bit 0.

Metal now starts local lights at 7 (the sun consumed 5), advances by 2 only
after all three draws succeed, and executes the same masked fullscreen clear
immediately after incrementing past 255. After a successful reset the next
marker is 5. This also covers a final light at the boundary and repeated wraps.
Inactive, invalid, culled and missing-cookie records consume no marker.

The reset is inside the existing accumulator encoder, with no color/depth
writes. The scene loads/stores the existing accumulator and depth/stencil
attachments. An attachment-wide stencil clear would lose geometry; clearing
the accumulator would lose the sun and previous lights. Neither is used.
Reset bind/draw failure returns false and aborts frame encoding before marker
reuse. Reset pipeline preparation errors are included in adapter status.

`R4LightMarker.h` contains the marker policy and exact reset pipeline descriptor.
Local-light telemetry adds `marker_resets`. Opt-in
`XRAY_METAL_LOCAL_LIGHT_DIAGNOSTICS=1` reports every frame for motion analysis;
normal runs retain the 300-frame cadence. The standalone probe supplies an
explicit default cookie; normal engine preparation retains its XRFS provider.

## Evidence

Evidence root: `scratch/release_runs/20260927-light-marker-rollover/`.

Baseline GPU trace from the audit: local pass has 372 draws (124 records),
last accumulation uses stencil reference 253. Baseline motion run
`20260927-marker-before-motion2` exits 0, scenario S1 returns the Bar camera to
its starting pose. At frames 300/600/900 it admits 124 and rejects 466/472/466
otherwise supported records. The exact audited save is read from the old
isolated runtime (SHA256
`f78e11c4ac6d7b956102cb523bf5f34507ded87a4e73c6f20a28565391ca2003`).
The live save changed, so it was not substituted silently.

### Focused CPU/GPU gates

Main executable and the two focused probes build successfully; compiler
warnings remain. `XRay.Metal.LocalLightAdapter.SelfTest` passes 1/1. It covers
7..255, immediate reset to 5 at terminal 125, and repeated wraps through 400.

`XRay.Metal.LocalLightRolloverGpuProbe` executes the actual converted shaders
and adapter on a deliberately synthetic 16x8 packed G-buffer. Geometry occupies
half the target; background occupies the other half. Packed normal is -Z,
depth .5, material/hemi use the original packing helper's equivalent. Light
far caps fit the orthographic fixture clip range. The small camera turn and
translation are applied to the actual volume bindings. All runs use API/GPU
validation and produce no validation errors.

| Records | Tail type | Bulk resets | Bulk vs 124-record chunks | Minimum prior-light RGB signal across camera poses |
|---|---|---|---|---|
| 125 | spot | 1 | exact equality | 0.572754 |
| 126 | OmniFace | 1 | exact equality | 0.577148 |
| 127 | point | 1 | exact equality | 0.581543 |
| 200 | spot | 1 | exact equality | 0.902344 |
| 400 | point | 3 | exact equality | 1.687500 |

Every bulk run admits/submits the entire list in both camera poses. A prefix
without the last light retains a positive contribution from prior records;
`bulk - prefix` agrees with the isolated last light within half-float rounding
(maximum absolute difference 0.000854). Each tail has positive RGB contribution
in each camera pose. The pre-existing HDR seed never decreases, background
color remains exactly unchanged, and depth/geometry-bit witness failures are 0.
This is pixel readback proof using production MSC bindings, not just counters.
It does not claim direct readback of every upper stencil bit or full Windows
image parity. Raw reports and previews are in `gpu-125` through `gpu-400`.

The initially too-large synthetic cone clipped its far cap, so its tail had
zero output and the gate correctly failed; the fixture's volume range was
corrected to .6. Regular lights were increased above RGBA16F quantization,
and an explicit positive prefix gate prevents vacuous preservation claims.
No production light geometry, source shaders or colors were altered for this.
Standalone linking/logger setup retries are retained in the evidence logs.

### Bar motion run

`20260927-marker-after-motion`: exit 0, 1920x1080, API/GPU validation enabled.
103 local-light telemetry rows; supported world submissions range 232..1135,
marker resets up to 9, `unsupported_skipped=0` throughout. The S1 scenario
performs turns, strafe, forward/back motion, then restores the exact Bar base
camera; scenario status OK, failures empty. The 15-second video and screenshots
are retained. The new screenshot shows additional light on the right building
and doorway; this observation is not a Windows brightness verdict.
`motion-summary.json` records counts and camera evidence.

### New Bar GPU trace

`20260927-marker-after-trace`: exit 0, capture STARTED/STOPPED recorded,
4.05 GiB trace. The captured frame submits 590 lights, including 16 shadowed
records, with 4 marker resets and no unsupported rejection. Local encoder
`re24` contains **1774 draws = 590 x 3 + 4 resets**; the old audit encoder had
372 draws.

GPU draw 374 accumulates with stencil reference 255. Draw 375 executes native
`r4_marker_reset_vertex/fragment`, 3 indices, color writes disabled, depth
Always/no write, stencil Always/Zero. Draw 378 resumes MSC accumulation with
reference 5. Reset draws repeat at 754, 1133 and 1512; the final accumulation
is draw 1773 with marker 177. These are inspected GPU states, not CPU admission
counts. The exported 1920x1080 accumulator PNGs immediately before/after the
first reset are byte-identical. PNG exports quantize HDR; raw half-float color
preservation is covered independently by the focused GPU tests above.

`trace-summary.json`, `after-gpu-encoders.log`, `after-rollover-draws.log`,
`reset-pipeline.log`, `reset-depth-stencil.log`, `repeated-rollovers.log` and
`reset-pixel-comparison.json` retain the evidence. The final accumulator and
scene screenshot are available; this remains a Metal mechanism acceptance,
not full Windows image parity.

### Reproduction

```sh
ninja -C build -j 4 Anomaly.DX11 XRay.Metal.LocalLightAdapterProbe XRay.Metal.LocalLightRolloverGpuProbe
ctest --test-dir build/src/Layers/xrRenderMetal -R '^XRay.Metal.LocalLightAdapter.SelfTest$' --output-on-failure
MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1 MTL_SHADER_VALIDATION_REPORT_TO_STDERR=1 \
  build/src/Layers/xrRenderMetal/XRay.Metal.LocalLightRolloverGpuProbe \
  --gpu "$PWD/build/metal-runtime-shaders/local-lights" 400 /tmp/r4-rollover-400
```

Run the GPU command with 125, 126, 127 and 200 as well. The probe links canonical
`du_sphere_part.cpp` data; fail-closed test UI/FS functions are never used by the
light fixture, which explicitly owns its default spot cookie. They are not
linked into the engine. The fake allocator used by the existing CPU probe now
also implements `mem_alloc`, required by the new descriptor checks.

### Remaining scope

Only A1 is addressed here. The 16-tile local shadow capacity and shadow fallback,
missing casters/cascades, producer motion vectors, IL controls, colored artifacts
and full Windows source/image parity remain the separate audit packets.

