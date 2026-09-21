# Direct Metal R4 architecture

Status: design accepted by the Phase 0 GO gate. The real-shader DXC/MSC gate,
early backend selector, lifecycle, resource layer, immutable PSO cache, and
standalone real-level static-geometry milestone are complete. The Metal
lifecycle is not yet connected to the main R4 renderer.

Reference backend: existing R4 D3D11 -> DXVK Native -> custom MoltenVK path.

## Existing boundary and design rule

`IRender_interface` and `IRenderFactory` already isolate gameplay, Lua, AI/A-Life, level logic, and UI ownership from the renderer. They are the correct outer boundary and should remain stable.

Inside R4, however, high-level pass logic is coupled directly to D3D11 through `CHW`, `RCache`, `CResourceManager`, `dx10StateManager`, D3D resource pointer typedefs, and `CRenderTarget`. Replacing those types across gameplay would be destabilizing. The new seam therefore belongs inside R4:

```text
engine/gameplay interfaces (unchanged)
  -> R4 scene, visibility, materials, pass ordering (shared)
     -> narrow R4 backend contract
        -> existing D3D11 backend (reference/default)
        -> native Metal backend (opt-in until parity)
```

The contract is shaped around the operations R4 actually performs. It is not a new general-purpose engine API.

## 1. Resource abstraction

Introduce backend-neutral R4 handles/descriptors for buffers, textures, texture views, samplers, shaders, pipeline state, render targets, depth targets, and queries. Descriptors retain R4-visible properties: dimensions, format, mip/array shape, usage, bind flags, CPU update policy, and debug name.

Existing `resptr_core` ownership and resource-manager lookup/deduplication stay shared. Backend objects live behind typed implementation objects. The D3D11 implementation initially wraps today's `ID3D11*` objects unchanged; the Metal implementation owns `id<MTLBuffer>`, `id<MTLTexture>`, `id<MTLSamplerState>`, and PSOs.

Do not expose Metal or D3D headers through the outer `xrEngine` renderer interfaces.

## 2. Command submission model

Use one R4 render-thread `FrameContext` per frame in flight (initially three). A context owns transient uniform/argument-buffer ranges, upload work, one or more Metal command buffers, and a monotonically increasing completion serial.

High-level R4 passes record through a compact backend command interface: begin/end render pass, viewport/scissor, pipeline, vertex/index buffers, resource tables, constants, draw, query, blit/copy, and present. The Metal backend may group compatible R4 phases in one command buffer, but the first parity implementation preserves pass order and boundaries. No mid-frame `waitUntilCompleted` is permitted.

The existing D3D11 path remains its own implementation and does not route through Metal shims.

## 3. Pipeline-state strategy

Metal PSOs are immutable. Cache them by a complete key containing converted vertex/fragment functions, vertex layout, color/depth/stencil formats, sample count, blend state, raster state, depth/stencil state, and relevant function constants.

Create common/high-frequency PSOs during level or shader warm-up and rare permutations asynchronously where correctness allows. A missing PSO must use a visible diagnostic/failure path; silently substituting a visually different shader is not acceptable. Persist a versioned cache only after the in-memory key is proven correct; invalidate it on shader source, compiler, MSC version, root signature, or attachment-format changes.

## 4. Render-target representation

Represent an R4 render target as a texture plus explicit color/depth/stencil view, resolve target if needed, attachment load/store actions, clear value, and lifetime class (persistent, history, level, frame transient).

The initial Metal backend uses stored textures matching the D3D11 reference formats and pass boundaries. Memoryless attachments, framebuffer fetch, pass merging, and tile-local data are later measured optimizations, never assumptions. Depth sampling is explicit: a pass needing shader-readable depth receives a compatible R32F copy/view because MSC shaders cannot generally sample a Metal depth texture as `texture2d<float>`.

## 5. Texture and buffer lifetime

