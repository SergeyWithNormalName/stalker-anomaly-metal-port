# Direct Metal R4 resource-layer evidence

Date: 2026-09-09

Host GPU: Apple M1 Pro

Generated receipt:
`/private/tmp/r4-metal-resource-proof-6.txt`, SHA-256
`2856439bd6108945734402a2db8282ca8d40d8ca1475564e32cafaefedcad195`.

## Artifact chain

```text
IR4Backend.h:
6b75f7d031bad8240e6e3cb00b57cbd6e6a6b62551900ef044101054ca7a1d00

IR4Resources.h:
436ae8f8b15074ad725ab58ea214001a786e811c9f449c12480ba4d6f2f9ce6f

MetalR4Backend.mm:
e26f6b1c1513cbf88e961495020429ab822e8ec312304a2bd9278ee5c1c06af1

MetalBootstrap.mm:
f84342dd3f1166b752bd4db9a67379393a91a07fbdda947ac548b5a7f2808686

AnomalyMetalBootstrap:
107e23c3e385c1d0230fda1b7762c6b65d3b52dd3de4e347368651159e50afb5
```

## Runtime result

The final run used Metal API and GPU validation before device creation.

```text
METAL_BOOTSTRAP_READY device="Apple M1 Pro" logical=960x540 drawable=1920x1080 pixel_format=BGRA8Unorm frame_contexts=3
METAL_RESOURCE_SMOKE_READY shared_uniform=144/256 private_vertex=48 private_index=6 rgba8_rt_view=ready logical_d24s8=Depth32Float_Stencil8 bc1_upload=8 argument_buffer_sampler=ready invalid_layouts=rejected
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600 resource_deferred_releases=1
```

The smoke performs real private-buffer and BC1 texture uploads through shared
staging plus blit encoders before the render pass. Staging allocations and all
test resource handles remain in the owning frame context until completion.
Shared uniform allocation preserves a 144-byte logical range in a 256-byte
allocation. The sampler enables argument-buffer support.

Negative cases verify that undersized linear row/image pitch, undersized BC
block-row pitch, and generic depth/stencil upload are rejected before Metal is
called. Size computations use checked multiplication and block geometry.

## Boundary

This proves resource creation and upload constructibility, not R4 integration.
No engine mesh, texture, argument buffer, resource residency declaration, or
indexed draw uses these handles yet. Texture coverage is currently 2D/2D-array;
3D LUTs, MSAA/resolve resources, and depth-copy policy remain pending.
