# E06 packet 5 — controls, diagnostics and production coverage

2026-09-27. Scoped continuation of the existing PARITY_MATRIX; no new census.
Windows image reference and human acceptance remain OPEN.

## AO/IL control contract

The engine captures source shader presence independently for AO and IL. Each
branch runs its trace and four blur draws only when that source is present and
its own intensity is positive. Disabled output is `(0,0,0,1)`: AO consumers read
alpha 1; IL consumers read RGB 0. This reproduces the Windows neutral clears,
including `combine_1`'s multiplicative IL expression when IL intensity is zero.

History ownership is separate per branch. Disabling/re-enabling or changing
scale invalidates only that branch; camera cut, target recreation/reload and
invalid accepted history invalidate both. A cold branch samples a zero previous
position texture, preventing the still-valid other branch's position history
from blending the cleared effect history. Metadata commits only after submission;
a discarded encode does not advance branch state or the submitted frame.

`XRay.Metal.AoIlControlsGpuProbe` runs the actual converted shaders through the
production adapter. Its inputs are a controlled synthetic packed G-buffer, so it
is distinct from the native gameplay check. Final API/shader-validation run:
18 states, 34 readbacks, all PASS, zero nonfinite output channels. It covers
independent intensity and presence gates, re-enable, independent scales,
discard + next submitted frame, cut, resize and explicit invalidation.
Evidence: `scratch/release_runs/20260927-packet5/ao-controls-final-gpu*`.

Production diagnostics repeat the actual converted `combine_1` on the same
G-buffer, AO, accumulator and constants with an explicit black IL input. Both
low/high targets must match exactly. A stale-white IL sensitivity control must
differ, proving that the comparison detects the original darkening failure.
Readbacks are forced on control changes as well as sampled frames and cuts.

The completed `p5-final` production route exits 0; telemetry reports `OK` and
no scenario failures, with observed camera turn/pan/cut/return. Binary SHA-256:
`053b1a0dbea255d06e3ef33cfef262d022a37c9f032199def0c3b300fee7fcaf`.
724/724 reports PASS, 839 texture readbacks and 3,043,123,200 finite RGBA channels,
zero nonfinite channels. At both IL-off frames 89 and 102 the low/high black
comparisons match exactly; the stale-white controls differ by 909,623/625,551
and 909,892/630,920 pixels respectively. Enabled branches reuse history;
re-enabled or rescaled branches go cold independently. The source-presence and
submission-discard cases also have the separate converted GPU-probe witness.
This run predates the final OmniFace value fix and reduced-SSR blur correction;
those need the subsequent lifecycle run below. Evidence summary:
`scratch/release_runs/20260927-packet5/final-evidence-summary.{md,json}`.

## Color artifacts and historical interpolant reports

`XRAY_METAL_COLOR_PROBE_DIR` records finite statistics and diagnostic PPMs for
production G-buffer, sun/emissive/local accumulator, each AO/IL trace and blur,
each general SSR pass, volume output/composite, base combine, bloom and final
scene input. HDR previews are tone mapped for inspection; JSON metrics are read
from the original attachment values. This preserves pass ordering and makes the
first damaged attachment observable.

`XRAY_METAL_DYNAMIC_DRAW_DIAGNOSTICS=1` restarts the active G-buffer encoder with
Load/Store attachments before binding each dynamic draw. Labels include frame,
object, asset, geometry and shader. CPU diagnostics separately report finite
submitted UV/positions and current clip-W. A label change alone would attribute
all errors to an encoder's final label, so real draw splits are used.

The historical 38 SKIN_0 TEXCOORD0 NaN/Inf reports remain real evidence. The old
capture did not identify the asset, vertex or clip-W; the pre-existing raw-1W UV
guard rejects malformed UVs, but that does not establish the old cause. The
completed `p5-c25` route exited 0 with zero new validation reports and 79 finite
attachment reports; `p5-final` adds 724 finite reports and no new shader errors. The dense `p5-ctrl` attempt was interrupted during precache
and is not a completed route PASS. No first damaged color pass has been observed
in these captures; artifact cause and old-report attribution remain OPEN.

## Source modernization checks

The bounded cached-DXBC audit is reproducible with
`scratch/release_runs/20260927-packet5/audit-ao-selflight-cached-dxbc.py`.
The larger AO payload has 407 instructions and four `sample_l` fetches; the
smaller has 269 instructions and two. Neither contains `loop/endloop`.
This supports the intended unrolled sampling pattern of the `j++` AO cook,
without claiming that the legacy CRC identifies its original HLSL revision or
all compile defines. There is no evidence of an `i++` runtime loop in those
cached payloads.

