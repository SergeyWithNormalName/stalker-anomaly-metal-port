# Direct Metal R4 lifecycle evidence

Date: 2026-09-09

Host GPU: Apple M1 Pro

The generated receipt is
`/private/tmp/r4-metal-lifecycle-proof-5.txt`, SHA-256
`82a054c8b9bd660a1112fde5b278b88db27eabd6363552832e9e7b733950ea33`.

## Artifact chain

```text
IR4Backend.h:
3227e3f6d0991da280305845e2c6d7b54627000893f2c98c127d08a0a377e2fb

MetalR4Backend.mm:
b7f6fd5b174818c8432e07e49dd17eef6d2ed1990918b1d8cb868a96f09c8830

MetalBootstrap.mm:
f01221ef3c4bc7b5a3fc29105623fd847662ec47dff2303b94be636c6d315080

AnomalyMetalBootstrap:
d41900aab20c2f5c9402ee666a935cf413f5c44191f06508f16f63bc3508962d
```

## Runtime result

The process ran with both `MTL_DEBUG_LAYER=1` and
`MTL_SHADER_VALIDATION=1` before device creation.

```text
Metal API Validation Enabled
Metal GPU Validation Enabled
METAL_BOOTSTRAP_READY device="Apple M1 Pro" logical=960x540 drawable=1920x1080 pixel_format=BGRA8Unorm frame_contexts=3
METAL_BOOTSTRAP_COMPLETE frames=600 submitted_serial=600 completed_serial=600 deferred_releases=600
```

The receipt wrapper also exposed an earlier post-main `SIGTRAP`: shutdown had
consumed each dispatch semaphore to drain its frame context but did not balance
the wait before ARC destroyed the semaphore. The final implementation signals
and clears each semaphore only after command completion and deferred releases.
A one-frame teardown test and the 600-frame run above both exit with status 0.

## Backend selector admission

The parser smoke covers default, explicit `dxvk`, explicit `metal`, missing,
unknown, `=`-form, and duplicate options:

```text
R4_BACKEND_SELECTION_READY default=dxvk opt_in=metal invalid_values=rejected
```

The complete `AnomalyDX11` target builds after the selector hunk. Two launches
from the real GAMMA runtime prove the early stop boundary:

```text
-render_backend metal   -> recognized explicit opt-in, exits 3 before renderer/window creation
-render_backend vulkan  -> rejected unknown value, exits 2 before renderer/window creation
```

No option and `-render_backend dxvk` preserve the existing default code path.
The selector does not yet create a Metal engine window because the main R4
device wrapper is still D3D11/DXVK.

## Boundary

This evidence proves a reusable render-thread lifecycle with three exact frame
contexts, explicit render-pass boundaries, submit/present, completion serials,
deferred release, resize-at-frame-boundary, and shutdown drain. It remains a
standalone clear/present backend: no real R4 resource or draw command is routed
through it yet.