- Static geometry and immutable textures: `MTLStorageModePrivate`, populated by blit from shared staging resources.
- Frequently updated constants and small dynamic streams: ring-allocated `MTLStorageModeShared` ranges, aligned and separated per frame in flight.
- Readback/screenshots: shared staging buffers synchronized by command-buffer completion.
- Resource destruction: enqueue the backend object with the last-used completion serial and release only after that serial completes.
- Samplers referenced by MSC descriptor entries remain alive as long as any entry can reach them.

The first implementation uses Metal's tracked hazards. Untracked hazards are considered only after correctness and profiling evidence.

## 6. Synchronization strategy

Use command-buffer ordering within a queue for normal pass dependencies, encoder boundaries for render/compute/blit transitions, and completion handlers plus a three-frame semaphore for CPU/GPU flight control. Cross-frame history resources carry explicit last-writer serials.

Avoid global waits, per-draw fences, and CPU polling. Resize and backend shutdown drain in-flight frames once at the boundary. Drawable ownership ends immediately after present. If a later Metal 4 command-queue path is adopted, the layer residency set and explicit wait/signal/present contract must be added as one coherent change; the initial backend uses the established Metal 3 command-buffer presentation API.

## 7. Shader pipeline

Production direction:

```text
existing R4/GAMMA HLSL source + permutation defines
  -> DXC Shader Model 6.x DXIL
  -> Metal Shader Converter
  -> metallib + MSC reflection + DXIL reflection metadata
  -> versioned R4 Metal shader cache
```

Do not hand-rewrite the shader library to MSL. Preserve the existing DXBC cache and DXVK path for reference.

Current feasibility findings:

- Active R4/GAMMA gameplay is overwhelmingly vertex and pixel shaders.
- The materialized `r3` tree currently contains 133 `.ps` sources, 50 `.vs` sources, one hull shader (`dx11/tess.hs`), one domain shader (`dx11/tess.ds`), and no geometry or compute shader sources. Tessellation is outside the benchmark contract because `r4_enable_tessellation off`.
- The existing cache contains 1,346 proven DXBC permutations and remains the exact reference set.
- Pinned ARM64 DXC `v1.9.2607` and an isolated, hash-pinned Metal Shader
  Converter 4.0.1 payload are discoverable project-locally.
- Nine real GAMMA/R4 representative entries compile to DXIL, convert to
  metallib, expose structurally validated reflection, and load on Apple M1 Pro.
- Metal API and GPU validation accept a depth-only shadow PSO and a four-MRT
  `SKIN_0` G-buffer PSO using the exact 36-byte engine vertex declaration.

The committed representative manifest and evidence chain are documented in
`DIRECT_METAL_SHADER_TOOLCHAIN.md` and
`DIRECT_METAL_SHADER_GATE_EVIDENCE.md`. Mass conversion remains deferred until
the runtime permutation/cache key is defined.

Runtime binding follows MSC rather than ad-hoc slot numbering:

- Bind the top-level argument buffer at `kIRArgumentBufferBindPoint`.
- Obtain top-level offsets from `IRRootSignatureGetResourceLocations`; never calculate them manually.
- Use DXIL reflection for offsets inside descriptor tables.
- Use MSC vertex buffer/attribute base indices (`kIRVertexBufferBindPoint`, `kIRStageInAttributeStartIndex`).
- Create samplers with argument-buffer support and keep them alive.
- Make every indirectly referenced resource resident.
- Store reflection beside each metallib and reject version mismatches.

## 8. Window and presentation

Retain SDL/Cocoa input, lifecycle, and window ownership. Backend selection determines the SDL graphics surface before window creation:

- D3D11/DXVK: today's `SDL_WINDOW_VULKAN` path.
- Metal: `SDL_WINDOW_METAL`, `SDL_Metal_CreateView`, and its `CAMetalLayer`.

The Metal layer uses BGRA8 SDR initially, `framebufferOnly = YES`, a physical drawable size obtained from `SDL_Metal_GetDrawableSize`, and ordinary command-buffer `presentDrawable`. Resize changes are applied between frames. HDR/EDR, MetalFX, and custom display-link pacing are out of scope until parity and baseline performance are healthy.

## 9. Fallback and backend selection

Add a single early selector owned by renderer startup, conceptually:

```text
-render_backend dxvk   (default, permanent reference)
-render_backend metal  (opt-in until all gates pass)
```

Selection occurs before SDL window flags and renderer global initialization. It must not be scattered as `#ifdef __APPLE__` through pass code. During migration, incomplete Metal phases fail with a clear capability report; they do not fall through mid-frame to D3D11.

Current admission behavior parses exact whitespace tokens immediately after
`Core._initialize`: no option or `dxvk` preserves the reference path; `metal`
is explicit opt-in. Invalid, missing, `=`-form, and duplicate values fail
before renderer probing. Until the main R4 device wrapper owns an
`IR4Backend`, the accepted Metal choice also stops there with a clear message;
it never silently falls back or creates a Vulkan window for Metal.

The bootstrap delivered with this document is a separate executable target on purpose: it proves the platform contract without changing the playable engine selector. Integrating the selector is the first step of the next milestone, after the backend contract skeleton exists.

## 10. Migration phases

1. Isolated SDL/CAMetalLayer clear/present bootstrap.
2. Add backend selection and an empty `IR4Backend` lifecycle without changing the default.
3. Buffer, texture, sampler, view, render/depth target, and transient-upload infrastructure with focused tests.
4. Representative DXC/MSC conversion harness and reflection/binding validation.
5. Static level geometry with depth and flat/unlit output.
6. Textures/materials, skinned geometry, and the complete G-buffer.
7. Deferred lighting, sun, and shadows.
8. Details/grass, particles, water, volumetrics.
9. HUD/UI, postprocessing, and GAMMA-specific permutations.
10. Visual-parity gate, then same-scene performance gate.
11. Only after parity: measured TBDR/pass-memory optimizations.
12. Only after a healthy native renderer: MetalFX experiment.

Every phase keeps `AnomalyDX11` buildable and runnable.

## Current lifecycle implementation

`src/Layers/xrRenderMetal/IR4Backend.h` is a Metal-free render-thread contract
for initialize, drawable resize, frame acquire, render-pass begin/end, submit,
present, deferred release, and shutdown. `MetalR4Backend.mm` implements it with
three independently gated frame contexts on one command queue. Each context
owns its drawable, command buffer, active encoder, completion serial, and
deferred-release list.

There are no mid-frame completion waits. A context is reused only after its
own completion semaphore fires. Deferred releases run on the render thread
after that completion. Shutdown submits any active frame, drains all three
contexts, checks command-buffer errors, releases retired resources, and only
then destroys the SDL Metal view.

The 2026-09-09 Apple M1 Pro acceptance run completed 600 frames at a 1920x1080
physical drawable with Metal API and GPU validation enabled:

```text
METAL_BOOTSTRAP_READY ... frame_contexts=3
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600
```

This is lifecycle evidence only. It does not yet mean that the engine's R4
draw stream, resources, or shaders are routed through the backend.

The complete artifact hashes, selector admissions, teardown finding, and
runtime receipt are preserved in `DIRECT_METAL_LIFECYCLE_EVIDENCE.md`.

The first buffer/texture/view/sampler map and its validated upload receipt are
documented in `DIRECT_METAL_FORMAT_MAP.md` and
`DIRECT_METAL_RESOURCE_EVIDENCE.md`.

## Current pipeline and static-geometry implementation

`IR4Pipeline.h` defines the initial backend-neutral immutable graphics state:
shader identity/source and entry points, vertex attributes/layouts, attachment
formats, sample count, blend state, depth compare/write state, cull mode, and
front-face winding. `MetalR4Backend.mm` serializes every field into a complete
in-memory cache key, owns the cached PSOs until shutdown, validates vertex and
index ranges before encoding, and retains bound resources in the owning frame
context through GPU completion.

`R4LevelGeometry.cpp` is a portable, read-only parser for the exact
`level.geom` VB/IB and `level` visual-container records consumed by the current
R4 loader. It rejects compressed/truncated/overflowing chunks, unsupported
declarations, invalid container IDs/ranges, out-of-range local indices, and
non-finite positions. The standalone backend uploads the active GAMMA
`l05_bar` profile winner into private buffers and draws all 8,075 validated
static visual ranges with a stable bounds-derived camera and real depth.

