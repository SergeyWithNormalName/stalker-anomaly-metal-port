# E06 6F — dynamic SKIN_0 interpolant validation

## Result

The validation reports are real, but this run does **not** identify their offending vertex or prove their root cause. The strongest concrete input-side defect candidate is the 1W CPU repack path: it copies raw UV floats into the SKIN_0 vertex stream without checking finiteness. Do not label this finding pre-existing; no baseline run with the same validation conditions was available. No source code was changed for this diagnosis.

## Evidence

- `scratch/release_runs/20260927-e06-S3-validation/engine.log` contains 38 reports, first at line 15521 and last at 16104. Each names vertex function `main`, pipeline `R4 dynamic OGF deffer_model_bump SKIN_0 MSC G-buffer`, and encoder `R4 engine scene G-buffer`; the log provides no asset, vertex index, or per-report frame.
- Chronology in that same log: `METAL_ENGINE_FIRST_FRAME camera=engine` at 13757; environment-ready frame 27 at 13792; first sampled dynamic telemetry frame 27 at 14756 (331 skinned draws, 81 bump, 114 flat). These markers do not establish that dynamic draws were absent before frame 27, nor do they map the later validation reports to a simulation frame. Bloom's frame-0 line is unrelated evidence.
- The later `METAL_E03_CAPTURE_START_ERROR reason=Capturing Metal Shader Validation is not supported.` at line 16125 is a distinct capture failure. It does not explain away the preceding 38 shader-validation reports.
- The committed-to-build MSC receipt at `build/metal-runtime-shaders/dynamic-world-e03/dynamic-world-msc-receipt.txt` records `model_skin_define=SKIN_0=1` and the `deffer_model_bump.vs` metallib SHA-256 `bac1c8c5550fc6894ad6b89c27c5dbc72d2e804789c8bd82f0aa7e4df4c14b73`. Its reflection JSON (`deffer_model_bump.vs.json`) declares input `texcoord0` as Float2 and output `texcoord0` as Float2.
- In the receipt's source tree, `build/metal-shader-sources/r3/skin.h:85-102` implements `skinning_0()` with `o.tc = v.tc`; `deffer_model_bump.vs:10-26` then assigns `float2 tc = I.tc` and `O.tcdh = float4(tc.xyyy)`. There is no UV arithmetic that could itself create NaN or Inf. `tcdh` is the `TEXCOORD0` varying. Projection separately sets `O.hpos = mul(m_WVP, w_pos)` at line 16; malformed clip coordinates could still invalidate raster interpolation, but no clip-space values were captured.
- Windows' `src/xrEngine/bone.h:78-85` defines `vertBoned1W` as position, normal, tangent, binormal, float `u,v`, then bone index: 60 bytes, UV at byte 48. `MetalR4DynamicWorldAdapter.cpp:744-824` validates payload shape, indices, bone, and positions, then copies bytes 48–55 directly to `destination.texcoord` at line 821. By comparison, multi-weight repacking checks both UV components with `std::isfinite` at lines 965–977. This proves an unchecked pass-through for 1W inputs, not that one of the 38 draws used a nonfinite UV.
- The Metal PSO binds `texcoord` as `Float2` at offset 28, stride 36 (`MetalR4DynamicWorldAdapter.cpp:1267-1279`; layout in `MetalR4DynamicWorldAdapter.h:45-56`), matching the compiled reflection. This makes an ABI offset/format mismatch less likely; it does not validate values.
- A separate camera hypothesis remains unproven. `SetCacheXform()` copies view/projection and marks `viewState.valid=true` without finite checks (`MetalRenderDeviceRender.mm:1203-1220`); scene propagation copies these matrices when the bool is true (`MetalR4EngineScene.mm:2113-2128`), and dynamic Prewarm/Draw guard only on that bool (`MetalR4DynamicWorldAdapter.cpp:1425-1430, 1837-1843`). No matrix or clip.w dump exists in this run. Environment readiness is not the dynamic-world validity gate.

## Next discriminating check

On a validation rerun, instrument or reject the exact 1W input at repack time when either float at offset 48/52 is nonfinite, and report the renderable key plus vertex index; this would confirm or eliminate the proven UV bypass without changing the shader contract. If all submitted UVs are finite, capture the offending vertex's `O.hpos`/clip.w and view-projection values to test invalid perspective interpolation. Keep the two checks separate. The existing probe's finite fixtures do not exercise a negative NaN-UV input, so it currently cannot close this finding.

## Current E06 verification boundary

Input guard now rejects nonfinite raw1W UV with vertex index; negative NaN self-test passes. Final luma/rain diagnostic exits0 under API/shader validation with zero reports. The earlier38 interpolant reports remain real historical evidence; these different captures do not prove their cause was repaired by the input guard.

## Packet 5 follow-up — emissive subset and validation boundary

The packet 5 `p5-final` production run completed with exit 0 and telemetry status
OK. Emitter telemetry was positive for the converted RGBA16F `models\\selflightl`
world subset with `SKIN_0`, `NONE`, and `SKIN_4`; however, the paired frame-40
sun/emissive PPMs were byte-identical, so visible pixel contribution is not
proven. Original `models\\selflight` and static emitters remain open. Strict
HUD and hardware `SKIN_4` PSOs are admitted, but positive actual-geometry
fixtures have not exercised those paths. Emitter/material and weather gaps remain
open, as do Windows reference and human acceptance.

This later run does not identify the source of the historical 38 interpolant
reports or prove the UV guard repaired their cause. The earlier NaN diagnosis
remains open, and colored artifacts have not been resolved by this packet.

## Packet 5 producer probe and final S3 boundary — 2026-09-27

A focused converted-producer probe found a real `NONE` path issue: the baseline
RGBA8 output maxed at 0.047363. The dedicated `Float2` UV stream fixes it; the
final probe reports 1,352 contributing pixels with max 1.0 for both `NONE` and
`SKIN_0`, and rejected-family count 0. This is synthetic-input coverage through
the actual converted producer, not proof of native-world visibility. The
frame-40 native sun/emissive PPMs remain byte-identical, so world-visible delta
is still unproven. HUD `SKIN_4` also lacks a positive actual-geometry fixture.

The final 1920×1080 S3 run (`p5-s3`, binary `c060de0…`) completed exit 0 with
scenario status OK, no failures, and 20.066 seconds elapsed. It used the same
source save as the historical 38-report capture (`24d870…`). All 179/179
validation reports passed across 220 texture reports with zero nonfinite
channels. The 3,095 actual dynamic-draw CPU diagnostics show finite UV, position,
and clip values, with zero near-zero clip-W cases; there were no new GPU shader
NaN reports. The historical cause remains open despite not reproducing on the
same save. Colored forms were not seen, but their first bad pass was not
identified, so that issue also remains open.
