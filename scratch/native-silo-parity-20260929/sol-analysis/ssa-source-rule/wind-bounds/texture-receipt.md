# `s_waves` texture contract receipt

## Frozen runtime asset

- Runtime path: `scratch/gameplay-rescue-20260929/runs/v2-canonical-oracle-01/runtime/gamedata/textures/fx/wind_wave.dds`.
- Resolved symlink: `<PRIVATE_LOCAL_PATH> Screen Space Shaders 23 - Ascii1457/gamedata/textures/fx/wind_wave.dds`.
- SHA-256: `4fb08cd504bb2a2e7be7f128d3eb2533b24afae8645ee977fa7321a2821b29e2`.
- File size: `262271` bytes.
- DDS header: legacy DDS, 256x256, mip count 9, `DDSD_PITCH` value 768 (=256*3), pixel flags `DDPF_RGB` (`0x40`), 24 bits, masks R=`0x00ff0000`, G=`0x0000ff00`, B=`0x000000ff`, A=0; no FOURCC and no cube/volume flags.
- Declared payload is tightly packed BGR24. The nine mip dimensions 256,128,64,32,16,8,4,2,1 contain 87,381 pixels total; at 3 bytes per pixel that is 262,143 bytes. With the 128-byte DDS header the expected end is exactly 262,271. The file has no unexplained tail. Base pitch is 768; the loader computes each mip row as width*3, matching this power-of-two layout. This conclusion comes from the declared mip chain and header, not an assumption based only on the total file size.

## Runtime decode and upload path

1. `MetalR4EngineScene.mm` resolves `fx/wind_wave.dds` into `options.flora.wavesTexturePath`.
2. `MetalR4FloraAdapter.cpp` calls `LoadR4DdsTexture`, then the flora `uploadDds` helper and labels it `R4 flora production wind-wave DDS`.
3. `R4DdsTexture.cpp::legacyFormat` accepts this exact RGB24 mask layout as `R4PixelFormat::RGBA8Unorm`, with three source bytes per pixel and `expandBgr24=true`.
4. The expansion loop writes each texel as `bgr[2], bgr[1], bgr[0], 255`. Every resulting channel is an 8-bit unsigned normalized value (RGB from source bytes, alpha 255). The `MetalR4Backend.mm` mapping creates `MTLPixelFormatRGBA8Unorm`; upload preserves the expanded mip row/image lengths. Sampling and linear mip/texel interpolation therefore remain within `[0,1]` per channel.

This closes the previous uncertainty about the wind-flow texture's numeric range for this frozen runtime asset and loader route. It does not establish bounds for the actual per-visual transformed H/UV extrema or current bender state/strengths. The overall wind-inflated SSA decision remains **no-go** until those measured inputs are captured and a conservative per-visual envelope is evaluated.

## Source pointers

- Resolved runtime path: `src/Layers/xrRenderMetal/MetalR4EngineScene.mm:6479-6480`.
- Flora load and upload call: `src/Layers/xrRenderMetal/MetalR4FloraAdapter.cpp:255-275,659-671`.
- RGB24 recognition: `src/Layers/xrRenderMetal/R4DdsTexture.cpp:128-136`.
- Exact mip layout and payload validation: `src/Layers/xrRenderMetal/R4DdsTexture.cpp:372-400`.
- BGR24 to RGBA8 expansion: `src/Layers/xrRenderMetal/R4DdsTexture.cpp:401-425`.
- Metal format mapping: `src/Layers/xrRenderMetal/MetalR4Backend.mm:476-477`.