This milestone intentionally uses a flat diagnostic MSL shader. It proves the
geometry/PSO/draw/depth seam, not production shader binding or visual parity.
Exact assets, runtime receipt, screenshot, and limitations are recorded in
`DIRECT_METAL_STATUS.md`.

The current diagnostic layer preserves `fsL_SHADERS` material slots, OGF
`shader_id`, the `OGF_CHILDREN_L` hierarchy, and `OGF_TREEDEF2` model
transforms. It maps raw `SHORT2` and `SHORT4.xy` base UVs using the same
`tc / 1024` rule as the R3 HLSL and loads strict 2D DDS mip chains. A visual
binding owns its per-object uniform while repeated material references share a
single texture asset and upload. Direct MSL diagnostics bind the texture and
sampler explicitly on the fragment encoder; converted production shaders will
instead use the MSC top-level argument buffer and reflection contract
documented above. The backend rejects BC textures on devices that do not
report `supportsBCTextureCompression`. A complete three-material Niva
hierarchy from the active `l05_bar` has passed 600 frames of Metal API and GPU
validation; the exact receipt and remaining Phase 8 boundary are recorded in
`DIRECT_METAL_STATUS.md`.

The next admitted material input is the active R4 `default` LMH specialization,
traced through `CBlender_deffer_flat` and `uber_deffer`: texture-list slot 2 is
bound as `s_hemi`, while slot 1 is intentionally unused by this DX10/11 path.
The diagnostic decodes `TEXCOORD0 SHORT2` as
`(tc + tangent.a, binormal.a) / 1024`, decodes `TEXCOORD1 SHORT2` as
`tc / 32768`, samples the BC3 hemi texture with a separate clamp/linear
sampler, and exposes `base.rgb * hemi.a` only as a causal proof. It is not a
substitute for the converted production shader, G-buffer outputs, or deferred
lighting.

## Current production MSC and G-buffer implementation

The first real R4 material has now crossed the production shader/runtime seam.
The exact active `deffer_base_lmh_flat.vs/.ps` GAMMA wrappers are compiled by
DXC to DXIL and offline-converted by Metal Shader Converter 4.0.1. The Metal
backend loads each single-function metallib, applies MSC's reserved vertex
attribute and buffer indices, and issues converted indexed draws through the
MSC runtime companion.

Automatic-linear reflection drives two independent per-draw top-level argument
buffers: 48 bytes for the vertex stage and 144 bytes for the fragment stage.
The backend writes their 24-byte entries with the MSC helpers, retains each
table and reached resource until its frame context completes, and declares
indirect buffer/texture residency with `useResource:usage:stages:`. Existing
direct MSL bindings remain available only for diagnostic paths.

`R4RenderPassDesc` now supports an explicit off-screen MRT set while an empty
set preserves the drawable path. The admitted production scene pass uses the
active non-MSAA R4 tuple:

```text
RT0 RGBA16Float  packed position/normal/hemi/material
RT1 RGBA8Unorm   albedo/gloss
RT2 RGBA8Unorm   heat
RT3 RGBA16Float  motion vectors
DS  Depth32Float stored for later deferred consumers
```

The first production lighting consumer now follows that G-buffer. Exact
`combine_1.vs` and non-MSAA `combine_1_nomsaa.ps` artifacts are compiled by
DXC and converted by MSC, then admitted by adjacent receipt hashes and complete
reflection. Their automatic-linear TLABs are 24 bytes for the VS (`s_tonemap`)
and 336 bytes for the PS (eight SRVs, three CBVs, and three samplers). The
fullscreen FVF::TL draw uses MSC attributes 11/12 and vertex buffer 6 and writes
the real pair of RGBA8Unorm R4 combine targets.