General SSR quality 0 uses the original five-pass chain. Both blurs retain the source
reduced viewport and UV range; full viewport is restored only for scene combine. DXC compatibility folds
only `[unroll (q_ssr_steps[SSFX_SSR_QUALITY].x)]` to `[unroll (8)]`; all ray steps,
loops and arithmetic remain unchanged. The original/folded include SHA-256 are
pinned in `ssr-annotation-receipt.json`. The new reproducible
`script/compile_r4_general_ssr_msc.py` rejects unreviewed source and preserves
CRLF bytes. Its recook matches all five executing metallibs byte-for-byte.

## Continued production paths

- General SSR: distinct pre-SSR scene copy; gloss, trace/history, horizontal and
  vertical blur, original scene composite; accepted production motion vectors;
  submit-owned history/cut/resize/disable resets. Quality 0 is the admitted cook.
- Selflightl: original SKIN_0/NONE/SKIN_4 pairs, RGBA16F first accumulator
  contribution after sun/before local lights and separate BGRA8 bloom producer.
  HUD capture now carries per-geometry emissive selection and shader flags.
  Current Bar evidence exercises world draw admission; the exported sun/emissive
  previews do not resolve a pixel delta, so a positive contribution fixture
  remains required. HUD and other permutations also need positive fixtures. `models_selflight.vs` outputs only SV_Position while
  `accum_emissive.ps` consumes TEXCOORD0 in both source and cached DXBC. That
  pre-existing stage mismatch stays explicit/fail-closed; no UV is fabricated.
- Volumetric: original far-cascade cuboid with `accum_sun.vs`, source sun range,
  low-quality raymarch, source spot/OmniFace spheres/frustum planes/slices,
  per-batch atlas consumption before overwrite, four blurs/merge when spots are
  active, raw HDR sun output when they are not, original RGB-only dual-target
  composite and real rain volume input. Every frame-arena CBV binds its returned
  offset. Jitter uses the source packing/rejection algorithm with a deterministic
  seed; Windows random-stream/image equality is not asserted.
- Fog scattering: original quarter/half-resolution blur and full-size composite
  copied back to the actual scene-color format before bloom; independent source
  presence/intensity gate and captured live fog values.

The final volumetric CPU source-contract probe PASS also checks source parent
controls and reconstructed spheres/LOD for all six point faces, with no live
D3D light object. It checks noncommuting accepted
camera matrices, source transformed frustum planes, OmniFace bounds,
SSS falloff/color/LOD/attenuation, immutable gloss controls and mode-1 exclusion.
The current profile uses `/8`, sun quality 1 and normal local-volume mode 0.
Source quality 5 computes 120 local slices from a 100-slice mesh; that invalid
range is rejected explicitly. Nondefault `/1,/2,/4`, other sun-quality cooks and
`accum_volumetric_lv` are outside the verified profile.

## Remaining E06 scope

General emitter completeness, static level emitters, the selflight stage mismatch,
exact selected dynamic AREF/detail/steep permutations, missing bump resources,
DOF, rain wetness/occlusion, thunder and HUD particles are still matrix gaps.
Build/cook/finite readback evidence does not close Windows reference, performance
or visual acceptance. The current changes preserve the existing DXVK fallback
and unrelated working-tree work.

## Diagnostic fixture timeout

Shader validation of the new general SSR chain makes this diagnostic route
slower than the runner's previous fixed 360-second bound. `p5-life` was
interrupted after the lifecycle events (retry, reload, encoded discard,
second-view rejection, resize/restore) and is not a full route PASS. The runner
now accepts `XRAY_PARITY_TIMEOUT_SECONDS` in 1..1800, default 360; invalid
zero/non-numeric/1801 controls exit 2 before launch and `bash -n` passes. The
full repeat uses 900 seconds. The executing runner was not edited.

## Accepted lifecycle and complete volume route

`p5-accept` exits 0 on binary
`b0ddc94c7a5fc0a2753de4b13604e855023adb75bed9119954ad9b0ddd62953a`.
514/514 reports PASS, 659 texture readbacks, zero nonfinite channels. IL-off
low/high pairs are exact at frames 89/93, and stale positive controls differ by
909,366/612,459 and 895,795/519,905 pixels. Native telemetry proves source
parent reconstruction activates all 25 volumes (7 spot + 18 point faces), with
sun/4 blurs/merge/composite and finite HDR signal. All six point faces use
matching rendered depth records consumed before atlas overwrite.

