# Native Metal and Windows R4 G-buffer format screen

## Finding

The proposed native FP32-versus-Windows FP16 G-buffer bandwidth gap is not present in the inspected native descriptors. The native R4 main G-buffer allocates RGBA16F, RGBA8, RGBA8, RGBA16F attachments, all at the current drawable extent. Its depth target is D32FloatStencil8. There is no RGBA32Float G-buffer attachment in this allocation path.

The current Windows R4 source also creates its position target as A16B16G16R16F, heat target as A8R8G8B8, and SSFX motion-vector target as A16B16G16R16F. Those are 16-bit-float / 8-bit-per-channel resources, not 32-bit-float resources. The Windows albedo/color target is conditional: for the measured `r4_hdr10_on 0` profile, it is A8R8G8B8 when mixed-depth MRT is supported, or A16B16G16R16F when mixed-depth MRT is not supported and FP16 blending is active. The run receipts do not record the effective `b_MRT_mixdepth` capability, so that one attachment's active format remains unresolved.

This rejects the broad FP32-versus-FP16 premise. It does not prove exact Windows/Metal channel or pass-semantic equivalence, nor does it prove byte-for-byte equality of every internal target. The possible albedo branch is the only format-width difference identified in this screen.

## Native descriptors and extent

The immutable Stage10 engine scene uses `ensureLevelDepthTarget`; its ordinary `MetalBootstrap.mm` implementation (pinned by hash below) calls `backend.DrawableSize(width, height)`, recreates targets when their dimensions differ, and assigns every G-buffer target the same `width`/`height`. For the production MSC path its color formats are, in order, RGBA16Float, RGBA8Unorm, RGBA8Unorm, RGBA16Float. The depth attachment is Depth32FloatStencil8. The Metal backend maps these enums directly to `MTLPixelFormatRGBA16Float`, `MTLPixelFormatRGBA8Unorm`, and `MTLPixelFormatDepth32Float_Stencil8`/equivalent depth mapping.

The matched native/Silo run reports 1920x1200 drawable output. Since the native internal target extent is explicitly taken from `DrawableSize` and revalidated against it, its expected measured G-buffer extent is 1920x1200. This is stronger than inferring size from `vid_mode`; it is still a code-and-drawable join rather than a per-attachment runtime descriptor dump.

## Windows R4 source branch

`r4_rendertarget.cpp` allocates `rt_Position` at `Device.dwWidth` × `Device.dwHeight` with `D3DFMT_A16B16G16R16F`. It allocates `rt_Heat` at the same dimensions with `D3DFMT_A8R8G8B8`, and `rt_ssfx_motion_vectors` with `D3DFMT_A16B16G16R16F`. The color target branch uses `mrtmixdepth`, `dx11_hdr10`, and `fp16_blend` as described above. `r4.cpp` derives `mrtmixdepth` from `HW.Caps.raster.b_MRT_mixdepth`; it initializes `fp16_blend` true in the inspected source unless the R4xx compatibility parameter disables it, and assigns `dx11_hdr10` from `ps_r4_hdr10_on`.

The captured Silo configuration has `vid_mode 1920x1200`, `r4_hdr10_on 0`, and `r3_msaa st_opt_off`. In the inspected R4 source, `SampleCount` starts at 1 and is changed only when `dx10_msaa` is active; the captured MSAA-off configuration therefore selects one sample. The matched MetalPerfTrace report confirms 1920x1200 drawable output and no MetalFX scaling. Its report explicitly says internal Windows target dimensions were not captured. The source allocates R4 targets at `Device.dwWidth`/`Device.dwHeight`, so 1920x1200 is the expected extent for this non-scaled run, but internal Windows attachment dimensions are not directly observed.

## PE/PDB provenance and limits

The existing comparator provenance pins the unchanged stock Windows executable to SHA-256 `c0bf73944e700425734d95b71f5c8fd201b669bac67d295357a141442103bdd3` and records that its associated PDB/machine-code report is from the same PE/PDB generation. That report documents SSA controls, not the render-target creation instructions. The local R4 source is therefore a source-contract reference; this screen does not claim an exact source-to-stock-binary match for the conditional albedo branch.

A narrow follow-up, only if the albedo bandwidth branch matters, is to capture `b_MRT_mixdepth` or the created `rt_Color` descriptor from the same stock Windows runtime and confirm `Device.dwWidth`/`Device.dwHeight`. No renderer change is justified by the alleged native FP32 target: it is absent from the native source descriptor path.

## Source anchors and hashes

- Native Stage10 `MetalR4EngineScene.mm`: SHA-256 `ce08343fa3b890673b9370564996ab88044d9aac1b1ee2d98cfe19a5b7d3b4ab`; `ensureLevelDepthTarget` at lines 3577–3656.
- Native ordinary `MetalBootstrap.mm`: SHA-256 `fb1b1052704eab1756bb12bdbc8bcdd46f96c5962481de1aa5e59c2b793ad27c`; format enum-to-Metal mapping is in `MetalR4Backend.mm` around lines 470–489.
- Windows source `r4_rendertarget.cpp`: SHA-256 `07f3a2ed748eeccc7a970bca68987f94438350988604a908637a3ac1523aa531`; target creation at lines 489–594.
- Windows source `r4.cpp`: SHA-256 `65500f4ca09f208dc4643643aa8ab4944bc42741ce0be1348d8a8d848d0f650c`; capability and HDR/MSAA setup at lines 230–233, 317–349, and 419–424.
- Matched Silo post-run `user.ltx`: SHA-256 `dc1a7125eacdddfada625715c47fd60ba8d0eaae5a05d2828f937fbf6aac419e`.
- Matched native/Windows run summary: SHA-256 `cc7d002b5fc716205ce76e0a9e066049f23ed3d603cfff34a99d9e20234b675a`.
- Windows PE/PDB comparator provenance note: SHA-256 `ed299be837bf0aa34b2b10d9e45a4c4321720d9fae13ce7a869249304141ddf2`.

No source, build, game, or capture action was performed for this screen.
