# Direct Metal R4 format map

Status: first resource-layer map, scoped to formats needed by real R4/GAMMA
content. The mapping starts from the engine's actual DXGI interpretation, not
from ambiguous legacy D3DFORMAT names.

## First scene and G-buffer formats

| R4 source | Existing DXGI interpretation | Direct Metal |
|---|---|---|
| `D3DFMT_A16B16G16R16F` | `R16G16B16A16_FLOAT` | `RGBA16Float` |
| `D3DFMT_A8R8G8B8` render targets | `R8G8B8A8_UNORM` | `RGBA8Unorm` |
| `D3DFMT_R32F` | `R32_FLOAT` | `R32Float` |
| `D3DFMT_R16F` | `R16_FLOAT` | `R16Float` |
| `D3DFMT_G16R16F` | `R16G16_FLOAT` | `RG16Float` |
| `D3DFMT_A2R10G10B10` | `R10G10B10A2_UNORM` | `RGB10A2Unorm` |

The current SDR reference configuration uses the first five classes. The
`RGB10A2Unorm` path is retained for later HDR parity; HDR/EDR presentation is
not enabled by the resource layer.

`CAMetalLayer` remains `BGRA8Unorm`. That is a presentation policy and must not
be used to change internal R4 `RGBA8Unorm` render targets to BGRA.

## Depth policy

R4 creates `D24S8` and typeless `R24G8` families with separate DSV and SRV
interpretations. Apple Silicon does not expose an equivalent arbitrary
24-bit typeless view family.

| R4 logical format | Direct Metal write target | Sample policy |
|---|---|---|
| `D16` | `Depth16Unorm` | explicit copy if a float texture is required |
| `D24S8` / `R24G8_TYPELESS` | `Depth32Float_Stencil8` | distinct `R32Float` copy/view resource |
| `D32F` / `R32_TYPELESS` | `Depth32Float` | distinct `R32Float` resource when MSC expects `texture2d<float>` |

The current resource API maps logical `Depth24UnormStencil8` to
`Depth32Float_Stencil8` deliberately. It does not claim 24-bit numerical or
typeless-view parity. Depth copying and comparison-sampler behavior remain
future render-pass work.

## DDS and material texture formats

The initial enum/map preserves these native Metal families:

- `R8`, `RG8`, `RG8Snorm`, `RGBA8`, `RGBA8Snorm`, `BGRA8`, and exact sRGB
  variants where they exist;
- `R16`, `RG16`, `RGBA16` unorm/float and `R32`, `RG32`, `RGBA32` float;
- BC1, BC2, BC3, BC4 unorm/snorm, BC5 unorm/snorm, BC6H float/ufloat, and BC7,
  preserving sRGB variants;
- `RGB10A2Unorm` and `RG11B10Float`.

BC uploads remain native 4x4 compressed blocks. `UploadTexture2D` accepts the
source block-row and image pitch and uses a shared staging buffer plus a blit
for private textures. It never treats BC data as bytes per pixel or silently
decompresses it.

The legacy DDS mask parser may choose `BGRA8` for some assets even though the
R4 render-target conversion maps `D3DFMT_A8R8G8B8` to `RGBA8`. The caller must
pass the decoded logical format; no label-based swizzle is inferred.

## Buffer and view policy

- Static vertex/index buffers: private allocation, uploaded from a shared
  staging buffer in an active frame before a render pass.
- Dynamic/uniform buffers: shared allocation on Apple Silicon. Uniform
  allocation size is rounded to 256 bytes while logical update bounds remain
  unchanged.
- GPU-visible staging objects are retained in the owning frame context and
  released only after its completion semaphore fires.
- Texture views require explicit `PixelFormatView` usage.
- Samplers are created with argument-buffer support for MSC bindings.

The current API covers 2D/2D-array textures and mip uploads. Three-dimensional
LUT textures, multisample/resolve textures, depth-copy commands, and actual
argument-buffer writes are still pending and must be added from observed R4
call sites rather than guessed.
