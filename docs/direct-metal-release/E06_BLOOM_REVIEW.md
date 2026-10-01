# E06 Bloom independent review

Date: 2026-09-27. Scope: read-only audit of the new MetalR4BloomAdapter, its bloom-e06 DXC/MSC artifacts, and the authored Windows R4 SSFX bloom path. No build, cook, game, source edit, or runtime visual claim was made in this review.

## Findings

- The core blur/build/downsample/upsample/combine pass chain matches authored Windows topology and formats. No confirmed pixel-path defect was found in these passes from source/reflection inspection.
- Emissive and HUD-mask parity remain **OPEN**. The central scene passes scene.lightingBlack for both inputs (MetalR4EngineScene.mm:3704-3708); bloom-from-scene works, but authored emissive contribution and HUD exclusion cannot be accepted.
- Runtime bundle admission validates reflection layout, stage, render-target count, and artifact presence. It only checks that bloom-msc-receipt.json exists; it does not parse/compare receipt scope, input/output hashes, compiler versions, or defines. The cook receipt contains those fields, but runtime admission alone does not establish their correspondence to the admitted files.

## Windows and Metal pass contract

Windows phase_ssfx_bloom (r4_rendertarget_phase_bloom.cpp:383-547) sets a half-resolution viewport for bloom build, uses quarter resolution for lens generation, then runs six downsample targets and five upsample targets. r4_rendertarget.cpp:614-630 allocates bloom1 at /2 RGBA16F, lens intermediates at /4 RGBA16F, lens output at /4 RGBA8, and pyramid targets /2,/4,/8,/16,/32,/64 plus upsample targets /32,/16,/8,/4,/2, all RGBA16F.

Metal target dimensions match: blurH and blurV /2 RGBA8; build and final /2 RGBA16F; six down textures /2 through /64; five up textures /32 through /2; lens build/scratch /4 RGBA16F and lens /4 RGBA8. Output() is a separate /2 RGBA16F texture consumed by presentation; that target substitution preserves the authored output extent/format.

Pass inputs match the blenders and HLSL:
- H/V pp_blur: s_position, full scene color / previous blur output; /2 RGBA8 targets. Windows generic blur binds position and generic scene for H, then rt_blur_h_2 for V (blender_blur.cpp); the adapter follows the same order.
- ssfx_bloom.ps: position, scene=rt_blur_2, emissive, HUD mask; CB0 ssfx_bloom_1; point sampler then linear sampler.
- Downsample: one source, CB0 blur_setup, linear sampler. Down[0] intentionally filters /2 to /2, followed by /2→/4→/8→/16→/32→/64.
- Upsample E[0] reads /64 twice, matching CBlender_ssfx_bloom_upsample::E[0]; E[1..4] read prior upsample plus /32,/16,/8,/4, producing /32,/16,/8,/4,/2.
- Combine: four textures (up[4], starburst, lens, dirt), CB0 with bloom1/bloom2/mask_control, CB1 static globals, two samplers. Lens-flares shader binds emissive + lens-colors and CB0 static globals with two samplers. Lens output uses /4 RGBA8 PSO; the preceding lens scratch pass uses the ordinary /4 RGBA16F upsample PSO.

## Static screen resolution and quad mapping

DXC dump places screen_res at byte offset 416 in the 528-byte static_globals block. Windows cl_screen_res binds (Device.dwWidth, Device.dwHeight, 1/w, 1/h) (Blender_Recorder_StandartBinding.cpp:610-617); it is not changed by u_setrt, which updates only render-target dimensions (r4_rendertarget.cpp:46-85). Viewport changes use set_viewport_size separately.

Windows bloom uses full source-sized quad positions with /2,/4,/N viewports, keeping full-resolution screen_res. Metal uses the same full-source-sized quad and an explicit viewport equal to each destination extent, and binds scene.baseLightingStaticGlobals with the full scene resolution. This preserves I.tc0 over the destination extent and the HLSL blur texel offsets in full-input pixels. Generic Windows /2 blur uses half-source quad positions under the inherited full viewport; because the full-resolution screen constant remains in effect, that maps the same full UV range over the /2 target as the Metal full quad with /2 viewport. Active pp_blur.ps takes SSFX_Blur(I.tc0, 0.25); SSFX_Blur uses screen_res.xy/its reciprocal for noise and sample offsets. No half-target screen_res update is present or expected.

## DXC / MSC admission comparison

The bloom-e06 receipt reports scope exact_active_ssfx_bloom_non_msaa, source root build/metal-shader-sources/r3, and MSC 4.0.1. DXC dumps and MSC reflection agree on the host binding map:

| Shader | Reflected TLAB sequence | Host path |
|---|---|---|
| stub_screen_space.vs | CBV0 | 528-byte static globals |
| pp_blur.ps | SRV0, SRV1, CBV0, CBV1, Sampler0 | position, image; uploaded globals, 528-byte static globals; linear sampler |
| ssfx_bloom.ps | SRV0-3, CBV0, Sampler0-1 | position/scene/emissive/HUD; uploaded bloom globals; point then linear |
| ssfx_bloom_downsample.ps | SRV0, CBV0, Sampler0 | source, uploaded blur setup, linear |
| ssfx_bloom_upsample.ps | SRV0-1, CBV0, Sampler0 | two sources, uploaded blur setup, linear |
| ssfx_bloom_combine.ps | SRV0-3, CBV0-1, Sampler0-1 | four sources, uploaded combine globals, static globals, linear then linear |
| ssfx_bloom_flares.ps | SRV0-1, CBV0, Sampler0-1 | emissive/lens-colors, static globals, linear then linear |

Contracts[] in MetalR4BloomAdapter.cpp yields those exact reflection entries and the render code uploads separate immutable constant slices for each pass. ssfx_bloom.ps DXC layout has ssfx_bloom_1 at byte 256; combine has ssfx_bloom_1, ssfx_bloom_2, mask_control at 256/272/288; down/up use blur_setup at 256; pp_blur has a 304-byte Globals block and static globals 528 bytes. Reflection entries all use 24-byte elements in space 0, and fragment stages report one render target.

## Evidence boundary

This is static source, DXC dump, receipt, and MSC-reflection review. It does not establish runtime admission or pixels, and it does not close Windows reference or human visual review. The black emissive and HUD-mask inputs keep those two parity items explicitly open.

## Integrator follow-up

Active SSFX producer is now15 real passes; a fullres BGRA8 target supplies only the admitted selflightl emitter subset. Mandatory legacy bloom_build256² for luminance is separate from SSFX pyramid/filtering and was restored. Final luma/rain validation exits0/zero reports. HUDmask and complete emitter/reference coverage remain OPEN.