For the first controlled base-lighting proof, the accumulator is a persistent
RGBA16Float texture cleared to zero, the two environment inputs are a real
six-face RGBA32Float cube with a complete mip chain, AO is white, and indirect
light is black. The converted shader consumes the production G-buffer position,
albedo/gloss, and motion textures. A final diagnostic MSL pass only copies the
low combine target to the drawable; it does not alter the lighting result.

That first combine milestone was enough to render a recognizable real `l05_bar`
material cluster through the genuine base/hemi/environment path, but it was not
direct-light parity. The auxiliary `accum_sun_nomsaa` permutation is not used as
a substitute for the real mask/cascade sequence described below.

The first direct-sun implementation now supplies that missing backend
infrastructure. Pass descriptors distinguish a drawable from true off-screen
and depth-only work, admit non-drawable attachment sizes, and expose explicit
load/store, depth/stencil, viewport, scissor, and debug-label state. Pipeline
identity includes color write masks and full front/back stencil state; stencil
reference remains dynamic on the encoder. This supports the real R4 marker flow
without contaminating the D3D/DXVK state manager.

The exact converted non-MSAA mask and far-cascade shaders now execute between
the G-buffer and `combine_1`. A converted shadow pass writes both ordinary
Depth32 and shader-readable R32 depth at 2048x2048. The R32 output is necessary
because MSC's `Texture2D<float>` binding cannot sample a Metal depth attachment
directly. GPU capture confirms the producer, marker mask, cascade consumer, and
combine passes in order. The next correctness step is to derive separate
near/middle/far volume and shadow transforms from live cascade state and widen
caster/material coverage before engine-runtime integration.

## 11. Visual-parity test plan

For each subsystem, capture Direct Metal and DXVK reference images from the same save, camera transform, time, weather, resolution, and settings. Use day outdoor, night outdoor, interior, heavy grass, sun/shadows, rain, water, NPC/mutant, weapon/HUD, scope, and PDA/UI scenes.

Automated checks cover resource existence, attachment formats, pipeline keys, shader/reflection hashes, vertex layouts, binding-table bounds, and read/write hazards. Image comparison uses linear-space absolute/relative error plus perceptual diff, with masks only for known temporal noise. Human review remains mandatory for motion, flicker, blending, gamma, shadow stability, and synchronization artifacts.

## 12. Benchmark gates

All comparisons use the same verified 1920x1080 physical drawable and fixed GAMMA scene.

- Bootstrap gate: stable visible clear/present; no FPS claim.
- First geometry gate: correct transforms/depth/resources at the same camera; performance reported but not used as final ROI.
- Parity gate: practical visual parity across the scene matrix before Apple-specific optimization.
- Performance gate: average, median, p95, p99, 1% low, CPU submission, GPU execution, and memory versus DXVK SAFE.
- Continue if parity-capable Direct Metal produces a useful measured gain or profiling identifies a narrow, credible fix.
- Stop and review if it is equal/slower, if critical shader stages require a major redesign, or if parity requires gameplay/core-engine destabilization.

No target such as 60 FPS is a promise. Stable frame pacing is a first-class gate, not a side effect of average FPS.

## Modules expected to change

Shared/high-level code retained: `xrEngine` renderer interfaces, `xrRender` scene/visibility/material logic, R4 pass ordering, resource naming, GAMMA configuration, gameplay/Lua/AI/A-Life.

Backend seam and implementations:

- New: `src/Layers/xrRenderMetal/` for Metal device/presentation/resources/commands/PSOs and later shader binding.
- New: a narrow backend contract under `src/Layers/xrRender/` or `xrRenderPC_R4/` after the bootstrap review.
- Adapted: `xrRenderDX10`/`CHW` and D3D resource implementations become the D3D11 backend side without semantic rewrites.
- Adapted: `CResourceManager`, `CBackend`/`RCache`, and `CRenderTarget` call the narrow backend contract as each migration phase lands.
- Adapted once: `Device_Initialize.cpp` and renderer startup for early backend selection/window flags.
- New: shader conversion manifest, cache tooling, reflection validation, and focused resource/pipeline tests.

No gameplay, save, GAMMA profile, DLTX/DXML, Lua, AI/A-Life, or configuration semantics are in scope.
