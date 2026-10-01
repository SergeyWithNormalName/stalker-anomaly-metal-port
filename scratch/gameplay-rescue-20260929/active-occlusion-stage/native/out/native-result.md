# Active-occlusion native Metal qualification fixture

One standalone offscreen probe run completed successfully. It used no game or app process, CMake, production source, screenshots, GPU capture, or Metal validation layer.

## Source and execution provenance

- Probe source SHA-256: `e47fbcb64b60bbb58db73fe75eed9d8dffb9b016006f085e584d97607d0cf6dd`.
- Frozen command file SHA-256: `2fe4932cd76bb0b5286f972eb62fa24287a106d8fccba48338561f3efb29a9c4`.
- CPU helper/header SHA-256: `786ba433a3352c7b676dddd184c2f069308f3b9e6f6c707acebfb07faae477e4` / `436a7601d0afb4a2e1ef702f0835c909f292bf163b13d18143dc6a1e91320731`.
- Compile exit 0; standalone run exit 0; timeout false. Exact argv and log paths are recorded in `native-run-receipt.json`.
- Device: Apple M1 Pro. OS: macOS `Version 27.2 (Build 26B5086k)`.

## Measured raster convention and fixture state

For both 128×128 and 1920×1200 full viewports, the probe measured clip +Y mapping to the lower readback row. Under `setFrontFacingWinding(MTLWindingClockwise)` plus BackCull, a logical positive signed area in upper-left-origin, y-down pixel coordinates survived; the reversed orientation was culled. The explicit helper inputs therefore used `ClipPositiveYMapsToLowerPixelY` and `PixelAreaWinding::PositiveIsClockwise`. Calibration pixel centroids and cull samples are in the raw log.

The fixture used passthrough float4 positions, 1× RGBA8Unorm and Depth32Float_Stencil8 attachments, full viewport, depth Less/write, stencil Always/Replace, and a 1-pixel raster edge guard. Each helper-hidden query was checked against every in-rectangle pixel center in the original unmerged GPU triangles: color matched, depth was finite and strictly less than query near depth, and stencil readback was 1. The exact adjacent-pair case also asserted an accepted CPU projected union while rendering both original triangles separately.

## Result

All 20 cases completed across the two target sizes with zero false-hidden results. Each size had three required hidden witnesses: a single triangle, the exact shared-seam pair with `pairedUnionsAccepted > 0`, and a grid-cell-edge rectangle whose far edge was `cellWidth + 8` pixels beyond the selected boundary. The smaller separations remained visible/unknown as intended: 0.5px and 2px at 128×128; 7.5px and 30px at 1920×1200. The reverse-index backface was culled with no query color; the frame-hole query had no color; slit, sloped-depth, near-plane uncertainty, and fractional/boundary cases stayed safe. Per-case pixel/sample counts and depth/stencil receipts are in `native-run.log`.

This qualifies only the synthetic passthrough fixture and the observed device raster convention with a 1px empirical edge guard. It does not qualify production MSC shader arithmetic, installed-material opacity, camera/WVP error bounds, native scene integration, culling, image correctness, or performance. The source explicitly reports native renderer equivalence as unqualified.