The accepted lifecycle events are acquire retry 31, producer reload 36,
encoded discard 41, second-view rejection 46, resize 51 and restore 56.
SSR reuses normal submitted history (33–35, 37–40, 43 onward), cold-starts
at skip/reload/discard/cut/resize, then resumes reuse. The first implementation
reset SSR at every Render entry because the scene clears stale pending metadata;
`DiscardUnsubmittedFrame` now dirties history only when an SSR encode is pending.
Scale 2 executes at frames 98/99 with actual 640×360 trace/blur on 1280×720
inputs. Exact SSR pipeline labels were included in shader validation for this
run; the earlier `p5-final` accidentally selected nonexistent SSR labels and is
not SSR shader-validation evidence.

The subsequent footprint-reset refinement rejects history if its last submitted
SSR scale differs, rather than interpreting old image coordinates at the new
scale. The scale metadata commits only on submission. The concluding run below
must verify that refinement and the SKIN_NONE UV correction.

## Positive converted emissive producer proof

The initial SKIN_NONE PSO reused G-buffer Short2 UVs encoded as `raw * 1024`.
`deffer_model_flat.vs` with SKIN_NONE consumes FLOAT2 directly, without unpacking.
The bounded `XRay.Metal.EmissiveAccumulatorGpuProbe` uses actual production
Prewarm/DrawEmissiveAccumulator, a 2×2 authored green-corner DDS and constant UV
(.75,.75). Its baseline fails with `rgb_max=0.047363` (1352 written pixels),
while correct sampling requires >=0.90. A white texture would miss this defect.

The fixed producer uses a dedicated immutable Float2 UV stream populated from
original OGF floats; the G-buffer Short2 stream is retained. The converted
API/shader-validation run passes: SKIN_NONE and CPU-SKIN_0 each write 1352 pixels
with `rgb_max=1`; the unsupported-family control rejects one draw and writes zero
pixels. This proves the producer/bindings, not native emitter completeness or
HUD SKIN_4 coverage. Shader source/cook is unchanged. Evidence:
`emissive-before-uv-valid-dds*`, `emissive-after-uv*` under the packet5 root.
The standalone patch is retained as before-fix review evidence and was already
applied; it is not a patch to apply again to the final tree.

Final binary after Float2 UV and submit-owned SSR scale metadata:
`c060de0deb9439f3c974591c837ea830baab3dd4478bdbd9b4a95aecddbd4a03`.
Main + emissive probe builds PASS; final producer source hashes are recorded in
`final-source-manifest.json`. S3 uses the original historical save SHA
`24d870ac646aadd09dac577da726153225be35513daefae048b37709b3058d97`
and original physical 1920×1080 size, with 1→2→1 SSR controls and draw-specific
SKIN_0 shader validation. Final S3 results follow below.

## Final original-S3 regression result

`p5-s3` exits 0 on the final c060de… binary. The original save SHA and physical
1920×1080 size match the earlier 38-report fixture. Scenario status OK, no
failures, actual elapsed 20.066 seconds. 179/179 reports PASS, 220 texture
readbacks, zero nonfinite channels. 3095 actual draw diagnostic lines report
finite UV/position/clip coordinates and no near-zero clip-W. The exact original
SKIN_0 bump pipeline plus AO/IL/SSR/volume/fog pipeline labels were selected for
shader validation; no new GPU reports were found.

Both SSR footprint changes cold-start correctly. Frame 90 scale 2 uses
960×540 trace/blur on 1920×1080 inputs: trace has 414 nonzero RGB pixels, horizontal
blur 3205 and vertical blur 3436. Frame 91 restores scale 1, cold-starts again,
and trace has 868 nonzero pixels with nonempty blurs. All five pass readbacks
are finite. Gloss RGB is zero by shader design; its alpha carries the weight.
Evidence: `s3-evidence-summary.json`, `s3-readback/`, `p5-s3/engine.log`.

The final image was inspected and does not reproduce the reported bright
cyan/red forms. No first damaged pass/draw was found in Bar or original S3;
color-artifact cause stays OPEN. The earlier 38 reports remain real historical
evidence and their cause is unresolved; absence in the repaired renderer does
not attribute them to the UV guard, new emitter fix, matrices or ownership.
The emitter UV issue was newly introduced in the packet5 NONE path and has an
independent discriminating before/after GPU proof; it is not assigned as the
cause of the earlier user artifacts.
